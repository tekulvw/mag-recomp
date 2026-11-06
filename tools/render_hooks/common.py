"""Shared machinery for the render adapter's observer hooks in lifted PPU sources.

A stage names the generated functions it instruments and the functions it only
depends on, each pinned inline by address to the SHA-256 of its complete lifted
body and to the lifted file that holds it. Pins identify the output of the
pinned PPU lifter (`LIFTER_REVISION`) on the retail v1.0 engine image; nothing
else of the lifted code is kept here.

Every pin is re-verified when a stage is applied. A missing, duplicated, moved
or changed function, or a seam that is not unique, refuses the whole
preparation. Hooks only add guarded lines: removing them restores the original
body byte for byte, which is checked before anything is written. The guards are
compile definitions that only the opt-in CMake stage options set, so a prepared
lift compiled without them is the original program.
"""
import hashlib
import re
from pathlib import Path

LIFTER_REVISION = 'dfa7073d2e164aec6890866c4951d8dedc28619d'
FUNCTION_BODY = re.compile(r'^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{.*?^}', re.M | re.S)
DIGEST = re.compile('[0-9a-f]{64}')
LIFTED_FILE = re.compile(r'ppu_recomp_[0-9]{3}\.cpp')


class HookError(ValueError):
    """A pinned source, seam or preparation input did not match."""


def sha256_text(text):
    return hashlib.sha256(text.encode()).hexdigest()


def signature(name):
    return f'void func_{name}(ppu_context* ctx) {{'


def guarded(guard, text):
    """One hook line set under a stage's compile guard, as its own lines."""
    return f'\n#if {guard}\n{text}\n#endif\n'


def bodies(source, names):
    """Map each wanted function name to its single body match in `source`."""
    found = {}
    for match in FUNCTION_BODY.finditer(source):
        name = match[1]
        if name not in names:
            continue
        if name in found:
            raise HookError(f'duplicate lifted function {name}')
        found[name] = match
    return found


def check_pins(stage):
    """Reject a malformed pin table before it is used."""
    for table in (stage.PINS, stage.DEPENDENCIES):
        for name, pin in table.items():
            if (not re.fullmatch('[0-9A-F]{8}', name) or set(pin) != {'sha256', 'file'}
                    or not DIGEST.fullmatch(pin['sha256']) or not LIFTED_FILE.fullmatch(pin['file'])):
                raise HookError(f'{stage.NAME}: malformed pin {name}')
    if set(stage.PINS) & set(stage.DEPENDENCIES):
        raise HookError(f'{stage.NAME}: a function is both instrumented and a dependency')
    if not set(getattr(stage, 'SHARED', ())) <= set(stage.PINS):
        raise HookError(f'{stage.NAME}: a shared function is not instrumented by the stage')


def replace_once(body, old, new, what):
    if body.count(old) != 1:
        raise HookError(f'{what}: seam is not unique')
    return body.replace(old, new, 1)


def instrument_source(source, stage, pins=None, dependencies=None, others=()):
    """Return `source` with `stage`'s hooks; idempotent on already hooked source.

    `pins` and `dependencies` default to the stage's tables; tests pass their own.
    Each instrumented body is verified after removing this stage's hooks, so a
    second application reproduces the same text instead of stacking hooks.

    A function that `stage` and other stages both declare in `SHARED` may already
    carry their hooks. Its pin is checked with every stage's hooks removed, and
    this stage's hooks are added next to theirs without touching them.
    """
    pins = stage.PINS if pins is None else pins
    dependencies = stage.DEPENDENCIES if dependencies is None else dependencies
    source = source.replace(stage.INCLUDE, '')
    found = bodies(source, set(pins) | set(dependencies))
    for name, match in found.items():
        if name in dependencies and sha256_text(match[0]) != dependencies[name]['sha256']:
            raise HookError(f'{stage.NAME}: dependency {name} does not match its pin')
    changed = False
    # Splice from the end so earlier match offsets stay valid.
    for name, match in sorted(found.items(), key=lambda item: item[1].start(), reverse=True):
        if name not in pins:
            continue
        current = stage.strip_body(match[0], name)
        sharing = [other for other in others if name in getattr(other, 'SHARED', ())]
        if sharing and name not in getattr(stage, 'SHARED', ()):
            raise HookError(f'{stage.NAME}: function {name} is not declared shared')
        original = current
        for other in sharing:
            original = other.strip_body(original, name)
        if sha256_text(original) != pins[name]['sha256']:
            raise HookError(f'{stage.NAME}: function {name} does not match its pin')
        hooked = stage.instrument_body(current, name)
        restored = stage.strip_body(hooked, name)
        for other in sharing:
            restored = other.strip_body(restored, name)
        if stage.strip_body(hooked, name) != current or restored != original:
            raise HookError(f'{stage.NAME}: hooks for {name} would change original statements')
        source = source[:match.start()] + hooked + source[match.end():]
        changed = True
    return stage.INCLUDE + source if changed else source


def locate(directory, stage):
    """Check every pinned function appears exactly once, in its pinned file."""
    names = set(stage.PINS) | set(stage.DEPENDENCIES)
    counts = dict.fromkeys(names, 0)
    for path in sorted(Path(directory).glob('ppu_recomp_*.cpp')):
        text = path.read_text()
        for name in names:
            occurrences = text.count(signature(name))
            if occurrences:
                pin = stage.PINS.get(name) or stage.DEPENDENCIES[name]
                if path.name != pin['file']:
                    raise HookError(f'{stage.NAME}: function {name} moved to {path.name}')
                counts[name] += occurrences
    missing = sorted(name for name, count in counts.items() if count != 1)
    if missing:
        raise HookError(f'{stage.NAME}: missing or duplicate functions {", ".join(missing)}')
    return sorted({pin['file'] for table in (stage.PINS, stage.DEPENDENCIES) for pin in table.values()})


def current_pins(directory, names):
    """Address -> {sha256, file} of the named functions in a lifted directory.

    For regenerating a stage's pin table after the lifter pin changes; the
    caller reviews the result before committing it.
    """
    result = {}
    for path in sorted(Path(directory).glob('ppu_recomp_*.cpp')):
        for name, match in bodies(path.read_text(), set(names)).items():
            if name in result:
                raise HookError(f'duplicate lifted function {name}')
            result[name] = {'sha256': sha256_text(match[0]), 'file': path.name}
    missing = sorted(set(names) - set(result))
    if missing:
        raise HookError(f'missing lifted functions {", ".join(missing)}')
    return dict(sorted(result.items()))


def apply_patches(body, patches, what):
    for old, new in patches:
        body = replace_once(body, old, new, what)
    return body


def remove_patches(body, patches):
    for old, new in reversed(patches):
        body = body.replace(new, old)
    return body
