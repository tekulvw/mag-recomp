#pragma once
// Performance and error reports (docs/player-settings.md, "Reports"), shared by the packaged
// macOS, Linux and Windows supervisors. Standard C++17 only; the HTTPS transport is the portal's
// (player_portal.h), so the synthetic tests drive every flow with a fake one.
//
// After a session the supervisor calls finish_session(): it reads the engine's session summaries
// (src/mag_session_monitor.h), the exit codes and the logs of that launch, decides whether a report
// is due, and if so writes it, complete and redacted, to a small queue on disk. A detached
// `MAG --send-reports` process then calls send_queued(), which posts each queued report to the
// linked account's portal (POST <portal>/api/v1/reports with the device token) and keeps what it
// could not send for the next launch. Nothing here shows a window or blocks play.
//
// Never in a report: the device token, ticket bytes, portal-link.json, the player's home folder
// (each becomes ~), or any game file content.
#include "player_portal.h"
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mag::reports {
namespace fs = std::filesystem;

// ---- Thresholds (named here, documented in docs/player-settings.md) ----
// A session must have run this long, with this many frames, before its frame pacing is judged.
inline constexpr double judge_min_seconds = 120;
inline constexpr uint64_t judge_min_frames = 3600;
// Slow frames: more than this share of frames took 50 ms or more...
inline constexpr double slow_frame_share = 0.05;
// ...or at least this many frames took 250 ms or more (outside the first minute's loading).
inline constexpr uint64_t long_frame_count = 30;
// Audio: at least this many underruns, plus one for every this many seconds of audio...
inline constexpr uint64_t underrun_floor = 20;
inline constexpr double underrun_every_seconds = 30;
// ...or any host audio device error, or this much drift against the 48 kHz clock over a session
// of at least judge_min_seconds.
inline constexpr double audio_drift_limit_ms = 2000;
// Reports created per rolling 24 hours, by kind: crashes, halts and launch errors; slow frames and
// audio; and the player's own requests. More are not created.
inline constexpr int daily_error_reports = 6, daily_performance_reports = 2, daily_manual_reports = 10;
// The queue: at most this many reports and bytes; the oldest go first.
inline constexpr size_t queue_max_reports = 5;
inline constexpr uintmax_t queue_max_bytes = 8u << 20;
// A queued report older than this is dropped unsent (the portal answers 503 while reports are off).
inline constexpr long long queue_max_age_seconds = 7 * 86400;
// Logs: the compressed total of one report's logs (the portal accepts 3 MiB), the uncompressed
// tail read from each log, and the most logs one report carries (the portal accepts 8).
inline constexpr size_t logs_max_compressed = 2u << 20;
inline constexpr size_t log_tail_bytes = 4u << 20;
inline constexpr size_t logs_max_count = 8;
// The portal's limits on the other parts of the body.
inline constexpr size_t client_max_bytes = 16u << 10, session_max_bytes = 12u << 10, metrics_max_bytes = 64u << 10;
// Sending: attempts per report before it is dropped, the first retry delay (doubled each time),
// the longest delay, and the delay after a 429 without Retry-After.
inline constexpr int send_max_attempts = 8;
// A report the portal answers 500 (server_error) for this many times is dropped: it is the report,
// not the portal, so retrying it would never end.
inline constexpr int server_error_max = 3;
inline constexpr long long retry_first_seconds = 60, retry_max_seconds = 6 * 3600, rate_limited_seconds = 3600;
// A sender that holds the queue lock longer than this is presumed gone.
inline constexpr long long lock_stale_seconds = 600;

// ---- JSON (a small reader for the session summaries, which are re-serialised into the report) ----
struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    std::string text;  // a string's value, or a number's literal
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;  // in file order
    const Json *get(const std::string &key) const;
    double number(double fallback = 0) const;
};
// At most `limit` bytes and 32 levels deep. False for anything malformed.
bool parse_json(const std::string &text, Json &out, size_t limit = 1u << 20);
std::string to_json(const Json &value);
// A JSON string. Invalid UTF-8 becomes U+FFFD, and NUL is dropped: the portal's database refuses
// it, so no string in a report (from the system, a log, a summary or the player) may carry one.
std::string quote(const std::string &value);

// ---- Redaction ----
struct Redaction {
    std::vector<std::string> secrets;  // exact strings to remove (the device token, a ticket's text)
    std::vector<std::pair<std::string, std::string>> folders;  // a folder and its label ("<MAG folder>")
    std::vector<std::string> homes;    // the player's home folders, each replaced by ~
};
// Removes the secrets, anything shaped like a device token (mgd_...) or a bearer credential, the
// labelled folders, the home folders (in either slash style and, for Windows paths, any case) and other users' profile
// folders (/Users/<name>, /home/<name>, <drive>:\Users\<name>), which become ~.
std::string redact(const std::string &text, const Redaction &);

// ---- What happened in the session ----
enum class Trigger { None, Crash, LaunchError, Halt, SlowFrames, Audio, Manual };
const char *trigger_name(Trigger);

struct Session {
    std::string platform;  // "macos", "linux" or "windows"
    std::string version;   // the product version (1 to 64 characters)
    fs::path logs;         // this launch's log folder
    fs::path settings;     // settings.json (send_reports; server, portal and display settings)
    fs::path queue;        // the report queue folder (home/reports in the MAG folder)
    fs::path receipts;     // the package receipts folder (launcher.json, engine.json)
    long long launcher_exit = -1, engine_exit = -1;  // -1: that phase did not run
    std::string launch_error;  // the supervisor stopped the launch with this message
    bool player_stop = false;  // the launch stopped for the player to correct (a missing name): no error
    std::string backend;       // the renderer ("Metal", "Vulkan")
    std::map<std::string, std::string> system;  // os, os_version, cpu, cpu_threads, memory_mb, gpu
    Redaction redaction;
    std::time_t now = 0;  // 0: the current time
};

// Exit codes: 90 is the launcher's handoff to the engine; 87 and 88 are the runtime's halts on an
// unresolved syscall or import; on POSIX, 128 + a signal; on Windows, an exception status.
bool is_halt_exit(long long code);
bool is_crash_exit(long long code, const std::string &platform);

struct Decision {
    Trigger trigger = Trigger::None;
    std::vector<std::string> reasons;  // why, for the report and the log
    bool manual = false;               // the player pressed Send report
    std::string request_id;            // the report id that Send report showed the player, when valid
    std::string user_comment;          // what the player typed with Send report (only with `manual`)
};
// The trigger for this session from its exits, logs, summaries and request marker, before the
// setting and the daily limits. Crash and halt come before a launch error, which comes before
// slow frames and audio; a manual request alone is Manual.
Decision decide(const Session &);
// The performance part alone, for one session summary (tests and decide()).
Trigger judge_summary(const Json &summary, std::vector<std::string> &reasons);

struct Finished {
    Trigger trigger = Trigger::None;
    std::string report_id;  // when queued
    std::string line;       // one line for supervisor.log (empty when there is nothing to say)
};
// Decide, honour the send_reports setting (automatic reports only when it is on; a manual request
// always), the account link (no linked account: no report) and the daily limits, then build the
// report and add it to the queue.
Finished finish_session(const Session &);

// The report body (the portal's schema_version 1) for a session and trigger. Exposed for tests.
std::string build_report(const Session &, const Decision &, const std::string &report_id);
// The report id a Send report marker holds ({"requested_at":...,"report_id":"<uuid>"}), when it
// holds a valid lowercase UUID; otherwise empty.
std::string requested_report_id(const fs::path &marker);
// The comment a Send report marker holds, clipped (player_report_request.h); empty without one.
std::string requested_comment(const fs::path &marker);
// The longest prefix of well-formed UTF-8 `text` that fits in `bytes`, cut between code points.
std::string utf8_prefix(const std::string &text, size_t bytes);
// Read the tail of a log (at most `limit` bytes, starting at a line), redacted.
std::string log_tail(const fs::path &path, size_t limit, const Redaction &);
std::string new_report_id();  // a random UUID version 4

// ---- The queue ----
struct Queued { std::string report_id, trigger; int attempts = 0; long long next_attempt = 0; uintmax_t bytes = 0; int server_errors = 0; };
std::vector<Queued> queued(const fs::path &queue);  // oldest first
bool enqueue(const fs::path &queue, const std::string &report_id, Trigger, const std::string &body, std::string &error);
bool queue_has_work(const fs::path &queue);

struct Sent {
    int sent = 0, dropped = 0, kept = 0;
    std::string line;  // one line for supervisor.log
};
// Post what is due, oldest first, with the device token of `link_file` to the portal it came
// from. Kept: 5xx, 429 (after Retry-After) and network failures, with backoff; dropped: 400,
// 401, 403, 409, 413, too many attempts, or older than queue_max_age_seconds. Automatic reports queued before the player turned
// reports off are dropped unsent. Only one sender runs at a time (a lock file in the queue).
Sent send_queued(portal::Transport &, const fs::path &queue, const fs::path &settings, std::time_t now = 0);

// ---- Supervisor plumbing ----
// MAG_REPORTS=0 in the supervisor's environment turns everything below off (test automation).
bool reports_disabled();
// At launch, once the log folder exists: a detached sender for reports earlier sessions left.
void start_session(const Session &, const fs::path &self);
// At the end of a supervised session (normal or not): finish_session(), its line in supervisor.log,
// and a detached sender when anything waits. Never throws.
void end_session(const Session &, const fs::path &self);
// What `MAG --send-reports <logs>` runs: send_queued() with the platform transport, and its line.
int run_sender(const fs::path &queue, const fs::path &settings, const fs::path &logs);
// Append one line (with a UTC time stamp) to the launch's supervisor.log.
void log_line(const fs::path &logs, const std::string &line);
// Start `self --send-reports <logs>` detached from this process (and from its job on Windows), so
// sending never delays the game or the supervisor's exit. False when it could not start.
bool spawn_sender(const fs::path &self, const fs::path &logs);
// The computer's description for client.system (os, os_version, cpu, cpu_threads, memory_mb).
std::map<std::string, std::string> system_info();
// The GPU the renderer reported in a log ("[RSX vulkan] using device N: <name>"), when it did.
std::string gpu_from_log(const std::string &log_text);
}  // namespace mag::reports
