"""Shared readable presentation; safe to import without a graphical runtime."""
from pathlib import Path
import os
import sys


def default_cache() -> Path:
    if sys.platform == 'win32':
        return Path(os.environ.get('LOCALAPPDATA', str(Path.home() / 'AppData/Local'))) / 'MAG Installer/Cache'
    if sys.platform == 'darwin':
        return Path.home() / 'Library/Caches/MAG Installer'
    return Path(os.environ.get('XDG_CACHE_HOME', str(Path.home() / '.cache'))) / 'mag-installer'


def default_parent() -> Path:
    home = Path.home()
    # The Windows runtime needs an ASCII installation path (native_build).
    if sys.platform == 'win32' and not str(home).isascii():
        return Path(os.environ.get('SystemDrive', 'C:') + '\\')
    return home


def destination(parent: Path, synthetic=False) -> Path:
    return Path(parent).expanduser() / ('MAG Demo' if synthetic else 'MAG')


def size_text(value):
    value = float(value)
    for unit in ('B', 'KiB', 'MiB', 'GiB', 'TiB'):
        if value < 1024 or unit == 'TiB':
            return f'{value:,.1f} {unit}'
        value /= 1024


def space_summary(summary):
    text = (f'Download: {size_text(summary.get("download_bytes", 0))}. '
            f'Known working space required: {size_text(summary.get("required_bytes", 0))}. '
            f'Free: {size_text(summary.get("free_bytes", 0))}.')
    if not summary.get('estimate_complete', False):
        text += ' Additional build working space is unknown; this is a lower bound, not a full estimate.'
    return text


def format_event(event: dict) -> str:
    name = event.get('event', 'progress')
    if name == 'stage-start':
        return {'validate': 'Checking input version', 'acquire': 'Preparing pinned tools',
                'generate': 'Generating local files', 'build': 'Building locally',
                'package': 'Preparing output',
                'prepare-inputs': 'Verifying copied game files', 'inventory': 'Analyzing game executables',
                'lift-launcher': 'Generating launcher code', 'lift-engine': 'Generating game engine code',
                'worker-spu': 'Generating launcher worker code', 'engine-spu': 'Generating game engine worker code',
                'firmware': 'Generating firmware support', 'spu-programs': 'Generating remaining game worker programs',
                'configure-launcher': 'Checking launcher build dependencies', 'configure-engine': 'Checking game engine build dependencies',
                'build-launcher': 'Compiling the launcher', 'build-engine': 'Compiling the game engine',
                'copy-inputs': 'Copying your game files into the private workspace', 'copy-source': 'Preparing a private source workspace'}.get(event.get('stage'), str(event.get('stage', 'Working')))
    if name == 'stage-activity':
        return f'{event.get("stage", "Working")} still running ({event.get("elapsed", 0):.0f}s). Log: {event.get("log", "see logs folder")}'
    if name == 'synthetic-complete':
        return 'Synthetic demonstration complete. No MAG application was built.'
    if name == 'local-build-complete':
        return 'Local build complete. Open the output folder to inspect the result.'
    if name == 'publish-start':
        return 'Finishing output'
    if 'download' in name:
        received = event.get('bytes', event.get('received', event.get('downloaded', 0)))
        return f'Downloading {event.get("tool", "tools")}: {size_text(received)}' + (f' / {size_text(event["total"])}' if event.get('total') else '')
    return str(event.get('message') or name.replace('-', ' '))
