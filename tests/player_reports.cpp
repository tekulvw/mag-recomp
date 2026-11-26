// Synthetic tests of the performance and error reports (src/player_reports.h). Every input is made
// up here: logs, summaries, receipts, settings and device tokens.
// Usage: player_reports selftest <empty scratch folder>
//        player_reports build <empty scratch folder>   (prints the queued report's path)
//        player_reports send <queue> <settings file>    (the platform transport; prints the line)
#include "player_reports.h"
#include "player_settings_text.h"
#include "player_gzip.h"
#include "player_report_request.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace mag::reports;
using mag::portal::Headers;
using mag::portal::Response;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static const std::string token = "mgd_SyntheticDeviceToken0123456789abcdef";
static const std::string users = "/Users", homes = "/home";  // split: see tools/check_public_tree.py
static const std::string home = users + "/synthetic-player";
static const std::string windows_home = "C:\\Users\\Synthetic.Player";

static void write(const fs::path &p, const std::string &text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
static std::string read(const fs::path &p) { std::ifstream in(p, std::ios::binary); std::ostringstream b; b << in.rdbuf(); return b.str(); }

struct FakeTransport : mag::portal::Transport {
    std::deque<Response> replies;
    std::vector<std::pair<std::string, Headers>> requests;
    Response post(const std::string &url, const std::string &, const Headers &headers) override {
        requests.emplace_back(url, headers);
        if (replies.empty()) return {0, "", "no reply"};
        Response r = replies.front(); replies.pop_front(); return r;
    }
    void add(int status, int retry_after = -1) { Response r{status, "{}", ""}; r.retry_after = retry_after; replies.push_back(r); }
};

static std::string summary(const char *role, long long frames, double seconds, long long over_50, long long over_250,
                           const std::string &audio = "{\"available\": false}") {
    return std::string("{\"schema_version\": 1, \"role\": \"") + role + "\", \"state\": \"ended\", \"frames\": {\"count\": " +
           std::to_string(frames) + ", \"seconds\": " + std::to_string(seconds) + ", \"slow\": {\"over_33ms\": " + std::to_string(over_50) +
           ", \"over_50ms\": " + std::to_string(over_50) + ", \"over_100ms\": 0, \"over_250ms\": " + std::to_string(over_250) +
           "}, \"histogram_ms\": [[16.5, " + std::to_string(frames) + "]]}, \"minutes\": [], \"hitches\": [], \"audio\": " + audio + "}\n";
}

// A launch folder like the supervisors make, with logs that mention secrets and home folders.
static Session launch(const fs::path &root, const std::string &settings_text = "{\"schema_version\":1,\"server_address\":\"play.magtest.online\",\"player_name\":\"Synthetic\"}") {
    Session s;
    s.platform = "linux";
    s.version = "0.7.0";
    s.logs = root / "logs/launch-0123456789abcdef0123456789abcdef";
    s.settings = root / "config/settings.json";
    s.queue = root / "home/reports";
    s.receipts = root / "resources/package-receipts";
    s.backend = "Vulkan";
    s.system = {{"os", "Synthetic OS 1"}, {"cpu", "Synthetic CPU"}, {"cpu_threads", "8"}};
    s.redaction.homes = {home, windows_home};
    write(s.settings, settings_text);
    write(s.settings.parent_path() / "portal-link.json",
          "{\"schema_version\":1,\"portal\":\"https://portal.invalid\",\"device_token\":\"" + token + "\",\"player_name\":\"Synthetic\"}");
    write(s.receipts / "launcher.json", "{\"image_name\":\"EBOOT\",\"sdk_revision\":\"" + std::string(40, 'a') + "\",\"binary_sha256\":\"" +
                                            std::string(64, 'b') + "\",\"build_path\":\"" + homes + "/builder/secret\"}");
    write(s.receipts / "engine.json", "{\"image_name\":\"mage_g\",\"sdk_revision\":\"" + std::string(40, 'a') + "\"}");
    write(s.logs / "launcher.log", "[boot] VFS root: " + home + "/MAG/game\nAuthorization: Bearer " + token + "\n");
    write(s.logs / "engine.log", "[RSX vulkan] using device 0: Synthetic GPU 9000\n[boot] " + windows_home + "\\MAG\\game\n" +
                                     "escaped C:\\\\Users\\\\Synthetic.Player\\\\MAG and c:/users/synthetic.player/MAG\n" +
                                     "other " + homes + "/someone/x and C:\\Users\\Public\\ok\n" + token + "\n");
    write(s.logs / "supervisor.log", "2026-11-23T00:00:00Z started\n");
    write(s.logs / "session-engine.json", summary("engine", 36000, 600, 3000, 2));
    return s;
}

static void json_and_quote() {
    Json v;
    CHECK(parse_json("{\"a\": [1, -2.5e3, true, null, \"x\\u00e9\\ud83d\\ude00\"], \"b\": {}}", v));
    CHECK(to_json(v) == "{\"a\":[1,-2.5e3,true,null,\"x\xC3\xA9\xF0\x9F\x98\x80\"],\"b\":{}}");
    CHECK(v.get("a") && v.get("a")->items[1].number() == -2500);
    for (const char *bad : {"", "{", "{\"a\":1,}", "[01]", "{\"a\":1}x", "{\"a\":1,\"a\":2}", "\"\\ud800\"", "nul"}) CHECK(!parse_json(bad, v));
    CHECK(quote("a\"b\\\n\x01") == "\"a\\\"b\\\\\\n\\u0001\"");
    CHECK(quote("\xff\xC3") == "\"\xEF\xBF\xBD\xEF\xBF\xBD\"");
}

static void redaction() {
    Redaction r{{"secret-ticket-text"}, {{"/Volumes/Games/MAG", "<MAG folder>"}}, {home, windows_home}};
    const std::string text = "t=" + token + " Bearer abc.def-ghi_jkl x secret-ticket-text " + home + "/MAG " + home + "x " +
                             "c:\\users\\SYNTHETIC.PLAYER\\MAG " + "/Volumes/Games/MAG/logs /Volumes/Games/MAGIC " + users + "/Shared/ok " + users + "/other/a " + homes + "/u/b D:\\Users\\Default\\ok";
    const std::string out = redact(text, r);
    CHECK(out.find("SyntheticDeviceToken") == std::string::npos);
    CHECK(out.find("abc.def") == std::string::npos);
    CHECK(out.find("secret-ticket-text") == std::string::npos);
    CHECK(out.find("synthetic-player") == std::string::npos);
    CHECK(out.find("SYNTHETIC") == std::string::npos);
    CHECK(out.find("~/MAG") != std::string::npos);
    CHECK(out.find("~\\MAG") != std::string::npos);
    CHECK(out.find(users + "/Shared/ok") != std::string::npos);
    CHECK(out.find("other") == std::string::npos);
    CHECK(out.find(homes + "/u") == std::string::npos);
    CHECK(out.find("D:\\Users\\Default\\ok") != std::string::npos);
    CHECK(out.find("<MAG folder>/logs") != std::string::npos && out.find("/Volumes/Games/MAGIC") != std::string::npos);
    CHECK(redact("mgd_short", r) == "mgd_short");  // too short to be a token
}

static void decisions(const fs::path &root) {
    std::vector<std::string> reasons;
    Json j;
    CHECK(parse_json(summary("engine", 36000, 600, 3000, 2), j));
    CHECK(judge_summary(j, reasons) == Trigger::SlowFrames);  // 8.3% over 50 ms
    reasons.clear();
    CHECK(parse_json(summary("engine", 36000, 600, 100, 2), j));
    CHECK(judge_summary(j, reasons) == Trigger::None);
    CHECK(parse_json(summary("engine", 36000, 600, 100, long_frame_count), j));
    CHECK(judge_summary(j, reasons) == Trigger::SlowFrames);
    reasons.clear();
    CHECK(parse_json(summary("engine", 1000, 30, 900, 100), j));  // too short to judge
    CHECK(judge_summary(j, reasons) == Trigger::None);
    const std::string audio = "{\"available\": true, \"underruns\": 39, \"device_errors\": 0, \"clock_seconds\": 600, \"drift_ms\": 1.0}";
    CHECK(parse_json(summary("engine", 36000, 600, 10, 0, audio), j));
    CHECK(judge_summary(j, reasons) == Trigger::None);  // 39 < 20 + 600 / 30
    const std::string bad_audio = "{\"available\": true, \"underruns\": 41, \"device_errors\": 0, \"clock_seconds\": 600, \"drift_ms\": 1.0}";
    CHECK(parse_json(summary("engine", 36000, 600, 10, 0, bad_audio), j));
    CHECK(judge_summary(j, reasons) == Trigger::Audio);
    const std::string errors = "{\"available\": true, \"underruns\": 0, \"device_errors\": 1, \"clock_seconds\": 5, \"drift_ms\": 0}";
    CHECK(parse_json(summary("engine", 10, 1, 0, 0, errors), j));
    CHECK(judge_summary(j, reasons) == Trigger::Audio);
    const std::string drift = "{\"available\": true, \"underruns\": 0, \"device_errors\": 0, \"clock_seconds\": 600, \"drift_ms\": -2500}";
    CHECK(parse_json(summary("engine", 36000, 600, 0, 0, drift), j));
    CHECK(judge_summary(j, reasons) == Trigger::Audio);

    CHECK(is_crash_exit(139, "linux") && !is_crash_exit(1, "linux") && !is_crash_exit(90, "macos"));
    CHECK(is_crash_exit(0xC0000005LL, "windows") && !is_crash_exit(139, "windows"));
    CHECK(is_halt_exit(88) && is_halt_exit(87) && !is_halt_exit(1));

    Session s = launch(root / "decide");
    s.launcher_exit = 90; s.engine_exit = 0;
    CHECK(decide(s).trigger == Trigger::SlowFrames);
    s.engine_exit = 139;
    CHECK(decide(s).trigger == Trigger::Crash);
    s.engine_exit = 88;
    write(s.logs / "engine.log", "x\n[hle] UNRESOLVED NID 0x99AB1A26 lr=0x00000000 r3..r6=0 0 0 0; halting\n");
    Decision d = decide(s);
    CHECK(d.trigger == Trigger::Halt && d.reasons.size() == 2 && d.reasons[1].find("0x99AB1A26") != std::string::npos);
    s.engine_exit = -1; s.launcher_exit = 1;
    CHECK(decide(s).trigger == Trigger::Crash);
    s.player_stop = true;  // a missing name, for the player to correct: neither a crash nor an error
    CHECK(decide(s).trigger == Trigger::None || decide(s).trigger == Trigger::SlowFrames);
    s.launch_error = "Enter a player name.";
    CHECK(decide(s).trigger != Trigger::LaunchError);
    s.player_stop = false;
    s.launcher_exit = 0; s.launch_error = "The game files are missing.";
    CHECK(decide(s).trigger == Trigger::LaunchError);
    s.launch_error.clear();
    fs::remove(s.logs / "session-engine.json");
    CHECK(decide(s).trigger == Trigger::None);
    write(s.logs / "report-requested", "{\"requested_at\":1}\n");
    d = decide(s);
    CHECK(d.trigger == Trigger::Manual && d.manual && d.request_id.empty());
    // The id Send report showed the player; anything but a lowercase UUID is ignored.
    write(s.logs / "report-requested", "{\"requested_at\":1,\"report_id\":\"0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d\"}\n");
    CHECK(decide(s).request_id == "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d");
    for (const char *bad : {"0A1B2C3D-4E5F-4A6B-8C7D-9E0F1A2B3C4D", "not-a-uuid", ""}) {
        write(s.logs / "report-requested", std::string("{\"requested_at\":1,\"report_id\":\"") + bad + "\"}\n");
        CHECK(decide(s).request_id.empty());
    }
}

// A summary with a phase and a timeline (src/mag_session_monitor.h).
static std::string with_timeline(const std::string &summary_text, const std::string &phase, const std::string &events) {
    const size_t end = summary_text.rfind('}');
    return summary_text.substr(0, end) + ", \"phase\": \"" + phase + "\", \"timeline\": [" + events + "]}\n";
}
static std::string gap(double ms, const char *verdict, const char *phase = "match", bool open = false) {
    return std::string("{\"at_s\": 300.0, \"kind\": \"network\", \"ms\": ") + std::to_string(ms) + ", \"phase\": \"" + phase +
           "\", \"endpoint\": \"tcp 192.0.2.10:10473\"" + (open ? ", \"open\": true" : "") + ", \"verdict\": \"" + verdict + "\"}";
}

static void network_reports(const fs::path &root) {
    const std::string calm = summary("engine", 36000, 600, 10, 0);
    const std::string dip = "{\"at_s\": 100.0, \"kind\": \"frames\", \"ms\": 900, \"phase\": \"match\", \"frames\": 1, \"worst_ms\": 900, \"verdict\": \"frames\"}";
    struct Case { std::string phase, events; Trigger want; };
    const std::vector<Case> cases = {
        {"match", gap(3000, "network"), Trigger::Network},
        {"menu", dip + ", " + gap(2000, "network"), Trigger::Network},     // a closed gap: the phase it began in counts
        {"match", gap(1999, "network"), Trigger::None},                     // shorter than 2 s
        {"match", gap(5000, "both"), Trigger::None},                        // a client stall, not the network
        {"match", gap(5000, "network", "menu"), Trigger::None},             // not in a match
        {"match", dip, Trigger::None},
        {"match", gap(4000, "network", "match", true), Trigger::Network},   // still silent at the end of a match
        {"menu", gap(4000, "network", "match", true), Trigger::None},       // open, but the match was over
    };
    for (const Case &c : cases) {
        Json j;
        std::vector<std::string> reasons;
        CHECK(parse_json(with_timeline(calm, c.phase, c.events), j));
        const Trigger got = judge_summary(j, reasons);
        if (got != c.want) std::fprintf(stderr, "network case %s / %s: got %s\n", c.phase.c_str(), c.events.c_str(), trigger_name(got));
        CHECK(got == c.want);
        CHECK((got == Trigger::Network) == (!reasons.empty() && reasons[0].find("went silent") != std::string::npos));
    }
    CHECK(std::string(trigger_name(Trigger::Network)) == "network");

    // Slow frames come before network gaps, which come before audio; the reasons name them all.
    Session s = launch(root / "network");
    s.launcher_exit = 90; s.engine_exit = 0;
    write(s.logs / "session-engine.json", with_timeline(summary("engine", 36000, 600, 3000, 2), "menu", gap(3000, "network")));
    Decision d = decide(s);
    CHECK(d.trigger == Trigger::SlowFrames && d.reasons.size() == 2);
    const std::string bad_audio = "{\"available\": true, \"underruns\": 41, \"device_errors\": 0, \"clock_seconds\": 600, \"drift_ms\": 1.0}";
    write(s.logs / "session-engine.json", with_timeline(summary("engine", 36000, 600, 10, 0, bad_audio), "menu", gap(3000, "network")));
    d = decide(s);
    CHECK(d.trigger == Trigger::Network && d.reasons.size() == 2);

    // A network report is queued under its own daily limit, beside the performance one.
    write(s.logs / "session-engine.json", with_timeline(calm, "menu", gap(3000, "network")));
    for (int i = 0; i < daily_network_reports; ++i) {
        const Finished f = finish_session(s);
        CHECK(f.trigger == Trigger::Network && !f.report_id.empty());
        if (i == 0) {
            Json report;
            CHECK(parse_json(read(s.queue / (f.report_id + ".json")), report, 8u << 20));
            CHECK(report.get("trigger") && report.get("trigger")->text == "network");
            const Json *timeline = report.get("metrics") ? report.get("metrics")->get("engine") : nullptr;
            CHECK(timeline && timeline->get("timeline") && timeline->get("timeline")->items.size() == 1);
        }
    }
    Finished f = finish_session(s);
    CHECK(f.trigger == Trigger::None && f.line.find("daily limit for network") != std::string::npos);
    write(s.logs / "session-engine.json", summary("engine", 36000, 600, 3000, 2));
    CHECK(finish_session(s).trigger == Trigger::SlowFrames);  // the performance limit is separate
}

// A summary with more members (src/mag_session_monitor.h): fps_drops, frozen, frames.loading.
static std::string with(const std::string &summary_text, const std::string &members) {
    const size_t end = summary_text.rfind('}');
    return summary_text.substr(0, end) + ", " + members + "}\n";
}
static std::string drop(const char *phase, double at, double seconds, double baseline, double mean) {
    return "{\"at_s\": " + std::to_string(at) + ", \"s\": " + std::to_string(seconds) + ", \"phase\": \"" + phase +
           "\", \"baseline_fps\": " + std::to_string(baseline) + ", \"mean_fps\": " + std::to_string(mean) + ", \"min_fps\": " +
           std::to_string(mean - 5) + ", \"frames\": " + std::to_string((long long)(mean * seconds)) + ", \"lost_frames\": " +
           std::to_string((baseline - mean) * seconds) + ", \"capture\": {\"from_s\": 0, \"to_s\": 0, \"frames\": 0}}";
}
static std::string drops(const std::string &episodes) {
    return "\"fps_drops\": {\"count\": 9, \"ratio\": 0.6, \"floor_fps\": 60, \"min_s\": 3, \"episodes\": [" + episodes + "]}";
}

static void drop_freeze_and_loading_reports(const fs::path &root) {
    const std::string calm = summary("engine", 36000, 600, 10, 0);
    auto judge = [](const std::string &text, std::vector<std::string> &reasons) {
        Json j;
        reasons.clear();
        CHECK(parse_json(text, j));
        return judge_summary(j, reasons);
    };
    std::vector<std::string> reasons;
    // A sustained drop in a match: slow frames, with the reason the report shows.
    CHECK(judge(with(calm, drops(drop("match", 512, 41, 98, 34))), reasons) == Trigger::SlowFrames);
    CHECK(reasons.size() == 1 && reasons[0] == "engine: FPS fell to 34 (35% of 98) for 41 s in a match");
    // The worst of several (most frames lost) is named, with the count.
    CHECK(judge(with(calm, drops(drop("match", 300, 5, 100, 50) + ", " + drop("menu", 400, 60, 120, 20) + ", " +
                                 drop("match", 700, 20, 90, 30))),
                reasons) == Trigger::SlowFrames);
    CHECK(reasons.size() == 1 && reasons[0] == "engine: FPS fell to 30 (33% of 90) for 20 s in a match (2 drops)");
    // Drops outside a match (menus, spawn screens, loading) are recorded but do not report.
    CHECK(judge(with(calm, drops(drop("menu", 400, 60, 120, 20))), reasons) == Trigger::None);
    CHECK(judge(with(calm, drops("")), reasons) == Trigger::None);
    // The existing triggers stay: frames over 50 ms and the drop are both named.
    CHECK(judge(with(summary("engine", 36000, 600, 3000, 0), drops(drop("match", 512, 41, 98, 34))), reasons) == Trigger::SlowFrames);
    CHECK(reasons.size() == 2);

    // Frames of 250 ms or more count outside loading only (the summary's frames.loading).
    auto loading = [](long long over_250, long long loading_250) {
        std::string s = summary("engine", 36000, 600, 100, over_250);
        const std::string key = "\"histogram_ms\"";
        return s.insert(s.find(key), "\"loading\": {\"over_50ms\": " + std::to_string(loading_250) + ", \"over_100ms\": " +
                                         std::to_string(loading_250) + ", \"over_250ms\": " + std::to_string(loading_250) +
                                         ", \"seconds\": 30.0}, ");
    };
    CHECK(judge(loading(40, 15), reasons) == Trigger::None);  // 25 in play
    CHECK(judge(loading(40, 10), reasons) == Trigger::SlowFrames);
    CHECK(reasons.size() == 1 && reasons[0] == "engine: 30 frames took 250 ms or more outside loading");

    // A freeze before the end: the engine's, from frozen_report_seconds.
    CHECK(judge(with(calm, "\"frozen\": {\"at_s\": 900.0, \"s\": 43.2, \"phase\": \"menu\"}"), reasons) == Trigger::SlowFrames);
    CHECK(reasons.size() == 1 && reasons[0] == "engine: no frame for 43 s before the session ended (frozen in the menu)");
    CHECK(judge(with(calm, "\"frozen\": {\"at_s\": 900.0, \"s\": 116.5, \"phase\": \"match\"}"), reasons) == Trigger::SlowFrames);
    CHECK(reasons.size() == 1 && reasons[0] == "engine: no frame for 117 s before the session ended (frozen in a match)");
    CHECK(judge(with(calm, "\"frozen\": {\"at_s\": 900.0, \"s\": 9.9, \"phase\": \"menu\"}"), reasons) == Trigger::None);
    CHECK(judge(with(summary("launcher", 36000, 600, 10, 0), "\"frozen\": {\"at_s\": 900.0, \"s\": 60, \"phase\": \"frontend\"}"), reasons) ==
          Trigger::None);

    // Audio drift that stalls explain (periods dropped after them, up to the stalls' length) is
    // the stalls', not the audio path's.
    auto drifted = [](double drift, long long dropped) {
        return "{\"available\": true, \"underruns\": 0, \"device_errors\": 0, \"dropped_periods\": " + std::to_string(dropped) +
               ", \"clock_seconds\": 6000, \"drift_ms\": " + std::to_string(drift) + "}";
    };
    const std::string stalls = "\"hitches\": [{\"at_s\": 5810, \"ms\": 13299.8, \"phase\": \"match\"}, {\"at_s\": 6110, \"ms\": 12866.6, \"phase\": \"match\"}, "
                               "{\"at_s\": 100, \"ms\": 600, \"phase\": \"match\"}]";
    auto session = [&](const std::string &audio, const std::string &extra) {
        std::string s = summary("engine", 360000, 6000, 10, 0, audio);
        const std::string empty = "\"hitches\": []";
        if (!extra.empty()) s.replace(s.find(empty), empty.size(), extra);
        return s;
    };
    CHECK(judge(session(drifted(-26246, 4900), stalls), reasons) == Trigger::None);  // 4900 x 5.33 ms = 26.1 s, within 26.2 s of stalls
    CHECK(judge(session(drifted(-26246, 4900), ""), reasons) == Trigger::Audio);   // no stall to explain it
    CHECK(reasons.size() == 1 && reasons[0] == "engine: audio clock drifted -26246 ms");
    CHECK(judge(session(drifted(-26246, 100), stalls), reasons) == Trigger::Audio);  // 533 ms explained
    CHECK(reasons.size() == 1 && reasons[0] == "engine: audio clock drifted -25712 ms beyond its stalls (-26246 ms in all)");
    CHECK(judge(session(drifted(2500, 4900), stalls), reasons) == Trigger::Audio);  // ahead: not a stall's
    // A freeze explains drift too.
    CHECK(judge(with(session(drifted(-15045, 2819), ""), "\"frozen\": {\"at_s\": 250.0, \"s\": 205.0, \"phase\": \"menu\"}"), reasons) ==
          Trigger::SlowFrames);
    CHECK(reasons.size() == 1 && reasons[0].find("no frame for 205 s") != std::string::npos);

    // A drop report is queued as slow_frames, its episodes in the metrics.
    Session s = launch(root / "drops");
    s.launcher_exit = 90; s.engine_exit = 0;
    write(s.logs / "session-engine.json", with(calm, drops(drop("match", 512, 41, 98, 34))));
    const Finished f = finish_session(s);
    CHECK(f.trigger == Trigger::SlowFrames && !f.report_id.empty());
    if (!f.report_id.empty()) {
        Json report;
        CHECK(parse_json(read(s.queue / (f.report_id + ".json")), report, 8u << 20));
        CHECK(report.get("trigger") && report.get("trigger")->text == "slow_frames");
        const Json *engine = report.get("metrics") ? report.get("metrics")->get("engine") : nullptr;
        CHECK(engine && engine->get("fps_drops") && engine->get("fps_drops")->get("episodes")->items.size() == 1);
        const Json *session_part = report.get("session");
        CHECK(session_part && session_part->get("reasons") && session_part->get("reasons")->items.size() == 1 &&
              session_part->get("reasons")->items[0].text == "engine: FPS fell to 34 (35% of 98) for 41 s in a match");
    }
}

static void finishing(const fs::path &root) {
    // Reports on (no field): a slow session is queued; the body follows the portal's contract.
    Session s = launch(root / "on");
    s.launcher_exit = 90; s.engine_exit = 0;
    Finished f = finish_session(s);
    if (f.report_id.empty()) std::fprintf(stderr, "finish: %s\n", f.line.c_str());
    CHECK(f.trigger == Trigger::SlowFrames && !f.report_id.empty());
    CHECK(queued(s.queue).size() == 1);
    const std::string body = read(s.queue / (f.report_id + ".json"));
    Json report;
    CHECK(parse_json(body, report, 8u << 20));
    CHECK(report.members.size() == 7);
    CHECK(report.get("trigger") && report.get("trigger")->text == "slow_frames");
    CHECK(report.get("client")->get("gpu")->text == "Synthetic GPU 9000");
    CHECK(report.get("client")->get("cpu_threads")->kind == Json::Kind::Number);  // counts are numbers
    CHECK(report.get("client")->get("os")->kind == Json::Kind::String);
    CHECK(report.get("client")->get("build")->get("launcher")->get("build_path") == nullptr);
    // The Debug page's controls trace: off, then on once its file sits next to the settings.
    CHECK(report.get("client")->get("controls_trace") && report.get("client")->get("controls_trace")->kind == Json::Kind::Bool &&
          !report.get("client")->get("controls_trace")->boolean);
    std::ofstream(s.settings.parent_path() / MAG_CONTROLS_TRACE_FILE).close();
    Json traced;
    CHECK(parse_json(build_report(s, decide(s), new_report_id()), traced, 8u << 20));
    CHECK(traced.get("client")->get("controls_trace")->boolean);
    fs::remove(s.settings.parent_path() / MAG_CONTROLS_TRACE_FILE);
    CHECK(body.find(token) == std::string::npos && body.find("synthetic-player") == std::string::npos);
    CHECK(body.find("portal-link") == std::string::npos);
    for (char c : f.report_id) CHECK(!(c >= 'A' && c <= 'F'));
    // The same session again: the performance limit allows two a day.
    CHECK(finish_session(s).trigger == Trigger::SlowFrames);
    f = finish_session(s);
    CHECK(f.trigger == Trigger::None && f.line.find("daily limit") != std::string::npos);

    // Reports off: nothing automatic, but the player's own request is sent.
    Session off = launch(root / "off", "{\"schema_version\":1,\"server_address\":\"play.magtest.online\",\"send_reports\":false}");
    off.launcher_exit = 90; off.engine_exit = 139;
    f = finish_session(off);
    CHECK(f.trigger == Trigger::None && f.line.find("off") != std::string::npos && queued(off.queue).empty());
    write(off.logs / "report-requested", "{\"requested_at\":1,\"report_id\":\"0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d\"}\n");
    f = finish_session(off);
    CHECK(f.trigger == Trigger::Manual && queued(off.queue).size() == 1);
    CHECK(f.report_id == "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d");  // the id the player was shown
    CHECK(f.line.find("0a1b2c3d") != std::string::npos);

    // Not linked: no report at all.
    Session unlinked = launch(root / "unlinked");
    fs::remove(unlinked.settings.parent_path() / "portal-link.json");
    unlinked.engine_exit = 139;
    f = finish_session(unlinked);
    CHECK(f.trigger == Trigger::None && f.line.find("not linked") != std::string::npos);

    // Payload caps: incompressible logs are cut to fit.
    Session big = launch(root / "big");
    std::string noise;
    uint64_t x = 88172645463325252ull;
    for (size_t i = 0; i < (6u << 20); ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; noise += char('!' + x % 90); if (i % 100 == 99) noise += '\n'; }
    write(big.logs / "engine.log", noise);
    write(big.logs / "launcher.log", noise);
    Decision crash;
    crash.trigger = Trigger::Crash;
    crash.reasons = {"the engine exited with code 139"};
    const std::string large = build_report(big, crash, new_report_id());
    Json parsed;
    CHECK(parse_json(large, parsed, 8u << 20));
    size_t compressed = 0;
    for (const auto &[name, log] : parsed.get("logs")->members) compressed += log.get("data")->text.size() / 4 * 3;
    CHECK(compressed <= logs_max_compressed + 8);
    CHECK(compressed > logs_max_compressed / 4);
}

static void sending(const fs::path &root) {
    Session s = launch(root / "send");
    const auto id = [] { return new_report_id(); };
    std::string error;
    for (int i = 0; i < 7; ++i) CHECK(enqueue(s.queue, id(), Trigger::Crash, "{\"n\":" + std::to_string(i) + "}", error));
    CHECK(queued(s.queue).size() == queue_max_reports);  // the oldest went first
    FakeTransport t;
    t.add(201); t.add(200); t.add(500);
    Sent sent = send_queued(t, s.queue, s.settings, 1000000);
    CHECK(sent.sent == 2 && sent.kept == 3 && sent.dropped == 0);
    CHECK(t.requests.size() == 3);
    CHECK(t.requests[0].first == "https://portal.invalid/api/v1/reports");
    CHECK(t.requests[0].second.size() == 1 && t.requests[0].second[0].second == "Bearer " + token);
    CHECK(sent.line.find(token) == std::string::npos);
    // The failed report waits retry_first_seconds; the two after it were not tried.
    auto q = queued(s.queue);
    CHECK(q.size() == 3 && q[0].attempts == 1 && q[0].next_attempt == 1000000 + retry_first_seconds);
    CHECK(q[1].attempts == 0 && q[2].attempts == 0);
    // Not due: kept. 429 with Retry-After: kept until then, and the rest wait for the next sender.
    t.requests.clear();
    t.add(429, 120);
    sent = send_queued(t, s.queue, s.settings, 1000010);
    q = queued(s.queue);
    CHECK(t.requests.size() == 1 && sent.kept == 3 && q.size() == 3);
    CHECK(q[0].next_attempt == 1000000 + retry_first_seconds && q[1].next_attempt == 1000010 + 120 && q[1].attempts == 0);
    // 413 drops that report; 401 drops it and stops.
    t.requests.clear();
    t.add(413); t.add(401);
    sent = send_queued(t, s.queue, s.settings, 2000000);
    CHECK(sent.dropped == 2 && sent.kept == 1 && t.requests.size() == 2);
    // Network failures back off and are dropped after send_max_attempts.
    long long now = 3000000;
    for (int i = 0; i < send_max_attempts; ++i) { t.add(0); send_queued(t, s.queue, s.settings, now); now += retry_max_seconds; }
    CHECK(queued(s.queue).empty());
    // One sender at a time.
    CHECK(enqueue(s.queue, id(), Trigger::Manual, "{}", error));
    write(s.queue / "send.lock", std::to_string(now) + "\n");
    sent = send_queued(t, s.queue, s.settings, now + 5);
    CHECK(sent.line.find("another sender") != std::string::npos && queued(s.queue).size() == 1);
    sent = send_queued(t, s.queue, s.settings, now + lock_stale_seconds + 1);  // a stale lock is taken over
    CHECK(sent.line.find("another sender") == std::string::npos);
    // 500 (server_error) three times for the same report: the report is the problem, so it is
    // dropped; 503 (and 502, or no connection) keeps it for the backoff and the attempt limit.
    fs::remove(s.queue / "send.lock");
    for (auto &e : queued(s.queue)) { fs::remove(s.queue / (e.report_id + ".json")); fs::remove(s.queue / (e.report_id + ".state")); }
    CHECK(enqueue(s.queue, id(), Trigger::Crash, "{}", error));
    long long at = now + 1000;
    for (int i = 0; i < server_error_max - 1; ++i) {
        t.add(500);
        sent = send_queued(t, s.queue, s.settings, at);
        CHECK(sent.kept == 1 && queued(s.queue).size() == 1 && queued(s.queue)[0].server_errors == i + 1);
        at += retry_max_seconds;
    }
    t.add(500);
    sent = send_queued(t, s.queue, s.settings, at);
    CHECK(sent.dropped == 1 && queued(s.queue).empty());
    CHECK(enqueue(s.queue, id(), Trigger::Crash, "{}", error));
    for (int i = 0; i < server_error_max + 1; ++i) { t.add(503); send_queued(t, s.queue, s.settings, at); at += retry_max_seconds; }
    CHECK(queued(s.queue).size() == 1 && queued(s.queue)[0].server_errors == 0 && queued(s.queue)[0].attempts == server_error_max + 1);
    // A state written before the 500 count existed reads as none.
    const std::string older = queued(s.queue)[0].report_id;
    write(s.queue / (older + ".state"), "crash 1 0 1\n");
    CHECK(queued(s.queue).size() == 1 && queued(s.queue)[0].server_errors == 0 && queued(s.queue)[0].attempts == 1);
    // A report queued more than queue_max_age_seconds ago is dropped unsent.
    fs::remove(s.queue / "send.lock");
    for (auto &e : queued(s.queue)) { fs::remove(s.queue / (e.report_id + ".json")); fs::remove(s.queue / (e.report_id + ".state")); }
    CHECK(enqueue(s.queue, id(), Trigger::Crash, "{}", error));
    t.requests.clear();
    sent = send_queued(t, s.queue, s.settings, std::time(nullptr) + queue_max_age_seconds + 60);
    CHECK(sent.dropped == 1 && t.requests.empty() && queued(s.queue).empty());
    // Reports turned off since: automatic ones are dropped unsent, the player's own request is not.
    fs::remove(s.queue / "send.lock");
    for (auto &e : queued(s.queue)) { fs::remove(s.queue / (e.report_id + ".json")); fs::remove(s.queue / (e.report_id + ".state")); }
    CHECK(enqueue(s.queue, id(), Trigger::Crash, "{}", error) && enqueue(s.queue, id(), Trigger::Manual, "{}", error));
    write(s.settings, "{\"schema_version\":1,\"server_address\":\"play.magtest.online\",\"send_reports\":false}");
    t.requests.clear();
    t.add(201);
    sent = send_queued(t, s.queue, s.settings, now + 10);
    CHECK(sent.dropped == 1 && sent.sent == 1 && t.requests.size() == 1 && queued(s.queue).empty());
}

// Send report's comment: the marker round trip, clipping, redaction and the payload field.
static void comments(const fs::path &root) {
    // Clipping: well-formed UTF-8 only, no control characters but newlines and tabs, CR LF to LF,
    // at most the limit in code points, cut between code points.
    CHECK(mag::clip_report_comment("a\r\nb\rc\x01\x7f\td\xff\xc3") == "a\nb\nc\td");
    CHECK(mag::clip_report_comment("\xc2\x85x") == "x");  // a C1 control
    const std::string e_acute = "\xc3\xa9", emoji = "\xf0\x9f\x98\x80";
    std::string many;
    for (int i = 0; i < 1200; ++i) many += emoji;
    const std::string clipped = mag::clip_report_comment(many);
    CHECK(mag::report_comment_length(clipped) == MAG_SETTINGS_REPORT_COMMENT_LIMIT && clipped.size() == 4u * MAG_SETTINGS_REPORT_COMMENT_LIMIT);
    CHECK(mag::report_comment_length(e_acute + "x" + emoji) == 3);
    CHECK(mag::report_comment_counter(e_acute + "x") == "2/1000");
    CHECK(utf8_prefix(e_acute + emoji, 4) == e_acute);  // never half a code point
    CHECK(utf8_prefix("abc", 9) == "abc");

    // The marker round trip, with quotes, backslashes and newlines.
    const fs::path marker = root / "comment-marker";
    const std::string text = "It froze \"twice\" in C:\\games\nthen " + e_acute;
    std::string error;
    CHECK(mag::write_report_request(marker, 1800000000, "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d", text, error));
    CHECK(requested_report_id(marker) == "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d");
    CHECK(requested_comment(marker) == text);
    CHECK(mag::write_report_request(marker, 1800000000, "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d", "", error));
    CHECK(requested_comment(marker).empty());  // an empty comment is allowed
    write(marker, "{\"requested_at\":1,\"report_id\":\"0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d\"}\n");  // before comments
    CHECK(requested_comment(marker).empty() && !requested_report_id(marker).empty());

    // In the report: redacted like the logs, only when the player asked.
    Session s = launch(root / "comment");
    s.launcher_exit = 90; s.engine_exit = 0;
    fs::remove(s.logs / "session-engine.json");
    const std::string said = "Stutter at " + home + "/MAG after linking with " + token + " - thanks";
    CHECK(mag::write_report_request(s.logs / "report-requested", 1800000000, "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d", said, error));
    Decision d = decide(s);
    CHECK(d.trigger == Trigger::Manual && d.user_comment == said);
    Json report;
    CHECK(parse_json(build_report(s, d, d.request_id), report, 8u << 20));
    const Json *comment = report.get("session") ? report.get("session")->get("user_comment") : nullptr;
    CHECK(comment && comment->text == "Stutter at ~/MAG after linking with mgd_[redacted] - thanks");
    CHECK(report.get("user_comment") == nullptr);  // never a top-level field
    // An automatic report never carries one, even when a comment was typed earlier and not sent.
    Decision automatic;
    automatic.trigger = Trigger::Crash;
    automatic.user_comment = said;
    CHECK(parse_json(build_report(s, automatic, new_report_id()), report, 8u << 20));
    CHECK(report.get("session")->get("user_comment") == nullptr);
    // A crash in the session the player reported keeps the player's words with the crash.
    s.engine_exit = 139;
    d = decide(s);
    CHECK(d.trigger == Trigger::Crash && d.manual && d.user_comment == said);
    // The longest comment (1000 four-byte scalar values) fits the session's 12 KiB whole.
    CHECK(mag::write_report_request(s.logs / "report-requested", 1800000000, "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d", many, error));
    s.engine_exit = 0;
    d = decide(s);
    std::string body = build_report(s, d, d.request_id);
    CHECK(parse_json(body, report, 8u << 20));
    const Json *session = report.get("session");
    CHECK(session && to_json(*session).size() <= session_max_bytes);
    comment = session ? session->get("user_comment") : nullptr;
    CHECK(comment && mag::report_comment_length(comment->text) == 1000 && comment->text.size() == 4000);
    CHECK(session && !session->get("user_comment_truncated"));
    std::ofstream(root / "long-comment.json", std::ios::binary) << body;
    // Redaction can lengthen a comment (a 12-character token becomes "mgd_[redacted]"): the report
    // still carries at most 1000 scalar values, which is the portal's limit.
    std::string tokens;
    while (mag::report_comment_length(tokens) + 13 <= 1000) tokens += "mgd_abcdefgh ";
    CHECK(mag::write_report_request(s.logs / "report-requested", 1800000000, "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d", tokens, error));
    d = decide(s);
    CHECK(parse_json(build_report(s, d, d.request_id), report, 8u << 20));
    comment = report.get("session")->get("user_comment");
    CHECK(comment && mag::report_comment_length(comment->text) <= 1000 && comment->text.find("abcdefgh") == std::string::npos);

    // NUL never reaches the portal (its database refuses it, and the 500 would repeat): not from
    // the system description, a log (the GPU line, a halt line), the launch error, a summary or the player.
    CHECK(quote(std::string("a\0b", 3)) == "\"ab\"");
    Session nul = launch(root / "nul");
    nul.system["cpu"] = std::string("Synthetic\0CPU", 13);
    nul.launch_error = std::string("stopped\0here", 12);
    write(nul.logs / "engine.log", std::string("[RSX vulkan] using device 0: GPU\0X\n", 36));
    write(nul.logs / "session-engine.json", "{\"schema_version\": 1, \"role\": \"eng\\u0000ine\", \"frames\": {\"count\": 1}}\n");
    Decision crash_with_comment;
    crash_with_comment.trigger = Trigger::Crash;
    crash_with_comment.manual = true;
    crash_with_comment.user_comment = std::string("it\0froze", 9);
    crash_with_comment.reasons = {std::string("[hle] UNRESOLVED NID\0 halting", 29)};
    body = build_report(nul, crash_with_comment, new_report_id());
    CHECK(body.find('\0') == std::string::npos && body.find("\\u0000") == std::string::npos);
    CHECK(parse_json(body, report, 8u << 20));
    CHECK(report.get("client")->get("cpu")->text == "SyntheticCPU" && report.get("client")->get("gpu")->text == "GPUX");
    CHECK(report.get("metrics")->get("engine")->get("role")->text == "engine");
    CHECK(report.get("session")->get("user_comment")->text == "itfroze");
}

int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "selftest") {
        const fs::path root = argv[2];
        json_and_quote();
        redaction();
        decisions(root);
        finishing(root);
        network_reports(root);
        drop_freeze_and_loading_reports(root);
        sending(root);
        comments(root);
        CHECK(gpu_from_log("a\n[RSX vulkan] using device 1: Some GPU (x)\r\nb") == "Some GPU (x)");
        CHECK(!system_info().empty());
        if (failures) { std::fprintf(stderr, "%d failures\n", failures); return 1; }
        std::printf("[player_reports] PASS\n");
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "build") {
        Session s = launch(argv[2]);
        s.launcher_exit = 90; s.engine_exit = 139;
        // The player also pressed Send report, with a comment that names a home folder and the token.
        std::string error;
        mag::write_report_request(s.logs / "report-requested", 1800000000, "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d",
                                  "Froze in " + home + "/MAG\nafter " + token, error);
        const Finished f = finish_session(s);
        if (f.report_id.empty()) { std::fprintf(stderr, "%s\n", f.line.c_str()); return 1; }
        std::printf("%s\n", (s.queue / (f.report_id + ".json")).string().c_str());
        return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "send") {
        auto transport = mag::portal::make_platform_transport();
        if (!transport) return 3;
        const Sent sent = send_queued(*transport, argv[2], argv[3]);
        std::printf("%s\n", sent.line.c_str());
        return 0;
    }
    std::fprintf(stderr, "usage: player_reports selftest|build <scratch> | send <queue> <settings>\n");
    return 2;
}
