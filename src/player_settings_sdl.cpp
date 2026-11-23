// The settings window on Linux and other SDL2 hosts (player_settings_window.h): a panel drawn
// with SDL2 in its own window, so the player build needs no toolkit beyond the SDL2 the game
// already links. Text uses the SDK's public-domain 8x8 font (libs/misc/osk_font8x8.h), as its
// on-screen keyboard dialog does.
#include "player_settings_sdl_panel.h"
#include "player_settings_window.h"
#include "libs/misc/osk_font8x8.h"
#include <algorithm>
#include <cstdio>
#include <utility>

namespace mag {
namespace {
constexpr int Glyph = 16;  // 8x8 glyphs drawn at twice their size
constexpr int RowHeight = 28, RowsTop = 104, RowsBottom = 410;
const char* const TabNames[] = {"General", "Keyboard & mouse", "Controller"};
const char* const FieldLabels[] = {"Game server", "TCP port", "UDP port", "Player name"};
const size_t FieldLimits[] = {253, 5, 5, 15};
const char* const DisplayChoices[] = {"Windowed", "Borderless fullscreen"};
const char* const ScopeChoices[] = {"On foot", "Menus"};
const char* const ModeChoices[] = {"Automatic", "Keyboard & mouse", "Controller"};

struct Color { Uint8 r, g, b; };
constexpr Color Text{230, 230, 230}, Dim{150, 150, 150}, Accent{110, 170, 255}, Warn{255, 170, 90},
    Panel{32, 34, 38}, Field{48, 51, 57}, Focus{70, 110, 170};

void fill(SDL_Renderer* r, SDL_Rect rect, Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(r, &rect);
}
void outline(SDL_Renderer* r, SDL_Rect rect, Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderDrawRect(r, &rect);
}
void text(SDL_Renderer* r, SDL_Texture* font, int x, int y, const std::string& s, Color c, size_t max = 200) {
    SDL_SetTextureColorMod(font, c.r, c.g, c.b);
    for (size_t i = 0; i < s.size() && i < max; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        SDL_Rect src{(ch - 0x20) * 8, 0, 8, 8}, dst{x + int(i) * Glyph, y, Glyph, Glyph};
        SDL_RenderCopy(r, font, &src, &dst);
    }
}
// Word wrap into lines of at most `columns` characters.
std::vector<std::string> wrap(const std::string& s, size_t columns) {
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
bool inside(SDL_Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
SDL_Rect prompt_box() { return {80, 170, SettingsPanel::width - 160, 200}; }
SDL_Rect prompt_yes() { return {180, 310, 180, 34}; }
SDL_Rect prompt_no() { return {400, 310, 180, 34}; }
}  // namespace

SettingsPanel::SettingsPanel(SettingsForm& form, std::filesystem::path settings, bool setup, std::string notice)
    : form_(form), settings_(std::move(settings)), setup_(setup), status_(std::move(notice)) {
    if (status_.empty() && !form_.controls_error.empty())
        status_ = "The saved controls could not be read: " + form_.controls_error + " Reset shows the defaults.";
    if (status_.empty() && form_.mode_override) status_ = "This launch overrides the input mode; Save applies the selected mode.";
    if (status_.empty())
        status_ = form_.controls ? "Tab moves, Enter chooses, Ctrl+Tab changes page. Escape cancels."
                                 : "Controls are not part of this build; only General is available.";
    held_controller = [] { return uint64_t(0); };
    focus_ = 0;
}

std::string* SettingsPanel::field(int id) {
    switch (id) {
    case 0: return &form_.server;
    case 1: return &form_.tcp_port;
    case 2: return &form_.udp_port;
    default: return &form_.player_name;
    }
}
int SettingsPanel::choice(int id) const {
    if (id == 0) return form_.borderless ? 1 : 0;
    if (id == 1) return scope_;
    return form_.profile.mode == InputMode::KBM ? 1 : form_.profile.mode == InputMode::Controller ? 2 : 0;
}
void SettingsPanel::set_choice(int id, int value) {
    if (id == 0) form_.borderless = value == 1;
    else if (id == 1) { scope_ = value; scroll_ = 0; refresh(); }
    else settings_form_set_mode(form_, value == 1 ? InputMode::KBM : value == 2 ? InputMode::Controller : InputMode::Auto);
}

std::vector<SettingsPanel::Item> SettingsPanel::layout() const {
    std::vector<Item> items;
    const int pages = form_.controls ? 3 : 1;
    for (int i = 0; i < pages; ++i) items.push_back({Kind::Tab, {10 + i * 250, 10, 240, 32}, i, 0, TabNames[i]});
    if (page_ == 0) {
        for (int i = 0; i < 4; ++i) items.push_back({Kind::Text, {260, 64 + i * 46, 470, 32}, i, 0, FieldLabels[i]});
        items.push_back({Kind::Choice, {260, 64 + 4 * 46, 470, 32}, 0, 0, "Display mode"});
    } else {
        items.push_back({Kind::Choice, {150, 60, 170, 32}, 1, 0, "Scope"});
        if (page_ == 1) items.push_back({Kind::Choice, {470, 60, 280, 32}, 2, 0, "Mode"});
        for (size_t row = 0; row < rows_.size(); ++row) {
            const int y = RowsTop + int(row) * RowHeight - scroll_;
            for (unsigned slot = 0; slot < 2; ++slot)
                items.push_back({Kind::Slot, {380 + int(slot) * 190, y, 182, RowHeight - 4}, int(row), slot, rows_[row].slot[slot]});
        }
        if (page_ == 1) {
            items.push_back({Kind::Slider, {260, 420, 200, 28}, 0, 0, "Mouse sensitivity"});
            items.push_back({Kind::Check, {480, 420, 140, 28}, 0, 0, "Invert Y"});
        }
        items.push_back({Kind::Button, {page_ == 1 ? 630 : 480, 420, page_ == 1 ? 120 : 270, 28}, Reset, 0,
                         page_ == 1 ? "Reset" : "Reset controller"});
    }
    items.push_back({Kind::Button, {290, 516, 230, 34}, Save, 0, setup_ ? "Save and play" : "Save"});
    items.push_back({Kind::Button, {540, 516, 210, 34}, Cancel, 0, "Cancel"});
    return items;
}

std::string SettingsPanel::focused() const {
    const auto items = layout();
    if (focus_ < 0 || size_t(focus_) >= items.size()) return "none";
    const Item& item = items[size_t(focus_)];
    switch (item.kind) {
    case Kind::Tab: return "tab:" + std::to_string(item.id);
    case Kind::Text: return "text:" + std::to_string(item.id);
    case Kind::Choice: return "choice:" + std::to_string(item.id);
    case Kind::Slot: return "slot:" + std::to_string(item.id) + ":" + std::to_string(item.slot);
    case Kind::Slider: return "slider";
    case Kind::Check: return "check";
    default: return "button:" + item.text;
    }
}

void SettingsPanel::refresh() {
    rows_ = page_ ? settings_form_rows(form_, device(), scope_ ? settings_scope_menu : settings_scope_on_foot)
                  : std::vector<SettingsBindingRow>{};
}

void SettingsPanel::set_page(int page) {
    end_capture(nullptr);
    page_ = page;
    scroll_ = 0;
    refresh();
    focus_ = page;  // the page's own tab
}

void SettingsPanel::ensure_visible() {
    const auto items = layout();
    if (focus_ < 0 || size_t(focus_) >= items.size() || items[size_t(focus_)].kind != Kind::Slot) return;
    const int y = RowsTop + items[size_t(focus_)].id * RowHeight;
    if (y - scroll_ < RowsTop) scroll_ = y - RowsTop;
    if (y + RowHeight - scroll_ > RowsBottom) scroll_ = y + RowHeight - RowsBottom;
}

void SettingsPanel::end_capture(const char* why) {
    if (!capture_.on) return;
    capture_.on = false;
    if (why) status_ = why;
}

void SettingsPanel::begin_capture(const Item& item, uint64_t now_ms) {
    capture_ = Capture{};
    capture_.on = true;
    capture_.action = rows_[size_t(item.id)].action;
    capture_.slot = item.slot;
    capture_.device = device();
    capture_.scope = scope_ ? settings_scope_menu : settings_scope_on_foot;
    capture_.deadline = now_ms + 15000;
    if (capture_.device == SettingsDevice::Controller) {
        capture_.blocked = held_controller ? held_controller() : 0;
        capture_.triggers = capture_.blocked & (3ull << 32);
        status_ = "Release held controls, then press a controller button. Escape cancels.";
    } else {
        status_ = "Press a key or mouse button. Escape cancels.";
    }
}

void SettingsPanel::accept(PhysicalControl control) {
    pending_capture_ = capture_;
    end_capture(nullptr);
    std::string conflict, error;
    if (settings_form_bind(form_, pending_capture_.scope, pending_capture_.action, pending_capture_.slot, control, false,
                           conflict, error)) {
        status_ = "Bound " + settings_form_control_name(control) + ". Save to keep it.";
    } else if (!conflict.empty()) {
        pending_ = control;
        prompt_ = Prompt::Replace;
        status_ = settings_form_control_name(control) + " is assigned to " + conflict + ". Replace that binding?";
        return;
    } else {
        status_ = error;
    }
    refresh();
}

void SettingsPanel::clear_slot(const Item& item) {
    std::string error;
    status_ = settings_form_clear(form_, device(), scope_ ? settings_scope_menu : settings_scope_on_foot,
                                  rows_[size_t(item.id)].action, item.slot, error)
                  ? "Binding cleared. Save to keep it." : error;
    refresh();
}

SettingsPanel::State SettingsPanel::answer(bool yes) {
    const Prompt prompt = prompt_;
    prompt_ = Prompt::None;
    std::string conflict, error;
    if (prompt == Prompt::Replace) {
        if (!yes) status_ = "Binding unchanged.";
        else if (settings_form_bind(form_, pending_capture_.scope, pending_capture_.action, pending_capture_.slot, pending_,
                                    true, conflict, error))
            status_ = "Bound " + settings_form_control_name(pending_) + ". Save to keep it.";
        else status_ = error;
    } else if (prompt == Prompt::Reset) {
        if (!yes) status_ = "Bindings unchanged.";
        else status_ = settings_form_reset(form_, device(), error) ? "Defaults restored. Save to keep them." : error;
    }
    refresh();
    return state_;
}

void SettingsPanel::save() {
    end_capture(nullptr);
    std::string error;
    if (settings_form_save(settings_, form_, error)) state_ = State::Saved;
    else status_ = error;
}

void SettingsPanel::activate(const Item& item, uint64_t now_ms) {
    switch (item.kind) {
    case Kind::Tab: set_page(item.id); break;
    case Kind::Choice: set_choice(item.id, (choice(item.id) + 1) % (item.id == 2 ? 3 : 2)); break;
    case Kind::Slot: begin_capture(item, now_ms); break;
    case Kind::Check: settings_form_set_invert_y(form_, !form_.profile.mouse.invert_y); break;
    case Kind::Button:
        if (item.id == Save) save();
        else if (item.id == Cancel) state_ = State::Cancelled;
        else if (item.id == Reset) { prompt_ = Prompt::Reset; status_ = device() == SettingsDevice::Controller
            ? "Reset the controller bindings? The keyboard and mouse settings are kept."
            : "Reset the keyboard and mouse bindings? The controller settings are kept."; }
        break;
    default: break;
    }
}

SettingsPanel::State SettingsPanel::tick(uint64_t now_ms) {
    if (capture_.on && now_ms > capture_.deadline) end_capture("Capture timed out.");
    return state_;
}

SettingsPanel::State SettingsPanel::handle(const SDL_Event& event, uint64_t now_ms) {
    tick(now_ms);
    if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
        state_ = State::Cancelled;
        return state_;
    }
    if (prompt_ != Prompt::None) {
        if (event.type == SDL_KEYDOWN) {
            // Physical keys (scancodes) throughout: they need no keyboard layout to be known.
            const SDL_Scancode key = event.key.keysym.scancode;
            if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER || key == SDL_SCANCODE_Y) return answer(true);
            if (key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_N) return answer(false);
        } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
            if (inside(prompt_yes(), event.button.x, event.button.y)) return answer(true);
            if (inside(prompt_no(), event.button.x, event.button.y)) return answer(false);
        }
        return state_;
    }
    if (capture_.on) {
        switch (event.type) {
        case SDL_KEYDOWN: {
            if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) { end_capture("Capture cancelled."); break; }
            const unsigned usage = unsigned(event.key.keysym.scancode);
            if (capture_.device == SettingsDevice::KeyboardMouse && !event.key.repeat && usage >= 4 && usage <= 231 &&
                usage != SDL_SCANCODE_LGUI && usage != SDL_SCANCODE_RGUI)
                accept({InputSource::Keyboard, usage});
            break;
        }
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
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) end_capture("Capture cancelled: the window lost focus.");
            break;
        default: break;
        }
        return state_;
    }

    auto items = layout();
    if (focus_ >= int(items.size())) focus_ = int(items.size()) - 1;
    Item* current = focus_ >= 0 ? &items[size_t(focus_)] : nullptr;
    switch (event.type) {
    case SDL_TEXTINPUT:
        if (current && current->kind == Kind::Text) {
            std::string* value = field(current->id);
            for (const char* c = event.text.text; *c; ++c) {
                if (*c < 0x20 || *c > 0x7E || value->size() >= FieldLimits[current->id]) continue;
                if ((current->id == 1 || current->id == 2) && (*c < '0' || *c > '9')) continue;
                *value += *c;
            }
        }
        break;
    case SDL_KEYDOWN: {
        const SDL_Scancode key = event.key.keysym.scancode;
        const bool shift = event.key.keysym.mod & KMOD_SHIFT, control = event.key.keysym.mod & KMOD_CTRL;
        if (key == SDL_SCANCODE_ESCAPE) { state_ = State::Cancelled; break; }
        if (key == SDL_SCANCODE_TAB && control) { set_page((page_ + (shift ? (form_.controls ? 2 : 0) : 1)) % (form_.controls ? 3 : 1)); break; }
        if (key == SDL_SCANCODE_S && control) { save(); break; }
        if (key == SDL_SCANCODE_TAB || key == SDL_SCANCODE_DOWN || key == SDL_SCANCODE_UP) {
            const int n = int(items.size());
            focus_ = (focus_ + ((key == SDL_SCANCODE_UP || (key == SDL_SCANCODE_TAB && shift)) ? n - 1 : 1)) % n;
            ensure_visible();
            break;
        }
        if (!current) break;
        if (key == SDL_SCANCODE_LEFT || key == SDL_SCANCODE_RIGHT) {
            const int step = key == SDL_SCANCODE_RIGHT ? 1 : -1;
            if (current->kind == Kind::Choice) {
                const int n = current->id == 2 ? 3 : 2;
                set_choice(current->id, (choice(current->id) + n + step) % n);
            } else if (current->kind == Kind::Slider) {
                settings_form_set_mouse_sensitivity(form_, settings_form_mouse_sensitivity(form_) + .05 * step);
            } else if (current->kind == Kind::Tab) {
                set_page(std::clamp(page_ + step, 0, form_.controls ? 2 : 0));
            }
            break;
        }
        if (key == SDL_SCANCODE_BACKSPACE || key == SDL_SCANCODE_DELETE) {
            if (current->kind == Kind::Text && !field(current->id)->empty()) field(current->id)->pop_back();
            else if (current->kind == Kind::Slot) clear_slot(*current);
            break;
        }
        if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER || (key == SDL_SCANCODE_SPACE && current->kind != Kind::Text)) activate(*current, now_ms);
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (event.button.button != SDL_BUTTON_LEFT) break;
        for (size_t i = 0; i < items.size(); ++i) {
            const Item& item = items[i];
            if (item.kind == Kind::Slot && (item.rect.y < RowsTop || item.rect.y + item.rect.h > RowsBottom)) continue;
            if (!inside(item.rect, event.button.x, event.button.y)) continue;
            focus_ = int(i);
            if (item.kind == Kind::Slider)
                settings_form_set_mouse_sensitivity(form_, .1 + 4.9 * double(event.button.x - item.rect.x) / item.rect.w);
            else if (item.kind != Kind::Text)
                activate(item, now_ms);
            break;
        }
        break;
    case SDL_MOUSEWHEEL:
        if (page_) {
            const int most = std::max(0, int(rows_.size()) * RowHeight - (RowsBottom - RowsTop));
            scroll_ = std::clamp(scroll_ - event.wheel.y * RowHeight, 0, most);
        }
        break;
    default: break;
    }
    return state_;
}

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
    fill(r, {0, 0, width, height}, Panel);
    const auto items = layout();
    // Labels and headings.
    if (page_ == 0) {
        for (int i = 0; i < 4; ++i) text(r, font, 20, 72 + i * 46, FieldLabels[i], Dim);
        text(r, font, 20, 72 + 4 * 46, "Display mode", Dim);
        for (size_t i = 0; const auto& line : wrap("Server, port and display changes apply the next time MAG starts. "
                                                   "A server needs a player name: 3 to 15 letters, digits, - or _, "
                                                   "starting with a letter.", 46))
            text(r, font, 20, 312 + int(i++) * 22, line, Dim);
    } else {
        text(r, font, 20, 68, "Bindings", Dim);
        if (page_ == 1) text(r, font, 350, 68, "Mode", Dim);
        SDL_Rect clip{0, RowsTop, width, RowsBottom - RowsTop};
        SDL_RenderSetClipRect(r, &clip);
        for (size_t row = 0; row < rows_.size(); ++row)
            text(r, font, 20, RowsTop + int(row) * RowHeight - scroll_ + 4, rows_[row].label, Text, 22);
        SDL_RenderSetClipRect(r, nullptr);
        if (page_ == 1) text(r, font, 20, 426, "Sensitivity", Dim);
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        const bool focus = int(i) == focus_ && prompt_ == Prompt::None;
        if (item.kind == Kind::Slot) {
            if (item.rect.y < RowsTop || item.rect.y + item.rect.h > RowsBottom) continue;
            const bool live = capture_.on && rows_[size_t(item.id)].action == capture_.action && item.slot == capture_.slot;
            fill(r, item.rect, live ? Focus : Field);
            text(r, font, item.rect.x + 6, item.rect.y + 4, live ? "Press..." : item.text,
                 rows_[size_t(item.id)].bound[item.slot] ? Text : Dim, 11);
        } else if (item.kind == Kind::Tab) {
            fill(r, item.rect, item.id == page_ ? Focus : Field);
            text(r, font, item.rect.x + 8, item.rect.y + 8, item.text, Text, 14);
        } else if (item.kind == Kind::Text) {
            fill(r, item.rect, Field);
            const std::string& value = item.id == 0 ? form_.server : item.id == 1 ? form_.tcp_port
                                       : item.id == 2 ? form_.udp_port : form_.player_name;
            const size_t room = size_t(item.rect.w / Glyph) - 1;
            const std::string shown = value.size() > room ? value.substr(value.size() - room) : value;
            text(r, font, item.rect.x + 6, item.rect.y + 8, shown + (focus ? "_" : ""), Text);
        } else if (item.kind == Kind::Choice) {
            fill(r, item.rect, Field);
            const char* const* names = item.id == 0 ? DisplayChoices : item.id == 1 ? ScopeChoices : ModeChoices;
            text(r, font, item.rect.x + 6, item.rect.y + 8, std::string("< ") + names[choice(item.id)] + " >", Text);
        } else if (item.kind == Kind::Slider) {
            fill(r, item.rect, Field);
            const double fraction = (settings_form_mouse_sensitivity(form_) - .1) / 4.9;
            fill(r, {item.rect.x + int(fraction * (item.rect.w - 10)), item.rect.y + 2, 10, item.rect.h - 4}, Accent);
            char value[16];
            std::snprintf(value, sizeof value, "%.2fx", settings_form_mouse_sensitivity(form_));
            text(r, font, 20 + 12 * Glyph, 426, value, Text);
        } else if (item.kind == Kind::Check) {
            text(r, font, item.rect.x, item.rect.y + 6, std::string(form_.profile.mouse.invert_y ? "[x] " : "[ ] ") + item.text, Text);
        } else {
            fill(r, item.rect, item.id == Save ? Focus : Field);
            text(r, font, item.rect.x + 10, item.rect.y + 10, item.text, Text, 18);
        }
        if (focus) outline(r, {item.rect.x - 2, item.rect.y - 2, item.rect.w + 4, item.rect.h + 4}, Accent);
    }
    const bool warn = status_.find("must") != std::string::npos || status_.find("could not") != std::string::npos ||
                      status_.find("Choose") == 0 || status_.find("Enter a") == 0 || status_.find("Ports") == 0;
    int y = 456;
    for (const auto& line : wrap(status_, 46)) {
        if (y > 496) break;
        text(r, font, 20, y, line, warn ? Warn : Text);
        y += 20;
    }
    if (prompt_ != Prompt::None) {
        fill(r, prompt_box(), Field);
        outline(r, prompt_box(), Accent);
        int line_y = prompt_box().y + 20;
        for (const auto& line : wrap(status_, 36)) { text(r, font, prompt_box().x + 20, line_y, line, Text); line_y += 22; }
        fill(r, prompt_yes(), Focus);
        text(r, font, prompt_yes().x + 20, prompt_yes().y + 10, prompt_ == Prompt::Replace ? "Replace" : "Reset", Text);
        fill(r, prompt_no(), Panel);
        text(r, font, prompt_no().x + 20, prompt_no().y + 10, "Keep", Text);
    }
}

SettingsWindowResult settings_window_run(SettingsForm& form, const std::filesystem::path& settings, bool setup,
                                         const std::string& notice) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[mag settings] no display for the settings window: %s\n", SDL_GetError());
        return SettingsWindowResult::Failed;
    }
    SDL_Window* window = SDL_CreateWindow(setup ? "MAG Setup" : "MAG Settings", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          SettingsPanel::width, SettingsPanel::height, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, 0) : nullptr;
    SDL_Texture* font = renderer ? SettingsPanel::make_font(renderer) : nullptr;
    if (!font) {
        std::fprintf(stderr, "[mag settings] cannot open the settings window: %s\n", SDL_GetError());
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return SettingsWindowResult::Failed;
    }
    SDL_RenderSetLogicalSize(renderer, SettingsPanel::width, SettingsPanel::height);
    std::vector<SDL_GameController*> controllers;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i))
            if (auto* c = SDL_GameControllerOpen(i)) controllers.push_back(c);
    SettingsPanel panel(form, settings, setup, notice);
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
    SDL_StartTextInput();
    SettingsPanel::State state = SettingsPanel::State::Open;
    while (state == SettingsPanel::State::Open) {
        panel.draw(renderer, font);
        SDL_RenderPresent(renderer);
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, 250)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED)
                if (auto* c = SDL_GameControllerOpen(event.cdevice.which)) controllers.push_back(c);
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
