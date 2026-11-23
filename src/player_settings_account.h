#pragma once
// The settings window's Account row on Windows and Linux (macOS has the same row through
// MAGAccountLinking in player_settings_macos.h): which portal applies to the server being edited,
// whether this computer is linked to it, and the link itself on a worker thread. The wording is
// player_settings_text.h's; the flow and storage are player_portal.h's.
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace mag {
struct AccountRow {
    bool enabled = false;  // the button: off when no portal applies
    std::string button;    // "Link Account…" or "Unlink"
    std::string status;    // the line under "Account"
};

class SettingsAccount {
public:
    explicit SettingsAccount(std::filesystem::path settings_file);
    ~SettingsAccount();
    // The portal for this server text and portal_url setting; empty when none applies.
    std::string portal(const std::string& server_text, const std::string& portal_url) const;
    // The account name this computer is linked to at `portal` ("" linked without a name yet);
    // nothing when not linked there.
    std::optional<std::string> linked_name(const std::string& portal) const;
    AccountRow row(const std::string& portal) const;

    // Starts linking on a worker thread. `progress` and `finished` are called on that thread; the
    // window posts them to its own. `open_page` opens the portal's page in the browser.
    void start_link(const std::string& portal, const std::string& client, std::function<void(const std::string&)> open_page,
                    std::function<void(const std::string&)> progress, std::function<void(bool linked, const std::string&)> finished);
    void cancel_link();
    bool linking() const { return linking_; }
    void link_ended() { linking_ = false; }
    // Forgets the link; `message` says what happened (or why it failed).
    bool unlink(std::string& message);

private:
    std::filesystem::path link_file_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    bool linking_ = false;
};
}  // namespace mag
