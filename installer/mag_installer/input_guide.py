"""The step-by-step guide to preparing MAG's files, and the checks run the moment a file is chosen.

Shared by the wizard and the ``guide`` command, which show the same text. The player does every
download, firmware installation and decryption with their own RPCS3; this module links to the
official pages, fills found paths into the commands and checks the chosen files by the same
SHA-256 identities a build checks. It never runs RPCS3 and never changes a file.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path
import shlex
import subprocess

from .model import InstallerError
from .native_build import (FIRMWARE_KNOWN_RELEASES, FIRMWARE_PUP_RELEASE, FIRMWARE_PUP_SHA256, FIRMWARE_PUP_URL,
                           SETTINGS_ARCHIVE_CHANGED, identity_problem, reviewed_identities)
from .rpcs3_detect import APP_VERSION, TITLE_ID, host_name, read_sfo


RPCS3_URL = 'https://rpcs3.net/download'
DONE, TODO, PROBLEM = 'Done', 'To do', 'Problem'
# The rendered guide indents its notes by LINE_INDENT and commands by COMMAND_INDENT, so the
# wizard's Copy commands button can pick out exactly the commands.
LINE_INDENT, COMMAND_INDENT = ' ' * 11, ' ' * 13


def load_reviewed(root):
    """The reviewed identities the wizard checks against: the installer manifest's copy of its kit's,
    else (a source checkout) the repository's own; None when neither is available."""
    root = Path(root)
    try:
        record = json.loads((root / 'release/player-build.json').read_text(encoding='utf-8'))
        if isinstance(record, dict) and isinstance(record.get('reviewed_inputs'), dict):
            return record['reviewed_inputs']
    except (OSError, ValueError):
        pass
    for source in (root.parent,):
        if (source / 'tools/prepare_inputs.py').is_file() and (source / 'profiles/retail-v1.0-firmware.json').is_file():
            try:
                return reviewed_identities(source)
            except (InstallerError, OSError, ValueError, SyntaxError, KeyError, TypeError):
                return None
    return None


@dataclass
class Finding:
    """One result of checking a chosen input: level is DONE, TODO or PROBLEM; message says how to fix it."""
    level: str
    message: str
    commands: list = field(default_factory=list)
    code: str = ''


@dataclass
class GameCheck:
    disc: Path | None
    findings: list

    @property
    def ok(self):
        return self.disc is not None and all(f.level == DONE for f in self.findings)


def rpcs3_argv(detection, host=None):
    """How to run the player's RPCS3 in the commands shown: the found one, else its usual place."""
    host = host or detection.host
    if detection.rpcs3:
        return [str(part) for part in detection.rpcs3[0]]
    return {'macos': ['/Applications/RPCS3.app/Contents/MacOS/rpcs3'], 'windows': [r'<your RPCS3 folder>\rpcs3.exe'],
            'linux': ['rpcs3']}[host]


def command_line(argv, host):
    if host == 'windows':
        return subprocess.list2cmdline([str(part) for part in argv])
    return ' '.join(part if part.startswith('<') else shlex.quote(str(part)) for part in map(str, argv))


def shell_name(host):
    return {'windows': 'Command Prompt', 'macos': 'Terminal', 'linux': 'a terminal'}[host]


def sprx_placeholder(host):
    return (r'<your RPCS3 folder>\dev_flash\sys\external\libsre.sprx' if host == 'windows'
            else '<RPCS3 folder>/dev_flash/sys/external/libsre.sprx')


def decrypt_commands(rpcs3, files, host):
    """One ``--decrypt`` per file (RPCS3 reads each option's single value); it writes the result beside each."""
    argv = list(rpcs3)
    for path in files:
        argv += ['--decrypt', str(path)]
    return [command_line(argv, host)]


def _disc_of(path):
    """(PS3_GAME-level folder, base folder holding firmware/), or (None, problem message)."""
    path = Path(path).expanduser()
    if path.is_file():
        name = path.name.lower()
        hint = (' Choose the folder holding PS3_GAME instead of a file in it.' if name in ('eboot.bin', 'param.sfo', 'eboot.elf')
                else ' Choose a folder.')
        return None, f'{path} is a file.{hint}'
    if not path.is_dir():
        return None, f'{path} does not exist. Choose the folder holding your PS3_GAME folder.'
    if (path / 'PS3_GAME').is_dir():
        return path / 'PS3_GAME', path
    if (path / 'PARAM.SFO').is_file():
        return path, path.parent if path.name == 'PS3_GAME' else path
    if path.name.upper() in ('USRDIR', 'BIN') and any((p / 'PARAM.SFO').is_file() for p in path.parents[:2]):
        disc = next(p for p in path.parents[:2] if (p / 'PARAM.SFO').is_file())
        return None, f'You chose {path.name}, inside the game folder. Choose {disc} (the PS3_GAME folder) or the folder that holds it.'
    return None, (f'{path} has no PS3_GAME folder. Choose the folder that holds the PS3_GAME folder copied from your '
                  f'{TITLE_ID} disc, or PS3_GAME itself.')


def default_firmware(game_folder):
    """Where the build looks for the firmware module when no file is chosen: firmware/libsre.prx next to PS3_GAME."""
    if not game_folder:
        return None
    disc, base = _disc_of(game_folder)
    return (base / 'firmware/libsre.prx') if disc is not None else None


def check_game(path, reviewed, rpcs3, host, token=None):
    """Check a chosen game folder as a build would: what is there, PARAM.SFO and the SHA-256 identities."""
    disc, base = _disc_of(path)
    if disc is None:
        return GameCheck(None, [Finding(PROBLEM, base, code='game-folder')])
    findings = []
    usrdir = disc / 'USRDIR'
    sfo = read_sfo(disc / 'PARAM.SFO')
    title, version, category = sfo.get('TITLE_ID', ''), sfo.get('APP_VER') or sfo.get('VERSION', ''), sfo.get('CATEGORY', '')
    if not (disc / 'PARAM.SFO').is_file():
        findings.append(Finding(PROBLEM, f'{disc} has no PARAM.SFO. Copy the complete PS3_GAME folder from your disc again.', code='game-incomplete'))
    elif title and title.upper() != TITLE_ID:
        findings.append(Finding(PROBLEM, f'This is a different game or release (title ID {title}). MAG Recomp needs the '
                                f'North American MAG disc, {TITLE_ID}, version {APP_VERSION}.', code='game-other-title'))
        return GameCheck(disc, findings)
    elif category and category != 'DG':
        findings.append(Finding(PROBLEM, 'This folder is MAG data that RPCS3 installed on its virtual hard disk (an update or '
                                'game data), not your disc\'s PS3_GAME folder. Choose the PS3_GAME folder copied from the disc.',
                                code='game-not-disc'))
        return GameCheck(disc, findings)
    elif version and version != APP_VERSION:
        findings.append(Finding(PROBLEM, f'This is MAG version {version}. Only the version {APP_VERSION} disc is supported; '
                                'use the PS3_GAME folder copied from your disc, without updates.', code='game-other-version'))
    missing = [name for name in ('USRDIR/EBOOT.BIN', 'USRDIR/bin/mage_g.self',
                                 'USRDIR/' + (reviewed or {}).get('settings_archive', {}).get('path', 'run/mag/ps3/game.psarc.sdat'))
               if not (disc / name).is_file()]
    if missing:
        findings.append(Finding(PROBLEM, 'Your copy is missing ' + ', '.join(missing) + '. Copy the complete PS3_GAME folder '
                                'from your disc again (the game loads its archives, movies and textures too).', code='game-incomplete'))
        return GameCheck(disc, findings)
    executables = (reviewed or {}).get('executables', {})
    pending = []
    for name, elf_name in (('EBOOT.BIN', 'EBOOT.elf'), ('bin/mage_g.self', 'bin/mage_g.elf')):
        original, decrypted = usrdir / name, usrdir / elf_name
        if executables:
            kind, _, _ = identity_problem(original, executables[name][0], token)
            if kind != 'ok':
                if _head(original) == b'\x7fELF':
                    findings.append(Finding(PROBLEM, f'USRDIR/{name} is already decrypted. Put the original, encrypted '
                                            f'USRDIR/{name} from your disc back; its decrypted copy goes beside it as USRDIR/{elf_name}.',
                                            code='game-original-decrypted'))
                else:
                    findings.append(Finding(PROBLEM, f'USRDIR/{name} is not MAG\'s retail {APP_VERSION} file: it comes from an '
                                            f'update or another release, or it was changed. Copy it from your own {TITLE_ID} disc again.',
                                            code='game-original-mismatch'))
                continue
        if not decrypted.is_file():
            pending.append(original)
            continue
        if executables:
            kind, _, _ = identity_problem(decrypted, executables[name][1], token, decrypted=True)
        else:
            head = _head(decrypted)
            kind = 'encrypted' if head == b'SCE\x00' else ('not-elf' if head != b'\x7fELF' else 'ok')
        if kind == 'encrypted':
            findings.append(Finding(PROBLEM, f'USRDIR/{elf_name} is still encrypted: it is a renamed copy of {name}. Delete it '
                                    'and decrypt the original with RPCS3:', decrypt_commands(rpcs3, [original], host), code='game-elf-encrypted'))
        elif kind == 'not-elf':
            findings.append(Finding(PROBLEM, f'USRDIR/{elf_name} is not a decrypted program. Delete it and decrypt the '
                                    'original with RPCS3:', decrypt_commands(rpcs3, [original], host), code='game-elf-not-elf'))
        elif kind != 'ok':
            findings.append(Finding(PROBLEM, f'USRDIR/{elf_name} is a decrypted program, but not the one from MAG\'s retail '
                                    f'{APP_VERSION} {name}: it comes from another release, or it was changed. Delete it and '
                                    'decrypt the original again:', decrypt_commands(rpcs3, [original], host), code='game-elf-other-release'))
    if pending:
        names = ' and '.join(path.name for path in pending)
        findings.append(Finding(TODO, f'Decrypt {names} with RPCS3. It writes '
                                + ' and '.join(('EBOOT.elf' if p.name == 'EBOOT.BIN' else 'mage_g.elf') for p in pending)
                                + ' beside the originals, where the build looks for them.',
                                decrypt_commands(rpcs3, pending, host), code='game-elf-missing'))
    settings = (reviewed or {}).get('settings_archive')
    if settings and not any(f.level == PROBLEM for f in findings):
        kind, _, _ = identity_problem(usrdir / settings['path'], settings['sha256'], token)
        if kind != 'ok':
            findings.append(Finding(PROBLEM, SETTINGS_ARCHIVE_CHANGED, code='game-settings-changed'))
    if not findings:
        findings.append(Finding(DONE, f'MAG {TITLE_ID} version {APP_VERSION} with both decrypted executables'
                                + ('.' if executables else '; fully checked when you click Build MAG.'), code='game-ok'))
    return GameCheck(disc, findings)


def _head(path):
    try:
        with Path(path).open('rb') as stream:
            return stream.read(4)
    except OSError:
        return b''


def firmware_steps(rpcs3, host, sprx=None, pup=None):
    pup_path = str(pup) if pup else '<path to PS3UPDAT.PUP>'
    return [command_line([*rpcs3, '--installfw', pup_path], host)], decrypt_commands(rpcs3, [sprx or sprx_placeholder(host)], host)


def check_firmware(path, reviewed, rpcs3, host, token=None, sprx=None):
    """Check the chosen firmware module (libsre.prx) as a build would."""
    path = Path(path).expanduser()
    decrypt = decrypt_commands(rpcs3, [sprx or (path if path.name.lower().endswith('.sprx') else
                                                sprx_placeholder(host))], host)
    if path.is_dir():
        return [Finding(PROBLEM, f'{path} is a folder. Choose the decrypted firmware file, libsre.prx.', code='firmware-folder')]
    if not path.is_file():
        return [Finding(TODO, f'{path} does not exist yet. Decrypt RPCS3\'s libsre.sprx; it writes libsre.prx beside it:',
                        decrypt, code='firmware-missing')]
    images = (reviewed or {}).get('firmware', [])
    if images:
        kind, actual, _ = identity_problem(path, [image['sha256'] for image in images], token, decrypted=True,
                                           known=FIRMWARE_KNOWN_RELEASES)
    else:
        head = _head(path)
        kind, actual = ('encrypted' if head == b'SCE\x00' else ('not-elf' if head != b'\x7fELF' else 'ok')), ''
    accepted = ' or '.join(image['release'] for image in images if image.get('release')) or FIRMWARE_PUP_RELEASE
    if kind == 'ok':
        release = next((image['release'] for image in images if image['sha256'] == actual and image.get('release')), '')
        return [Finding(DONE, 'The firmware module ' + (f'from system software {release} ' if release else '') + 'is correct'
                        + ('.' if images else '; fully checked when you click Build MAG.'), code='firmware-ok')]
    if kind == 'encrypted':
        return [Finding(PROBLEM, f'{path.name} is still encrypted (it is libsre.sprx, or a renamed copy). Decrypt it with '
                        'RPCS3, then choose the libsre.prx it writes beside it:', decrypt, code='firmware-encrypted')]
    if kind == 'not-elf':
        return [Finding(PROBLEM, f'{path.name} is not a decrypted firmware module. Choose the libsre.prx that RPCS3\'s '
                        '--decrypt wrote beside libsre.sprx:', decrypt, code='firmware-not-elf')]
    install, again = firmware_steps(rpcs3, host, sprx)
    if kind == 'known-release':
        return [Finding(PROBLEM, f'This is the module from {FIRMWARE_KNOWN_RELEASES[actual]}, decrypted correctly, but MAG '
                        f'Recomp is reviewed only for system software {accepted}. Install {FIRMWARE_PUP_RELEASE} in RPCS3 and '
                        'decrypt again:', install + again, code='firmware-unreviewed-release')]
    return [Finding(PROBLEM, f'{path.name} is a decrypted file, but not a reviewed libsre module: it comes from a different '
                    f'system software release or another file, or it was changed. MAG Recomp accepts system software '
                    f'{accepted}; Sony hosts only the latest release, and one newer than {FIRMWARE_PUP_RELEASE} is refused '
                    'until it is reviewed.', install + again, code='firmware-other-release')]


@dataclass
class Step:
    number: int
    title: str
    state: str
    lines: list = field(default_factory=list)
    commands: list = field(default_factory=list)
    link: str = ''
    notes: list = field(default_factory=list)   # shown after the link


def build_guide(detection, game=None, firmware=None, game_path=None, firmware_path=None):
    """The checklist: what is done, what is missing and the next step for each, for this computer.

    game and firmware are check_game and check_firmware results for the chosen (or found) inputs."""
    host = detection.host
    rpcs3 = rpcs3_argv(detection)
    shell = shell_name(host)
    sprx = detection.firmware_encrypted[0] if detection.firmware_encrypted else None
    pup = detection.pup_files[0] if detection.pup_files else None
    firmware_ok = bool(firmware) and all(f.level == DONE for f in firmware)
    steps = []
    if detection.rpcs3_found():
        where = ' '.join(map(str, detection.rpcs3[0])) if detection.rpcs3 else f'RPCS3\'s data in {detection.data[0].data}'
        steps.append(Step(1, 'Install RPCS3', DONE, [f'Found {where}.']))
    else:
        steps.append(Step(1, 'Install RPCS3', TODO,
                          ['Download RPCS3 from its official site and start it once. The installer only links to it:'],
                          link=RPCS3_URL))
    installed = bool(sprx) or firmware_ok
    pup_lines = ([f'Found {pup}.'] if pup else [f'Download PS3UPDAT.PUP (system software {FIRMWARE_PUP_RELEASE}) from '
                                                 'Sony\'s official page. The installer only links to it:'])
    pup_notes = [f'The {FIRMWARE_PUP_RELEASE} file has SHA-256 {FIRMWARE_PUP_SHA256}. 4.86 also works; Sony hosts only the latest release.']
    steps.append(Step(2, 'Get PS3 system software from Sony', DONE if installed or pup else TODO,
                      [] if installed else pup_lines, link='' if installed or pup else FIRMWARE_PUP_URL,
                      notes=[] if installed else pup_notes))
    install, _ = firmware_steps(rpcs3, host, sprx, pup)
    if installed:
        lines = [f'Found {sprx}.'] if sprx else ['The firmware module below is already decrypted.']
        release = detection.firmware_release
        if release and release not in ('4.93', '4.86') and not firmware_ok:
            lines.append(f'RPCS3 has system software {release} installed; MAG Recomp is reviewed for 4.93 and 4.86. '
                         f'Install {FIRMWARE_PUP_RELEASE} the same way if the module is refused.')
        steps.append(Step(3, 'Install it in RPCS3', DONE, lines))
    else:
        steps.append(Step(3, 'Install it in RPCS3', TODO,
                          [f'Run in {shell} (or use RPCS3\'s File > Install Firmware):'], install))
    # 4. The firmware module.
    if firmware:
        finding = firmware[0]
        state, commands = finding.level, list(finding.commands)
        message = with_shell(finding.message, shell) if commands else finding.message
        lines = [f'{firmware_path}: {message}' if firmware_path else message]
    else:
        state = TODO
        commands = decrypt_commands(rpcs3, [sprx or sprx_placeholder(host)], host)
        lines = [('Run in ' if sprx else 'After step 3, run in ') + f'{shell}. It writes libsre.prx beside libsre.sprx:']
    steps.append(Step(4, 'Decrypt the firmware module', state, lines, commands))
    # 5 and 6. The game folder and its two executables.
    best = detection.best_game()
    folder = game_path or (best.disc if best else None)
    if game is None or game.disc is None:
        lines = []
        if game is not None:
            lines.append(game.findings[0].message)
        lines.append(f'Copy the complete PS3_GAME folder from your own {TITLE_ID} (version {APP_VERSION}) disc copy to a '
                     'folder of your choice, or point RPCS3 at it (it lists it in games.yml).')
        for candidate in detection.games:
            if not candidate.usable and candidate.note:
                lines.append(f'{candidate.disc} ({candidate.where}): {candidate.note}')
        steps.append(Step(5, 'Copy your MAG disc\'s PS3_GAME folder', PROBLEM if game is not None else TODO, lines))
        steps.append(Step(6, 'Decrypt the game\'s two executables', TODO,
                          [f'Once the game folder is chosen, the exact commands appear here. RPCS3\'s --decrypt writes '
                           'EBOOT.elf and mage_g.elf beside EBOOT.BIN and mage_g.self.']))
    else:
        source = next((c.where for c in detection.games if c.disc == game.disc), '')
        file_problems = [f for f in game.findings if f.code.startswith(('game-other', 'game-not-disc', 'game-incomplete',
                                                                         'game-original', 'game-settings'))]
        lines = [f'{game.disc}' + (f' (found in {source}).' if source else '.')] + [f.message for f in file_problems]
        steps.append(Step(5, 'Copy your MAG disc\'s PS3_GAME folder', PROBLEM if file_problems else DONE, lines))
        elf = [f for f in game.findings if f.code.startswith('game-elf')]
        if file_problems:
            steps.append(Step(6, 'Decrypt the game\'s two executables', TODO, ['First fix the game folder above.']))
        elif not elf:
            steps.append(Step(6, 'Decrypt the game\'s two executables', DONE, ['EBOOT.elf and mage_g.elf are correct.']))
        else:
            state = PROBLEM if any(f.level == PROBLEM for f in elf) else TODO
            lines = [f.message for f in elf]
            commands = [c for f in elf for c in f.commands]
            lines[-1] = with_shell(lines[-1], shell)
            steps.append(Step(6, 'Decrypt the game\'s two executables', state, lines, commands))
    ready = all(step.state == DONE for step in steps)
    picks = [f'Game folder: {folder or "not chosen yet"}',
             f'Firmware file: {firmware_path or "not chosen yet (default: firmware/libsre.prx next to PS3_GAME)"}']
    steps.append(Step(7, 'Choose the files and build', DONE if ready else TODO,
                      picks + (['Everything is ready: click Build MAG.'] if ready else
                               ['Each file is checked when you choose it, and again before the build starts.'])))
    return steps


def with_shell(message, shell):
    """A message followed by commands, naming where to run them."""
    return message.rstrip(':.') + f'. Run in {shell}:'


def render(steps, detection=None):
    """The guide as plain text: the wizard shows it in a read-only, selectable text box; guide prints it."""
    done = sum(step.state == DONE for step in steps[:-1])
    out = [f'Prepare MAG\'s files ({done} of {len(steps) - 1} steps done)', '']
    for step in steps:
        out.append(f'{step.state:<8} {step.number}. {step.title}')
        for line in step.lines:
            out.append(LINE_INDENT + line)
        if step.link:
            out.append(LINE_INDENT + step.link)
        for line in step.notes:
            out.append(LINE_INDENT + line)
        for command in step.commands:
            out.append(COMMAND_INDENT + command)
    if detection is not None and detection.incomplete:
        out += ['', f'The search for RPCS3\'s files stopped after {detection.seconds:.0f} seconds; choose any file it did not find yourself.']
    return '\n'.join(out)


def suggested_build(detection, game_path, firmware_path, host=None):
    """A build command line with the found paths filled in, for the guide command."""
    host = host or detection.host
    argv = ['build', '--source', str(game_path or '<game folder>')]
    if firmware_path:
        argv += ['--firmware', str(firmware_path)]
    argv += ['--parent', '<installation location>']
    return command_line(argv, host)


def survey(detection, reviewed, game_path=None, firmware_path=None, token=None):
    """Check the chosen inputs, else the found ones, and build the guide: (steps, game, firmware, paths)."""
    host = detection.host
    rpcs3 = rpcs3_argv(detection)
    best = detection.best_game()
    game_path = Path(game_path).expanduser() if game_path else (best.disc if best else None)
    if not firmware_path and detection.firmware_decrypted:
        firmware_path = detection.firmware_decrypted[0]
    if not firmware_path and game_path:
        default = default_firmware(game_path)
        firmware_path = default if default and default.is_file() else None
    sprx = detection.firmware_encrypted[0] if detection.firmware_encrypted else None
    game = check_game(game_path, reviewed, rpcs3, host, token) if game_path else None
    firmware = check_firmware(firmware_path, reviewed, rpcs3, host, token, sprx) if firmware_path else None
    steps = build_guide(detection, game, firmware, game_path, firmware_path)
    return steps, game, firmware, game_path, firmware_path


__all__ = ['DONE', 'TODO', 'PROBLEM', 'RPCS3_URL', 'FIRMWARE_PUP_URL', 'Finding', 'GameCheck', 'Step', 'build_guide',
           'check_firmware', 'check_game', 'default_firmware', 'load_reviewed', 'render', 'rpcs3_argv', 'survey',
           'suggested_build', 'host_name']
