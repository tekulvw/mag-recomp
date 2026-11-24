// The settings window on Linux and other SDL2 hosts (player_settings_window.h): the macOS MAG
// Settings panel (player_settings_text.h) drawn with SDL2 in its own window, so the player build
// needs no toolkit beyond the SDL2 the game already links. Text uses the SDK's public-domain 8x8
// font (libs/misc/osk_font8x8.h), as its on-screen keyboard dialog does; the window scales the
// panel by a whole number so the glyphs stay sharp.
#include "player_settings_sdl_panel.h"
#include "player_settings_debug.h"
#include "player_settings_text.h"
#include "player_settings_window.h"
#include "libs/misc/osk_font8x8.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mag {
namespace {
constexpr int Glyph = 8, Line = 11;
constexpr int Margin = MAG_SETTINGS_MARGIN, Content = SettingsPanel::width - 2 * MAG_SETTINGS_MARGIN;
constexpr int ControlWidth = MAG_SETTINGS_CONTROL_WIDTH, ControlHeight = 20, RowHeight = 30;
constexpr int ListHeight = 200, BindingRow = 24;

struct Color { Uint8 r, g, b; };
// The macOS light palette: window, group fill, separators, text, secondary text, accent, error.
constexpr Color Window{236, 236, 236}, GroupFill{255, 255, 255}, Border{217, 217, 217}, Ink{28, 28, 30},
    Secondary{110, 110, 115}, Accent{10, 100, 216}, AccentSoft{204, 228, 247}, Error{215, 0, 21}, Control{250, 250, 250},
    Disabled{170, 170, 175};

void fill(SDL_Renderer* r, SDL_Rect rect, Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(r, &rect);
}
void outline(SDL_Renderer* r, SDL_Rect rect, Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderDrawRect(r, &rect);
}
// UTF-8 to the font's ASCII: the shared wording uses a few typographic characters.
std::string ascii(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out += char(c); continue; }
        const std::string rest = s.substr(i, 3);
        if (rest == "\xE2\x80\xA6") { out += "..."; i += 2; }
        else if (rest == "\xE2\x96\xB8") { out += '>'; i += 2; }
        else if (rest == "\xE2\x96\xBE") { out += 'v'; i += 2; }
        else if (s.compare(i, 2, "\xC3\x97") == 0) { out += 'x'; i += 1; }
        else if (s.compare(i, 2, "\xC2\xB7") == 0) { out += '-'; i += 1; }
        else { out += '?'; while (i + 1 < s.size() && ((unsigned char)s[i + 1] & 0xC0) == 0x80) ++i; }
    }
    return out;
}
void text(SDL_Renderer* r, SDL_Texture* font, int x, int y, const std::string& utf8, Color c, size_t max = 200, bool bold = false) {
    const std::string s = ascii(utf8);
    SDL_SetTextureColorMod(font, c.r, c.g, c.b);
    for (size_t i = 0; i < s.size() && i < max; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        SDL_Rect src{(ch - 0x20) * 8, 0, 8, 8}, dst{x + int(i) * Glyph, y, Glyph, Glyph};
        SDL_RenderCopy(r, font, &src, &dst);
        if (bold) { dst.x += 1; SDL_RenderCopy(r, font, &src, &dst); }
    }
}
// Word wrap into lines of at most `columns` characters.
std::vector<std::string> wrap(const std::string& utf8, size_t columns) {
    const std::string s = ascii(utf8);
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(' ', i);
        std::string word = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? s.size() : j + 1;
        if (!line.empty() && line.size() + 1 + word.size() > columns) { lines.push_back(line); line.clear(); }
        while (word.size() > columns) { lines.push_back(word.substr(0, columns)); word.erase(0, columns); }
        line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}
int text_height(const std::string& s, int width) { return std::max(1, int(wrap(s, size_t(width / Glyph)).size())) * Line - 3; }
bool inside(SDL_Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
bool contained(SDL_Rect inner, SDL_Rect outer) {
    return inner.y >= outer.y && inner.y + inner.h <= outer.y + outer.h;
}
std::string fill_in(const char* pattern, const std::string& a, const std::string& b = {}) {
    std::string out, args[2] = {a, b};
    int next = 0;
    for (const char* p = pattern; *p; ++p) {
        if (p[0] == '%' && p[1] == 's' && next < 2) { out += args[next++]; ++p; }
        else out += *p;
    }
    return out;
}
std::string gain_text(double multiple) {
    char value[32];
    std::snprintf(value, sizeof value, MAG_CONTROLS_SENSITIVITY_FORMAT, multiple);
    return value;
}
// The question dialog and the choice list, drawn over the panel.
SDL_Rect question_box(int height) { return {60, std::max(40, height / 2 - 60), SettingsPanel::width - 120, 120}; }
SDL_Rect question_cancel(int height) { const SDL_Rect b = question_box(height); return {b.x + b.w - 200, b.y + b.h - 34, 84, 22}; }
SDL_Rect question_confirm(int height) { const SDL_Rect b = question_box(height); return {b.x + b.w - 108, b.y + b.h - 34, 96, 22}; }
}  // namespace

SettingsPanel::SettingsPanel(SettingsForm& form, std::filesystem::path settings, SettingsWindowOptions options,
                             SettingsDisplay* display)
    : form_(form), settings_(std::move(settings)), options_(std::move(options)), display_(display),
      account_(std::make_unique<SettingsAccount>(settings_)), mailbox_(std::make_shared<Mailbox>()) {
    held_controller = [] { return uint64_t(0); };
    error_ = options_.notice;
    fill_controls_hint();
    focus_ = "text:server";
    changed();
}
SettingsPanel::~SettingsPanel() { account_->cancel_link(); }

void SettingsPanel::fill_controls_hint() {
    if (!form_.controls_error.empty() && !form_.controls_dirty) hint_ = form_.controls_error + MAG_CONTROLS_HINT_RECOVERY;
    else if (form_.mode_override)
        hint_ = fill_in(MAG_CONTROLS_HINT_OVERRIDE, mag_controls_mode_labels[*form_.mode_override == InputMode::KBM ? 1
                                                                       : *form_.mode_override == InputMode::Controller ? 2 : 0]);
    else hint_ = MAG_CONTROLS_HINT;
}

// ---- Fields and choices ----
std::string* SettingsPanel::field(const std::string& name) {
    if (name == "text:server") return &form_.server;
    if (name == "text:name") return &form_.player_name;
    if (name == "text:tcp") return &form_.tcp_port;
    if (name == "text:udp") return &form_.udp_port;
    return nullptr;
}
int SettingsPanel::choice_value(const std::string& name) const {
    if (name == "choice:display") return form_.borderless ? 1 : 0;
    if (name == "choice:output") return form_.output;
    if (name == "choice:rendering") return form_.render_scale;
    if (name == "choice:aa") return form_.anti_aliasing;
    if (name == "choice:scope") return scope_;
    return form_.profile.mode == InputMode::KBM ? 1 : form_.profile.mode == InputMode::Controller ? 2 : 0;
}
int SettingsPanel::choice_count(const std::string& name) const {
    if (name == "choice:display") return MAG_SETTINGS_DISPLAY_MODES;
    if (name == "choice:output") return MAG_SETTINGS_OUTPUT_SIZES;
    if (name == "choice:rendering") return MAG_SETTINGS_RENDER_SCALES;
    if (name == "choice:aa") return MAG_SETTINGS_ANTI_ALIASING_MODES;
    if (name == "choice:scope") return MAG_CONTROLS_SCOPES;
    return MAG_CONTROLS_MODES;
}
const char* SettingsPanel::choice_label(const std::string& name, int value) const {
    value = std::clamp(value, 0, choice_count(name) - 1);
    if (name == "choice:display") return mag_settings_display_mode_labels[value];
    if (name == "choice:output") return mag_settings_output_labels[value];
    if (name == "choice:rendering") return mag_settings_render_scale_labels[value];
    if (name == "choice:aa") return mag_settings_anti_aliasing_labels[value];
    if (name == "choice:scope") return mag_controls_scope_labels[value];
    return mag_controls_mode_labels[value];
}
void SettingsPanel::set_choice(const std::string& name, int value) {
    value = std::clamp(value, 0, choice_count(name) - 1);
    if (name == "choice:display") form_.borderless = value == 1;
    else if (name == "choice:output") form_.output = value;
    else if (name == "choice:scope") { end_capture(nullptr); scope_ = value; scroll_ = 0; refresh_rows(); }
    else if (name == "choice:mode")
        settings_form_set_mode(form_, value == 1 ? InputMode::KBM : value == 2 ? InputMode::Controller : InputMode::Auto);
    changed();
}

// ---- Layout: top to bottom, as the macOS stack views place it ----
SettingsPanel::Layout SettingsPanel::layout() const {
    Layout out;
    const bool general = !controls_page_ && !debug_page_, locked = pending_;
    int y = 14;
    auto item = [&](std::string name, Kind kind, SDL_Rect rect, std::string label, bool enabled = true, bool selected = false) {
        out.items.push_back({std::move(name), kind, rect, std::move(label), enabled && !locked, selected});
    };
    auto words = [&](int x, int at, const std::string& s, int tone, int width, bool bold = false) {
        out.labels.push_back({x, at, s, tone, bold, width / Glyph});
        return text_height(s, width);
    };
    {  // General, Controls (when the build has the catalog) and Debug, side by side
        std::vector<std::pair<const char*, const char*>> pages = {{"segment:general", MAG_SETTINGS_PAGE_GENERAL}};
        if (form_.controls) pages.push_back({"segment:controls", MAG_SETTINGS_PAGE_CONTROLS});
        pages.push_back({"segment:debug", MAG_SETTINGS_PAGE_DEBUG});
        const int n = int(pages.size());
        for (int i = 0; i < n; ++i) {
            const std::string name = pages[size_t(i)].first;
            const bool selected = name == "segment:general" ? general : name == "segment:controls" ? controls_page_ : debug_page_;
            item(name, Kind::Segment, {Margin + Content * i / n, y, Content * (i + 1) / n - Content * i / n, 22}, pages[size_t(i)].second,
                 true, selected);
        }
        y += 22 + 10;
    }
    const int inner = Margin + 12, inner_width = Content - 24, labels = inner_width - ControlWidth - 16;
    const int control_x = inner + inner_width - ControlWidth;
    // A group of rows: a title and a hint on the left, the control on the right, separators between.
    struct RowSpec { const char* title; std::string hint; std::string name; Kind kind; std::string label; bool enabled; };
    auto group = [&](const std::vector<RowSpec>& rows, bool advanced_part) {
        const int top = y;
        y += 8;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) { out.lines.push_back({inner, y + 2, inner_width, 1}); y += 5; }
            const RowSpec& row = rows[i];
            const int title = Glyph, hint = row.hint.empty() ? 0 : text_height(row.hint, labels);
            const int words_height = title + (hint ? 3 + hint : 0), height = std::max(RowHeight, words_height + 8);
            const int text_y = y + (height - words_height) / 2;
            out.labels.push_back({inner, text_y, row.title, 0, false, labels / Glyph});
            if (hint) out.labels.push_back({inner, text_y + title + 3, row.hint, 1, false, labels / Glyph});
            item(row.name, row.kind, {control_x, y + (height - ControlHeight) / 2, ControlWidth, ControlHeight}, row.label, row.enabled);
            y += height;
        }
        if (advanced_part) {
            out.lines.push_back({inner, y + 2, inner_width, 1});
            y += 6;
            item("link:advanced", Kind::Link, {inner, y, 300, 12}, advanced_ ? MAG_SETTINGS_ADVANCED_OPEN : MAG_SETTINGS_ADVANCED_CLOSED);
            y += 14;
            if (advanced_) {
                const int column = (inner_width - 16) / 2;
                words(inner, y + 2, MAG_SETTINGS_TCP_PORT, 1, column);
                words(inner + column + 16, y + 2, MAG_SETTINGS_UDP_PORT, 1, column);
                y += 14;
                item("text:tcp", Kind::Text, {inner, y, column, ControlHeight}, form_.tcp_port);
                item("text:udp", Kind::Text, {inner + column + 16, y, column, ControlHeight}, form_.udp_port);
                y += ControlHeight + 4;
            }
        }
        y += 8;
        out.groups.push_back({Margin, top, Content, y - top});
    };
    if (general) {
        y += words(Margin, y, MAG_SETTINGS_HEADING_CONNECTION, 0, Content, true) + 8;
        group({{MAG_SETTINGS_SERVER, MAG_SETTINGS_SERVER_HINT, "text:server", Kind::Text, form_.server, true},
               {MAG_SETTINGS_NAME, MAG_SETTINGS_NAME_HINT, "text:name", Kind::Text, form_.player_name, true},
               {MAG_SETTINGS_ACCOUNT, account_status_, "button:account", Kind::Button, account_button_,
                !account_portal().empty() || account_->linking()}},
              true);
        y += 6;
        y += words(Margin, y, MAG_SETTINGS_CONNECTION_NOTE, 1, Content) + 14;
        y += words(Margin, y, MAG_SETTINGS_HEADING_GRAPHICS, 0, Content, true) + 8;
        group({{MAG_SETTINGS_DISPLAY_MODE, "", "choice:display", Kind::Choice, "", true},
               {MAG_SETTINGS_OUTPUT, MAG_SETTINGS_OUTPUT_HINT, "choice:output", Kind::Choice, "", !form_.borderless},
               {MAG_SETTINGS_RENDERING, MAG_SETTINGS_METAL_ONLY_HINT, "choice:rendering", Kind::Choice, "", false},
               {MAG_SETTINGS_ANTI_ALIASING, MAG_SETTINGS_METAL_ONLY_HINT, "choice:aa", Kind::Choice, "", false}},
              false);
        y += 6;
        y += words(Margin, y, MAG_SETTINGS_GRAPHICS_NOTE_NO_SCALE, 1, Content) + 14;
        y += words(Margin, y, MAG_SETTINGS_HEADING_REPORTS, 0, Content, true) + 8;
        const char* blocker = report_blocker();
        group({{MAG_SETTINGS_SEND_REPORTS, MAG_SETTINGS_SEND_REPORTS_HINT, "check:reports", Kind::Check, MAG_SETTINGS_SEND_REPORTS_ON, true},
               {MAG_SETTINGS_SEND_REPORT_NOW, !report_status_.empty() ? report_status_ : blocker ? blocker : MAG_SETTINGS_SEND_REPORT_HINT,
                "button:report", Kind::Button, MAG_SETTINGS_SEND_REPORT_NOW, !blocker}},
              false);
        y += 10;
    } else if (debug_page_) {
        // One row per mag_settings_debug_items entry: switches act at once, actions run.
        y += words(Margin, y, MAG_SETTINGS_HEADING_DEBUG, 0, Content, true) + 8;
        std::vector<RowSpec> rows;
        const bool logs = !logs_folder().empty();
        for (const auto& entry : mag_settings_debug_items) {
            const bool action = entry.kind == MAG_SETTINGS_DEBUG_ACTION;
            const bool open_logs = !std::strcmp(entry.id, "open_logs");
            rows.push_back({entry.title, open_logs && !logs ? MAG_SETTINGS_DEBUG_NO_LOGS : entry.hint,
                            std::string(action ? "button:debug:" : "check:debug:") + entry.id, action ? Kind::Button : Kind::Check,
                            entry.control, !open_logs || logs});
        }
        group(rows, false);
        y += 6;
        y += words(Margin, y, MAG_SETTINGS_DEBUG_NOTE, 1, Content) + 10;
    } else {
        item("segment:kbm", Kind::Segment, {Margin, y, Content / 2, 22}, MAG_CONTROLS_DEVICE_KBM, true, !controller_);
        item("segment:controller", Kind::Segment, {Margin + Content / 2, y, Content - Content / 2, 22}, MAG_CONTROLS_DEVICE_CONTROLLER, true, controller_);
        y += 22 + 10;
        const bool usable = form_.controls_error.empty() || form_.controls_dirty;
        bool menus = false;
        for (auto device : {SettingsDevice::KeyboardMouse, SettingsDevice::Controller})
            menus = menus || !settings_form_rows(form_, device, settings_scope_menu).empty();
        words(Margin, y + 6, MAG_CONTROLS_INPUT_MODE, 0, 80);
        item("choice:mode", Kind::Choice, {Margin + 88, y, 150, ControlHeight}, "", usable);
        if (menus) {
            words(Margin + 254, y + 6, MAG_CONTROLS_SCOPE, 0, 70);
            item("choice:scope", Kind::Choice, {Margin + 326, y, 110, ControlHeight}, "");
        }
        y += ControlHeight + 9;
        y += words(Margin, y, controller_ ? MAG_CONTROLS_AIM_ASSIST_CONTROLLER : MAG_CONTROLS_AIM_ASSIST_KBM, 1, Content) + 9;
        if (!controller_ && scope_ == 0) {
            words(Margin, y + 6, MAG_CONTROLS_SENSITIVITY, 0, 144);
            item("slider", Kind::Slider, {Margin + 150, y + 3, 155, 14}, "", usable);
            words(Margin + 313, y + 6, gain_text(settings_form_mouse_sensitivity(form_)), 0, 56);
            item("check:invert", Kind::Check, {Margin + 380, y + 2, 120, 16}, MAG_CONTROLS_INVERT_Y, usable);
            y += ControlHeight + 9;
        }
        words(Margin + 8, y, MAG_CONTROLS_COLUMN_ACTION, 0, 160);
        words(Margin + 168, y, MAG_CONTROLS_COLUMN_PRIMARY, 1, 170);
        words(Margin + 344, y, MAG_CONTROLS_COLUMN_SECONDARY, 1, 170);
        y += Glyph + 6;
        out.list = {Margin, y, Content, ListHeight};
        int row_y = y + 4 - scroll_;
        std::string previous;
        for (size_t index = 0; index < rows_.size(); ++index) {
            const SettingsBindingRow& row = rows_[index];
            if (row.group != previous) {
                previous = row.group;
                if (!row.group.empty()) {
                    out.labels.push_back({Margin + 8, row_y + 4, row.group, 1, false, Content / Glyph, true});
                    row_y += 16;
                }
            }
            out.labels.push_back({Margin + 8, row_y + 6, row.label, 0, false, 19, true});
            for (unsigned slot = 0; slot < 2; ++slot) {
                const int x = Margin + 168 + int(slot) * 176;
                const std::string suffix = std::to_string(index) + ":" + std::to_string(slot);
                Item bind{"slot:" + suffix, Kind::Slot, {x, row_y, 142, ControlHeight}, usable ? row.slot[slot] : MAG_CONTROLS_UNBOUND,
                          usable && !locked, false, int(index), slot};
                Item clear{"clear:" + suffix, Kind::Clear, {x + 146, row_y, 20, ControlHeight}, MAG_CONTROLS_CLEAR,
                           usable && row.bound[slot] && !locked, false, int(index), slot};
                out.items.push_back(bind);
                out.items.push_back(clear);
            }
            row_y += BindingRow;
        }
        y += ListHeight + 9;
        y += words(Margin, y, hint_, 1, Content) + 9;
        item("button:reset", Kind::Button, {Margin, y, Content, 22}, controller_ ? MAG_CONTROLS_RESET_CONTROLLER : MAG_CONTROLS_RESET_KBM);
        y += 22 + 9;
        y += words(Margin, y, MAG_CONTROLS_NOTE, 1, Content) + 9;
    }
    if (!error_.empty()) y += words(Margin, y, error_, 2, Content) + 9;
    if (pending_) {
        y += words(Margin, y, MAG_SETTINGS_CONFIRM_QUESTION, 0, Content) + 7;
        char countdown[128];
        const long left = long((deadline_ > now_ ? deadline_ - now_ + 999 : 0) / 1000);
        std::snprintf(countdown, sizeof countdown, MAG_SETTINGS_CONFIRM_COUNTDOWN, left);
        y += words(Margin, y, countdown, 1, Content) + 7;
        out.items.push_back({"button:revert", Kind::Button, {Margin, y, 90, 22}, MAG_SETTINGS_CONFIRM_REVERT, true, false});
        out.items.push_back({"button:keep", Kind::Button, {Margin + 98, y, 120, 22}, MAG_SETTINGS_CONFIRM_KEEP, true, true});
        y += 22 + 9;
    }
    out.lines.push_back({Margin, y, Content, 1});
    y += 1 + 9;
    const int play = options_.setup ? 100 : 0, apply = controls_page_ ? 116 : 84;
    int x = Margin + Content - play - (play ? 8 : 0);
    if (play) item("button:play", Kind::Button, {x, y, play, 24}, MAG_SETTINGS_PLAY, !form_.recovery, true);
    x -= apply + (play ? 8 : 0);
    item("button:apply", Kind::Button, {x, y, apply, 24}, controls_page_ ? MAG_SETTINGS_APPLY_CONTROLS : MAG_SETTINGS_APPLY, dirty_,
         !options_.setup);
    x -= 8 + 84;
    item("button:cancel", Kind::Button, {x, y, 84, 24}, MAG_SETTINGS_CANCEL);
    out.labels.push_back({Margin, y + 8, status_, 1, false, (x - Margin - 8) / Glyph});
    y += 24 + 14;
    out.height = y;
    return out;
}

int SettingsPanel::height() const { return layout().height; }
int SettingsPanel::scroll_limit() const {
    int total = 8;
    std::string previous;
    for (const auto& row : rows_) {
        if (row.group != previous) { previous = row.group; if (!row.group.empty()) total += 16; }
        total += BindingRow;
    }
    return std::max(0, total - ListHeight);
}
const SettingsPanel::Item* SettingsPanel::find(const Layout& l, const std::string& name) const {
    for (const auto& i : l.items)
        if (i.name == name) return &i;
    return nullptr;
}
bool SettingsPanel::shown(const std::string& name) const {
    const Layout l = layout();
    const Item* i = find(l, name);
    return i && (l.list.w == 0 || (i->kind != Kind::Slot && i->kind != Kind::Clear) || contained(i->rect, l.list));
}
bool SettingsPanel::enabled(const std::string& name) const {
    const Layout l = layout();
    const Item* i = find(l, name);
    return i && i->enabled;
}
std::string SettingsPanel::label(const std::string& name) const {
    const Layout l = layout();
    const Item* i = find(l, name);
    if (!i) return {};
    if (i->kind == Kind::Choice) return choice_label(name, choice_value(name));
    return i->text;
}
SDL_Rect SettingsPanel::rect(const std::string& name) const {
    const Layout l = layout();
    const Item* i = find(l, name);
    return i ? i->rect : SDL_Rect{0, 0, 0, 0};
}
std::string SettingsPanel::focused() const { return focus_; }
bool SettingsPanel::focus(const std::string& name) {
    const Layout l = layout();
    if (!find(l, name)) return false;
    focus_ = name;
    ensure_visible(l);
    return true;
}
std::string SettingsPanel::account_status() const { return account_status_; }
std::string SettingsPanel::report_status() const { return report_status_; }

// ---- Reports: Send report asks the supervisor for a report when MAG closes ----
const char* SettingsPanel::report_blocker() const {
    return report_request_blocker(account_->linked_name(account_portal()).has_value());
}
void SettingsPanel::request_report_now() {
    if (report_blocker()) return;
    std::string error;
    if (!request_report(error)) { report_status_ = MAG_SETTINGS_REPORT_FAILED; return; }
    report_status_ = MAG_SETTINGS_REPORT_REQUESTED;
}

void SettingsPanel::ensure_visible(const Layout& l) {
    const Item* i = find(l, focus_);
    if (!i || (i->kind != Kind::Slot && i->kind != Kind::Clear) || l.list.h == 0) return;
    if (i->rect.y < l.list.y + 2) scroll_ -= l.list.y + 2 - i->rect.y;
    else if (i->rect.y + i->rect.h > l.list.y + l.list.h - 2) scroll_ += i->rect.y + i->rect.h - (l.list.y + l.list.h - 2);
    scroll_ = std::clamp(scroll_, 0, scroll_limit());
}

// ---- State ----
std::string SettingsPanel::account_portal() const { return account_->portal(form_.server, form_.saved.portal_url); }

void SettingsPanel::changed() {
    dirty_ = debug_page_ ? false : controls_page_ ? form_.controls_dirty : settings_form_general_dirty(form_);
    status_ = dirty_ ? MAG_SETTINGS_STATUS_DIRTY : MAG_SETTINGS_STATUS_CLEAN;
    if (!account_->linking()) {
        const AccountRow row = account_->row(account_portal());
        account_status_ = row.status;
        account_button_ = row.button;
    }
}

void SettingsPanel::refresh_rows() {
    rows_ = controls_page_ ? settings_form_rows(form_, controller_ ? SettingsDevice::Controller : SettingsDevice::KeyboardMouse,
                                                scope_ ? settings_scope_menu : settings_scope_on_foot)
                           : std::vector<SettingsBindingRow>{};
    scroll_ = std::clamp(scroll_, 0, scroll_limit());
}

void SettingsPanel::show_page(int page) {
    end_capture(nullptr);
    controls_page_ = page == 1;
    debug_page_ = page == 2;
    error_.clear();
    scroll_ = 0;
    refresh_rows();
    focus_ = page == 1 ? "segment:controls" : page == 2 ? "segment:debug" : "segment:general";
    changed();
}
void SettingsPanel::set_device(bool controller) {
    end_capture(nullptr);
    controller_ = controller;
    scroll_ = 0;
    refresh_rows();
    fill_controls_hint();
}

// ---- General: Apply, the display confirmation, Play MAG, Cancel ----
bool SettingsPanel::save_general(const PortableSettings& draft) {
    std::string error;
    if (!settings_form_apply_general(settings_, form_, draft, error)) { show_error(error); return false; }
    if (!options_.setup) saved_anything_ = true;
    error_.clear();
    changed();
    status_ = MAG_SETTINGS_STATUS_SAVED_NO_SCALE;
    return true;
}

void SettingsPanel::apply(uint64_t now_ms) {
    if (pending_ || debug_page_) return;
    std::string error;
    if (controls_page_) {
        end_capture(nullptr);
        if (!settings_form_apply_controls(form_, error)) { show_error(error); return; }
        if (!options_.setup) saved_anything_ = true;
        if (controls_saved) controls_saved();
        error_.clear();
        fill_controls_hint();
        hint_ = MAG_CONTROLS_SAVED;
        changed();
        return;
    }
    PortableSettings draft;
    if (!settings_form_general_draft(form_, draft, error)) { show_error(error); return; }
    if (!display_ || !settings_form_display_changed(form_.saved, draft)) { save_general(draft); return; }
    if (draft.display_mode != "borderless" && !display_->fits(draft)) { show_error(MAG_SETTINGS_OUTPUT_TOO_BIG); return; }
    pending_ = true;
    pending_draft_ = draft;
    error_.clear();
    display_->preview(draft);
    now_ = now_ms;
    deadline_ = now_ms + MAG_SETTINGS_CONFIRM_SECONDS * 1000;
    focus_ = "button:keep";
}
void SettingsPanel::keep() {
    if (!pending_) return;
    pending_ = false;
    if (!save_general(pending_draft_)) { if (display_) display_->restore(); }
    else if (display_) display_->kept();
    focus_ = "button:apply";
}
void SettingsPanel::revert() {
    if (!pending_) return;
    pending_ = false;
    if (display_) display_->restore();
    settings_form_fill_general(form_, form_.saved);
    changed();
    status_ = MAG_SETTINGS_DISPLAY_REVERTED;
    focus_ = "choice:display";
}

void SettingsPanel::play(uint64_t now_ms) {
    if (pending_) return;
    if (form_.recovery) { show_error(MAG_SETTINGS_RECOVERY_PLAY); return; }
    std::string error;
    PortableSettings draft;
    if (!settings_form_general_draft(form_, draft, error) || !settings_form_play_ready(draft, error)) { show_error(error); return; }
    if (form_.controls_dirty && !settings_form_apply_controls(form_, error)) { show_error(error); return; }
    if (settings_form_general_dirty(form_)) {
        if (controls_page_ || debug_page_) show_page(0);
        apply(now_ms);
        if (pending_ || settings_form_general_dirty(form_)) return;  // confirm the display first, then Play again
    }
    state_ = State::Saved;
}

void SettingsPanel::cancel() {
    end_capture(nullptr);
    account_->cancel_link();
    if (pending_) revert();
    settings_form_discard(form_);
    state_ = options_.setup || !saved_anything_ ? State::Cancelled : State::Saved;
}

void SettingsPanel::account_clicked() {
    if (account_->linking()) {
        account_->cancel_link();
        changed();
        return;
    }
    const std::string portal = account_portal();
    if (portal.empty()) return;
    if (account_->linked_name(portal)) {
        std::string message;
        if (!account_->unlink(message)) { show_error(message); return; }
        error_.clear();
        account_status_ = message;
        account_button_ = MAG_SETTINGS_ACCOUNT_LINK;
        return;
    }
    error_.clear();
    account_button_ = MAG_SETTINGS_ACCOUNT_CANCEL;
    account_status_ = MAG_SETTINGS_ACCOUNT_CONTACTING;
    auto mailbox = std::make_shared<Mailbox>();
    mailbox_ = mailbox;
    auto opener = open_page;
    account_->start_link(
        portal, "MAG launcher (Linux)", [opener](const std::string& url) { if (opener) opener(url); },
        [mailbox](const std::string& message) {
            std::lock_guard<std::mutex> lock(mailbox->lock);
            mailbox->progress = message;
            mailbox->fresh = true;
            mailbox->signal.notify_all();
        },
        [mailbox](bool linked, const std::string& message) {
            std::lock_guard<std::mutex> lock(mailbox->lock);
            mailbox->finished = true;
            mailbox->linked = linked;
            mailbox->message = message;
            mailbox->signal.notify_all();
        });
}

// ---- Controls ----
void SettingsPanel::end_capture(const char* why) {
    if (!capture_.on) return;
    capture_.on = false;
    if (why) hint_ = why;
}

void SettingsPanel::begin_capture(size_t row, unsigned slot, uint64_t now_ms) {
    end_capture(nullptr);
    if (row >= rows_.size()) return;
    capture_ = Capture{};
    capture_.on = true;
    capture_.row = row;
    capture_.slot = slot;
    capture_.device = controller_ ? SettingsDevice::Controller : SettingsDevice::KeyboardMouse;
    capture_.scope = scope_ ? settings_scope_menu : settings_scope_on_foot;
    capture_.deadline = now_ms + MAG_CONTROLS_CAPTURE_SECONDS * 1000;
    int keys = 0;
    const Uint8* state = SDL_GetKeyboardState(&keys);
    for (int key = 0; state && key < keys && key < 512; ++key) capture_.held[key] = state[key] != 0;
    if (capture_.device == SettingsDevice::Controller) {
        capture_.blocked = held_controller ? held_controller() : 0;
        capture_.triggers = capture_.blocked & (3ull << 32);
        hint_ = MAG_CONTROLS_CAPTURE_CONTROLLER;
    } else {
        hint_ = MAG_CONTROLS_CAPTURE_KEYS;
    }
}

void SettingsPanel::accept(PhysicalControl control) {
    const Capture capture = capture_;
    end_capture(nullptr);
    if (capture.row >= rows_.size()) return;
    std::string conflict, error;
    if (!settings_form_bind(form_, capture.scope, rows_[capture.row].action, capture.slot, control, false, conflict, error)) {
        if (conflict.empty()) { hint_ = error; return; }
        asked_capture_ = capture;
        asked_control_ = control;
        hint_ = MAG_CONTROLS_HINT;
        ask(Question::Replace, MAG_CONTROLS_REPLACE_TITLE,
            fill_in(MAG_CONTROLS_REPLACE_TEXT, settings_form_control_name(control), conflict), MAG_CONTROLS_REPLACE);
        return;
    }
    hint_ = MAG_CONTROLS_CHANGED;
    refresh_rows();
    changed();
}

void SettingsPanel::clear_binding(size_t row, unsigned slot) {
    end_capture(nullptr);
    if (row >= rows_.size()) return;
    std::string error;
    if (!settings_form_clear(form_, controller_ ? SettingsDevice::Controller : SettingsDevice::KeyboardMouse,
                             scope_ ? settings_scope_menu : settings_scope_on_foot, rows_[row].action, slot, error)) {
        hint_ = error;
        return;
    }
    hint_ = MAG_CONTROLS_CHANGED;
    refresh_rows();
    changed();
}

void SettingsPanel::reset_device() {
    end_capture(nullptr);
    if (!form_.controls_error.empty() && !form_.controls_dirty)
        ask(Question::Recover, MAG_CONTROLS_RECOVER_TITLE, MAG_CONTROLS_RECOVER_TEXT, MAG_CONTROLS_RECOVER);
    else
        ask(Question::Reset, controller_ ? MAG_CONTROLS_RESET_CONTROLLER_TITLE : MAG_CONTROLS_RESET_KBM_TITLE, MAG_CONTROLS_RESET_TEXT,
            MAG_CONTROLS_RESET);
}

void SettingsPanel::ask(Question question, const std::string& title, const std::string& text, const std::string& confirm) {
    question_ = question;
    question_title_ = title;
    question_text_ = text;
    question_confirm_ = confirm;
}

void SettingsPanel::answer(bool yes) {
    const Question question = question_;
    question_ = Question::None;
    question_title_.clear();
    if (!yes) return;
    std::string conflict, error;
    const auto device = controller_ ? SettingsDevice::Controller : SettingsDevice::KeyboardMouse;
    if (question == Question::Replace) {
        const Capture& c = asked_capture_;
        if (c.row < rows_.size() &&
            settings_form_bind(form_, c.scope, rows_[c.row].action, c.slot, asked_control_, true, conflict, error))
            hint_ = MAG_CONTROLS_CHANGED;
        else hint_ = error;
    } else if (question == Question::Reset || question == Question::Recover) {
        if (!settings_form_reset(form_, device, error)) hint_ = error;
        else hint_ = question == Question::Recover ? MAG_CONTROLS_DEFAULTS_READY : MAG_CONTROLS_CHANGED;
    }
    refresh_rows();
    changed();
}

// ---- Events ----
void SettingsPanel::activate(const Item& item, uint64_t now_ms, int x) {
    if (!item.enabled) return;
    const std::string& n = item.name;
    switch (item.kind) {
    case Kind::Segment:
        if (n == "segment:general" || n == "segment:controls" || n == "segment:debug")
            show_page(n == "segment:controls" ? 1 : n == "segment:debug" ? 2 : 0);
        else set_device(n == "segment:controller");
        break;
    case Kind::Choice:
        open_choice_ = n;
        choice_cursor_ = choice_value(n);
        break;
    case Kind::Link:
        advanced_ = !advanced_;
        break;
    case Kind::Slot: begin_capture(size_t(item.row), item.slot, now_ms); break;
    case Kind::Clear: clear_binding(size_t(item.row), item.slot); break;
    case Kind::Check:
        if (n.rfind("check:debug:", 0) == 0) {
            // A switch acts at once: it is a file the game reads at launch, not a settings draft.
            const std::string id = n.substr(std::strlen("check:debug:"));
            const bool on = !debug_switch(id, settings_);
            std::string error;
            if (!set_debug_switch(id, settings_, on, error)) show_error(error);
            else { error_.clear(); status_ = on ? MAG_SETTINGS_DEBUG_SWITCH_ON : MAG_SETTINGS_DEBUG_SWITCH_OFF; }
            break;
        }
        if (n == "check:reports") form_.send_reports = !form_.send_reports;
        else settings_form_set_invert_y(form_, !form_.profile.mouse.invert_y);
        changed();
        break;
    case Kind::Slider:
        if (x >= 0) settings_form_set_mouse_sensitivity(form_, .1 + 4.9 * double(x - item.rect.x) / std::max(1, item.rect.w));
        changed();
        break;
    case Kind::Button:
        if (n == "button:apply") apply(now_ms);
        else if (n == "button:play") play(now_ms);
        else if (n == "button:cancel") cancel();
        else if (n == "button:keep") keep();
        else if (n == "button:revert") revert();
        else if (n == "button:account") account_clicked();
        else if (n == "button:reset") reset_device();
        else if (n == "button:report") request_report_now();
        else if (n == "button:debug:open_logs") {
            const std::filesystem::path folder = logs_folder();
            std::error_code ignored;
            if (!folder.empty()) {
                std::filesystem::create_directories(folder, ignored);
                if (open_page) open_page(folder.string());
            }
        }
        break;
    case Kind::Text: break;
    }
}

bool SettingsPanel::wait_for_account_link(uint64_t timeout_ms) {
    const auto mailbox = mailbox_;
    {
        std::unique_lock<std::mutex> lock(mailbox->lock);
        if (!mailbox->signal.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return mailbox->finished; })) return false;
    }
    tick(now_);
    return true;
}

SettingsPanel::State SettingsPanel::tick(uint64_t now_ms) {
    now_ = std::max(now_, now_ms);
    if (capture_.on && now_ms > capture_.deadline) end_capture(MAG_CONTROLS_CAPTURE_TIMEOUT);
    if (pending_ && now_ms >= deadline_) revert();
    // The account link's worker reports here.
    if (account_->linking()) {
        std::string progress, message;
        bool finished = false, linked = false;
        {
            std::lock_guard<std::mutex> lock(mailbox_->lock);
            if (mailbox_->fresh) { progress = mailbox_->progress; mailbox_->fresh = false; }
            finished = mailbox_->finished;
            linked = mailbox_->linked;
            message = mailbox_->message;
        }
        if (!progress.empty()) account_status_ = progress;
        if (finished) {
            account_->link_ended();
            changed();
            if (!linked) {
                if (!message.empty()) show_error(message);
            } else if (const auto name = account_->linked_name(account_portal()); name && !name->empty() && *name != form_.player_name) {
                // The account's name is the one MAG signs in with; offer it in the field for Apply.
                form_.player_name = *name;
                changed();
            }
        }
    }
    return state_;
}

SettingsPanel::State SettingsPanel::handle(const SDL_Event& event, uint64_t now_ms) {
    tick(now_ms);
    if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
        cancel();
        return state_;
    }
    const Layout l = layout();
    // A question waits for its answer.
    if (question_ != Question::None) {
        if (event.type == SDL_KEYDOWN) {
            // Physical keys (scancodes) throughout: they need no keyboard layout to be known.
            const SDL_Scancode key = event.key.keysym.scancode;
            if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER) answer(true);
            else if (key == SDL_SCANCODE_ESCAPE) answer(false);
        } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
            if (inside(question_confirm(l.height), event.button.x, event.button.y)) answer(true);
            else if (inside(question_cancel(l.height), event.button.x, event.button.y)) answer(false);
        }
        return state_;
    }
    // An open choice list.
    if (!open_choice_.empty()) {
        const Item* box = find(l, open_choice_);
        const int count = choice_count(open_choice_);
        if (event.type == SDL_KEYDOWN) {
            const SDL_Scancode key = event.key.keysym.scancode;
            if (key == SDL_SCANCODE_UP) choice_cursor_ = std::max(0, choice_cursor_ - 1);
            else if (key == SDL_SCANCODE_DOWN) choice_cursor_ = std::min(count - 1, choice_cursor_ + 1);
            else if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER || key == SDL_SCANCODE_SPACE) {
                const std::string name = open_choice_;
                open_choice_.clear();
                set_choice(name, choice_cursor_);
            } else if (key == SDL_SCANCODE_ESCAPE) open_choice_.clear();
        } else if (event.type == SDL_MOUSEBUTTONDOWN && box) {
            const SDL_Rect list{box->rect.x, box->rect.y + box->rect.h, box->rect.w, count * 16 + 4};
            const std::string name = open_choice_;
            open_choice_.clear();
            if (inside(list, event.button.x, event.button.y)) set_choice(name, (event.button.y - list.y - 2) / 16);
        }
        return state_;
    }
    if (capture_.on) {
        switch (event.type) {
        case SDL_KEYDOWN: {
            const unsigned usage = unsigned(event.key.keysym.scancode);
            if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) { end_capture(MAG_CONTROLS_CAPTURE_CANCELED); break; }
            if (capture_.device == SettingsDevice::KeyboardMouse && !event.key.repeat && usage >= 4 && usage <= 231 &&
                !(usage < 512 && capture_.held[usage]) && usage != SDL_SCANCODE_LGUI && usage != SDL_SCANCODE_RGUI)
                accept({InputSource::Keyboard, usage});
            break;
        }
        case SDL_KEYUP:
            if (unsigned(event.key.keysym.scancode) < 512) capture_.held[event.key.keysym.scancode] = false;
            break;
        case SDL_MOUSEBUTTONDOWN: {
            if (capture_.device != SettingsDevice::KeyboardMouse) break;
            const Uint8 b = event.button.button;
            const uint32_t index = b == SDL_BUTTON_LEFT ? 0 : b == SDL_BUTTON_RIGHT ? 1 : b == SDL_BUTTON_MIDDLE ? 2
                                   : b == SDL_BUTTON_X1 ? 3 : 4;
            accept({InputSource::Mouse, index});
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN: {
            const unsigned code = event.cbutton.button;
            if (capture_.device != SettingsDevice::Controller || code == SDL_CONTROLLER_BUTTON_GUIDE || code > 14) break;
            if (capture_.blocked & (1ull << code)) break;
            accept({InputSource::Controller, code});
            break;
        }
        case SDL_CONTROLLERBUTTONUP:
            if (event.cbutton.button < 64) capture_.blocked &= ~(1ull << event.cbutton.button);
            break;
        case SDL_CONTROLLERAXISMOTION: {
            if (event.caxis.axis != SDL_CONTROLLER_AXIS_TRIGGERLEFT && event.caxis.axis != SDL_CONTROLLER_AXIS_TRIGGERRIGHT) break;
            const unsigned code = event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 32 : 33;
            const uint64_t bit = 1ull << code;
            const double value = event.caxis.value / 32767.0;
            if (value <= .45) { capture_.triggers &= ~bit; capture_.blocked &= ~bit; break; }
            if (value < .55 || (capture_.triggers & bit)) break;  // hysteresis: down at 0.55, up at 0.45
            capture_.triggers |= bit;
            if (capture_.device == SettingsDevice::Controller && !(capture_.blocked & bit)) accept({InputSource::Controller, code});
            break;
        }
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) end_capture(MAG_CONTROLS_CAPTURE_CANCELED);
            break;
        default: break;
        }
        return state_;
    }

    // Focus moves over the items that can take it.
    std::vector<const Item*> order;
    for (const auto& i : l.items)
        if (i.enabled && i.kind != Kind::Clear) order.push_back(&i);
    const Item* current = find(l, focus_);
    switch (event.type) {
    case SDL_TEXTINPUT:
        if (current && current->kind == Kind::Text) {
            std::string* value = field(current->name);
            const bool port = current->name == "text:tcp" || current->name == "text:udp";
            const size_t limit = port ? 5 : current->name == "text:name" ? 15 : 253;
            for (const char* c = event.text.text; *c; ++c) {
                if (*c < 0x20 || *c > 0x7E || value->size() >= limit) continue;
                *value += *c;
            }
            changed();
        }
        break;
    case SDL_KEYDOWN: {
        const SDL_Scancode key = event.key.keysym.scancode;
        const bool shift = event.key.keysym.mod & KMOD_SHIFT, control = event.key.keysym.mod & KMOD_CTRL;
        if (key == SDL_SCANCODE_ESCAPE) { cancel(); break; }
        if (key == SDL_SCANCODE_TAB && control && !pending_) {
            // General, Controls (when there are controls), Debug, around.
            const int page = controls_page_ ? 1 : debug_page_ ? 2 : 0;
            int next = (page + (shift ? 2 : 1)) % 3;
            if (next == 1 && !form_.controls) next = shift ? 0 : 2;
            show_page(next);
            break;
        }
        if (key == SDL_SCANCODE_TAB || (key == SDL_SCANCODE_DOWN && (!current || current->kind != Kind::Slider)) ||
            (key == SDL_SCANCODE_UP && (!current || current->kind != Kind::Slider))) {
            if (order.empty()) break;
            int at = 0;
            for (size_t i = 0; i < order.size(); ++i)
                if (order[i]->name == focus_) at = int(i);
            const int n = int(order.size());
            at = (at + ((key == SDL_SCANCODE_UP || (key == SDL_SCANCODE_TAB && shift)) ? n - 1 : 1)) % n;
            focus_ = order[size_t(at)]->name;
            ensure_visible(layout());
            break;
        }
        if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER) {
            // Enter on a text field is the default button: Play MAG in setup, otherwise Apply.
            if (!current || current->kind == Kind::Text) {
                if (options_.setup) play(now_ms);
                else if (dirty_) apply(now_ms);
            } else activate(*current, now_ms);
            break;
        }
        if (!current) break;
        if (key == SDL_SCANCODE_LEFT || key == SDL_SCANCODE_RIGHT) {
            const int step = key == SDL_SCANCODE_RIGHT ? 1 : -1;
            if (current->kind == Kind::Choice && current->enabled) set_choice(current->name, choice_value(current->name) + step);
            else if (current->kind == Kind::Slider) {
                settings_form_set_mouse_sensitivity(form_, settings_form_mouse_sensitivity(form_) + .05 * step);
                changed();
            }
            break;
        }
        if (key == SDL_SCANCODE_BACKSPACE || key == SDL_SCANCODE_DELETE) {
            if (current->kind == Kind::Text) {
                std::string* value = field(current->name);
                if (!value->empty()) value->pop_back();
                changed();
            } else if (current->kind == Kind::Slot) clear_binding(size_t(current->row), current->slot);
            break;
        }
        if (key == SDL_SCANCODE_SPACE && current->kind != Kind::Text) activate(*current, now_ms);
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (event.button.button != SDL_BUTTON_LEFT) break;
        for (const auto& item : l.items) {
            if ((item.kind == Kind::Slot || item.kind == Kind::Clear) && !contained(item.rect, l.list)) continue;
            if (!inside(item.rect, event.button.x, event.button.y) || !item.enabled) continue;
            if (item.kind != Kind::Clear) focus_ = item.name;
            activate(item, now_ms, event.button.x);
            break;
        }
        break;
    case SDL_MOUSEWHEEL:
        if (controls_page_) scroll_ = std::clamp(scroll_ - event.wheel.y * BindingRow, 0, scroll_limit());
        break;
    default: break;
    }
    return state_;
}

// ---- Drawing ----
SDL_Texture* SettingsPanel::make_font(SDL_Renderer* renderer) {
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 95 * 8, 8, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return nullptr;
    auto* pixels = static_cast<Uint32*>(surface->pixels);
    const Uint32 on = SDL_MapRGBA(surface->format, 255, 255, 255, 255), off = SDL_MapRGBA(surface->format, 0, 0, 0, 0);
    for (int g = 0; g < 95; ++g)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                pixels[y * (surface->pitch / 4) + g * 8 + x] = (osk_font8x8[g][y] >> x) & 1 ? on : off;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

void SettingsPanel::draw(SDL_Renderer* r, SDL_Texture* font) const {
    const Layout l = layout();
    fill(r, {0, 0, width, l.height}, Window);
    for (const SDL_Rect& g : l.groups) {
        fill(r, g, GroupFill);
        outline(r, g, Border);
    }
    for (const SDL_Rect& line : l.lines) fill(r, line, Border);
    if (l.list.w) {
        fill(r, l.list, GroupFill);
        outline(r, l.list, Border);
    }
    for (const Label& label : l.labels) {
        if (label.listed && !(label.y >= l.list.y + 2 && label.y + Glyph <= l.list.y + l.list.h - 2)) continue;
        int y = label.y;
        const Color c = label.tone == 2 ? Error : label.tone == 1 ? Secondary : Ink;
        if (label.listed) {  // one line in the list, cut to its column
            text(r, font, label.x, y, label.text, c, size_t(label.columns));
            continue;
        }
        for (const auto& line : wrap(label.text, size_t(std::max(1, label.columns)))) {
            text(r, font, label.x, y, line, c, 200, label.bold);
            y += Line;
        }
    }
    for (const Item& item : l.items) {
        if ((item.kind == Kind::Slot || item.kind == Kind::Clear) && !contained(item.rect, {0, l.list.y + 1, width, l.list.h - 2})) continue;
        const bool focus = item.name == focus_ && question_ == Question::None && open_choice_.empty();
        const Color ink = item.enabled ? Ink : Disabled;
        const SDL_Rect& b = item.rect;
        switch (item.kind) {
        case Kind::Segment:
            fill(r, b, item.selected ? AccentSoft : Control);
            outline(r, b, item.selected ? Accent : Border);
            text(r, font, b.x + (b.w - int(ascii(item.text).size()) * Glyph) / 2, b.y + (b.h - Glyph) / 2, item.text, ink);
            break;
        case Kind::Text: {
            fill(r, b, GroupFill);
            outline(r, b, focus ? Accent : Border);
            const std::string value = item.text;
            const char* placeholder = item.name == "text:server" ? MAG_SETTINGS_SERVER_PLACEHOLDER
                                      : item.name == "text:name" ? MAG_SETTINGS_NAME_PLACEHOLDER : MAG_SETTINGS_PORT_PLACEHOLDER;
            const size_t room = size_t(b.w / Glyph) - 2;
            const std::string shown = value.size() > room ? value.substr(value.size() - room) : value;
            if (value.empty() && !focus) text(r, font, b.x + 5, b.y + 6, placeholder, Disabled);
            else text(r, font, b.x + 5, b.y + 6, shown + (focus ? "_" : ""), Ink);
            break;
        }
        case Kind::Choice:
            fill(r, b, item.enabled ? Control : Window);
            outline(r, b, focus ? Accent : Border);
            text(r, font, b.x + 6, b.y + 6, choice_label(item.name, choice_value(item.name)), ink, size_t(b.w / Glyph) - 3);
            text(r, font, b.x + b.w - 14, b.y + 6, "v", ink);
            break;
        case Kind::Link:
            text(r, font, b.x, b.y + 2, item.text, focus ? Accent : Secondary);
            break;
        case Kind::Slot: {
            const bool live = capture_.on && size_t(item.row) == capture_.row && item.slot == capture_.slot;
            fill(r, b, live ? AccentSoft : Control);
            outline(r, b, live || focus ? Accent : Border);
            const std::string shown = live ? std::string("Press...") : item.text;
            text(r, font, b.x + (b.w - int(std::min<size_t>(ascii(shown).size(), 16)) * Glyph) / 2, b.y + 6, shown,
                 item.enabled ? Ink : Disabled, 16);
            break;
        }
        case Kind::Clear:
            fill(r, b, Control);
            outline(r, b, Border);
            text(r, font, b.x + 6, b.y + 6, item.text, item.enabled ? Ink : Disabled);
            break;
        case Kind::Slider: {
            fill(r, {b.x, b.y + b.h / 2 - 1, b.w, 3}, Border);
            const double fraction = (settings_form_mouse_sensitivity(form_) - .1) / 4.9;
            fill(r, {b.x + int(fraction * (b.w - 8)), b.y, 8, b.h}, item.enabled ? Accent : Disabled);
            if (focus) outline(r, {b.x - 2, b.y - 2, b.w + 4, b.h + 4}, Accent);
            break;
        }
        case Kind::Check:
            fill(r, {b.x, b.y + 3, 10, 10}, GroupFill);
            outline(r, {b.x, b.y + 3, 10, 10}, focus ? Accent : Secondary);
            if (item.name.rfind("check:debug:", 0) == 0 ? debug_switch(item.name.substr(12), settings_)
                : item.name == "check:reports" ? form_.send_reports : form_.profile.mouse.invert_y)
                fill(r, {b.x + 3, b.y + 6, 4, 4}, Accent);
            text(r, font, b.x + 16, b.y + 4, item.text, ink);
            break;
        case Kind::Button:
            fill(r, b, item.selected && item.enabled ? Accent : Control);
            outline(r, b, focus ? Accent : Border);
            text(r, font, b.x + (b.w - int(ascii(item.text).size()) * Glyph) / 2, b.y + (b.h - Glyph) / 2, item.text,
                 item.selected && item.enabled ? GroupFill : ink);
            break;
        }
    }
    // An open choice list, then a question, over everything else.
    if (const Item* box = open_choice_.empty() ? nullptr : find(l, open_choice_)) {
        const int count = choice_count(open_choice_);
        const SDL_Rect list{box->rect.x, box->rect.y + box->rect.h, box->rect.w, count * 16 + 4};
        fill(r, list, GroupFill);
        outline(r, list, Accent);
        for (int i = 0; i < count; ++i) {
            if (i == choice_cursor_) fill(r, {list.x + 1, list.y + 2 + i * 16, list.w - 2, 16}, AccentSoft);
            text(r, font, list.x + 6, list.y + 6 + i * 16, choice_label(open_choice_, i), Ink);
        }
    }
    if (question_ != Question::None) {
        const SDL_Rect box = question_box(l.height);
        fill(r, box, GroupFill);
        outline(r, box, Border);
        text(r, font, box.x + 14, box.y + 14, question_title_, Accent, 200, true);
        int y = box.y + 32;
        for (const auto& line : wrap(question_text_, size_t((box.w - 28) / Glyph))) { text(r, font, box.x + 14, y, line, Ink); y += Line; }
        const SDL_Rect cancel = question_cancel(l.height), confirm = question_confirm(l.height);
        fill(r, cancel, Control);
        outline(r, cancel, Border);
        text(r, font, cancel.x + (cancel.w - 6 * Glyph) / 2, cancel.y + 7, MAG_SETTINGS_CANCEL, Ink);
        fill(r, confirm, Accent);
        text(r, font, confirm.x + (confirm.w - int(ascii(question_confirm_).size()) * Glyph) / 2, confirm.y + 7, question_confirm_, GroupFill);
    }
}

// ---- The window ----
namespace {
// Opens a page in the player's browser without waiting for it (xdg-open, as ./MAG --link does).
void open_browser(const std::string& url) {
#ifndef _WIN32
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) return;
    const pid_t pid = fork();
    if (pid < 0) return;
    if (!pid) {
        if (fork() == 0) {
            setsid();
            const int null = open("/dev/null", O_RDWR);
            if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
            execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
        }
        _exit(0);
    }
    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
#else
    (void)url;
#endif
}

bool fits_screen(const PortableSettings& s) {
    SDL_Rect usable{0, 0, 0, 0};
    if (SDL_GetDisplayUsableBounds(0, &usable) != 0) return true;
    return s.output_width <= usable.w && s.output_height <= usable.h;
}

// Before the game: a preview window in this process stands in for the game window.
class PreviewDisplay final : public SettingsDisplay {
public:
    ~PreviewDisplay() override { close(); }
    bool fits(const PortableSettings& s) override { return fits_screen(s); }
    void preview(const PortableSettings& s) override {
        if (!window_) {
            window_ = SDL_CreateWindow(MAG_SETTINGS_PREVIEW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 360, SDL_WINDOW_HIDDEN);
            renderer_ = window_ ? SDL_CreateRenderer(window_, -1, 0) : nullptr;
            font_ = renderer_ ? SettingsPanel::make_font(renderer_) : nullptr;
        }
        if (!window_) return;
        if (s.display_mode == "borderless") SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
        else {
            SDL_SetWindowFullscreen(window_, 0);
            SDL_SetWindowSize(window_, s.output_width, s.output_height);
            SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
        SDL_ShowWindow(window_);
    }
    void restore() override { close(); }
    void kept() override { close(); }
    void paint() {
        if (!renderer_ || !font_) return;
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer_, &w, &h);
        fill(renderer_, {0, 0, w, h}, Window);
        SDL_RenderSetScale(renderer_, 3, 3);
        const int columns = int(std::strlen(MAG_SETTINGS_PREVIEW_LABEL));
        text(renderer_, font_, (w / 3 - columns * Glyph) / 2, (h / 3 - Glyph) / 2, MAG_SETTINGS_PREVIEW_LABEL, Secondary);
        SDL_RenderSetScale(renderer_, 1, 1);
        SDL_RenderPresent(renderer_);
    }

private:
    void close() {
        if (font_) SDL_DestroyTexture(font_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        font_ = nullptr; renderer_ = nullptr; window_ = nullptr;
    }
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* font_ = nullptr;
};

// In game: the game process applies each request on its own window's thread
// (mag_player_settings_sdl_pump), so this process never touches that window.
class GameDisplay final : public SettingsDisplay {
public:
    explicit GameDisplay(int fd) : fd_(fd) {}
    bool fits(const PortableSettings& s) override { return fits_screen(s); }
    void preview(const PortableSettings& s) override {
        send(s.display_mode == "borderless" ? std::string("display borderless\n")
                                            : "display windowed " + std::to_string(s.output_width) + " " + std::to_string(s.output_height) + "\n");
    }
    void restore() override { send("display restore\n"); }
    void kept() override { send("display keep\n"); }
    void send(const std::string& line) {
#ifndef _WIN32
        if (fd_ >= 0 && write(fd_, line.data(), line.size()) < 0) {}
#else
        (void)line;
#endif
    }

private:
    int fd_;
};
}  // namespace

SettingsForward settings_bring_forward(const SettingsForwardHooks& hooks, unsigned wait_ms) {
    hooks.raise();
    if (hooks.focused(wait_ms)) return SettingsForward::Focused;
    if (hooks.driver() != "x11" || !hooks.activate_x11()) return SettingsForward::NotFocused;
    return hooks.focused(wait_ms) ? SettingsForward::FocusedOnRetry : SettingsForward::NotFocused;
}

SettingsWindowResult settings_window_run(SettingsForm& form, const std::filesystem::path& settings,
                                         const SettingsWindowOptions& options) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[mag settings] no display for the settings window: %s\n", SDL_GetError());
        return SettingsWindowResult::Failed;
    }
    PreviewDisplay preview;
    GameDisplay game(options.game_events);
    SettingsDisplay* display = options.game_events >= 0 ? static_cast<SettingsDisplay*>(&game) : &preview;
    SettingsPanel panel(form, settings, options, display);
    panel.open_page = open_browser;
    panel.controls_saved = [&game] { game.send("controls\n"); };
    // A whole-number scale keeps the 8x8 glyphs sharp: twice where the screen holds the tallest page.
    SDL_Rect usable{0, 0, 1280, 720};
    SDL_GetDisplayUsableBounds(0, &usable);
    // The General page (with Reports) is usually the tallest; the Controls page needs about 560.
    int height = panel.height();
    const int scale = usable.w >= 2 * SettingsPanel::width && usable.h >= 2 * std::max(560, height) ? 2 : 1;
    SDL_Window* window = SDL_CreateWindow(MAG_SETTINGS_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          SettingsPanel::width * scale, height * scale, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, 0) : nullptr;
    SDL_Texture* font = renderer ? SettingsPanel::make_font(renderer) : nullptr;
    if (!font) {
        std::fprintf(stderr, "[mag settings] cannot open the settings window: %s\n", SDL_GetError());
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return SettingsWindowResult::Failed;
    }
    SDL_RenderSetLogicalSize(renderer, SettingsPanel::width, height);
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);
    std::vector<SDL_GameController*> controllers;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i))
            if (auto* c = SDL_GameControllerOpen(i)) controllers.push_back(c);
    panel.held_controller = [&controllers] {
        uint64_t held = 0;
        for (auto* c : controllers) {
            for (int b = 0; b <= 14; ++b)
                if (SDL_GameControllerGetButton(c, SDL_GameControllerButton(b))) held |= 1ull << b;
            if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 32767 * 45 / 100) held |= 1ull << 32;
            if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 32767 * 45 / 100) held |= 1ull << 33;
        }
        return held;
    };
    const Uint32 window_id = SDL_GetWindowID(window);
    SettingsForwardHooks forward;
    forward.driver = [] { const char* name = SDL_GetCurrentVideoDriver(); return std::string(name ? name : ""); };
    forward.raise = [window] {
        SDL_RaiseWindow(window);
        SDL_SetWindowInputFocus(window);  // X11 only; elsewhere SDL says it is unsupported
    };
    forward.focused = [window](unsigned ms) {
        const Uint64 end = SDL_GetTicks64() + ms;
        do {
            SDL_PumpEvents();  // focus changes arrive as events; they stay queued for the loop below
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) return true;
            SDL_Delay(10);
        } while (SDL_GetTicks64() < end);
        return false;
    };
    forward.activate_x11 = [window] { return settings_activate_x11(window); };
    switch (settings_bring_forward(forward)) {
    case SettingsForward::Focused: break;
    case SettingsForward::FocusedOnRetry:
        std::fprintf(stderr, "[mag settings] the window manager gave the settings window focus on the second request\n");
        break;
    case SettingsForward::NotFocused:
        std::fprintf(stderr, "[mag settings] the settings window did not get the keyboard (%s); click it to type\n",
                     forward.driver().c_str());
        break;
    }
    SDL_StartTextInput();
    SettingsPanel::State state = SettingsPanel::State::Open;
    while (state == SettingsPanel::State::Open) {
        if (const int now = panel.height(); now != height) {
            height = now;
            SDL_SetWindowSize(window, SettingsPanel::width * scale, height * scale);
            SDL_RenderSetLogicalSize(renderer, SettingsPanel::width, height);
        }
        panel.draw(renderer, font);
        SDL_RenderPresent(renderer);
        preview.paint();
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, 100)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED)
                if (auto* c = SDL_GameControllerOpen(event.cdevice.which)) controllers.push_back(c);
            // The preview window's own events (its close button) are not the panel's.
            if (event.type == SDL_WINDOWEVENT && event.window.windowID != window_id) continue;
            state = panel.handle(event, SDL_GetTicks64());
        } else {
            state = panel.tick(SDL_GetTicks64());
        }
    }
    SDL_StopTextInput();
    for (auto* c : controllers) SDL_GameControllerClose(c);
    SDL_DestroyTexture(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return state == SettingsPanel::State::Saved ? SettingsWindowResult::Saved : SettingsWindowResult::Cancelled;
}
}  // namespace mag
