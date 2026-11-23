#pragma once
// The settings window's Send report button (player_settings_text.h, "Reports"). The packaged
// supervisors set MAG_REPORT_REQUEST to <launch log folder>/report-requested; the button writes
// that marker, and the supervisor sends a report (trigger "manual") when the session ends, with
// or without the send_reports setting. Portable C++17, shared by every settings window.
#include <cstdint>
#include <filesystem>
#include <string>

namespace mag {
// Where the marker goes: MAG_REPORT_REQUEST, or empty when this launch has no supervisor that
// sends reports (the button is then disabled).
std::filesystem::path report_request_path();
// Writes {"requested_at":<unix seconds>}\n to `path`, replacing any earlier request, owner-only
// (0600 on POSIX). False with an explanation in `error` when it cannot.
bool write_report_request(const std::filesystem::path &path, int64_t unix_seconds, std::string &error);
// Why Send report is disabled (MAG_SETTINGS_REPORT_UNAVAILABLE without a request path, then
// MAG_SETTINGS_REPORT_NEEDS_LINK when this computer is not linked to an account), or nullptr when
// it can be used.
const char *report_request_blocker(bool linked);
// The same, at report_request_path() with the current time.
bool request_report(std::string &error);
}  // namespace mag
