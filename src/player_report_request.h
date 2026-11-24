#pragma once
// The settings window's Send report button (player_settings_text.h, "Reports"). The packaged
// supervisors set MAG_REPORT_REQUEST to <launch log folder>/report-requested; the button writes
// that marker, and the supervisor sends a report (trigger "manual") when the session ends, with
// or without the send_reports setting. Portable C++17, shared by every settings window.
#include <cstdint>
#include <filesystem>
#include <string>
#include "player_settings_text.h"

namespace mag {
// Where the marker goes: MAG_REPORT_REQUEST, or empty when this launch has no supervisor that
// sends reports (the button is then disabled).
std::filesystem::path report_request_path();
// Writes {"requested_at":<unix seconds>,"report_id":"<id>","comment":"<text>"}\n to `path`,
// replacing any earlier request, owner-only (0600 on POSIX). The supervisor sends the report under
// that id, so the player can quote it, with the comment (clipped by clip_report_comment; it may be
// empty) as session.user_comment. False with an explanation in `error` when it cannot.
bool write_report_request(const std::filesystem::path &path, int64_t unix_seconds, const std::string &report_id,
                          const std::string &comment, std::string &error);
// The comment as a report carries it: well-formed UTF-8 (anything else is dropped), no control
// characters but newlines and tabs (CR LF and CR become LF), at most `limit` code points, cut only
// between code points.
std::string clip_report_comment(const std::string &utf8, size_t limit = MAG_SETTINGS_REPORT_COMMENT_LIMIT);
// Code points in well-formed UTF-8 (what the counter shows).
size_t report_comment_length(const std::string &utf8);
// The counter's text, MAG_SETTINGS_REPORT_COMMENT_COUNTER: "12/1000".
std::string report_comment_counter(const std::string &utf8);
// A new report id: a random, lowercase UUID version 4.
std::string new_report_request_id();
// The status line after a request: MAG_SETTINGS_REPORT_REQUESTED with the id's first 8 characters.
std::string report_requested_status(const std::string &report_id);
// Why Send report is disabled (MAG_SETTINGS_REPORT_UNAVAILABLE without a request path, then
// MAG_SETTINGS_REPORT_NEEDS_LINK when this computer is not linked to an account), or nullptr when
// it can be used.
const char *report_request_blocker(bool linked);
// The same, at report_request_path() with the current time and a new id, which `report_id` receives.
bool request_report(std::string &report_id, const std::string &comment, std::string &error);
}  // namespace mag
