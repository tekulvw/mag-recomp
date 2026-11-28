"""Exact retail input identities and isolated paths during the 2.12 branch port.

Target selection is build/audit scaffolding, not a claim of native 2.12 support.
This module has no workspace overlay: original input identities remain source-bound.
"""
from dataclasses import dataclass
import os
from pathlib import Path
from subprocess import run as git_run
from types import MappingProxyType
from typing import Mapping

DEFAULT_TARGET = 'retail-v1.0'
ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class RetailTarget:
    name: str
    app_version: str
    executables: Mapping[str, tuple[str, str]]
    local_usrdir: str
    native_supported: bool = False
    title_id: str = 'BCUS98110'

    @property
    def profile(self):
        return f'profiles/{self.name}.json'

    @property
    def analysis(self):
        return f'analysis/{self.name}'

    @property
    def generated(self):
        return f'generated/{self.name}'


_TARGETS = MappingProxyType({
    DEFAULT_TARGET: RetailTarget(DEFAULT_TARGET, '01.00', MappingProxyType({
        'EBOOT.BIN': ('ebe4a52656cc38483d24656029a55ddef6fd53adc7f23b4c52c42f272ac8d1f9',
                      'eb916f92e7c32a9401739d79835630273e1f41a92e6a5d17753d9374062da7d9'),
        'bin/mage_g.self': ('54b324a39e5453916768d8e83057ea72865cf53b23a1a8f058e25915d401840c',
                            '0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c'),
    }), 'local/retail-v1.0/PS3_GAME/USRDIR', native_supported=True),
    'retail-v2.12': RetailTarget('retail-v2.12', '02.12', MappingProxyType({
        'EBOOT.BIN': ('aff08358d8fe4e9418c3e5065fec94b234191b3edc495d3eb15bdf2c42013317',
                      'feb5ed5a3e8f3aab74601ae3edc737e3c010208523d3eb322be997cbe3d79318'),
        'bin/mage_g.self': ('8e3ceada7c5b1c6b82cb1b7ae6109725bda1b5abd87e5cb42f923b5449ce332a',
                            '705a6ec6a25458b2401b869e75e2904597304e2c06d9534a171a1dcd54aca2b3'),
    }), 'local/retail-v2.12/dev_hdd0/game/BCUS98110/USRDIR'),
})
TARGET_NAMES = tuple(_TARGETS)


def get_target(name=DEFAULT_TARGET) -> RetailTarget:
    if not isinstance(name, str) or name not in _TARGETS:
        raise ValueError(f'Unknown retail target: {name!r}; expected {", ".join(TARGET_NAMES)}')
    return _TARGETS[name]


def validate_target_profile(manifest, target=DEFAULT_TARGET):
    """Require a complete exact pair for 2.12; keep legacy v1 receipt compatibility."""
    selected = get_target(target)
    if manifest.get('target', target) != target:
        raise ValueError('Image profile belongs to a different target')
    for key in ('title_id', 'app_version'):
        if key in manifest and manifest[key] != getattr(selected, key):
            raise ValueError(f'Image profile {key} does not match target {target}')
    if target == DEFAULT_TARGET:
        # Existing v1 receipts predate explicit target fields. Keep their shape and
        # verification flow, but never accept a known foreign executable identity.
        foreign = {h for name in TARGET_NAMES if name != target
                   for pair in get_target(name).executables.values() for h in pair}
        if any(row.get(key) in foreign for row in manifest.get('images', {}).values()
               for key in ('source_sha256', 'elf_sha256')):
            raise ValueError('Image profile contains another target executable')
        return
    if (manifest.get('target') != selected.name or manifest.get('title_id') != selected.title_id
            or manifest.get('app_version') != selected.app_version):
        raise ValueError(f'Incomplete explicit profile for target {target}')
    expected = {Path(name).stem: pair for name, pair in selected.executables.items()}
    if set(manifest.get('images', {})) != set(expected):
        raise ValueError('Target profile must contain the complete launcher/engine pair')
    for image, (source_hash, elf_hash) in expected.items():
        row = manifest['images'][image]
        if row.get('source_sha256') != source_hash or row.get('elf_sha256') != elf_hash:
            raise ValueError(f'Target executable identity mismatch: {image}')


def validate_target_output(output, target=DEFAULT_TARGET, workspace=None):
    """Refuse canonical data roots of another target, including resolved aliases."""
    get_target(target)
    workspace = Path(workspace or os.environ.get('MAG_WORKSPACE') or ROOT).resolve()
    output = Path(output).resolve()
    if any(part.casefold() == '.git' for part in output.parts):
        raise ValueError('Target outputs cannot enter Git metadata')
    for other in TARGET_NAMES:
        if other == target:
            continue
        protected = [workspace / kind / other for kind in ('analysis', 'generated', 'local', 'build', 'runs')]
        protected += [workspace / get_target(other).profile,
                      workspace / f'profiles/{other}-inventory.json']
        for path in protected:
            path = path.resolve()
            if output == path or path in output.parents or output in path.parents:
                raise ValueError(f'Output overlaps another target ({other}): {output}')
    if target != DEFAULT_TARGET:
        ancestor = output.parent
        while not ancestor.exists():
            ancestor = ancestor.parent
        probe = git_run(['git', '-C', str(ancestor), 'rev-parse', '--show-toplevel'],
                        capture_output=True, text=True)
        if probe.returncode == 0:
            ignored = git_run(['git', '-C', probe.stdout.strip(), 'check-ignore', '-q',
                               '--no-index', str(output) + os.sep], capture_output=True)
            if ignored.returncode != 0:
                raise ValueError('Target outputs must be outside Git or explicitly ignored')
        else:
            metadata = git_run(['git', '-C', str(ancestor), 'rev-parse', '--is-inside-git-dir'],
                               capture_output=True, text=True)
            if metadata.returncode == 0 and metadata.stdout.strip() == 'true':
                raise ValueError('Target outputs cannot enter Git metadata')
