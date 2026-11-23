#pragma once
// The settings panel's logic on SDL2 hosts (player_settings_sdl.cpp): one panel state that takes
// SDL events and draws itself with an SDL renderer and the SDK's public-domain 8x8 font. It owns no
// window and no event loop, so tests drive it with synthetic events and a software renderer.
#include "player_settings_form.h"
#include <SDL2/SDL.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mag {
class SettingsPanel {
public:
    static constexpr int width = 760, height = 560;
    enum class State { Open, Saved, Cancelled };

    SettingsPanel(SettingsForm& form, std::filesystem::path settings, bool setup, std::string notice);
    // One event; time in milliseconds (SDL_GetTicks64 in the window). Returns the panel's state.
    State handle(const SDL_Event& event, uint64_t now_ms);
    // Time passing without events: ends a binding capture after 15 seconds.
    State tick(uint64_t now_ms);
    void draw(SDL_Renderer* renderer, SDL_Texture* font) const;
    // The font texture (white glyphs on transparent) the panel draws with.
    static SDL_Texture* make_font(SDL_Renderer* renderer);

    // Controller codes held now, as a mask (SDL buttons 0-14, triggers 32 and 33): a binding
    // capture ignores them until they are released. The window queries its open controllers.
    std::function<uint64_t()> held_controller;

    // For tests and the window: what the panel shows.
    int page() const { return page_; }
    const std::string& status() const { return status_; }
    bool capturing() const { return capture_.on; }
    bool asking() const { return prompt_ != Prompt::None; }
    std::string focused() const;  // a short name of the focused item

private:
    enum class Kind { Tab, Text, Choice, Slot, Button, Slider, Check };
    enum class Prompt { None, Replace, Reset };
    enum Button { Save, Cancel, Reset, Yes, No };
    struct Item {
        Kind kind;
        SDL_Rect rect;
        int id = 0;           // field, choice, button, tab or row index
        unsigned slot = 0;
        std::string text;     // label drawn beside or on it
    };
    struct Capture {
        bool on = false;
        ActionId action = 0;
        unsigned slot = 0;
        SettingsDevice device = SettingsDevice::KeyboardMouse;
        uint32_t scope = settings_scope_on_foot;
        uint64_t deadline = 0, blocked = 0, triggers = 0;
    };

    std::vector<Item> layout() const;
    SettingsDevice device() const { return page_ == 2 ? SettingsDevice::Controller : SettingsDevice::KeyboardMouse; }
    std::string* field(int id);
    int choice(int id) const;
    void set_choice(int id, int value);
    void activate(const Item& item, uint64_t now_ms);
    void begin_capture(const Item& item, uint64_t now_ms);
    void end_capture(const char* why);
    void accept(PhysicalControl control);
    void clear_slot(const Item& item);
    void save();
    void set_page(int page);
    void refresh();
    void ensure_visible();
    State answer(bool yes);

    SettingsForm& form_;
    std::filesystem::path settings_;
    bool setup_;
    std::string status_;
    State state_ = State::Open;
    int page_ = 0, focus_ = 0, scope_ = 0, scroll_ = 0;
    std::vector<SettingsBindingRow> rows_;
    Capture capture_;
    Prompt prompt_ = Prompt::None;
    PhysicalControl pending_{};
    Capture pending_capture_{};
};
}  // namespace mag
