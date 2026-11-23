// The settings window's Account row (player_settings_account.h).
#include "player_settings_account.h"
#include "player_portal.h"
#include "player_settings_portable.h"
#include "player_settings_text.h"
#include <chrono>
#include <thread>

namespace mag {
namespace {
std::string fill_in(const char* pattern, const std::string& first, const std::string& second = {}) {
    std::string out, args[2] = {first, second};
    int next = 0;
    for (const char* p = pattern; *p; ++p) {
        if (p[0] == '%' && p[1] == 's' && next < 2) { out += args[next++]; ++p; }
        else out += *p;
    }
    return out;
}
}  // namespace

SettingsAccount::SettingsAccount(std::filesystem::path settings_file) : link_file_(portal::link_path(settings_file)) {}
SettingsAccount::~SettingsAccount() { cancel_link(); }

std::string SettingsAccount::portal(const std::string& server_text, const std::string& portal_url) const {
    std::string host = server_text, setting = portal_url, error;
    if (!check_server_address(host, error) || host.empty() || !portal::check_portal_url(setting, error)) return {};
    return portal::portal_for(host, setting);
}

std::optional<std::string> SettingsAccount::linked_name(const std::string& portal) const {
    portal::Link link;
    bool exists = false;
    std::string error;
    if (portal.empty() || !portal::load_link(link_file_, link, exists, error) || !exists || link.portal != portal) return std::nullopt;
    return link.player_name;
}

AccountRow SettingsAccount::row(const std::string& portal) const {
    AccountRow row;
    const auto name = linked_name(portal);
    row.enabled = !portal.empty();
    row.button = name ? MAG_SETTINGS_ACCOUNT_UNLINK : MAG_SETTINGS_ACCOUNT_LINK;
    if (portal.empty()) row.status = MAG_SETTINGS_ACCOUNT_NO_PORTAL;
    else if (!name) row.status = MAG_SETTINGS_ACCOUNT_SIGN_IN + portal;
    else if (name->empty()) row.status = MAG_SETTINGS_ACCOUNT_NO_NAME;
    else row.status = MAG_SETTINGS_ACCOUNT_LINKED + *name;
    return row;
}

void SettingsAccount::start_link(const std::string& address, const std::string& client, std::function<void(const std::string&)> open_page,
                                 std::function<void(const std::string&)> progress,
                                 std::function<void(bool, const std::string&)> finished) {
    cancel_link();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cancel_ = cancel;
    linking_ = true;
    // The worker owns copies and the shared cancel flag only, so the window may go first.
    std::thread([=, file = link_file_] {
        auto transport = portal::make_platform_transport();
        portal::Link link;
        portal::LinkUi ui;
        ui.show = [&](const portal::LinkStart& start) {
            if (cancel->load()) return;
            open_page(start.verification_uri_complete.empty() ? start.verification_uri : start.verification_uri_complete);
            progress(fill_in(MAG_SETTINGS_ACCOUNT_ENTER_CODE, start.user_code, start.verification_uri));
        };
        ui.wait = [&](int seconds) {
            for (int tick = 0; tick < seconds * 4; ++tick) {
                if (cancel->load()) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            return !cancel->load();
        };
        portal::Outcome outcome = transport ? portal::run_link(*transport, address, client, ui, link)
                                            : portal::Outcome{portal::Status::Unreachable, "This build cannot reach the account portal."};
        std::string error;
        if (outcome.ok() && !portal::save_link(file, link, error)) outcome = {portal::Status::Storage, error};
        if (cancel->load()) return;
        finished(outcome.ok(), outcome.ok() || outcome.status == portal::Status::Cancelled ? std::string() : outcome.message);
    }).detach();
}

void SettingsAccount::cancel_link() {
    if (cancel_) cancel_->store(true);
    cancel_.reset();
    linking_ = false;
}

bool SettingsAccount::unlink(std::string& message) {
    portal::Link link;
    bool exists = false;
    std::string error;
    portal::load_link(link_file_, link, exists, error);
    if (!portal::forget_link(link_file_, error)) { message = error; return false; }
    message = link.portal.empty() ? std::string(MAG_SETTINGS_ACCOUNT_NOT_LINKED_HERE) : fill_in(MAG_SETTINGS_ACCOUNT_UNLINKED, link.portal);
    return true;
}
}  // namespace mag
