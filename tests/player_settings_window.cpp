// Synthetic driver for the Windows/Linux settings form and the SDL2 settings panel: synthetic
// SDL events into the real panel, drawn into a software renderer; no window or display.
#include "player_controls_settings.h"
#include "player_portal.h"
#include "player_settings_debug.h"
#include "player_settings_form.h"
#include "player_settings_sdl_panel.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace mag;
namespace fs = std::filesystem;

// The launch hook's window entry points are not part of this driver.
extern "C" int mag_player_settings_setup_wanted(int) { return 0; }
extern "C" int mag_player_settings_setup(void) { return 0; }

// The account portal: a link approved at the first poll, as Rook_9 (no network).
namespace mag::portal {
class FakeTransport final : public Transport {
public:
    Response post(const std::string& url, const std::string&, const Headers&) override {
        if (url.find("/device/start") != std::string::npos)
            return {200, "{\"device_code\":\"mgl_code\",\"user_code\":\"BCDF-GHJK\",\"verification_uri\":\"https://magtest.online/link\","
                         "\"verification_uri_complete\":\"https://magtest.online/link?code=BCDF-GHJK\",\"expires_in\":600,\"interval\":1}", ""};
        if (url.find("/device/token") != std::string::npos)
            return {200, "{\"device_token\":\"mgd_synthetic_token\",\"player_name\":\"Rook_9\"}", ""};
        return {404, "{}", ""};
    }
};
std::unique_ptr<Transport> make_platform_transport() { return std::make_unique<FakeTransport>(); }
}  // namespace mag::portal

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static uint64_t now = 1000;
static SDL_Event key(SDL_Scancode code, SDL_Keymod mod = KMOD_NONE) {
    SDL_Event e{};
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = code;
    e.key.keysym.sym = SDLK_UNKNOWN;  // the panel reads physical keys only
    e.key.keysym.mod = Uint16(mod);
    return e;
}
static SDL_Event typed(const char* text) {
    SDL_Event e{};
    e.type = SDL_TEXTINPUT;
    std::snprintf(e.text.text, sizeof e.text.text, "%s", text);
    return e;
}
static SDL_Event mouse(Uint8 button, int x = 5, int y = 5) {
    SDL_Event e{};
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.button = button;
    e.button.x = x;
    e.button.y = y;
    return e;
}
static SDL_Event motion(int x, int y, Uint32 state = 0) {
    SDL_Event e{};
    e.type = SDL_MOUSEMOTION;
    e.motion.x = x;
    e.motion.y = y;
    e.motion.state = state;
    return e;
}
static SDL_Event release(int x, int y) {
    SDL_Event e = mouse(SDL_BUTTON_LEFT, x, y);
    e.type = SDL_MOUSEBUTTONUP;
    return e;
}
static SDL_Event wheel(int y) {
    SDL_Event e{};
    e.type = SDL_MOUSEWHEEL;
    e.wheel.y = y;
    return e;
}
static SDL_Event pad(Uint32 type, Uint8 button) {
    SDL_Event e{};
    e.type = type;
    e.cbutton.button = button;
    return e;
}
static SDL_Event trigger(Uint8 axis, double value) {
    SDL_Event e{};
    e.type = SDL_CONTROLLERAXISMOTION;
    e.caxis.axis = axis;
    e.caxis.value = Sint16(value * 32767);
    return e;
}
static SettingsPanel::State send(SettingsPanel& panel, const SDL_Event& e) { return panel.handle(e, now += 10); }
static std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
// `background`: points that must show the window's background (where clipping keeps the page out).
static void draw(SettingsPanel& panel, const std::vector<SDL_Point>& background = {}) {
    const int height = panel.height();
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, SettingsPanel::width, height, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    SDL_Texture* font = SettingsPanel::make_font(renderer);
    CHECK(font != nullptr);
    panel.draw(renderer, font);
    SDL_RenderPresent(renderer);
    // The macOS light window background, and a group box's white fill beside its first row.
    auto pixel = [&](int x, int y) { return static_cast<Uint32*>(surface->pixels)[y * (surface->pitch / 4) + x]; };
    Uint8 r, g, b, a;
    SDL_GetRGBA(pixel(2, 2), surface->format, &r, &g, &b, &a);
    CHECK(r == 236 && g == 236 && b == 236);
    for (const SDL_Point& p : background) {
        CHECK(p.x >= 0 && p.y >= 0 && p.x < SettingsPanel::width && p.y < height);
        if (p.x < 0 || p.y < 0 || p.x >= SettingsPanel::width || p.y >= height) continue;
        SDL_GetRGBA(pixel(p.x, p.y), surface->format, &r, &g, &b, &a);
        if (r != 236 || g != 236 || b != 236) std::printf("not background at %d,%d: %d %d %d\n", p.x, p.y, r, g, b);
        CHECK(r == 236 && g == 236 && b == 236);
    }
    SDL_DestroyTexture(font);
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
}
// Where a display change would show: records what the panel asked for.
struct FakeDisplay : SettingsDisplay {
    std::vector<std::string> calls;
    bool fit = true;
    bool fits(const PortableSettings&) override { return fit; }
    void preview(const PortableSettings& s) override {
        calls.push_back(s.display_mode == "borderless" ? "borderless" : "windowed " + std::to_string(s.output_width));
    }
    void restore() override { calls.push_back("restore"); }
    void kept() override { calls.push_back("kept"); }
};
static void type_into(SettingsPanel& panel, const std::string& name, const char* value) {
    CHECK(panel.focus(name));
    for (int i = 0; i < 40; ++i) send(panel, key(SDL_SCANCODE_BACKSPACE));
    send(panel, typed(value));
}
static void click(SettingsPanel& panel, const std::string& name, Uint8 button = SDL_BUTTON_LEFT) {
    const SDL_Rect r = panel.rect(name);
    CHECK(r.w > 0);
    send(panel, mouse(button, r.x + r.w / 2, r.y + r.h / 2));
}

// A small synthetic catalog: two on-foot actions and one system button.
static void register_controls(const fs::path& folder) {
    ControlsSettingsRegistration registration;
    registration.path = folder / "controls.json";
    registration.actions = {{1, "Move forward", 1, 3}, {2, "Fire", 1, 7}, {3, "Start", 17, 7, "Native menu / command buttons"}};
    // More groups than the list shows at once, as the game's catalog has; unbound by default.
    for (ActionId id = 10; id < 26; ++id)
        registration.actions.push_back({id, "Group action " + std::to_string(id), 1, 7, id < 18 ? "Commands" : "Vehicles"});
    registration.defaults.bindings = {{{InputSource::Keyboard, 26}, 1}, {{InputSource::Mouse, 0}, 2},
                                      {{InputSource::Controller, 10}, 2}, {{InputSource::Keyboard, 41}, 3, 17},
                                      {{InputSource::Controller, 6}, 3, 17}};
    mag_controls_settings_register(registration);
}

static void general(const fs::path& folder) {
    const fs::path settings = folder / "settings.json";
    SettingsForm form;
    std::string error;
    CHECK(settings_form_load(settings, form, error));
    CHECK(form.server == "play.magtest.online" && form.tcp_port.empty() && !form.controls);
    FakeDisplay display;
    SettingsWindowOptions setup;
    setup.setup = true;
    SettingsPanel panel(form, settings, setup, &display);
    // The macOS panel's General page: General and Debug, no Controls segment without a catalog; no file yet, so Apply is on.
    CHECK(panel.shown("segment:general") && !panel.shown("segment:controls") && panel.shown("segment:debug") &&
          panel.focused() == "text:server");
    CHECK(panel.label("button:play") == "Play MAG" && panel.enabled("button:play"));
    CHECK(panel.enabled("button:apply") && panel.status() == "Unsaved changes");
    CHECK(panel.account_status() == "Sign in with your account at https://magtest.online");
#if MAG_PLAYER_RENDER_SCALE_SUPPORTED
    // The Vulkan renderer's rendering scale (Linux builds); FXAA stays the Metal renderer's.
    CHECK(panel.enabled("choice:rendering") && !panel.enabled("choice:aa"));
#else
    CHECK(!panel.enabled("choice:rendering") && !panel.enabled("choice:aa"));
#endif
    // Voice: Off at 100% by default; the rows are disabled where the build has no voice service.
    CHECK(panel.label("choice:voice") == "Off" && panel.label("choice:voice_volume") == "100%");
#if MAG_PLAYER_VOICE_SUPPORTED
    CHECK(panel.enabled("choice:voice") && panel.enabled("choice:voice_volume"));
    CHECK(panel.focus("choice:voice"));
    send(panel, key(SDL_SCANCODE_RIGHT));
    CHECK(form.voice_chat == 1 && panel.label("choice:voice") == "Push to talk");
    send(panel, key(SDL_SCANCODE_LEFT));
    click(panel, "choice:voice_volume");
    CHECK(panel.choosing());
    send(panel, key(SDL_SCANCODE_DOWN));
    send(panel, key(SDL_SCANCODE_RETURN));
    CHECK(!panel.choosing() && form.voice_volume == 125 && panel.label("choice:voice_volume") == "125%");
    CHECK(panel.focus("choice:voice_volume"));
    send(panel, key(SDL_SCANCODE_LEFT));
    CHECK(form.voice_volume == 100 && form.voice_chat == 0);
#else
    CHECK(panel.shown("choice:voice") && !panel.enabled("choice:voice") && !panel.enabled("choice:voice_volume"));
#endif
    CHECK(panel.label("choice:output") == "1280 \xC3\x97 720");
    type_into(panel, "text:server", "Example.org");
    CHECK(panel.account_status() == "Accounts are for play.magtest.online" && !panel.enabled("button:account"));
    // Advanced connection settings: the ports, with the macOS messages.
    CHECK(!panel.shown("text:tcp"));
    click(panel, "link:advanced");
    CHECK(panel.shown("text:tcp") && panel.label("link:advanced").find("Advanced") != std::string::npos);
    type_into(panel, "text:tcp", "12a");
    type_into(panel, "text:name", "ab");
    click(panel, "button:play");
    CHECK(panel.error() == "Ports must contain digits, or be blank for the game default.");
    type_into(panel, "text:tcp", "12345");
    click(panel, "button:play");
    CHECK(panel.error().find("player name") != std::string::npos && !fs::exists(settings));
    type_into(panel, "text:name", "");
    click(panel, "button:play");
    CHECK(panel.error() == "Enter a player name to play on a server. It is the name you sign in with.");
    type_into(panel, "text:name", "abcd_7");
    // Borderless: the output size is the desktop's; Play first confirms the display.
    CHECK(panel.focus("choice:display"));
    send(panel, key(SDL_SCANCODE_RIGHT));
    CHECK(form.borderless && !panel.enabled("choice:output"));
    draw(panel);
    CHECK(panel.focus("text:name"));
    CHECK(send(panel, key(SDL_SCANCODE_RETURN)) == SettingsPanel::State::Open);  // Enter in a field is Play MAG here
    CHECK(panel.pending() && panel.shown("button:keep") && !panel.enabled("text:server"));
    CHECK(display.calls == std::vector<std::string>{"borderless"} && !fs::exists(settings));
    click(panel, "button:keep");
    CHECK(!panel.pending() && display.calls.back() == "kept" && panel.status().find("Saved") == 0);
    CHECK(panel.focus("text:name"));
    CHECK(send(panel, key(SDL_SCANCODE_RETURN)) == SettingsPanel::State::Saved);
    const std::string saved = read(settings);
    CHECK(saved.find("\"server_address\": \"example.org\"") != std::string::npos);
    CHECK(saved.find("\"tcp_port\": 12345") != std::string::npos && saved.find("\"udp_port\": 0") != std::string::npos);
    CHECK(saved.find("\"player_name\": \"abcd_7\"") != std::string::npos);
    CHECK(saved.find("\"display_mode\": \"borderless\"") != std::string::npos);

    // In game: a windowed size previews, then Revert or the 15 seconds bring the old display back.
    SettingsForm again;
    CHECK(settings_form_load(settings, again, error) && again.borderless && again.tcp_port == "12345");
    FakeDisplay game;
    SettingsPanel ingame(again, settings, SettingsWindowOptions{}, &game);
    CHECK(!ingame.shown("button:play") && !ingame.enabled("button:apply") && ingame.status() == "No unsaved changes");
    CHECK(ingame.focus("choice:display"));
    send(ingame, key(SDL_SCANCODE_LEFT));
    click(ingame, "choice:output");
    CHECK(ingame.choosing());
    send(ingame, key(SDL_SCANCODE_DOWN));
    send(ingame, key(SDL_SCANCODE_RETURN));
    CHECK(!ingame.choosing() && again.output == 1 && ingame.enabled("button:apply"));
    click(ingame, "button:apply");
    CHECK(ingame.pending() && game.calls == std::vector<std::string>{"windowed 1920"});
    click(ingame, "button:revert");
    CHECK(!ingame.pending() && game.calls.back() == "restore" && ingame.status() == "Display changes reverted");
    CHECK(again.borderless && read(settings) == saved);
    send(ingame, key(SDL_SCANCODE_LEFT));
    click(ingame, "button:apply");
    CHECK(ingame.pending());
    ingame.tick(now + 16000);
    CHECK(!ingame.pending() && game.calls.back() == "restore" && read(settings) == saved);
    game.fit = false;
    send(ingame, key(SDL_SCANCODE_LEFT));
    click(ingame, "button:apply");
    CHECK(!ingame.pending() && ingame.error().find("does not fit") != std::string::npos);
    // Cancel (Escape) discards it; nothing was saved, so the window reports Cancelled.
    CHECK(send(ingame, key(SDL_SCANCODE_ESCAPE)) == SettingsPanel::State::Cancelled);
    CHECK(again.borderless && read(settings) == saved);

    // The Account row: link through a fake portal, then unlink.
    std::ofstream(settings) << "{\"server_address\": \"play.magtest.online\", \"player_name\": \"Rook_7\"}";
    SettingsForm account;
    CHECK(settings_form_load(settings, account, error));
    SettingsPanel linking(account, settings, SettingsWindowOptions{}, &game);
    std::string opened;
    linking.open_page = [&](const std::string& url) { opened = url; };
    CHECK(linking.label("button:account") == "Link Account\xE2\x80\xA6");
    click(linking, "button:account");
    CHECK(linking.label("button:account") == "Cancel");
    // Wait for the link's worker itself (it sleeps the portal's 1-second poll interval on its own
    // thread), not for a fixed number of ticks, which a loaded machine can outlast.
    CHECK(linking.wait_for_account_link(60000));
    CHECK(linking.account_status() == "Linked as Rook_9" && linking.label("button:account") == "Unlink");
    CHECK(opened == "https://magtest.online/link?code=BCDF-GHJK" && account.player_name == "Rook_9");
    CHECK(fs::exists(folder / "portal-link.json"));
    click(linking, "button:account");
    CHECK(linking.account_status().find("Unlinked here") == 0 && !fs::exists(folder / "portal-link.json"));
    CHECK(linking.enabled("button:apply"));  // the account's name waits for Apply
    click(linking, "button:apply");
    CHECK(read(settings).find("\"player_name\": \"Rook_9\"") != std::string::npos);
    CHECK(send(linking, key(SDL_SCANCODE_ESCAPE)) == SettingsPanel::State::Saved);  // Apply saved something

    // Reports: the checkbox is on by default, Cancel restores it, Apply writes send_reports only
    // when off. Send report needs MAG_REPORT_REQUEST (a packaged launch) and a linked account, and
    // works with the checkbox off.
    {
        std::ofstream(settings) << "{\"server_address\": \"play.magtest.online\", \"player_name\": \"Rook_7\"}";
        SettingsForm reports;
        CHECK(settings_form_load(settings, reports, error) && reports.send_reports);
        const fs::path marker = folder / "report-requested";
        unsetenv("MAG_REPORT_REQUEST");
        SettingsPanel panel(reports, settings, SettingsWindowOptions{}, &game);
        CHECK(panel.shown("check:reports") && panel.label("check:reports") == "On" && panel.enabled("check:reports"));
        CHECK(panel.shown("button:report") && !panel.enabled("button:report"));
        click(panel, "check:reports");
        CHECK(!reports.send_reports && panel.enabled("button:apply") && panel.status() == "Unsaved changes");
        send(panel, key(SDL_SCANCODE_ESCAPE));
        CHECK(reports.send_reports);  // Cancel put it back
        SettingsPanel again(reports, settings, SettingsWindowOptions{}, &game);
        click(again, "check:reports");
        click(again, "button:apply");
        CHECK(read(settings).find("\"send_reports\": false") != std::string::npos);
        PortableSettings stored;
        bool exists = false;
        CHECK(load_portable_settings(settings, stored, exists, error) && !stored.send_reports);
        click(again, "check:reports");
        click(again, "button:apply");
        CHECK(read(settings).find("send_reports") == std::string::npos);
        // Not linked: the button explains; linked with a request path: it writes the marker.
        setenv("MAG_REPORT_REQUEST", marker.string().c_str(), 1);
        SettingsPanel unlinked(reports, settings, SettingsWindowOptions{}, &game);
        CHECK(!unlinked.enabled("button:report") && !fs::exists(marker));
        // The comment box is shown, and disabled with the button: no link here, then no request path.
        CHECK(unlinked.shown("note:report") && !unlinked.enabled("note:report"));
        unsetenv("MAG_REPORT_REQUEST");
        {
            SettingsPanel no_path(reports, settings, SettingsWindowOptions{}, &game);
            CHECK(no_path.shown("note:report") && !no_path.enabled("note:report") && !no_path.enabled("button:report"));
        }
        setenv("MAG_REPORT_REQUEST", marker.string().c_str(), 1);
        std::string link_error;
        CHECK(portal::save_link(folder / "portal-link.json", {"https://magtest.online", "mgd_synthetic_token", "Rook_9"}, link_error));
        reports.send_reports = false;  // an explicit request works with reports off
        SettingsPanel linked(reports, settings, SettingsWindowOptions{}, &game);
        CHECK(linked.enabled("button:report") && linked.enabled("note:report"));
        // The comment: typed text (ASCII and UTF-8), Enter for a new line, Backspace one code point.
        click(linked, "note:report");
        CHECK(linked.focused() == "note:report");
        send(linked, typed("Froze at "));
        send(linked, typed("caf\xc3\xa9"));
        send(linked, key(SDL_SCANCODE_RETURN));  // a new line, not Apply
        send(linked, typed("ok \xf0\x9f\x98\x80"));
        CHECK(linked.report_comment() == "Froze at caf\xc3\xa9\nok \xf0\x9f\x98\x80" && linked.label("note:report") == linked.report_comment());
        send(linked, key(SDL_SCANCODE_BACKSPACE));  // the whole four-byte code point
        CHECK(linked.report_comment() == "Froze at caf\xc3\xa9\nok ");
        send(linked, key(SDL_SCANCODE_SPACE));  // Space types (TEXTINPUT); as a key it does nothing here
        CHECK(linked.report_comment() == "Froze at caf\xc3\xa9\nok " && linked.focused() == "note:report");
        CHECK(report_comment_length(linked.report_comment()) == 17 && report_comment_counter(linked.report_comment()) == "17/1000");
        draw(linked);
        // At most MAG_SETTINGS_REPORT_COMMENT_LIMIT code points.
        {
            SettingsPanel full(reports, settings, SettingsWindowOptions{}, &game);
            click(full, "note:report");
            for (int i = 0; i < 260; ++i) send(full, typed("\xc3\xa9\xc3\xa9xy"));  // 1040 code points
            CHECK(report_comment_length(full.report_comment()) == MAG_SETTINGS_REPORT_COMMENT_LIMIT);
            CHECK(report_comment_counter(full.report_comment()) == "1000/1000");
            send(full, typed("z"));
            CHECK(report_comment_length(full.report_comment()) == MAG_SETTINGS_REPORT_COMMENT_LIMIT);
            draw(full);  // a long comment shows its last lines
        }
        // Tab reaches the box, and Send report follows it.
        CHECK(linked.focus("text:server"));
        for (int i = 0; i < 40 && linked.focused() != "note:report"; ++i) send(linked, key(SDL_SCANCODE_TAB));
        CHECK(linked.focused() == "note:report");
        click(linked, "button:report");
        const std::string request = read(marker);
        CHECK(request.rfind("{\"requested_at\":", 0) == 0 && request.back() == '\n');
        // The status shows the first 8 characters of the id the marker carries, for staff to match.
        const size_t id_at = request.find("\"report_id\":\"");
        CHECK(id_at != std::string::npos && request.size() >= id_at + 13 + 36);
        const std::string id = id_at == std::string::npos ? std::string() : request.substr(id_at + 13, 36);
        CHECK(id.size() == 36 && id[14] == '4' && id.find_first_not_of("0123456789abcdef-") == std::string::npos);
        CHECK(linked.report_status() == "Report " + id.substr(0, 8) + " will be sent when MAG closes.");
        // The comment went with the request, escaped, and the box is empty again for the next one.
        CHECK(request.find("\"comment\":\"Froze at caf\xc3\xa9\\nok \"}") != std::string::npos);
        CHECK(linked.report_comment().empty() && linked.label("note:report").empty());
        CHECK((fs::status(marker).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
        draw(linked);
        unsetenv("MAG_REPORT_REQUEST");
        CHECK(portal::forget_link(folder / "portal-link.json", link_error));
    }

    // A damaged settings file: Play waits until Apply replaces it.
    std::ofstream(settings) << "{broken";
    SettingsForm damaged;
    CHECK(!settings_form_load(settings, damaged, error) && damaged.server == "play.magtest.online");
    SettingsWindowOptions recovery = setup;
    recovery.notice = "damaged";
    SettingsPanel fixing(damaged, settings, recovery, &display);
    CHECK(!fixing.enabled("button:play") && fixing.error() == "damaged" && read(settings) == "{broken");
    type_into(fixing, "text:name", "Rook_7");
    click(fixing, "button:apply");
    CHECK(fixing.enabled("button:play") && read(settings).find("Rook_7") != std::string::npos);
    std::printf("general: fields, validation, account, display preview, keep, revert, timeout, cancel, damaged file\n");
}

// The General page as the macOS panel and the Win32 window edit it: Apply writes General alone,
// dirty tracking, display changes that need the timed confirmation, Cancel, Play MAG's name rule,
// and the fields the window does not edit kept from the file.
static void general_apply(const fs::path& folder) {
    fs::create_directories(folder);
    const fs::path settings = folder / "settings.json";
    SettingsForm form;
    std::string error;
    CHECK(settings_form_load(settings, form, error) && form.needs_save && !form.recovery);
    CHECK(settings_form_general_dirty(form));  // no file yet: Apply writes the defaults
    PortableSettings draft;
    CHECK(settings_form_general_draft(form, draft, error) && !settings_form_play_ready(draft, error));
    CHECK(error == "Enter a player name to play on a server. It is the name you sign in with.");
    form.player_name = " Rook_7 ";
    form.output = 1;
    CHECK(settings_form_general_draft(form, draft, error) && settings_form_play_ready(draft, error));
    CHECK(draft.player_name == "Rook_7" && draft.output_width == 1920 && draft.output_height == 1080);
    CHECK(settings_form_display_changed(form.saved, draft));  // a windowed size: confirm it first
    CHECK(settings_form_apply_general(settings, form, draft, error) && !form.needs_save && !settings_form_general_dirty(form));
    std::string saved = read(settings);
    CHECK(saved.find("\"output_width\": 1920") != std::string::npos && saved.find("\"player_name\": \"Rook_7\"") != std::string::npos);
    // Borderless ignores the windowed size; the size is a display change only when windowed.
    PortableSettings next = form.saved;
    next.display_mode = "borderless";
    CHECK(settings_form_display_changed(form.saved, next));
    PortableSettings resized = next;
    resized.output_width = 3840, resized.output_height = 2160;
    CHECK(!settings_form_display_changed(next, resized));
    // Ports, with the macOS panel's messages; nothing is written.
    form.tcp_port = "12a";
    CHECK(!settings_form_general_draft(form, draft, error) && error == "Ports must contain digits, or be blank for the game default.");
    form.tcp_port = "70000";
    CHECK(!settings_form_general_draft(form, draft, error) && error == "Ports must be between 1 and 65535, or blank.");
    form.tcp_port = "0";
    CHECK(!settings_form_general_draft(form, draft, error) && error == "Ports must be between 1 and 65535, or blank.");
    form.tcp_port = " 00080 ";
    CHECK(settings_form_general_draft(form, draft, error) && draft.tcp_port == 80);
    CHECK(settings_form_general_dirty(form));
    // Cancel: the fields return to the file's values.
    settings_form_discard(form);
    CHECK(form.tcp_port.empty() && form.output == 1 && !settings_form_general_dirty(form));
    CHECK(read(settings) == saved);
    // The window does not edit portal_url, ticket_file, the rendering scale or anti-aliasing: kept.
    std::ofstream(settings) << "{\"server_address\": \"example.org\", \"player_name\": \"Rook_7\", \"portal_url\": \"https://portal.example.org\","
                               " \"render_scale\": 200, \"anti_aliasing\": \"fxaa\"}";
    CHECK(settings_form_load(settings, form, error) && !form.needs_save && form.render_scale == 2 && form.anti_aliasing == 1);
    form.player_name = "Rook_8";
    CHECK(settings_form_general_draft(form, draft, error) && settings_form_apply_general(settings, form, draft, error));
    saved = read(settings);
    CHECK(saved.find("\"portal_url\": \"https://portal.example.org\"") != std::string::npos);
    CHECK(saved.find("\"render_scale\": 200") != std::string::npos && saved.find("\"anti_aliasing\": \"fxaa\"") != std::string::npos);
    // Voice: a volume between the menu's steps shows as the nearest and is kept until another is
    // picked; the microphone, which has no row, is kept as the file holds it.
    std::ofstream(settings) << "{\"server_address\": \"\", \"voice_chat\": \"ptt\", \"voice_volume\": 90, \"voice_input\": \"Desk Mic\"}";
    CHECK(settings_form_load(settings, form, error) && form.voice_chat == 1 && form.voice_volume == 90 && form.voice_input == "Desk Mic");
    CHECK(settings_form_voice_volume_step(form) == 4 && !settings_form_general_dirty(form));
    settings_form_set_voice_volume_step(form, 4);
    CHECK(form.voice_volume == 90 && !settings_form_general_dirty(form));
    settings_form_set_voice_volume_step(form, 6);
    CHECK(form.voice_volume == 150 && settings_form_general_dirty(form));
    form.voice_chat = 0;
    CHECK(settings_form_general_draft(form, draft, error) && settings_form_apply_general(settings, form, draft, error));
    saved = read(settings);
    CHECK(saved.find("voice_chat") == std::string::npos && saved.find("\"voice_volume\": 150") != std::string::npos &&
          saved.find("\"voice_input\": \"Desk Mic\"") != std::string::npos);
    // A damaged file: recovery until an explicit Apply replaces it.
    std::ofstream(settings) << "{broken";
    CHECK(!settings_form_load(settings, form, error) && form.recovery && form.needs_save && settings_form_general_dirty(form));
    CHECK(read(settings) == "{broken");
    std::printf("general apply: draft, dirty, display change, ports, cancel, kept fields, recovery\n");
}

// The Debug page: the controls trace switch is the file the game looks for, made and removed at
// once; Open logs folder opens the supervisor's logs folder. Apply and Cancel do not touch them.
static void debug(const fs::path& folder) {
    fs::create_directories(folder);
    const fs::path settings = folder / "settings.json", trace = folder / "controls-trace", logs = folder / "logs";
    std::ofstream(settings) << "{\"server_address\": \"\"}";
    SettingsForm form;
    std::string error;
    CHECK(settings_form_load(settings, form, error));
    // The portable switches, as every host's window uses them.
    CHECK(controls_trace_path(settings) == trace && !debug_switch("controls_trace", settings));
    CHECK(set_debug_switch("controls_trace", settings, true, error) && fs::exists(trace) && fs::file_size(trace) == 0);
    CHECK(debug_switch("controls_trace", settings) && set_debug_switch("controls_trace", settings, true, error));
    CHECK(set_debug_switch("controls_trace", settings, false, error) && !fs::exists(trace));
    CHECK(set_debug_switch("controls_trace", settings, false, error));  // already off
    CHECK(!set_debug_switch("unknown", settings, true, error) && !debug_switch("unknown", settings));
    unsetenv("MAG_PLAYER_LOGS_ROOT");
    char buffer[4096];
    CHECK(logs_folder().empty() && !mag_settings_logs_folder(buffer, sizeof buffer));
    {
        SettingsPanel panel(form, settings, SettingsWindowOptions{});
        click(panel, "segment:debug");
        CHECK(panel.debug_page() && !panel.enabled("button:debug:open_logs"));  // no supervisor: no logs folder
    }
    setenv("MAG_PLAYER_LOGS_ROOT", logs.c_str(), 1);
    CHECK(logs_folder() == logs && mag_settings_logs_folder(buffer, sizeof buffer) && logs == buffer);
    CHECK(mag_settings_set_debug_switch("controls_trace", settings.c_str(), 1) && mag_settings_debug_switch("controls_trace", settings.c_str()));
    CHECK(mag_settings_set_debug_switch("controls_trace", settings.c_str(), 0) && !fs::exists(trace));
    // The window: General, Controls and Debug segments; Debug lists each item from the shared table.
    SettingsPanel panel(form, settings, SettingsWindowOptions{});
    std::string opened;
    panel.open_page = [&](const std::string& path) { opened = path; };
    CHECK(panel.shown("segment:general") && panel.shown("segment:debug"));
    click(panel, "segment:debug");
    CHECK(panel.debug_page() && !panel.controls_page() && !panel.shown("text:server") && !panel.enabled("button:apply"));
    CHECK(panel.label("check:debug:controls_trace") == "Record controls trace" && panel.label("button:debug:open_logs") == "Open logs folder");
    click(panel, "check:debug:controls_trace");
    CHECK(fs::exists(trace) && panel.status() == "On \xC2\xB7 takes effect at the next launch");
    draw(panel);
    click(panel, "check:debug:controls_trace");
    CHECK(!fs::exists(trace) && panel.status() == "Off \xC2\xB7 takes effect at the next launch");
    click(panel, "button:debug:open_logs");
    CHECK(opened == logs.string() && fs::is_directory(logs));
    // Ctrl+Tab goes around the pages; Escape after a switch reports nothing saved.
    send(panel, key(SDL_SCANCODE_TAB, KMOD_CTRL));
    CHECK(!panel.debug_page() && !panel.controls_page());
    CHECK(send(panel, key(SDL_SCANCODE_ESCAPE)) == SettingsPanel::State::Cancelled);
    unsetenv("MAG_PLAYER_LOGS_ROOT");
    std::printf("debug: controls trace switch, logs folder, pages\n");
}

static void controls(const fs::path& folder) {
    register_controls(folder);
    const fs::path settings = folder / "settings.json";
    std::ofstream(settings) << "{\"server_address\": \"\"}";
    SettingsForm form;
    std::string error;
    CHECK(settings_form_load(settings, form, error) && form.controls);
    SettingsPanel panel(form, settings, SettingsWindowOptions{});
    int saved_controls = 0;
    panel.controls_saved = [&] { ++saved_controls; };
    uint64_t held = 1;  // Cross / A held when a capture begins
    panel.held_controller = [&] { return held; };
    click(panel, "segment:controls");
    CHECK(panel.controls_page() && panel.label("button:apply") == "Apply controls" && !panel.enabled("button:apply"));
    CHECK(panel.hint() == "Choose a binding, then press a key or button. Escape cancels capture.");
    // Start is a system button: it applies on foot and in menus, under its group.
    CHECK(panel.label("slot:0:0") == "W" && panel.label("slot:1:0") == "Left mouse" && panel.label("slot:1:1") == "Unbound");
    CHECK(panel.label("slot:2:0") == "Escape");
    // Fire, secondary: Q.
    click(panel, "slot:1:1");
    CHECK(panel.capturing() && panel.hint() == "Press a key or mouse button. Escape cancels.");
    send(panel, key(SDL_SCANCODE_Q));
    CHECK(!panel.capturing() && panel.label("slot:1:1") == "Q" && panel.hint() == "Binding changed. Apply controls to save.");
    CHECK(panel.enabled("button:apply") && panel.status() == "Unsaved changes");
    // Move forward, primary: Q again asks; Cancel leaves it, Replace takes it.
    click(panel, "slot:0:0");
    send(panel, key(SDL_SCANCODE_Q));
    CHECK(panel.asking() && panel.question() == "Replace existing binding?");
    send(panel, key(SDL_SCANCODE_ESCAPE));
    CHECK(!panel.asking() && panel.label("slot:0:0") == "W");
    click(panel, "slot:0:0");
    send(panel, key(SDL_SCANCODE_Q));
    send(panel, key(SDL_SCANCODE_RETURN));
    CHECK(panel.label("slot:0:0") == "Q" && panel.label("slot:1:1") == "Unbound");
    // A mouse button, Escape and the 15-second timeout.
    click(panel, "slot:0:0");
    send(panel, mouse(SDL_BUTTON_RIGHT));
    CHECK(panel.label("slot:0:0") == "Right mouse");
    // The wheel binds a notch up or down, and does not scroll the list while capturing.
    click(panel, "slot:0:0");
    send(panel, wheel(-1));
    CHECK(!panel.capturing() && panel.label("slot:0:0") == "Wheel down");
    click(panel, "slot:0:0");
    send(panel, wheel(1));
    CHECK(panel.label("slot:0:0") == "Wheel up");
    click(panel, "slot:0:0");
    send(panel, key(SDL_SCANCODE_ESCAPE));
    CHECK(!panel.capturing() && panel.hint() == "Binding capture canceled.");
    click(panel, "slot:0:0");
    CHECK(panel.tick(now + 16000) == SettingsPanel::State::Open && !panel.capturing());
    CHECK(panel.hint() == "No binding received. Choose a binding to try again.");
    // Clearing a slot with its x.
    click(panel, "clear:0:0");
    CHECK(panel.label("slot:0:0") == "Unbound" && !panel.enabled("clear:0:0"));
    // Sensitivity, inversion and mode.
    CHECK(panel.focus("slider"));
    send(panel, key(SDL_SCANCODE_RIGHT));
    send(panel, key(SDL_SCANCODE_RIGHT));
    CHECK(settings_form_mouse_sensitivity(form) > 1.09 && settings_form_mouse_sensitivity(form) < 1.11);
    click(panel, "check:invert");
    CHECK(form.profile.mouse.invert_y);
    CHECK(panel.focus("choice:mode"));
    send(panel, key(SDL_SCANCODE_RIGHT));
    CHECK(form.profile.mode == InputMode::KBM);
    // The menu scope lists only actions that apply there, and hides the mouse row.
    CHECK(panel.focus("choice:scope"));
    send(panel, key(SDL_SCANCODE_RIGHT));
    CHECK(panel.label("slot:0:0") == "Escape" && !panel.shown("slot:1:0") && !panel.shown("slider"));
    send(panel, key(SDL_SCANCODE_LEFT));  // back to on foot
    draw(panel);
    // Many actions in several groups scroll, and a focused binding scrolls into view.
    const std::string last = "slot:" + std::to_string(2 + 16) + ":0";
    CHECK(panel.scroll_limit() > 0 && !panel.shown(last));
    CHECK(panel.focus(last) && panel.shown(last) && panel.scroll() > 0);
    send(panel, SDL_Event{});
    // Controller page: held buttons bind only after a release; Guide is reserved; triggers use hysteresis.
    click(panel, "segment:controller");
    CHECK(panel.controller() && panel.label("button:reset") == "Reset Controller" && !panel.shown("slider"));
    click(panel, "slot:0:1");
    send(panel, pad(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A));
    CHECK(panel.capturing() && panel.hint() == "Release held controls, then press a controller button. Escape cancels.");
    send(panel, pad(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_GUIDE));
    CHECK(panel.capturing());
    send(panel, pad(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A));
    send(panel, pad(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A));
    CHECK(!panel.capturing() && panel.label("slot:0:0") == "R1 / RB" && panel.label("slot:0:1") == "Cross / A");
    held = 0;
    click(panel, "slot:0:1");
    send(panel, trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, .5));
    CHECK(panel.capturing());
    send(panel, trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, .6));
    CHECK(!panel.capturing() && panel.label("slot:0:1") == "R2 / RT" && form.profile.controller_remap);
    // Reset asks first; Cancel changes nothing, Reset restores this device only.
    click(panel, "button:reset");
    CHECK(panel.asking() && panel.question() == "Reset controller bindings?");
    send(panel, key(SDL_SCANCODE_ESCAPE));
    CHECK(panel.label("slot:0:1") == "R2 / RT");
    click(panel, "button:reset");
    send(panel, key(SDL_SCANCODE_RETURN));
    CHECK(panel.label("slot:0:1") == "Unbound");
    draw(panel);
    // Apply controls saves only the controls, and tells the game.
    click(panel, "button:apply");
    CHECK(saved_controls == 1 && panel.hint() == "Controls saved. They take effect at the next input update.");
    CHECK(!panel.enabled("button:apply"));
    const auto profile = mag_controls_settings_load();
    CHECK(profile.mouse.invert_y && profile.mode == InputMode::KBM);
    CHECK(profile.mouse.radians_per_count > .00109 && profile.mouse.radians_per_count < .00111);
    bool forward_unbound = true;
    for (const auto& b : profile.bindings)
        if (b.action == 1 && b.control.source == InputSource::Keyboard) forward_unbound = false;
    CHECK(forward_unbound);  // an explicit clear survives the save
    CHECK(send(panel, key(SDL_SCANCODE_ESCAPE)) == SettingsPanel::State::Saved);
    // An unreadable controls profile: Reset reviews the defaults before anything replaces it.
    std::ofstream(folder / "controls.json") << "{broken";
    SettingsForm damaged;
    CHECK(settings_form_load(settings, damaged, error) && !damaged.controls_error.empty());
    CHECK(!settings_form_save(settings, damaged, error) && error.find("Reset") != std::string::npos);
    SettingsPanel recovering(damaged, settings, SettingsWindowOptions{});
    click(recovering, "segment:controls");
    CHECK(recovering.hint().find("Use Reset to review defaults") != std::string::npos && !recovering.enabled("slot:0:0"));
    click(recovering, "button:reset");
    CHECK(recovering.question() == "Replace the unreadable controls profile?");
    send(recovering, key(SDL_SCANCODE_RETURN));
    CHECK(recovering.hint() == "Defaults ready. Apply controls to replace the unreadable profile, or Cancel to preserve it.");
    CHECK(read(folder / "controls.json") == "{broken");
    click(recovering, "button:apply");
    CHECK(mag_controls_settings_load().bindings.size() == 5);
    std::printf("controls: capture, conflict, clear, scope, groups and scrolling, mouse, controller, reset, apply, recovery\n");
}

// A screen shorter than the panel: the page scrolls between the segments and the footer, which
// stay in the window. With no view height, or one the layout fits in, nothing moves.
static bool same(const SDL_Rect& a, const SDL_Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
static void scrolling(const fs::path& folder) {
    fs::create_directories(folder);
    const fs::path settings = folder / "settings.json";
    std::ofstream(settings) << "{\"server_address\": \"play.magtest.online\", \"player_name\": \"Rook_7\"}";
    SettingsForm form;
    std::string error;
    CHECK(settings_form_load(settings, form, error) && form.controls);
    SettingsWindowOptions setup;
    setup.setup = true;
    SettingsPanel panel(form, settings, setup);
    const std::vector<std::string> names = {"segment:general", "text:server", "text:name", "choice:display", "choice:voice_volume",
                                            "check:reports", "note:report", "button:cancel", "button:apply", "button:play"};
    auto rects = [&] {
        std::vector<SDL_Rect> out;
        for (const auto& name : names) out.push_back(panel.rect(name));
        return out;
    };
    auto unchanged = [&](const std::vector<SDL_Rect>& before) {
        const auto now = rects();
        for (size_t i = 0; i < now.size(); ++i)
            if (!same(now[i], before[i])) return false;
        return true;
    };
    const int natural = panel.height();
    const auto unlimited = rects();
    const SDL_Rect segments = unlimited[0];
    CHECK(panel.natural_height() == natural && panel.page_scroll_limit() == 0 && panel.page_thumb().w == 0);
    CHECK(natural > 720);  // the General page outgrows a 720-pixel screen, which is why it scrolls
    for (const int view : {natural, natural + 100}) {
        panel.set_view_height(view);
        CHECK(panel.height() == natural && panel.page_scroll_limit() == 0 && unchanged(unlimited));
    }
    constexpr int margin = MAG_SETTINGS_MARGIN;
    for (const int view : {720 - 2 * MAG_SETTINGS_WORK_AREA_MARGIN, 400}) {
        panel.set_view_height(view);
        panel.set_page_scroll(0);
        CHECK(panel.focus("text:server") && panel.page_scroll() == 0);
        const int height = panel.height(), limit = panel.page_scroll_limit();
        const SDL_Rect page = panel.page_rect(), bottom = panel.rect("button:cancel");
        CHECK(height <= view && panel.natural_height() == natural && limit > 0 && limit == natural - height);
        CHECK(page.y == segments.y + segments.h + 10 && page.h >= MAG_SETTINGS_MIN_PAGE_HEIGHT && page.y + page.h < bottom.y);
        CHECK(same(panel.rect("segment:general"), segments));  // pinned at the top
        for (const char* name : {"button:cancel", "button:apply", "button:play"}) {  // pinned at the bottom
            const SDL_Rect r = panel.rect(name);
            CHECK(r.y >= page.y + page.h && r.y >= 0 && r.y + r.h <= height && r.h == 24);
        }
        CHECK(panel.visible("text:server") && panel.shown("note:report") && !panel.visible("note:report"));
        CHECK(same(panel.rect("text:server"), unlimited[1]));  // not scrolled: where it was
        draw(panel, {{margin + 4, page.y + page.h + 4}, {margin + 4, bottom.y + 2}});  // nothing drawn below the page
        // The wheel over the page scrolls it, down to its end, where the comment box is in view.
        send(panel, motion(page.x + 100, page.y + 20));
        send(panel, wheel(-1));
        CHECK(panel.page_scroll() == std::min(40, limit));
        for (int i = 0; i < 100; ++i) send(panel, wheel(-1));
        CHECK(panel.page_scroll() == limit && panel.visible("note:report") && !panel.visible("text:server"));
        const SDL_Rect note = panel.rect("note:report"), thumb = panel.page_thumb();
        CHECK(note.y == unlimited[6].y - limit && note.y + note.h <= page.y + page.h);
        CHECK(thumb.w > 0 && thumb.y + thumb.h == page.y + page.h - 2);
        draw(panel, {{margin + 4, page.y - 4}, {margin + 200, page.y - 4}, {margin + 4, bottom.y + 2}});  // nothing over the segments
        send(panel, wheel(3));
        CHECK(panel.page_scroll() == std::max(0, limit - 120));
        // Tab and focus() bring what they reach into view.
        panel.set_page_scroll(0);
        for (int i = 0; i < 40 && panel.focused() != "check:reports"; ++i) send(panel, key(SDL_SCANCODE_TAB));
        CHECK(panel.focused() == "check:reports" && panel.visible("check:reports") && panel.page_scroll() > 0);
        for (int i = 0; i < 40 && panel.focused() != "text:server"; ++i) send(panel, key(SDL_SCANCODE_TAB, KMOD_SHIFT));
        CHECK(panel.focused() == "text:server" && panel.visible("text:server"));
        CHECK(panel.focus("note:report") && panel.visible("note:report") && panel.page_scroll() > 0);
        CHECK(panel.focus("button:apply") && panel.visible("button:apply"));  // the footer is always in view
        // A click where an item has scrolled out (under the segments, over the footer) does nothing.
        CHECK(panel.focus("text:server"));
        panel.set_page_scroll(0);
        const SDL_Rect name0 = panel.rect("text:name"), reports0 = panel.rect("check:reports");
        panel.set_page_scroll(name0.y + name0.h / 2 - page.y);
        const SDL_Rect name = panel.rect("text:name");
        CHECK(panel.page_scroll() == name0.y + name0.h / 2 - page.y && name.y < page.y && name.y + name.h > page.y);
        send(panel, mouse(SDL_BUTTON_LEFT, name.x + 5, page.y - 3));
        CHECK(panel.focused() == "text:server");
        send(panel, mouse(SDL_BUTTON_LEFT, name.x + 5, page.y + 3));
        CHECK(panel.focused() == "text:name" && panel.visible("text:name"));  // and the rest of it comes in
        panel.set_page_scroll(reports0.y + reports0.h / 2 - (page.y + page.h));
        const SDL_Rect reports = panel.rect("check:reports");
        CHECK(reports.y < page.y + page.h && reports.y + reports.h > page.y + page.h);
        const bool sending = form.send_reports;
        send(panel, mouse(SDL_BUTTON_LEFT, reports.x + 5, page.y + page.h + 3));
        CHECK(form.send_reports == sending && panel.focused() == "text:name");
        send(panel, mouse(SDL_BUTTON_LEFT, reports.x + 5, page.y + page.h - 3));
        CHECK(form.send_reports != sending && panel.focused() == "check:reports" && panel.visible("check:reports"));
        click(panel, "check:reports");
        CHECK(form.send_reports == sending);
        // The scrollbar: a click on its track moves the thumb there, and the thumb drags.
        panel.set_page_scroll(0);
        const SDL_Rect top = panel.page_thumb();
        send(panel, mouse(SDL_BUTTON_LEFT, top.x + 2, page.y + page.h - 3));
        send(panel, release(top.x + 2, page.y + page.h - 3));
        CHECK(panel.page_scroll() == limit && panel.page_thumb().y + panel.page_thumb().h == page.y + page.h - 2);
        const SDL_Rect end = panel.page_thumb();
        send(panel, mouse(SDL_BUTTON_LEFT, end.x + 2, end.y + 2));
        CHECK(panel.page_scroll() == limit);  // grabbing the thumb does not move it
        send(panel, motion(end.x + 2, end.y + 2 - (end.y - top.y) / 2, SDL_BUTTON_LMASK));
        CHECK(panel.page_scroll() > 0 && panel.page_scroll() < limit);
        send(panel, motion(end.x + 2, 0, SDL_BUTTON_LMASK));
        CHECK(panel.page_scroll() == 0);
        send(panel, release(end.x + 2, 0));
        send(panel, motion(end.x + 2, end.y + 2));
        CHECK(panel.page_scroll() == 0);  // released
        // Page Down and Page Up.
        send(panel, key(SDL_SCANCODE_PAGEDOWN));
        CHECK(panel.page_scroll() == std::min(limit, page.h - 40));
        send(panel, key(SDL_SCANCODE_PAGEUP));
        CHECK(panel.page_scroll() == 0);
        // A choice low in the window: its list draws over the footer, then closes.
        CHECK(panel.focus("choice:voice_volume"));
        send(panel, key(SDL_SCANCODE_SPACE));
        CHECK(panel.choosing());
        draw(panel);
        send(panel, key(SDL_SCANCODE_ESCAPE));
        CHECK(!panel.choosing() && panel.visible("choice:voice_volume"));
    }
    // The Controls page: the wheel over the bindings list scrolls the list, elsewhere the page.
    panel.set_view_height(400);
    click(panel, "segment:controls");
    CHECK(panel.controls_page() && panel.page_scroll() == 0 && panel.page_scroll_limit() > 0 && panel.height() <= 400);
    const SDL_Rect page = panel.page_rect(), slot = panel.rect("slot:0:0");
    send(panel, motion(slot.x + 4, slot.y + 4));
    send(panel, wheel(-1));
    CHECK(panel.scroll() == 24 && panel.page_scroll() == 0);
    send(panel, motion(page.x + 4, page.y + 4));
    send(panel, wheel(-1));
    CHECK(panel.scroll() == 24 && panel.page_scroll() == std::min(40, panel.page_scroll_limit()));
    const std::string last = "slot:" + std::to_string(2 + 16) + ":0";
    CHECK(panel.focus(last) && panel.visible(last) && panel.scroll() > 24);
    CHECK(panel.focus("button:reset") && panel.visible("button:reset"));
    draw(panel, {{margin + 4, page.y - 4}});
    click(panel, "button:reset");
    CHECK(panel.asking());
    draw(panel);  // the question over a scrolled page
    send(panel, key(SDL_SCANCODE_ESCAPE));
    CHECK(!panel.asking());
    // No view height again: the General page is laid out as before.
    panel.set_view_height(0);
    click(panel, "segment:general");
    CHECK(!panel.controls_page() && panel.height() == natural && panel.page_scroll_limit() == 0 && unchanged(unlimited));
    std::printf("scrolling: view height, pinned segments and footer, wheel, focus, clipped clicks, scrollbar, controls list\n");
}

// The window comes forward: SDL's raise, then on X11 alone a second, pager-style request.
static void forward() {
    struct Fake {
        std::string driver = "x11", calls;
        std::vector<bool> focus;  // answers to each wait, in order
        bool sent = true;
        SettingsForwardHooks hooks() {
            SettingsForwardHooks h;
            h.driver = [this] { return driver; };
            h.raise = [this] { calls += "raise "; };
            h.focused = [this](unsigned ms) {
                calls += "wait" + std::to_string(ms) + " ";
                const bool now = !focus.empty() && focus.front();
                if (!focus.empty()) focus.erase(focus.begin());
                return now;
            };
            h.activate_x11 = [this] { calls += "x11 "; return sent; };
            return h;
        }
    };
    Fake granted;
    granted.focus = {true};
    CHECK(settings_bring_forward(granted.hooks()) == SettingsForward::Focused && granted.calls == "raise wait250 ");
    Fake refused;
    refused.focus = {false, true};
    CHECK(settings_bring_forward(refused.hooks(), 100) == SettingsForward::FocusedOnRetry &&
          refused.calls == "raise wait100 x11 wait100 ");
    Fake stubborn;
    stubborn.focus = {false, false};
    CHECK(settings_bring_forward(stubborn.hooks()) == SettingsForward::NotFocused && stubborn.calls == "raise wait250 x11 wait250 ");
    Fake unsent;
    unsent.focus = {false, true};
    unsent.sent = false;
    CHECK(settings_bring_forward(unsent.hooks()) == SettingsForward::NotFocused && unsent.calls == "raise wait250 x11 ");
    Fake wayland;
    wayland.driver = "wayland";
    wayland.focus = {false, true};
    CHECK(settings_bring_forward(wayland.hooks()) == SettingsForward::NotFocused && wayland.calls == "raise wait250 ");
    const SettingsX11Activation request;
    CHECK(request.source == 2 && request.timestamp == 0 && request.requestor == 0);  // a pager, CurrentTime
    // Not an X11 window (the dummy driver): nothing is sent.
    SDL_Window* window = SDL_CreateWindow("forward", 0, 0, 32, 32, SDL_WINDOW_HIDDEN);
    CHECK(window && !settings_activate_x11(window) && !settings_activate_x11(nullptr));
    if (window) SDL_DestroyWindow(window);
    std::printf("forward: raise, focus wait, the X11 retry, other drivers\n");
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a hang still shows how far it got
    const fs::path folder = argv[1];
    general(folder / "general");
    general_apply(folder / "apply");
    debug(folder / "debug");
    fs::create_directories(folder / "controls");
    controls(folder / "controls");
    scrolling(folder / "scrolling");  // after controls(): the catalog it registers gives a Controls page
    forward();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("settings window checks passed\n");
    return 0;
}
