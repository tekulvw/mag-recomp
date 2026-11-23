#pragma once
// The settings panel on SDL2 hosts (player_settings_sdl.cpp): the macOS MAG Settings panel's pages,
// rows, wording and behaviour (player_settings_text.h), drawn with an SDL renderer and the SDK's
// public-domain 8x8 font. It owns no window and no event loop, so tests drive it with synthetic
// events and a software renderer. Coordinates are the panel's logical points (the macOS panel's
// width); the window scales them by a whole number.
#include "player_settings_account.h"
#include "player_settings_form.h"
#include "player_settings_window.h"
#include <SDL2/SDL.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mag {
// Where a display change is previewed: the game window (through the game process), or a preview
// window before the game starts.
class SettingsDisplay {
public:
    virtual ~SettingsDisplay() = default;
    virtual bool fits(const PortableSettings&) = 0;  // a windowed size the screen can hold
    virtual void preview(const PortableSettings&) = 0;
    virtual void restore() = 0;                      // back to how it was before the preview
    virtual void kept() {}                           // the preview became the display
};

class SettingsPanel {
public:
    static constexpr int width = 620;  // MAG_SETTINGS_WIDTH
    enum class State { Open, Saved, Cancelled };

    SettingsPanel(SettingsForm& form, std::filesystem::path settings, SettingsWindowOptions options,
                  SettingsDisplay* display = nullptr);
    ~SettingsPanel();
    // One event; time in milliseconds (SDL_GetTicks64 in the window). Returns the panel's state.
    State handle(const SDL_Event& event, uint64_t now_ms);
    // Time passing without events: capture and display timeouts, account progress.
    State tick(uint64_t now_ms);
    void draw(SDL_Renderer* renderer, SDL_Texture* font) const;
    int height() const;  // the current layout's height, in points
    // The font texture (white glyphs on transparent) the panel draws with.
    static SDL_Texture* make_font(SDL_Renderer* renderer);

    // Controller codes held now, as a mask (SDL buttons 0-14, triggers 32 and 33): a binding
    // capture ignores them until they are released. The window queries its open controllers.
    std::function<uint64_t()> held_controller;
    // Opens the account portal's page in the browser.
    std::function<void(const std::string&)> open_page;
    // Called after Apply controls saved them (the game applies them at once).
    std::function<void()> controls_saved;

    // For tests and the window: what the panel shows. Items are named, as "text:server",
    // "choice:display", "button:apply", "slot:<row>:<slot>", "clear:<row>:<slot>".
    std::string focused() const;
    bool focus(const std::string& name);             // false when no such item is shown
    bool shown(const std::string& name) const;
    bool enabled(const std::string& name) const;
    std::string label(const std::string& name) const;  // a button's or binding's text, a field's value
    SDL_Rect rect(const std::string& name) const;
    bool controls_page() const { return controls_page_; }
    bool controller() const { return controller_; }
    const std::string& error() const { return error_; }
    const std::string& status() const { return status_; }
    const std::string& hint() const { return hint_; }
    std::string account_status() const;
    bool pending() const { return pending_; }        // a display change waits for Keep or Revert
    bool asking() const { return question_ != Question::None; }
    std::string question() const { return question_title_; }
    bool capturing() const { return capture_.on; }
    bool choosing() const { return !open_choice_.empty(); }
    int scroll() const { return scroll_; }
    int scroll_limit() const;

private:
    enum class Kind { Segment, Text, Choice, Button, Link, Slot, Clear, Slider, Check };
    enum class Question { None, Replace, Reset, Recover };
    struct Item {
        std::string name;
        Kind kind;
        SDL_Rect rect;
        std::string text;
        bool enabled = true;
        bool selected = false;  // a segment's state, the default button
        int row = -1;
        unsigned slot = 0;
    };
    // tone: 0 text, 1 secondary, 2 error; `listed` labels belong to the scrolling bindings list.
    struct Label { int x, y; std::string text; int tone; bool bold; int columns; bool listed = false; };
    struct Layout {
        std::vector<Item> items;
        std::vector<Label> labels;
        std::vector<SDL_Rect> groups;
        std::vector<SDL_Rect> lines;  // separators
        SDL_Rect list{0, 0, 0, 0};   // the bindings list (scrolls)
        int height = 0;
    };
    struct Capture {
        bool on = false;
        size_t row = 0;
        unsigned slot = 0;
        SettingsDevice device = SettingsDevice::KeyboardMouse;
        uint32_t scope = settings_scope_on_foot;
        uint64_t deadline = 0, blocked = 0, triggers = 0;
        bool held[512] = {};  // keys held when capture began (scancodes)
    };

    Layout layout() const;
    const Item* find(const Layout&, const std::string& name) const;
    std::string* field(const std::string& name);
    int choice_value(const std::string& name) const;
    int choice_count(const std::string& name) const;
    const char* choice_label(const std::string& name, int value) const;
    void set_choice(const std::string& name, int value);
    void activate(const Item& item, uint64_t now_ms, int x = -1);
    void changed();
    void show_page(bool controls);
    void set_device(bool controller);
    void refresh_rows();
    void ensure_visible(const Layout&);
    void show_error(const std::string& message) { error_ = message; }
    // General.
    void apply(uint64_t now_ms);
    void play(uint64_t now_ms);
    void cancel();
    void keep();
    void revert();
    bool save_general(const PortableSettings& draft);
    void account_clicked();
    std::string account_portal() const;
    // Controls.
    void begin_capture(size_t row, unsigned slot, uint64_t now_ms);
    void end_capture(const char* why);
    void accept(PhysicalControl control);
    void clear_binding(size_t row, unsigned slot);
    void reset_device();
    void ask(Question question, const std::string& title, const std::string& text, const std::string& confirm);
    void answer(bool yes);
    void fill_controls_hint();

    SettingsForm& form_;
    std::filesystem::path settings_;
    SettingsWindowOptions options_;
    SettingsDisplay* display_;
    std::unique_ptr<SettingsAccount> account_;
    State state_ = State::Open;
    bool saved_anything_ = false;  // in game: Apply saved something, so closing reports Saved
    bool controls_page_ = false, controller_ = false, advanced_ = false, dirty_ = false;
    int scope_ = 0, scroll_ = 0;
    std::string focus_;
    std::string error_, status_, hint_, open_choice_;
    int choice_cursor_ = 0;
    std::vector<SettingsBindingRow> rows_;
    Capture capture_;
    // The display confirmation.
    bool pending_ = false;
    PortableSettings pending_draft_;
    uint64_t deadline_ = 0, now_ = 0;
    // A question waiting for an answer.
    Question question_ = Question::None;
    std::string question_title_, question_text_, question_confirm_;
    Capture asked_capture_;
    PhysicalControl asked_control_{};
    // Account progress from the link's worker thread.
    struct Mailbox {
        std::mutex lock;
        bool finished = false, linked = false;
        std::string progress, message;
        bool fresh = false;
    };
    std::shared_ptr<Mailbox> mailbox_;
    std::string account_status_, account_button_;
};
}  // namespace mag
