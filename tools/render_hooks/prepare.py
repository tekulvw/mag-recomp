"""Prepare a copy of a lifted engine with the render adapter's observer hooks.

    python3 tools/render_hooks/prepare.py --base generated/retail-v1.0/mage_g-ctr64 \\
        --output generated/retail-v1.0/mage_g-render --stage configuration --stage cohort

The base lift is left untouched and must be complete, unprepared and produced by
the pinned PPU lifter from the retail v1.0 engine image. The output directory
must be new. Its provenance.json keeps every base field, updates the hashes of
the rewritten sources and adds a `render_adapter_hooks` record that the CMake
option MAG_RENDER_ADAPTER checks. `--print-pins DIRECTORY` prints the current
pin tables of a lift, for review after the lifter pin changes.
"""
import argparse
import json
import os
import shutil
import sys
from pathlib import Path
if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.movie_picture_hooks import RETAIL_ELF
from tools.prepare_inputs import ROOT, sha256_file
from tools.provenance import validate_lift
from tools.render_hooks import common, configuration, cohort, controls, shadow_view, ui_handoff, update_loop
from tools.render_hooks.common import HookError

STAGES = {stage.NAME: stage for stage in (configuration, shadow_view, update_loop, cohort, ui_handoff, controls)}
REQUIRES = {'shadow-view': {'configuration'}, 'cohort': {'configuration', 'update-loop'}, 'ui-handoff': {'cohort'}}
RECORD = 'render_adapter_hooks'
SCHEMA_VERSION = 1


def tool_files():
    """Repository-relative paths of every file whose code shapes the hooks."""
    directory = Path(__file__).resolve().parent
    return sorted(path.relative_to(ROOT).as_posix() for path in directory.glob('*.py'))


def select(names):
    names = list(dict.fromkeys(names))
    unknown = [name for name in names if name not in STAGES]
    if not names or unknown:
        raise HookError('choose stages from ' + ', '.join(STAGES) + (f' (unknown: {", ".join(unknown)})' if unknown else ''))
    for name in names:
        missing = REQUIRES.get(name, set()) - set(names)
        if missing:
            raise HookError(f'stage {name} requires {", ".join(sorted(missing))}')
    # Always apply in the fixed order, whatever order was asked for.
    return [stage for name, stage in STAGES.items() if name in names]


def check_tables(stages):
    owners = {}
    for stage in stages:
        common.check_pins(stage)
        for name, pin in stage.PINS.items():
            if name in owners:
                other = owners[name]
                if name not in getattr(stage, 'SHARED', ()) or name not in getattr(other, 'SHARED', ()):
                    raise HookError(f'function {name} is instrumented by both {other.NAME} and {stage.NAME}')
                if pin != other.PINS[name]:
                    raise HookError(f'shared function {name} has different pins in {other.NAME} and {stage.NAME}')
                continue
            owners[name] = stage
    for stage in stages:
        for name in stage.DEPENDENCIES:
            if name in owners:
                raise HookError(f'{stage.NAME} dependency {name} is instrumented by {owners[name].NAME}')


def check_base(base):
    record = validate_lift(base, RETAIL_ELF)
    if record.get('sdk_revision') != common.LIFTER_REVISION:
        raise HookError(f'render hook pins are for PPU lifter {common.LIFTER_REVISION}, '
                        f'not {record.get("sdk_revision")}; regenerate them with --print-pins')
    if RECORD in record:
        raise HookError('base lift already has render adapter hooks; start from an unprepared lift')
    if any(path.is_symlink() for path in base.iterdir()):
        raise HookError('base lift must not contain symbolic links')
    return record


def prepare(base, output, names):
    base, output = Path(base).resolve(), Path(output).resolve()
    stages = select(names)
    check_tables(stages)
    record = check_base(base)
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise HookError(f'use a fresh output directory: {output}')
    partial = output.with_name(output.name + '.partial')
    if partial.exists():
        raise HookError(f'remove the stale partial output first: {partial}')
    shutil.copytree(base, partial)
    try:
        changed = set()
        for stage in stages:
            for filename in common.locate(partial, stage):
                path = partial / filename
                source = path.read_text()
                others = [other for other in stages if other is not stage]
                result = common.instrument_source(source, stage, others=others)
                if common.instrument_source(result, stage, others=others) != result:
                    raise HookError(f'{stage.NAME}: hooks are not idempotent in {filename}')
                if result != source:
                    path.write_text(result)
                    changed.add(filename)
        for filename in sorted(changed):
            record['outputs'][filename] = sha256_file(partial / filename)
        record[RECORD] = {
            'schema_version': SCHEMA_VERSION,
            'stages': [stage.NAME for stage in stages],
            'lifter_revision': common.LIFTER_REVISION,
            'base_provenance_sha256': sha256_file(base / 'provenance.json'),
            'tools': {name: sha256_file(ROOT / name) for name in tool_files()},
            'functions': {stage.NAME: sorted(stage.PINS) for stage in stages},
            'dependencies': {stage.NAME: sorted(stage.DEPENDENCIES) for stage in stages},
            'changed_outputs': sorted(changed),
            'meaning': 'Guarded observer hooks only; removing them restores every original body',
        }
        (partial / 'provenance.json').write_text(json.dumps(record, indent=2) + '\n')
        validate_lift(partial, RETAIL_ELF)
        if output.exists():
            output.rmdir()
        os.replace(partial, output)
    except BaseException:
        shutil.rmtree(partial, ignore_errors=True)
        raise
    return record


def print_pins(directory, names):
    directory = Path(directory)
    if RECORD in json.loads((directory / 'provenance.json').read_text()):
        raise HookError('print pins from an unprepared lift')
    stages = [stage for name, stage in STAGES.items() if not names or name in names]
    tables = {}
    for stage in stages:
        tables[stage.NAME] = {
            'functions': common.current_pins(directory, stage.PINS),
            'dependencies': common.current_pins(directory, stage.DEPENDENCIES),
        }
    print(json.dumps(tables, indent=2))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--base', type=Path, help='complete unprepared engine lift')
    parser.add_argument('--output', type=Path, help='new directory for the prepared copy')
    parser.add_argument('--stage', action='append', default=[], choices=list(STAGES))
    parser.add_argument('--print-pins', type=Path, metavar='DIRECTORY',
                        help='print the pin tables computed from a lift instead of preparing')
    args = parser.parse_args(argv)
    try:
        if args.print_pins:
            print_pins(args.print_pins, args.stage)
            return 0
        if not args.base or not args.output:
            parser.error('--base and --output are required')
        record = prepare(args.base, args.output, args.stage)
    except ValueError as error:
        print(f'render hooks refused: {error}', file=sys.stderr)
        return 1
    print(f'Prepared {args.output} with stages {", ".join(record[RECORD]["stages"])}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
