#pragma once
/* The MAG Settings window's pages, fields, labels and wording, in one place for every host: the
 * macOS AppKit panel (player_settings_macos.m, player_controls_macos.mm), the Win32 window
 * (player_settings_win32.cpp) and the Linux panel when it follows. Plain C so Objective-C, C and
 * C++ all include it; strings are UTF-8 (compile with UTF-8 source) (in Objective-C, @MAG_SETTINGS_X is an NSString).
 * Validation lives with the settings files (player_settings_store.m on macOS,
 * player_settings_file.cpp elsewhere), and both use the messages below.
 *
 * Layout, top to bottom (docs/player-settings.md):
 *   [General | Controls]                       (Controls only when a catalog is registered)
 *   General:  Connection  { Game server, Player name, Account, > Advanced: TCP port, UDP port }
 *             note, Graphics { Display mode, Output resolution, Rendering resolution, Anti-aliasing }, note,
 *             Reports { [x] Send performance and error reports, Send report }
 *   Debug:    Diagnostics { one row per mag_settings_debug_items entry }, note
 *   Controls: [Keyboard & Mouse | Controller], Input mode + Bindings, aim assist, mouse row,
 *             Action / Primary / Secondary rows, hint, Reset, note
 *   error line, display confirmation (15 s), separator, footer: status, Cancel, Apply, Play MAG */

#define MAG_SETTINGS_TITLE "MAG Settings"
#define MAG_SETTINGS_PREVIEW_TITLE "MAG Display Preview"
#define MAG_SETTINGS_PREVIEW_LABEL "Display preview"
#define MAG_SETTINGS_WIDTH 620           /* window content width, points */
#define MAG_SETTINGS_MARGIN 24
#define MAG_SETTINGS_CONTROL_WIDTH 205   /* each row's control */
#define MAG_SETTINGS_ROW_HEIGHT 42

/* Pages. */
#define MAG_SETTINGS_PAGE_GENERAL "General"
#define MAG_SETTINGS_PAGE_CONTROLS "Controls"
#define MAG_SETTINGS_PAGE_DEBUG "Debug"

/* General: Connection. */
#define MAG_SETTINGS_HEADING_CONNECTION "Connection"
#define MAG_SETTINGS_SERVER "Game server"
#define MAG_SETTINGS_SERVER_HINT "Hostname or IPv4 address"
#define MAG_SETTINGS_SERVER_PLACEHOLDER "Use game default"
#define MAG_SETTINGS_NAME "Player name"
#define MAG_SETTINGS_NAME_HINT "Your name on the server, typed in for you at sign-in"
#define MAG_SETTINGS_NAME_PLACEHOLDER "Default"
#define MAG_SETTINGS_ACCOUNT "Account"
#define MAG_SETTINGS_ADVANCED_CLOSED "▸ Advanced connection settings"
#define MAG_SETTINGS_ADVANCED_OPEN "▾ Advanced connection settings"
#define MAG_SETTINGS_TCP_PORT "TCP port"
#define MAG_SETTINGS_UDP_PORT "UDP port"
#define MAG_SETTINGS_PORT_PLACEHOLDER "Default"
#define MAG_SETTINGS_CONNECTION_NOTE \
    "Without a linked account, servers sign in by name. Blank keeps game settings. Changes apply next launch."

/* General: the Account row (src/player_portal.h). */
#define MAG_SETTINGS_ACCOUNT_LINK "Link Account…"
#define MAG_SETTINGS_ACCOUNT_UNLINK "Unlink"
#define MAG_SETTINGS_ACCOUNT_CANCEL "Cancel"
#define MAG_SETTINGS_ACCOUNT_NO_PORTAL "Accounts are for play.magtest.online"
#define MAG_SETTINGS_ACCOUNT_SIGN_IN "Sign in with your account at "  /* + the portal */
#define MAG_SETTINGS_ACCOUNT_NO_NAME "Linked · claim a player name on the account page"
#define MAG_SETTINGS_ACCOUNT_LINKED "Linked as "  /* + the account's name */
#define MAG_SETTINGS_ACCOUNT_CONTACTING "Contacting the account portal…"
#define MAG_SETTINGS_ACCOUNT_ENTER_CODE "Enter %s at %s (opened in your browser)"  /* code, page */
#define MAG_SETTINGS_ACCOUNT_NOT_LINKED_HERE "Not linked on this computer"
#define MAG_SETTINGS_ACCOUNT_UNLINKED "Unlinked here · also remove it at %s/account"  /* portal */

/* General: Graphics. */
#define MAG_SETTINGS_HEADING_GRAPHICS "Graphics"
#define MAG_SETTINGS_DISPLAY_MODE "Display mode"
#define MAG_SETTINGS_OUTPUT "Output resolution"
#define MAG_SETTINGS_OUTPUT_HINT "Fullscreen uses the desktop size"
#define MAG_SETTINGS_RENDERING "Rendering resolution"
#define MAG_SETTINGS_ANTI_ALIASING "Anti-aliasing"
#define MAG_SETTINGS_NEXT_LAUNCH_HINT "Applies on the next launch"
/* Where the renderer has no such option (rendering scale and FXAA are the Metal renderer's): the
 * control stays visible, disabled, with this hint. */
#define MAG_SETTINGS_METAL_ONLY_HINT "macOS (Metal) only for now"
#define MAG_SETTINGS_GRAPHICS_NOTE \
    "Rendering percentages scale the game's 720p-height targets. Output resolution sets the window size."
#define MAG_SETTINGS_GRAPHICS_NOTE_NO_SCALE "Output resolution sets the window size."

/* General: Reports (src/player_report_request.h; docs/player-settings.md, "Reports"). The checkbox
 * is the send_reports setting; Send report asks the supervisor to send one when MAG closes, even
 * with the checkbox off. Reports go under the linked account, so Send report needs one. */
#define MAG_SETTINGS_HEADING_REPORTS "Reports"
#define MAG_SETTINGS_SEND_REPORTS "Send performance and error reports"
#define MAG_SETTINGS_SEND_REPORTS_HINT \
    "Frame-rate and audio-timing statistics, crash and error details and recent MAG logs go to magtest.online under your linked account. Never game files, passwords or tokens."
#define MAG_SETTINGS_SEND_REPORTS_ON "On"  /* the checkbox's own label */
#define MAG_SETTINGS_SEND_REPORT_NOW "Send report"
#define MAG_SETTINGS_SEND_REPORT_HINT "Send one now, for a problem you just saw"
#define MAG_SETTINGS_REPORT_REQUESTED "A report will be sent when MAG closes."
#define MAG_SETTINGS_REPORT_NEEDS_LINK "Link an account to send reports"
#define MAG_SETTINGS_REPORT_UNAVAILABLE "Reports are sent only when MAG starts from its app"
#define MAG_SETTINGS_REPORT_FAILED "The report request could not be saved."

/* Choices, in menu order, with the values the settings file stores. */
#define MAG_SETTINGS_DISPLAY_MODES 2
static const char *const mag_settings_display_mode_labels[MAG_SETTINGS_DISPLAY_MODES] = {"Windowed", "Borderless fullscreen"};
static const char *const mag_settings_display_mode_values[MAG_SETTINGS_DISPLAY_MODES] = {"windowed", "borderless"};
#define MAG_SETTINGS_OUTPUT_SIZES 4
static const char *const mag_settings_output_labels[MAG_SETTINGS_OUTPUT_SIZES] = {
    "1280 × 720", "1920 × 1080", "2560 × 1440", "3840 × 2160"};
static const int mag_settings_output_widths[MAG_SETTINGS_OUTPUT_SIZES] = {1280, 1920, 2560, 3840};
static const int mag_settings_output_heights[MAG_SETTINGS_OUTPUT_SIZES] = {720, 1080, 1440, 2160};
#define MAG_SETTINGS_RENDER_SCALES 4
static const char *const mag_settings_render_scale_labels[MAG_SETTINGS_RENDER_SCALES] = {
    "Native (100%)", "1080p (150%)", "1440p (200%)", "2160p (300%)"};
static const int mag_settings_render_scales[MAG_SETTINGS_RENDER_SCALES] = {100, 150, 200, 300};
#define MAG_SETTINGS_ANTI_ALIASING_MODES 2
static const char *const mag_settings_anti_aliasing_labels[MAG_SETTINGS_ANTI_ALIASING_MODES] = {"Off", "FXAA"};
static const char *const mag_settings_anti_aliasing_values[MAG_SETTINGS_ANTI_ALIASING_MODES] = {"off", "fxaa"};

/* Display confirmation: windowed size or mode changes ask before they are saved. */
#define MAG_SETTINGS_CONFIRM_SECONDS 15
#define MAG_SETTINGS_CONFIRM_QUESTION "Keep these display settings?"
#define MAG_SETTINGS_CONFIRM_COUNTDOWN "Previous display settings return in %ld seconds."
#define MAG_SETTINGS_CONFIRM_REVERT "Revert"
#define MAG_SETTINGS_CONFIRM_KEEP "Keep changes"
#define MAG_SETTINGS_DISPLAY_REVERTED "Display changes reverted"
#define MAG_SETTINGS_OUTPUT_TOO_BIG \
    "This output size does not fit this display in windowed mode. Choose a smaller size or borderless fullscreen."

/* Footer. */
#define MAG_SETTINGS_CANCEL "Cancel"
#define MAG_SETTINGS_APPLY "Apply"
#define MAG_SETTINGS_APPLY_CONTROLS "Apply controls"
#define MAG_SETTINGS_PLAY "Play MAG"
#define MAG_SETTINGS_STATUS_DIRTY "Unsaved changes"
#define MAG_SETTINGS_STATUS_CLEAN "No unsaved changes"
#define MAG_SETTINGS_STATUS_SAVED "Saved · connection, rendering and AA apply next launch"
#define MAG_SETTINGS_STATUS_SAVED_NO_SCALE "Saved · connection and display apply next launch"

/* Messages: validation, recovery and launch. */
#define MAG_SETTINGS_PORT_DIGITS "Ports must contain digits, or be blank for the game default."
#define MAG_SETTINGS_PORT_RANGE "Ports must be between 1 and 65535, or blank."
#define MAG_SETTINGS_NAME_RULE "A player name has 3 to 15 letters, digits, - or _, and starts with a letter."
#define MAG_SETTINGS_NAME_NEEDED "Enter a player name to play on a server. It is the name you sign in with."
#define MAG_SETTINGS_RECOVERY_PLAY "Review these defaults and click Apply to replace the damaged settings file."
#define MAG_SETTINGS_DAMAGED "The settings file is damaged. Review these defaults and Apply to replace it."

/* Debug page: diagnostic switches and actions for bug reports. Each entry is one row (a title, a
 * hint, and a checkbox or a button on the right); a new switch or action is one more entry here
 * and one more case in player_settings_debug.cpp, and every host's window lists it. Switches act
 * at once (they are files the game reads at launch, not settings drafts), so Apply and Cancel do
 * not concern them. */
#define MAG_SETTINGS_HEADING_DEBUG "Diagnostics"
#define MAG_SETTINGS_DEBUG_NOTE "These help with bug reports. Switches take effect at the next launch."
#define MAG_SETTINGS_DEBUG_SWITCH_ON "On · takes effect at the next launch"
#define MAG_SETTINGS_DEBUG_SWITCH_OFF "Off · takes effect at the next launch"
#define MAG_SETTINGS_DEBUG_NO_LOGS "Start MAG from its own launcher to open its logs"
/* The file the controls census looks for next to the settings (docs/native-controls.md, Census). */
#define MAG_CONTROLS_TRACE_FILE "controls-trace"
typedef enum { MAG_SETTINGS_DEBUG_SWITCH, MAG_SETTINGS_DEBUG_ACTION } MagSettingsDebugKind;
typedef struct {
    const char *id;       /* player_settings_debug.cpp's name for it */
    MagSettingsDebugKind kind;
    const char *title, *hint, *control;  /* the row's title and hint; the checkbox's or button's text */
} MagSettingsDebugItem;
#define MAG_SETTINGS_DEBUG_ITEMS 2
static const MagSettingsDebugItem mag_settings_debug_items[MAG_SETTINGS_DEBUG_ITEMS] = {
    {"controls_trace", MAG_SETTINGS_DEBUG_SWITCH, "Controls trace",
     "Records how inputs reach the game, for bug reports. Takes effect at the next launch.", "Record controls trace"},
    {"open_logs", MAG_SETTINGS_DEBUG_ACTION, "Logs", "This installation's launch logs, to attach to a report", "Open logs folder"},
};

/* Controls page (docs/player-controls-settings.md). */
#define MAG_CONTROLS_DEVICE_KBM "Keyboard & Mouse"
#define MAG_CONTROLS_DEVICE_CONTROLLER "Controller"
#define MAG_CONTROLS_INPUT_MODE "Input mode"
#define MAG_CONTROLS_MODES 3
static const char *const mag_controls_mode_labels[MAG_CONTROLS_MODES] = {"Automatic", "Keyboard & Mouse", "Controller"};
#define MAG_CONTROLS_SCOPE "Bindings"
#define MAG_CONTROLS_SCOPES 2
static const char *const mag_controls_scope_labels[MAG_CONTROLS_SCOPES] = {"On foot", "Menus"};
#define MAG_CONTROLS_AIM_ASSIST_KBM "Aim assist: Disabled for keyboard/mouse"
#define MAG_CONTROLS_AIM_ASSIST_CONTROLLER \
    "Aim assist follows input mode. Automatic keeps it disabled once KBM gameplay is detected."
#define MAG_CONTROLS_SENSITIVITY "Mouse sensitivity"
#define MAG_CONTROLS_SENSITIVITY_FORMAT "%.2f×"
#define MAG_CONTROLS_INVERT_Y "Invert Y"
#define MAG_CONTROLS_COLUMN_ACTION "Action"
#define MAG_CONTROLS_COLUMN_PRIMARY "Primary"
#define MAG_CONTROLS_COLUMN_SECONDARY "Secondary"
#define MAG_CONTROLS_UNBOUND "Unbound"
#define MAG_CONTROLS_CLEAR "×"
#define MAG_CONTROLS_CLEAR_TIP "Clear binding"
#define MAG_CONTROLS_TIP_ON_FOOT "Assign this on-foot binding; menu bindings are preserved"
#define MAG_CONTROLS_TIP_MENU "Assign this menu binding; on-foot bindings are preserved"
#define MAG_CONTROLS_HINT "Choose a binding, then press a key or button. Escape cancels capture."
#define MAG_CONTROLS_HINT_OVERRIDE "Startup mode override: %s. Apply controls to use the selected mode."
#define MAG_CONTROLS_HINT_RECOVERY " Use Reset to review defaults before replacing this profile."
#define MAG_CONTROLS_CAPTURE_KEYS "Press a key or mouse button. Escape cancels."
#define MAG_CONTROLS_CAPTURE_KEYS_MACOS "Press a key or mouse button. Escape cancels. Command shortcuts stay with macOS."
#define MAG_CONTROLS_CAPTURE_CONTROLLER "Release held controls, then press a controller button. Escape cancels."
#define MAG_CONTROLS_CAPTURE_CANCELED "Binding capture canceled."
#define MAG_CONTROLS_CAPTURE_TIMEOUT "No binding received. Choose a binding to try again."
#define MAG_CONTROLS_CAPTURE_SECONDS 15
#define MAG_CONTROLS_CHANGED "Binding changed. Apply controls to save."
#define MAG_CONTROLS_SAVED "Controls saved. They take effect at the next input update."
#define MAG_CONTROLS_RESET_KBM "Reset Keyboard & Mouse"
#define MAG_CONTROLS_RESET_CONTROLLER "Reset Controller"
#define MAG_CONTROLS_NOTE \
    "Bindings apply to on-foot gameplay and its HUD actions. Stick movement and look retain native response."
#define MAG_CONTROLS_DEFAULTS_READY "Defaults ready. Apply controls to replace the unreadable profile, or Cancel to preserve it."

/* Controls page: questions (an alert on macOS, a task dialog on Windows). */
#define MAG_CONTROLS_REPLACE_TITLE "Replace existing binding?"
#define MAG_CONTROLS_REPLACE_TEXT "%s is assigned to %s. Replace its conflicting bindings for this action?"  /* control, action */
#define MAG_CONTROLS_REPLACE_OTHER "another action"
#define MAG_CONTROLS_REPLACE "Replace"
#define MAG_CONTROLS_RESET_KBM_TITLE "Reset keyboard and mouse bindings?"
#define MAG_CONTROLS_RESET_CONTROLLER_TITLE "Reset controller bindings?"
#define MAG_CONTROLS_RESET_TEXT "The other device's settings will be preserved. Apply controls to save the reset."
#define MAG_CONTROLS_RESET "Reset"
#define MAG_CONTROLS_RECOVER_TITLE "Replace the unreadable controls profile?"
#define MAG_CONTROLS_RECOVER_TEXT \
    "This restores both keyboard/mouse and controller defaults. The existing file stays untouched until you choose Apply controls."
#define MAG_CONTROLS_RECOVER "Review defaults"
