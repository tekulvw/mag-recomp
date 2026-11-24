"""Package private local native outputs into a self-contained Windows MAG folder.

Layout: <MAG folder>\\MAG\\MAG.exe (supervisor), <MAG folder>\\MAG\\bin\\{MagRecomp,MagEngine}.exe,
receipts and runtime metadata, and the game files in <MAG folder>\\game. Nothing refers to the
build workspace, so the MAG folder can be moved as a whole and the build folder deleted. The
executables use the static CRT, so no redistributable DLLs are copied. Nothing is launched here.
"""
from __future__ import annotations
import json
import os
from pathlib import Path
import shutil
import subprocess

from .model import InstallerError
from .safety import Cancellation, contained, file_digest
from .native_package import playback_environment


# Mirrors the macOS supervisor and the src/retail_handoff.cpp contract: verify the
# exact local images, run the launcher; on exit 90 verify its exclusive handoff
# request and run the engine. Paths are resolved from MAG.exe's own location.
_SUPERVISOR = r'''
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <bcrypt.h>
/* Older MinGW-w64 headers lack the pseudo-handle; the value is the Windows SDK's. */
#ifndef BCRYPT_SHA256_ALG_HANDLE
#define BCRYPT_SHA256_ALG_HANDLE ((BCRYPT_ALG_HANDLE)0x00000041)
#endif
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "supervisor_config.h"
#include "player_portal_win32.h"
#include "player_reports_win32.h"
#ifndef MAG_RUNTIME_DEFAULTS  /* a config written before packaged defaults existed: none */
#define MAG_RUNTIME_DEFAULTS {{NULL, NULL}}
#endif
#ifndef MAG_VERSION
#define MAG_VERSION "unknown"
#endif

static wchar_t g_root[MAX_PATH] = L".";

static void fail(const wchar_t *message, const wchar_t *detail) {
    wchar_t text[4096];
    swprintf(text, 4096, L"MAG could not start: %ls%ls%ls\n", message, detail ? L": " : L"", detail ? detail : L"");
    /* The session's report (player_reports_win32.h) is decided before the message waits for the player. */
    mag_reports_end(text);
    wchar_t path[MAX_PATH + 32];
    swprintf(path, MAX_PATH + 32, L"%ls\\launch-error.log", g_root);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        char utf8[8192]; int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof utf8, NULL, NULL);
        DWORD written; if (n > 1) WriteFile(file, utf8, (DWORD)(n - 1), &written, NULL); CloseHandle(file);
    }
    MessageBoxW(NULL, text, L"MAG", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

static void join(wchar_t *out, const wchar_t *base, const wchar_t *relative) {
    wchar_t combined[MAX_PATH * 2];
    swprintf(combined, MAX_PATH * 2, L"%ls\\%ls", base, relative);
    if (!GetFullPathNameW(combined, MAX_PATH, out, NULL)) fail(L"Path is too long", combined);
}

static void hex(const unsigned char *digest, size_t length, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) { out[i * 2] = digits[digest[i] >> 4]; out[i * 2 + 1] = digits[digest[i] & 15]; }
    out[length * 2] = 0;
}

static unsigned char *read_file(const wchar_t *path, DWORD *size_out) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) fail(L"Cannot open local file", path);
    BY_HANDLE_FILE_INFORMATION info; LARGE_INTEGER size;
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 0x7fffffff)
        { CloseHandle(file); fail(L"Invalid local file", path); }
    unsigned char *data = (unsigned char *)malloc((size_t)size.QuadPart + 1);
    DWORD total = 0, count = 0;
    if (!data) fail(L"Out of memory reading", path);
    while (total < (DWORD)size.QuadPart) {
        if (!ReadFile(file, data + total, (DWORD)size.QuadPart - total, &count, NULL) || !count) { CloseHandle(file); fail(L"Cannot read local file", path); }
        total += count;
    }
    CloseHandle(file);
    *size_out = total;
    return data;
}

static void sha256(const unsigned char *data, DWORD length, char *out) {
    unsigned char digest[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)data, length, digest, 32) < 0) fail(L"SHA-256 is unavailable", NULL);
    hex(digest, 32, out);
}

static void check(const wchar_t *path, const char *expected) {
    DWORD size; unsigned char *data = read_file(path, &size); char actual[65];
    sha256(data, size, actual); free(data);
    if (strcmp(actual, expected)) fail(L"Local file identity changed", path);
}

static unsigned long long be(const unsigned char *p, int n) { unsigned long long x = 0; while (n--) x = (x << 8) | *p++; return x; }

static void key_hash(const wchar_t *path, char *out) {
    DWORD length; unsigned char *b = read_file(path, &length);
    if (length < 64 || memcmp(b, "\177ELF\2\2\1", 7)) fail(L"Invalid launcher ELF", path);
    unsigned long long offset = be(b + 32, 8), size = be(b + 54, 2), count = be(b + 56, 2);
    if (size != 56 || offset > length || count > (length - offset) / size) fail(L"Invalid ELF program headers", path);
    for (unsigned long long i = 0; i < count; i++) {
        const unsigned char *s = b + offset + i * size;
        unsigned long long file = be(s + 8, 8), va = be(s + 16, 8), len = be(s + 32, 8);
        if (be(s, 4) == 1 && va <= 0x32A660 && len >= 16 && 0x32A670 - va <= len) {
            unsigned long long at = file + 0x32A660 - va;
            if (at > length || length - at < 16) fail(L"Invalid key segment", path);
            sha256(b + at, 16, out); free(b); return;
        }
    }
    fail(L"Launcher key segment unavailable", path);
}

static void set_env(const wchar_t *name, const wchar_t *value) { if (!SetEnvironmentVariableW(name, value)) fail(L"Cannot set environment", name); }

/* The macOS launcher starts the game from an empty environment. Windows needs the
 * system's, so drop inherited runtime overrides and diagnostics instead: only the
 * variables set below reach the launcher and engine. */
static void clear_inherited(void) {
    static const wchar_t *prefixes[] = {L"MAG_", L"PS3_", L"PS3RECOMP_", L"RSX_", L"SPU_"};
    static wchar_t names[1024][256];
    int count = 0;
    LPWCH block = GetEnvironmentStringsW();
    if (!block) fail(L"Cannot read the environment", NULL);
    for (LPWCH entry = block; *entry; entry += wcslen(entry) + 1) {
        const wchar_t *equals = wcschr(entry + 1, L'=');
        if (*entry == L'=' || !equals || equals - entry >= 256) continue;
        for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
            if (!_wcsnicmp(entry, prefixes[i], wcslen(prefixes[i]))) {
                if (count == 1024) { FreeEnvironmentStringsW(block); fail(L"Too many inherited MAG variables", NULL); }
                wmemcpy(names[count], entry, (size_t)(equals - entry)); names[count][equals - entry] = 0; count++;
                break;
            }
        }
    }
    FreeEnvironmentStringsW(block);
    for (int i = 0; i < count; i++) SetEnvironmentVariableW(names[i], NULL);
}
static void set_env_utf8(const wchar_t *name, const char *value) {
    wchar_t wide[256]; MultiByteToWideChar(CP_UTF8, 0, value, -1, wide, 256); set_env(name, wide);
}

static DWORD run(HANDLE job, const wchar_t *binary, const wchar_t *elf, const wchar_t *cwd, const wchar_t *log) {
    SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
    HANDLE output = CreateFileW(log, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) fail(L"Cannot create launch log", log);
    STARTUPINFOW startup = {sizeof startup};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = NULL; startup.hStdOutput = output; startup.hStdError = output;
    wchar_t command[MAX_PATH * 3];
    swprintf(command, MAX_PATH * 3, L"\"%ls\" \"%ls\"", binary, elf);
    PROCESS_INFORMATION process;
    if (!CreateProcessW(binary, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, cwd, &startup, &process))
        { CloseHandle(output); fail(L"Cannot start", binary); }
    AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread); CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess); CloseHandle(output);
    return code;
}

/* The settings window (MagEngine --mag-player-settings-setup, src/player_settings_session.h): the
 * same window F10 opens in game, with Play MAG, before the game starts. 0: Play MAG; 3: cancelled;
 * anything else: it could not open (no desktop, say), and the launch goes on with the saved settings. */
static DWORD run_settings(HANDLE job, const wchar_t *engine, const wchar_t *logs, const wchar_t *notice) {
    wchar_t log[MAX_PATH]; join(log, logs, L"settings.log");
    SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
    HANDLE output = CreateFileW(log, FILE_APPEND_DATA, FILE_SHARE_READ, &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOW startup = {sizeof startup};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = NULL; startup.hStdOutput = output; startup.hStdError = output;
    wchar_t command[MAX_PATH * 2];
    swprintf(command, MAX_PATH * 2, L"\"%ls\" --mag-player-settings-setup%ls%ls", engine, notice ? L" " : L"", notice ? notice : L"");
    PROCESS_INFORMATION process;
    AllowSetForegroundWindow(ASFW_ANY);
    if (!CreateProcessW(engine, command, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, logs, &startup, &process)) {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        return 1;
    }
    AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread); CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    return code;
}

/* Account messages: kept in the launch's supervisor.log, and shown unless --no-prompt. */
static void notice(const wchar_t *logs, const wchar_t *message, int prompt, UINT icon) {
    wchar_t path[MAX_PATH]; join(path, logs, L"supervisor.log");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        char utf8[4096]; int n = WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8, sizeof utf8 - 1, NULL, NULL);
        DWORD written; if (n > 1) { utf8[n - 1] = '\n'; WriteFile(file, utf8, (DWORD)n, &written, NULL); } CloseHandle(file);
    }
    if (prompt) MessageBoxW(NULL, message, L"MAG account", MB_OK | icon);
}
/* Before each phase: a fresh ticket from the linked account (src/player_portal_win32.cpp), or the
 * name sign-in when there is none. The launcher phase also takes the account's player name. */
static void portal_phase(const wchar_t *settings, const wchar_t *server, const wchar_t *ticket, int launcher, const wchar_t *logs, int prompt) {
    wchar_t name[32], message[1024];
    int state = mag_portal_phase(settings, server, ticket, launcher, name, 32, message, 1024);
    SetEnvironmentVariableW(L"MAG_PLAYER_PORTAL_TICKET", NULL);
    SetEnvironmentVariableW(L"PS3RECOMP_NP_TICKET_REFILL", NULL);
    if (state == MAG_PORTAL_READY) {
        set_env(L"MAG_PLAYER_PORTAL_TICKET", ticket);
        /* Each sign-in takes a fresh ticket; the portal code keeps the file filled. */
        set_env(L"PS3RECOMP_NP_TICKET_REFILL", L"1");
        if (launcher && name[0]) set_env(L"MAG_PLAYER_SET_NAME", name);
    }
    if (state == MAG_PORTAL_STOP) fail(message, NULL);
    if (message[0]) notice(logs, message, prompt, state == MAG_PORTAL_READY ? MB_ICONINFORMATION : MB_ICONWARNING);
}

/* A command's result for automation: on standard output (standard error when it failed) when the
 * caller redirected it, otherwise in a message box unless --no-prompt. */
static void report(const wchar_t *message, int ok, int prompt) {
    HANDLE out = GetStdHandle(ok ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    char utf8[4096]; int n = WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8, sizeof utf8 - 1, NULL, NULL);
    DWORD written;
    if (out && out != INVALID_HANDLE_VALUE && n > 0) { utf8[n - 1] = '\n'; WriteFile(out, utf8, (DWORD)n, &written, NULL); }
    else if (prompt) MessageBoxW(NULL, message, L"MAG account", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show) {
    (void)instance; (void)previous; (void)arguments; (void)show;
    /* First-run choices; the game validates and saves them in the player's settings file. */
    static const wchar_t *usage = L"Usage: MAG.exe [--server <hostname or IPv4>] [--name <player name>] [--no-prompt] | MAG.exe --link | MAG.exe --unlink"
                                  L" | MAG.exe [--server <host>] [--portal-url <url>] --link-token < token";
    /* MAG_SKIP_SETUP=1 from the caller: no setup window, as with --no-prompt (read before the
     * inherited MAG_ variables are dropped). */
    wchar_t skip[8] = L""; GetEnvironmentVariableW(L"MAG_SKIP_SETUP", skip, 8);
    /* MAG_REPORTS=0 from the caller: no performance and error reports (test automation). */
    wchar_t reports[8] = L""; GetEnvironmentVariableW(L"MAG_REPORTS", reports, 8);
    const wchar_t *send_logs = NULL;  /* --send-reports <launch log folder>: the detached report sender */
    const wchar_t *server = NULL, *player = NULL, *portal_url = NULL;
    int account = 0;  /* --link 1, --unlink 2, --link-token 3: manage the account link, then exit */
    int prompt = 1;  /* --no-prompt: never ask (automation): no setup window, and a missing name stops with its message */
    int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) fail(L"Cannot read the command line", NULL);
    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i], **target = NULL; size_t prefix = 0;
        if (!wcscmp(a, L"--no-prompt")) { prompt = 0; continue; }
        if (!wcscmp(a, L"--link")) { account = 1; continue; }
        if (!wcscmp(a, L"--unlink")) { account = 2; continue; }
        if (!wcscmp(a, L"--link-token")) { account = 3; continue; }
        if (!wcscmp(a, L"--send-reports") && i + 1 < argc) { send_logs = argv[++i]; continue; }
        if (!wcsncmp(a, L"--server", 8)) { target = &server; prefix = 8; }
        else if (!wcsncmp(a, L"--name", 6)) { target = &player; prefix = 6; }
        else if (!wcsncmp(a, L"--portal-url", 12)) { target = &portal_url; prefix = 12; }
        if (!target) fail(usage, a);
        if (a[prefix] == L'=') *target = a + prefix + 1;
        else if (!a[prefix] && i + 1 < argc) *target = argv[++i];
        else fail(usage, a);
    }
    /* --portal-url only names the portal a token is for; the device token itself is read from
       standard input, never the command line, which other processes can read. */
    if ((portal_url && account != 3) || (account == 3 && player)) fail(usage, NULL);
    if (account) {
        /* The link lives next to the settings file, outside the installation. */
        wchar_t appdata[MAX_PATH], config[MAX_PATH], settings[MAX_PATH], message[1024];
        DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        if (!length || length >= MAX_PATH) fail(L"Cannot find your application data folder (APPDATA)", NULL);
        join(config, appdata, L"MAG");
        if (!CreateDirectoryW(config, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) fail(L"Cannot create the settings folder", config);
        join(settings, config, L"settings.json");
        if (account == 1) return mag_portal_link_dialog(settings, server) ? 0 : 1;
        if (account == 3) {
            int ok = mag_portal_link_token(settings, server, portal_url, message, 1024);
            report(message, ok, prompt);
            return ok ? 0 : 1;
        }
        int ok = mag_portal_unlink(settings, message, 1024);
        MessageBoxW(NULL, message, L"MAG account", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        return ok ? 0 : 1;
    }
    wchar_t self[MAX_PATH], app[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH) fail(L"Cannot locate MAG.exe", NULL);
    lstrcpyW(app, self); *wcsrchr(app, L'\\') = 0;
    join(g_root, app, L"..");
    if (send_logs) {
        wchar_t appdata[MAX_PATH], config[MAX_PATH], settings[MAX_PATH];
        DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        if (!length || length >= MAX_PATH) return 1;
        join(config, appdata, L"MAG"); join(settings, config, L"settings.json");
        return mag_reports_send(g_root, send_logs, settings);
    }
    wchar_t vfs[MAX_PATH], launcher[MAX_PATH], engine[MAX_PATH];
    wchar_t launcher_elf[MAX_PATH], engine_elf[MAX_PATH], engine_self[MAX_PATH];
    /* The game files are in the MAG folder, beside the folder that holds MAG.exe. */
    join(vfs, app, MAG_GAME_ROOT);
    { wchar_t probe[MAX_PATH]; join(probe, vfs, L"PS3_GAME"); DWORD kind = GetFileAttributesW(probe);
      if (kind == INVALID_FILE_ATTRIBUTES || !(kind & FILE_ATTRIBUTE_DIRECTORY))
          fail(L"The game files are missing. Keep the game folder beside the folder that holds MAG.exe", probe); }
    join(launcher, app, L"bin\\MagRecomp.exe");
    join(engine, app, L"bin\\MagEngine.exe");
    join(launcher_elf, vfs, L"PS3_GAME\\USRDIR\\EBOOT.elf");
    join(engine_elf, vfs, L"PS3_GAME\\USRDIR\\bin\\mage_g.elf");
    join(engine_self, vfs, L"PS3_GAME\\USRDIR\\bin\\mage_g.self");
    check(launcher, MAG_LAUNCHER_SHA256); check(engine, MAG_ENGINE_SHA256);
    check(launcher_elf, MAG_LAUNCHER_ELF_SHA256); check(engine_elf, MAG_ENGINE_ELF_SHA256);
    check(engine_self, MAG_ENGINE_SELF_SHA256);
    char key[65]; key_hash(launcher_elf, key);

    unsigned char random[16]; char token[33];
    if (BCryptGenRandom(NULL, random, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) fail(L"No system random source", NULL);
    hex(random, 16, token);
    wchar_t wtoken[33]; MultiByteToWideChar(CP_UTF8, 0, token, -1, wtoken, 33);
    wchar_t logs_root[MAX_PATH], logs[MAX_PATH], name[64], request[MAX_PATH], home[MAX_PATH], log[MAX_PATH];
    join(logs_root, g_root, L"logs");
    CreateDirectoryW(logs_root, NULL);
    swprintf(name, 64, L"launch-%ls", wtoken);
    join(logs, logs_root, name);
    if (!CreateDirectoryW(logs, NULL)) fail(L"Cannot create launch log directory", logs);
    join(request, logs, L"handoff-request.json");
    join(home, g_root, L"home");
    CreateDirectoryW(home, NULL);
    /* Saves and trophies live in the installation, not in this launch's log folder (the
     * runtime's default is the working directory), so progress survives every launch. */
    wchar_t gamedata[MAX_PATH];
    join(gamedata, home, L"gamedata");
    if (!CreateDirectoryW(gamedata, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) fail(L"Cannot create the save data folder", gamedata);
    wchar_t player_root[MAX_PATH], appdata[MAX_PATH], config[MAX_PATH], settings[MAX_PATH];
    join(player_root, home, L"player");
    CreateDirectoryW(player_root, NULL);
    { wchar_t stale[MAX_PATH]; join(stale, player_root, L"launch-message.txt"); DeleteFileW(stale);
      join(stale, player_root, L"needs-player-name"); DeleteFileW(stale); }
    DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (!length || length >= MAX_PATH) fail(L"Cannot find your application data folder (APPDATA)", NULL);
    join(config, appdata, L"MAG");
    if (!CreateDirectoryW(config, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) fail(L"Cannot create the settings folder", config);
    join(settings, config, L"settings.json");

    clear_inherited();
    set_env(L"HOME", home);
    set_env(L"PS3_GAMEDATA_ROOT", gamedata);
    set_env(L"PS3_VFS_ROOT", vfs);
    set_env(L"PS3_BOOT_PATH", L"/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN");
    set_env(L"PS3_HLE_UNRESOLVED", L"halt");
    set_env(L"PS3_SYSCALL_UNRESOLVED", L"halt");
    /* Required runtime defaults recorded at packaging (movie playback for decoder builds). */
    static const wchar_t *defaults[][2] = MAG_RUNTIME_DEFAULTS;
    for (size_t i = 0; defaults[i][0]; i++) set_env(defaults[i][0], defaults[i][1]);
    set_env(L"MAG_HANDOFF_REQUEST", request);
    set_env(L"MAG_HANDOFF_TOKEN", wtoken);
    set_env(L"MAG_ENGINE_BINARY", engine);
    set_env(L"MAG_ENGINE_ELF", engine_elf);
    set_env(L"MAG_ENGINE_SELF", engine_self);
    set_env_utf8(L"MAG_ENGINE_BINARY_SHA256", MAG_ENGINE_SHA256);
    set_env_utf8(L"MAG_ENGINE_ELF_SHA256", MAG_ENGINE_ELF_SHA256);
    set_env_utf8(L"MAG_ENGINE_SELF_SHA256", MAG_ENGINE_SELF_SHA256);
    set_env_utf8(L"MAG_ENGINE_KEY_SHA256", key);
    set_env(L"MAG_PLAYER_SETTINGS_PATH", settings);
    set_env(L"MAG_PLAYER_OVERLAY_ROOT", player_root);
    set_env(L"MAG_PLAYER_LOGS_ROOT", logs_root);  /* MAG Settings' Debug page: Open logs folder */
    set_env(L"MAG_PLAYER_SETUP", L"1");
    /* The session summary (src/mag_session_monitor.h) and the settings window's Send report
     * request go to this launch's log folder; the report is decided when the session ends. */
    set_env(L"MAG_SESSION_DIR", logs);
    { wchar_t request_marker[MAX_PATH], receipts[MAX_PATH];
      join(request_marker, logs, L"report-requested"); set_env(L"MAG_REPORT_REQUEST", request_marker);
      join(receipts, app, L"resources\\package-receipts");
      mag_reports_begin(self, g_root, logs, settings, receipts, MAG_VERSION, reports[0] && !wcscmp(reports, L"0")); }
    /* This supervisor opens setup itself, below; the launcher never does. */
    set_env(L"MAG_SKIP_SETUP", L"1");
    if (server) set_env(L"MAG_PLAYER_SET_SERVER", server);
    if (player) set_env(L"MAG_PLAYER_SET_NAME", player);

    /* Closing the supervisor (or its crash) also ends the game processes. */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) fail(L"Cannot create the process job", NULL);

    /* Setup at every launch: the settings window, filled with the saved settings, so Play MAG is
     * one click. Command-line choices, --no-prompt and MAG_SKIP_SETUP go straight in. */
    if (prompt && !server && !player && !(skip[0] && wcscmp(skip, L"0"))) {
        DWORD chosen = run_settings(job, engine, logs, NULL);
        if (chosen == 3) { mag_reports_end(NULL); return 0; }  /* Cancel: the player chose not to play */
        if (chosen) notice(logs, L"The settings window could not open; starting with the saved settings.", 0, MB_ICONWARNING);
    }
    wchar_t ticket[MAX_PATH]; join(ticket, player_root, L"portal-ticket.bin");
    wchar_t sign_in[1024];
    mag_portal_take_notice(ticket, sign_in, 1024);  /* an earlier launch's note is done */
    portal_phase(settings, server, ticket, 1, logs, prompt);
    join(log, logs, L"launcher.log");
    DWORD result = run(job, launcher, launcher_elf, logs, log);
    mag_reports_launcher_exit(result);
    /* A server needs a player name. When the game reports none, the settings window asks for it
     * (or for the account link, which supplies it), then the launcher starts again. */
    wchar_t marker[MAX_PATH]; join(marker, player_root, L"needs-player-name");
    for (int attempt = 2; result != 90 && prompt && attempt <= 4 && GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES; attempt++) {
        wchar_t again[64];
        DWORD answer = run_settings(job, engine, logs, L"--notice=player-name");
        if (answer == 3) { mag_reports_end(NULL); return 0; }  /* Cancel: the player chose not to play */
        if (answer) break;  /* no window: the launch message below explains */
        DeleteFileW(marker);
        { wchar_t note[MAX_PATH]; join(note, player_root, L"launch-message.txt"); DeleteFileW(note); }
        portal_phase(settings, server, ticket, 1, logs, prompt);  /* linked meanwhile: the account's ticket and name */
        swprintf(again, 64, L"launcher-%d.log", attempt);
        join(log, logs, again);
        result = run(job, launcher, launcher_elf, logs, log);
        mag_reports_launcher_exit(result);
    }
    DeleteFileW(marker);
    if (result != 90) {
        /* A sign-in refused or expired during the session: say so once the game is gone. */
        if (mag_portal_take_notice(ticket, sign_in, 1024)) notice(logs, sign_in, prompt, MB_ICONWARNING);
        /* A launch the player must correct (such as choosing a name) explains itself. */
        wchar_t note[MAX_PATH]; join(note, player_root, L"launch-message.txt");
        if (GetFileAttributesW(note) != INVALID_FILE_ATTRIBUTES) {
            DWORD size; unsigned char *text = read_file(note, &size); wchar_t message[2048];
            int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)text, (int)(size < 4000 ? size : 4000), message, 2047);
            message[n > 0 ? n : 0] = 0; free(text); DeleteFileW(note); mag_reports_player_stop(); fail(message, NULL);
        }
        if (result) fail(L"The game stopped before it started. Details are in", log);
        mag_reports_end(NULL);
        return 0;
    }

    char expected[256];
    snprintf(expected, sizeof expected, "{\"version\":1,\"token\":\"%s\",\"guest_path\":\"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self\",\"kind\":\"mage_g\"}\n", token);
    DWORD size; unsigned char *body = read_file(request, &size);
    if (size != strlen(expected) || memcmp(body, expected, size)) fail(L"Engine handoff request identity mismatch", request);
    free(body);
    check(engine, MAG_ENGINE_SHA256); check(engine_elf, MAG_ENGINE_ELF_SHA256); check(engine_self, MAG_ENGINE_SELF_SHA256);
    const wchar_t *cleared[] = {L"MAG_HANDOFF_REQUEST", L"MAG_HANDOFF_TOKEN", L"MAG_ENGINE_BINARY", L"MAG_ENGINE_ELF",
        L"MAG_ENGINE_SELF", L"MAG_ENGINE_BINARY_SHA256", L"MAG_ENGINE_ELF_SHA256", L"MAG_ENGINE_SELF_SHA256", L"MAG_ENGINE_KEY_SHA256",
        L"MAG_PLAYER_SETUP", L"MAG_PLAYER_SET_SERVER", L"MAG_PLAYER_SET_NAME", L"MAG_SKIP_SETUP"};
    for (size_t i = 0; i < sizeof cleared / sizeof cleared[0]; i++) SetEnvironmentVariableW(cleared[i], NULL);
    set_env(L"PS3_BOOT_PATH", L"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self");
    /* The engine signs in again (MAS, statistics) with its own fresh ticket. */
    portal_phase(settings, NULL, ticket, 0, logs, prompt);
    join(log, logs, L"engine.log");
    DWORD code = run(job, engine, engine_elf, logs, log);
    mag_reports_engine_exit(code);
    mag_reports_end(NULL);
    if (mag_portal_take_notice(ticket, sign_in, 1024)) notice(logs, sign_in, prompt, MB_ICONWARNING);
    return (int)code;
}
'''


def _c_wide(value):
    # Installation paths are ASCII on Windows (see native_build preflight).
    if any(ord(c) < 32 or ord(c) > 126 or c == '"' for c in value):
        raise InstallerError('Unsupported character in package metadata')
    return 'L"' + value.replace('\\', '\\\\') + '"'


def _c_hex(value):
    if not isinstance(value, str) or len(value) != 64 or any(c not in '0123456789abcdef' for c in value):
        raise InstallerError('Package metadata requires lowercase SHA-256 identities')
    return '"' + value + '"'


# Portable sources the supervisor compiles with it: the account link (HTTPS through WinHTTP) and
# the settings rules, from the workspace's own source.
SUPERVISOR_SOURCES = ('player_portal.h', 'player_portal.cpp', 'player_portal_winhttp.cpp', 'player_portal_win32.h',
                      'player_portal_win32.cpp', 'player_settings_portable.h', 'player_settings_file.cpp', 'player_settings_text.h', 'player_sign_in.h',
                      'player_reports.h', 'player_reports.cpp', 'player_reports_win32.h', 'player_reports_win32.cpp',
                      'player_gzip.h', 'player_gzip.cpp', 'player_sign_in_refusal.h')


def copy_supervisor_sources(source, scratch):
    """Copy SUPERVISOR_SOURCES from a source tree's src folder into the supervisor's build folder."""
    from .native_package import _ordinary
    for name in SUPERVISOR_SOURCES:
        shutil.copyfile(_ordinary(contained(source, 'src/' + name)), Path(scratch) / name)


def _compile(clang_cl, environment, source_dir, output, token):
    def tool(argv):
        token.check()
        try:
            result = subprocess.run(argv, cwd=source_dir, env=environment, stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, timeout=300)
        except (OSError, subprocess.SubprocessError) as exc:
            raise InstallerError(f'Cannot compile the MAG supervisor: {exc}') from exc
        if result.returncode:
            raise InstallerError('MAG supervisor compilation failed: ' + (result.stdout + result.stderr)[-3000:])
    # The C++ sources first (C++20, exceptions), then the C supervisor linked with them.
    portable = [name for name in SUPERVISOR_SOURCES if name.endswith('.cpp')]
    tool([clang_cl, '/nologo', '/O2', '/MT', '/W3', '/EHsc', '/std:c++20', '/DUNICODE', '/D_UNICODE', '/c',
          *[str(source_dir / name) for name in portable], f'/Fo{source_dir}\\'])
    # clang-cl refuses a named /Fo when the command line holds more than one input, so the C
    # supervisor compiles on its own and a separate step links every object.
    tool([clang_cl, '/nologo', '/O2', '/MT', '/W3', '/DUNICODE', '/D_UNICODE', '/c', str(source_dir / 'supervisor.c'),
          f'/Fo{source_dir / "supervisor.obj"}'])
    tool([clang_cl, '/nologo', '/MT', str(source_dir / 'supervisor.obj'),
          *[str(source_dir / name.replace('.cpp', '.obj')) for name in portable],
          f'/Fe{output}', '/link', '/SUBSYSTEM:WINDOWS',
          'bcrypt.lib', 'user32.lib', 'shell32.lib', 'gdi32.lib', 'winhttp.lib', 'advapi32.lib'])
    if not Path(output).is_file():
        raise InstallerError('MAG supervisor compilation produced no MAG.exe')


def package(config, workspace, destination, token=None, events=None, environment=None):
    """Build the MAG folder's MAG and game subfolders from a finished workspace; launches nothing."""
    from .native_package import _ordinary, _digest, check_workspace, stage_game, package_receipt, product_version, GAME_FOLDER, WORKSPACE_GAME
    token = token or Cancellation(); events = events or (lambda event: None)
    workspace, destination = Path(workspace).absolute(), Path(destination).absolute()
    if destination.exists() or destination.is_symlink(): raise InstallerError('MAG folder destination already exists')
    if destination.name != 'MAG' or not destination.parent.is_dir(): raise InstallerError('Choose a new MAG folder under the installation directory')
    workspace, folder = check_workspace(workspace, destination.parent)
    manifest = json.loads(contained(workspace, 'profiles/retail-v1.0.json').read_text(encoding='utf-8'))
    framework = json.loads(contained(workspace, 'profiles/framework.json').read_text(encoding='utf-8'))
    records, originals = {}, {}
    for role, folder_name, image in [('launcher', 'build', 'EBOOT'), ('engine', 'build-engine', 'mage_g')]:
        binary = _ordinary(contained(workspace, folder_name + '/MagRecomp.exe'))
        receipt = _ordinary(binary.with_suffix('.receipt.json'))
        record = json.loads(receipt.read_text(encoding='utf-8')); expected = manifest['images'][image]
        if (record.get('binary_sha256') != _digest(binary, token) or record.get('elf_sha256') != expected['elf_sha256'] or record.get('image_name') != image):
            raise InstallerError(f'Native source receipt mismatch: {receipt}')
        if record.get('sdk_revision') not in set(framework['strict_runtime_revisions']) & set(framework['strict_hle_runtime_revisions']):
            raise InstallerError('Packaged runtime must support strict syscall/HLE policies')
        relative = WORKSPACE_GAME + '/PS3_GAME/USRDIR/' + ('EBOOT.elf' if role == 'launcher' else 'bin/mage_g.elf')
        if _digest(_ordinary(contained(workspace, relative)), token) != record['elf_sha256']:
            raise InstallerError(f'Native runtime ELF identity mismatch: {relative}')
        records[role], originals[role] = record, (binary, receipt)
    if environment is None:
        from .managed_tools import msvc_environment
        environment = msvc_environment(config['msvc']['vcvars'])
    destination.mkdir(mode=0o700)
    try:
        game = stage_game(workspace, folder, token, events)
        bin_dir = destination / 'bin'; bin_dir.mkdir()
        resources = destination / 'resources'; resources.mkdir()
        package_receipts = resources / 'package-receipts'; package_receipts.mkdir()
        for role, name in [('launcher', 'MagRecomp.exe'), ('engine', 'MagEngine.exe')]:
            source, receipt = originals[role]
            shutil.copyfile(source, bin_dir / name)
            record = package_receipt(records[role], bin_dir / name, receipt, token)
            (package_receipts / (role + '.json')).write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        if config.get('ffmpeg'):
            # The movie decoder the executables link: its DLLs beside them, and its LGPL notice.
            from .managed_tools import WINDOWS_FFMPEG_DLLS
            ffmpeg_root = Path(config['ffmpeg']['include']).parent
            for name in WINDOWS_FFMPEG_DLLS:
                shutil.copyfile(_ordinary(ffmpeg_root / 'bin' / name), bin_dir / name)
            licenses = resources / 'licenses'; licenses.mkdir()
            shutil.copyfile(_ordinary(ffmpeg_root / 'LICENSE.txt'), licenses / 'FFmpeg-LICENSE.txt')
        game_relative = os.path.relpath(game, destination)
        engine_self = manifest['images']['mage_g']['source_sha256']
        runtime = {'schema_version': 2, 'game': Path(game_relative).as_posix(),
                   'launcher': 'bin/MagRecomp.exe', 'engine': 'bin/MagEngine.exe',
                   'engine_self_sha256': engine_self, 'signing': 'unsigned-local',
                   'environment': playback_environment(records)}
        (resources / 'runtime.json').write_text(json.dumps(runtime, indent=2) + '\n', encoding='utf-8')
        scratch = destination / '.supervisor-build'; scratch.mkdir()
        (scratch / 'supervisor_config.h').write_text(
            '#define MAG_GAME_ROOT ' + _c_wide(game_relative.replace('/', '\\')) + '\n'
            '#define MAG_LAUNCHER_SHA256 ' + _c_hex(_digest(bin_dir / 'MagRecomp.exe', token)) + '\n'
            '#define MAG_ENGINE_SHA256 ' + _c_hex(_digest(bin_dir / 'MagEngine.exe', token)) + '\n'
            '#define MAG_LAUNCHER_ELF_SHA256 ' + _c_hex(records['launcher']['elf_sha256']) + '\n'
            '#define MAG_ENGINE_ELF_SHA256 ' + _c_hex(records['engine']['elf_sha256']) + '\n'
            '#define MAG_ENGINE_SELF_SHA256 ' + _c_hex(engine_self) + '\n'
            '#define MAG_VERSION "' + product_version(workspace) + '"\n'
            '#define MAG_RUNTIME_DEFAULTS {' + ''.join('{' + _c_wide(key) + ', ' + _c_wide(value) + '}, '
                                                      for key, value in sorted(playback_environment(records).items()))
            + '{NULL, NULL}}\n', encoding='utf-8')
        (scratch / 'supervisor.c').write_text(_SUPERVISOR, encoding='utf-8')
        copy_supervisor_sources(workspace, scratch)
        _compile(config['msvc']['clang_cl'], environment, scratch, destination / 'MAG.exe', token)
        shutil.rmtree(scratch)
        events({'event': 'app-packaged', 'output': str(destination), 'launcher': str(destination / 'MAG.exe'),
                'message': 'MAG was packaged. Start it with MAG.exe; its MAG folder is self-contained, so move it as a whole.'})
        return destination
    except BaseException:
        shutil.rmtree(destination, ignore_errors=True)
        shutil.rmtree(folder / GAME_FOLDER, ignore_errors=True)  # check_workspace: it was not there
        raise
