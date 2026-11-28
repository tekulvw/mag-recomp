#pragma once
// The settings panel on SDL2 hosts (player_settings_sdl.cpp): the macOS MAG Settings panel's pages,
// rows, wording and behaviour (player_settings_text.h), drawn with an SDL renderer and the SDK's
// public-domain 8x8 font. It owns no window and no event loop, so tests drive it with synthetic
// events and a software renderer. Coordinates are the panel's logical points (the macOS panel's
// width); the window scales them by a whole number. A screen shorter than the layout gets a view
// height: the page then scrolls between the segments and the footer, which stay in the window.
#include "player_settings_account.h"
#include "player_settings_form.h"
#include "player_settings_window.h"
#include "player_report_request.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <condition_variable>
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
    // The window's height, in points: the layout's, or less when the view height cuts the page.
    int height() const;
    int natural_height() const;  // the whole layout's, as though the page did not scroll
    // The most the window may be, in points (0: no limit); a taller layout scrolls its page.
    void set_view_height(int points) { view_height_ = std::max(0, points); }
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
    bool debug_page() const { return debug_page_; }
    // Tests: waits (up to `timeout_ms` of wall time) until the account link's worker has finished,
    // then takes its result as tick() would. False on timeout. The worker sleeps through the
    // portal's poll interval on its own thread, so a fixed number of short ticks cannot be relied on.
    bool wait_for_account_link(uint64_t timeout_ms);
    bool controller() const { return controller_; }
    const std::string& error() const { return error_; }
    const std::string& status() const { return status_; }
    const std::string& hint() const { return hint_; }
    std::string account_status() const;
    std::string report_status() const;  // after Send report: requested, or why it failed
    std::string report_comment() const; // the comment box ("note:report"), sent with Send report
    bool pending() const { return pending_; }        // a display change waits for Keep or Revert
    bool asking() const { return question_ != Question::None; }
    std::string question() const { return question_title_; }
    bool capturing() const { return capture_.on; }
    bool choosing() const { return !open_choice_.empty(); }
    int scroll() const { return scroll_; }
    int scroll_limit() const;
    // The page between the segments and the footer: where it shows in the window, how far it is
    // scrolled and can scroll (0 when it fits), and its scrollbar's thumb (empty when it fits).
    SDL_Rect page_rect() const;
    int page_scroll() const;
    int page_scroll_limit() const;
    void set_page_scroll(int points) { page_scroll_ = std::max(0, points); }
    SDL_Rect page_thumb() const;
    bool visible(const std::string& name) const;  // shown, and wholly inside the page (and the list)

private:
    enum class Kind { Segment, Text, Choice, Button, Link, Slot, Clear, Slider, Check, Note };
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
        bool paged = false;  // part of the page, which scrolls and is clipped to its viewport
    };
    // tone: 0 text, 1 secondary, 2 error; `listed` labels belong to the scrolling bindings list.
    struct Label { int x, y; std::string text; int tone; bool bold; int columns; bool listed = false; bool paged = false; };
    struct Layout {
        std::vector<Item> items;
        std::vector<Label> labels;
        std::vector<SDL_Rect> groups;
        std::vector<SDL_Rect> lines;  // separators
        SDL_Rect list{0, 0, 0, 0};   // the bindings list (scrolls)
        int height = 0, natural = 0;  // the window's; the whole layout's
        // The page's viewport, its scroll and limit (0: it fits), and how many `lines` belong to it.
        SDL_Rect page{0, 0, 0, 0};
        int page_scroll = 0, page_limit = 0;
        size_t paged_lines = 0;
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
    void show_page(int page);  // 0 General, 1 Controls, 2 Debug
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
    const char* report_blocker() const;
    void request_report_now();
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
    bool controls_page_ = false, debug_page_ = false, controller_ = false, advanced_ = false, dirty_ = false;
    int scope_ = 0, scroll_ = 0;
    int view_height_ = 0, page_scroll_ = 0;
    int drag_ = -1;                       // dragging the page's thumb: where it was grabbed, from its top
    int pointer_x_ = -1, pointer_y_ = -1;  // the last mouse position: the wheel scrolls what it is over
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
        std::condition_variable signal;  // set when the worker writes progress or finishes
        bool finished = false, linked = false;
        std::string progress, message;
        bool fresh = false;
    };
    std::shared_ptr<Mailbox> mailbox_;
    std::string account_status_, account_button_, report_status_, report_comment_;
};

// Bringing the settings window to the front with the keyboard, as the Windows window does with
// SetForegroundWindow. F10 starts it as another process, and a window manager's focus-stealing
// prevention may otherwise leave it behind the game window (in front of nothing when the game is
// borderless). The window asks SDL to raise it and give it input focus, and waits briefly for the
// focus. X11: SDL's raise is a _NET_ACTIVE_WINDOW request with the window's own user time, which a
// new window does not have yet, so a strict window manager may refuse it; then the window asks
// once more as a pager does (source 2, current time), which window managers honour. Wayland: SDL
// asks the compositor through xdg-activation without an input serial, and only the compositor
// decides; there is no second request to make. Each step is a hook, so tests drive the order.
enum class SettingsForward { Focused, FocusedOnRetry, NotFocused };
struct SettingsForwardHooks {
    std::function<std::string()> driver;      // SDL's video driver name
    std::function<void()> raise;              // SDL_RaiseWindow and SDL_SetWindowInputFocus
    std::function<bool(unsigned ms)> focused; // whether the window has input focus within `ms`
    std::function<bool()> activate_x11;       // the pager-style _NET_ACTIVE_WINDOW; false: not sent
};
SettingsForward settings_bring_forward(const SettingsForwardHooks& hooks, unsigned wait_ms = 250);
// The data of that X11 request: source indication, timestamp, requestor's active window.
struct SettingsX11Activation { long source = 2, timestamp = 0, requestor = 0; };
// Sends it for an SDL window on the X11 driver (player_settings_sdl_x11.cpp). False when the
// window is not an X11 window or libX11 cannot be reached.
bool settings_activate_x11(SDL_Window* window);
}  // namespace mag
