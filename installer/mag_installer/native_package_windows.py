"""Package private local native outputs into a Windows MAG folder.

Layout: <install>/MAG/MAG.exe (supervisor), <install>/MAG/bin/{MagRecomp,MagEngine}.exe,
receipts and runtime metadata. Assets stay in the owned installation workspace;
move the complete installation folder. The executables use the static CRT, so
no redistributable DLLs are copied. Nothing is launched here.
"""
from __future__ import annotations
import json
import os
from pathlib import Path
import shutil
import subprocess

from .model import InstallerError
from .safety import Cancellation, contained, file_digest


# Mirrors the macOS supervisor and the src/retail_handoff.cpp contract: verify the
# exact local images, run the launcher; on exit 90 verify its exclusive handoff
# request and run the engine. Paths are resolved from MAG.exe's own location.
_SUPERVISOR = r'''
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "supervisor_config.h"

static wchar_t g_root[MAX_PATH] = L".";

static void fail(const wchar_t *message, const wchar_t *detail) {
    wchar_t text[4096];
    swprintf(text, 4096, L"MAG could not start: %ls%ls%ls\n", message, detail ? L": " : L"", detail ? detail : L"");
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show) {
    (void)instance; (void)previous; (void)arguments; (void)show;
    wchar_t self[MAX_PATH], app[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH) fail(L"Cannot locate MAG.exe", NULL);
    lstrcpyW(app, self); *wcsrchr(app, L'\\') = 0;
    join(g_root, app, L"..");
    wchar_t workspace[MAX_PATH], vfs[MAX_PATH], launcher[MAX_PATH], engine[MAX_PATH];
    wchar_t launcher_elf[MAX_PATH], engine_elf[MAX_PATH], engine_self[MAX_PATH];
    join(workspace, app, MAG_WORKSPACE);
    join(vfs, workspace, L"local\\retail-v1.0");
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

    set_env(L"HOME", home);
    set_env(L"PS3_VFS_ROOT", vfs);
    set_env(L"PS3_BOOT_PATH", L"/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN");
    set_env(L"PS3_HLE_UNRESOLVED", L"halt");
    set_env(L"PS3_SYSCALL_UNRESOLVED", L"halt");
    set_env(L"MAG_HANDOFF_REQUEST", request);
    set_env(L"MAG_HANDOFF_TOKEN", wtoken);
    set_env(L"MAG_ENGINE_BINARY", engine);
    set_env(L"MAG_ENGINE_ELF", engine_elf);
    set_env(L"MAG_ENGINE_SELF", engine_self);
    set_env_utf8(L"MAG_ENGINE_BINARY_SHA256", MAG_ENGINE_SHA256);
    set_env_utf8(L"MAG_ENGINE_ELF_SHA256", MAG_ENGINE_ELF_SHA256);
    set_env_utf8(L"MAG_ENGINE_SELF_SHA256", MAG_ENGINE_SELF_SHA256);
    set_env_utf8(L"MAG_ENGINE_KEY_SHA256", key);

    /* Closing the supervisor (or its crash) also ends the game processes. */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) fail(L"Cannot create the process job", NULL);

    join(log, logs, L"launcher.log");
    DWORD result = run(job, launcher, launcher_elf, logs, log);
    if (result != 90) return (int)result;

    char expected[256];
    snprintf(expected, sizeof expected, "{\"version\":1,\"token\":\"%s\",\"guest_path\":\"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self\",\"kind\":\"mage_g\"}\n", token);
    DWORD size; unsigned char *body = read_file(request, &size);
    if (size != strlen(expected) || memcmp(body, expected, size)) fail(L"Engine handoff request identity mismatch", request);
    free(body);
    check(engine, MAG_ENGINE_SHA256); check(engine_elf, MAG_ENGINE_ELF_SHA256); check(engine_self, MAG_ENGINE_SELF_SHA256);
    const wchar_t *cleared[] = {L"MAG_HANDOFF_REQUEST", L"MAG_HANDOFF_TOKEN", L"MAG_ENGINE_BINARY", L"MAG_ENGINE_ELF",
        L"MAG_ENGINE_SELF", L"MAG_ENGINE_BINARY_SHA256", L"MAG_ENGINE_ELF_SHA256", L"MAG_ENGINE_SELF_SHA256", L"MAG_ENGINE_KEY_SHA256"};
    for (size_t i = 0; i < sizeof cleared / sizeof cleared[0]; i++) SetEnvironmentVariableW(cleared[i], NULL);
    set_env(L"PS3_BOOT_PATH", L"/dev_bdvd/PS3_GAME/USRDIR/bin/mage_g.self");
    join(log, logs, L"engine.log");
    return (int)run(job, engine, engine_elf, logs, log);
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


def _compile(clang_cl, environment, source_dir, output, token):
    token.check()
    argv = [clang_cl, '/nologo', '/O2', '/MT', '/W3', '/DUNICODE', '/D_UNICODE', str(source_dir / 'supervisor.c'),
            f'/Fe{output}', f'/Fo{source_dir / "supervisor.obj"}', '/link', '/SUBSYSTEM:WINDOWS', 'bcrypt.lib', 'user32.lib']
    try:
        result = subprocess.run(argv, cwd=source_dir, env=environment, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=300)
    except (OSError, subprocess.SubprocessError) as exc:
        raise InstallerError(f'Cannot compile the MAG supervisor: {exc}') from exc
    if result.returncode or not Path(output).is_file():
        raise InstallerError('MAG supervisor compilation failed: ' + (result.stdout + result.stderr)[-3000:])


def package(config, workspace, destination, token=None, events=None, environment=None):
    """Build <install>/MAG beside its owned workspace without launching anything."""
    from .native_package import _ordinary, _digest
    token = token or Cancellation(); events = events or (lambda event: None)
    workspace, destination = Path(workspace).absolute(), Path(destination).absolute()
    if destination.exists() or destination.is_symlink(): raise InstallerError('MAG folder destination already exists')
    if destination.name != 'MAG' or not destination.parent.is_dir(): raise InstallerError('Choose a new MAG folder under the installation directory')
    root = destination.parent.resolve()
    workspace_real = workspace.resolve()
    if workspace.is_symlink() or not workspace.is_dir() or workspace_real == root or not workspace_real.is_relative_to(root):
        raise InstallerError('Owned workspace must be an ordinary descendant of the installation folder')
    manifest = json.loads(contained(workspace, 'profiles/retail-v1.0.json').read_text(encoding='utf-8'))
    framework = json.loads(contained(workspace, 'profiles/framework.json').read_text(encoding='utf-8'))
    records, originals = {}, {}
    for role, folder, image in [('launcher', 'build', 'EBOOT'), ('engine', 'build-engine', 'mage_g')]:
        binary = _ordinary(contained(workspace, folder + '/MagRecomp.exe'))
        receipt = _ordinary(binary.with_suffix('.receipt.json'))
        record = json.loads(receipt.read_text(encoding='utf-8')); expected = manifest['images'][image]
        if (record.get('binary_sha256') != _digest(binary, token) or record.get('elf_sha256') != expected['elf_sha256'] or record.get('image_name') != image):
            raise InstallerError(f'Native source receipt mismatch: {receipt}')
        if record.get('sdk_revision') not in set(framework['strict_runtime_revisions']) & set(framework['strict_hle_runtime_revisions']):
            raise InstallerError('Packaged runtime must support strict syscall/HLE policies')
        relative = 'local/retail-v1.0/PS3_GAME/USRDIR/' + ('EBOOT.elf' if role == 'launcher' else 'bin/mage_g.elf')
        if _digest(_ordinary(contained(workspace, relative)), token) != record['elf_sha256']:
            raise InstallerError(f'Native runtime ELF identity mismatch: {relative}')
        records[role], originals[role] = record, (binary, receipt)
    if environment is None:
        from .managed_tools import msvc_environment
        environment = msvc_environment(config['msvc']['vcvars'])
    destination.mkdir(mode=0o700)
    try:
        bin_dir = destination / 'bin'; bin_dir.mkdir()
        resources = destination / 'resources'; resources.mkdir()
        receipts = resources / 'source-receipts'; receipts.mkdir()
        package_receipts = resources / 'package-receipts'; package_receipts.mkdir()
        for role, name in [('launcher', 'MagRecomp.exe'), ('engine', 'MagEngine.exe')]:
            source, receipt = originals[role]
            shutil.copyfile(source, bin_dir / name)
            shutil.copyfile(receipt, receipts / (role + '.json'))
            record = {key: records[role][key] for key in ('image_name', 'elf_sha256', 'sdk_revision')}
            record.update(binary_sha256=_digest(bin_dir / name, token), kind='local-packaged-native',
                          source_receipt_sha256=_digest(receipts / (role + '.json'), token),
                          original_binary_sha256=records[role]['binary_sha256'])
            (package_receipts / (role + '.json')).write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        workspace_relative = os.path.relpath(workspace, destination)
        engine_self = manifest['images']['mage_g']['source_sha256']
        runtime = {'schema_version': 1, 'workspace': Path(workspace_relative).as_posix(),
                   'launcher': 'bin/MagRecomp.exe', 'engine': 'bin/MagEngine.exe',
                   'engine_self_sha256': engine_self, 'signing': 'unsigned-local'}
        (resources / 'runtime.json').write_text(json.dumps(runtime, indent=2) + '\n', encoding='utf-8')
        scratch = destination / '.supervisor-build'; scratch.mkdir()
        (scratch / 'supervisor_config.h').write_text(
            '#define MAG_WORKSPACE ' + _c_wide(workspace_relative.replace('/', '\\')) + '\n'
            '#define MAG_LAUNCHER_SHA256 ' + _c_hex(_digest(bin_dir / 'MagRecomp.exe', token)) + '\n'
            '#define MAG_ENGINE_SHA256 ' + _c_hex(_digest(bin_dir / 'MagEngine.exe', token)) + '\n'
            '#define MAG_LAUNCHER_ELF_SHA256 ' + _c_hex(records['launcher']['elf_sha256']) + '\n'
            '#define MAG_ENGINE_ELF_SHA256 ' + _c_hex(records['engine']['elf_sha256']) + '\n'
            '#define MAG_ENGINE_SELF_SHA256 ' + _c_hex(engine_self) + '\n', encoding='utf-8')
        (scratch / 'supervisor.c').write_text(_SUPERVISOR, encoding='utf-8')
        _compile(config['msvc']['clang_cl'], environment, scratch, destination / 'MAG.exe', token)
        shutil.rmtree(scratch)
        events({'event': 'app-packaged', 'output': str(destination), 'launcher': str(destination / 'MAG.exe'),
                'message': 'MAG was packaged. Start it with MAG.exe; keep the whole installation folder together.'})
        return destination
    except BaseException:
        shutil.rmtree(destination, ignore_errors=True)
        raise
