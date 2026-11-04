"""Render adapter observer hooks on synthetic lifted sources: pins, seams and preparation."""
import hashlib
import json
import re
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.movie_picture_hooks import RETAIL_ELF
from tools.render_hooks import common, configuration, cohort, prepare, shadow_view, ui_handoff
from tools.render_hooks.common import HookError

STAGES = list(prepare.STAGES.values())
GUARD_LINE = re.compile(r'\n#if [^\n]*\n.*?\n#endif\n', re.S)


def synthetic_body(stage, name):
    """A made-up body holding every seam the stage needs for `name`, once each."""
    entry = common.signature(name)
    lines = [entry, f'        synthetic_prologue(ctx, 0x{name});']
    for old, _ in stage.patches(name):
        if old != entry:
            lines += ['        synthetic_step(ctx);', old.rstrip('\n')]
    for lr, target, _ in getattr(stage, 'CALLS', {}).get(name, []):
        lines += [f'        ctx->lr = 0x{lr}; func_{target}(ctx); DRAIN_TRAMPOLINE(ctx);', '        synthetic_after(ctx);']
    lines += ['        return;', '}']
    return '\n'.join(lines)


def dependency_body(name):
    return f'{common.signature(name)}\n        synthetic_dependency(ctx);\n        return;\n}}'


def synthetic_lift(stage):
    """Files and pin tables for one stage, laid out as its real pins say."""
    files, pins, dependencies = {}, {}, {}
    for table, make, out in ((stage.PINS, lambda n: synthetic_body(stage, n), pins),
                             (stage.DEPENDENCIES, dependency_body, dependencies)):
        for name, pin in table.items():
            body = make(name)
            files.setdefault(pin['file'], ['#include "ppu_recomp.h"'])
            files[pin['file']].append(body)
            out[name] = {'sha256': hashlib.sha256(body.encode()).hexdigest(), 'file': pin['file']}
    return {name: '\n\n'.join(parts) + '\n' for name, parts in files.items()}, pins, dependencies


class StageTables(unittest.TestCase):
    def test_committed_pin_tables_are_well_formed_and_disjoint(self):
        prepare.check_tables(STAGES)
        self.assertEqual(len({stage.NAME for stage in STAGES}), len(STAGES))

    def test_role_tables_cover_exactly_the_instrumented_functions(self):
        self.assertEqual(set(configuration.SCOPES), set(configuration.PINS))
        self.assertEqual(set(ui_handoff.SCOPES), set(ui_handoff.PINS))
        self.assertEqual(len(shadow_view.PINS), 1)
        self.assertEqual(set(cohort.CALLS) | set(cohort.ENTRIES) | set(cohort.RETURNS)
                         | {cohort.SORT_SELECTED, cohort.FOG_PARAMETERS}, set(cohort.PINS))

    def test_every_hook_line_is_guarded_by_its_stage(self):
        for stage in STAGES:
            self.assertTrue(stage.INCLUDE.startswith('#if ' + stage.GUARD + '\n'))
            for name in stage.PINS:
                for _, new in stage.patches(name):
                    for block in GUARD_LINE.findall(new):
                        self.assertTrue(block.startswith('\n#if ' + stage.GUARD + '\n'), (stage.NAME, name))


class InstrumentSource(unittest.TestCase):
    def test_each_stage_hooks_strip_back_to_the_original_and_are_idempotent(self):
        for stage in STAGES:
            files, pins, dependencies = synthetic_lift(stage)
            for filename, source in files.items():
                with self.subTest(stage=stage.NAME, file=filename):
                    hooked = common.instrument_source(source, stage, pins, dependencies)
                    self.assertEqual(common.instrument_source(hooked, stage, pins, dependencies), hooked)
                    restored = GUARD_LINE.sub('', hooked.replace(stage.INCLUDE, ''))
                    names = [n for n, p in pins.items() if p['file'] == filename]
                    if names:
                        self.assertTrue(hooked.startswith(stage.INCLUDE))
                        self.assertNotEqual(hooked, source)
                    else:
                        self.assertEqual(hooked, source)
                    for name in names:
                        self.assertEqual(stage.strip_body(common.bodies(hooked, {name})[name][0], name),
                                         common.bodies(source, {name})[name][0])
                    # Removing every guarded block leaves only original statements.
                    original_lines = [line for line in source.split('\n') if line.strip()]
                    self.assertEqual([line for line in restored.split('\n') if line.strip()], original_lines)

    def test_cohort_scopes_keep_the_complete_original_call_inside(self):
        files, pins, dependencies = synthetic_lift(cohort)
        hooked = common.instrument_source(files[pins['00D10EB8']['file']], cohort, pins, dependencies)
        body = common.bodies(hooked, {'00D10EB8'})['00D10EB8'][0]
        for lr, target, role in cohort.CALLS['00D10EB8']:
            opened = body.index(f'MagRenderCohortScope mag_render_cohort_{lr}(ctx,MagRenderCohortRole::{role});')
            call = body.index(f'func_{target}(ctx); DRAIN_TRAMPOLINE(ctx);', opened)
            closed = body.index('        }', call)
            self.assertLess(opened, call)
            self.assertLess(call, closed)

    def test_ui_word_writer_reports_around_its_final_header_store(self):
        files, pins, dependencies = synthetic_lift(ui_handoff)
        hooked = common.instrument_source(files[pins['00982504']['file']], ui_handoff, pins, dependencies)
        self.assertLess(hooked.index('before_header'), hooked.index(ui_handoff.WORD_DATA_STORE))
        self.assertLess(hooked.index(ui_handoff.WORD_HEADER_STORE), hooked.index('after_header'))

    def test_changed_function_or_dependency_is_refused(self):
        files, pins, dependencies = synthetic_lift(configuration)
        filename = pins['00E8155C']['file']
        changed = files[filename].replace('synthetic_prologue(ctx, 0x00E8155C);', 'synthetic_prologue(ctx, 1);')
        with self.assertRaisesRegex(HookError, 'does not match its pin'):
            common.instrument_source(changed, configuration, pins, dependencies)
        name = next(iter(dependencies))
        source = files[dependencies[name]['file']].replace('synthetic_dependency(ctx);', 'other(ctx);', 1)
        with self.assertRaisesRegex(HookError, 'dependency .* does not match'):
            common.instrument_source(source, configuration, pins, dependencies)

    def test_missing_or_repeated_seam_is_refused(self):
        body = synthetic_body(ui_handoff, '00982504')
        twice = body.replace(ui_handoff.WORD_HEADER_STORE, ui_handoff.WORD_HEADER_STORE + '\n' + ui_handoff.WORD_HEADER_STORE)
        for broken in (body.replace(ui_handoff.WORD_HEADER_STORE, '        other_store();'), twice):
            pins = {'00982504': {'sha256': hashlib.sha256(broken.encode()).hexdigest(), 'file': 'ppu_recomp_004.cpp'}}
            with self.assertRaisesRegex(HookError, 'not unique'):
                common.instrument_source(broken + '\n', ui_handoff, pins, {})
        far = synthetic_body(cohort, '00D11810').replace(
            'ctx->lr = 0x00D11834; ', 'ctx->lr = 0x00D11834;' + ' ' * (cohort.CALL_WINDOW + 1))
        pins = {'00D11810': {'sha256': hashlib.sha256(far.encode()).hexdigest(), 'file': 'ppu_recomp_005.cpp'}}
        with self.assertRaisesRegex(HookError, 'no complete call'):
            common.instrument_source(far + '\n', cohort, pins, {})

    def test_duplicate_function_is_refused(self):
        body = dependency_body('00E8155C')
        with self.assertRaisesRegex(HookError, 'duplicate'):
            common.bodies(body + '\n' + body + '\n', {'00E8155C'})


class Prepare(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='mag-render-hooks-'))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.base = self.tmp / 'base'
        self.base.mkdir()
        tables = {}
        for stage in STAGES:
            files, pins, dependencies = synthetic_lift(stage)
            for name, text in files.items():
                path = self.base / name
                path.write_text((path.read_text() + '\n' if path.exists() else '') + text)
            tables[stage] = (pins, dependencies)
        # Bodies are hashed per function, so files shared between stages still match.
        for stage, (pins, dependencies) in tables.items():
            self.enterContext(mock.patch.object(stage, 'PINS', pins))
            self.enterContext(mock.patch.object(stage, 'DEPENDENCIES', dependencies))
        (self.base / 'ppu_recomp_999.cpp').write_text('void func_00000001(ppu_context* ctx) {\n}\n')
        (self.base / 'ppu_recomp.h').write_text('#pragma once\n')
        self.write_provenance(self.base)

    def write_provenance(self, directory, **extra):
        outputs = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                   for pattern in ('*.c', '*.cpp', '*.h') for p in sorted(directory.glob(pattern))}
        record = dict(status='complete', exit_code=0, audit_exit=0, elf_sha256=RETAIL_ELF,
                      sdk_revision=common.LIFTER_REVISION, outputs=outputs, **extra)
        (directory / 'provenance.json').write_text(json.dumps(record))

    def test_prepares_a_verified_copy_and_leaves_the_base_untouched(self):
        before = {p.name: p.read_bytes() for p in self.base.iterdir()}
        output = self.tmp / 'out'
        record = prepare.prepare(self.base, output, ['ui-handoff', 'configuration', 'cohort', 'shadow-view'])
        self.assertEqual({p.name: p.read_bytes() for p in self.base.iterdir()}, before)
        hooks = record['render_adapter_hooks']
        self.assertEqual(hooks['stages'], ['configuration', 'shadow-view', 'cohort', 'ui-handoff'])
        self.assertEqual(set(hooks['tools']), set(prepare.tool_files()))
        self.assertTrue(all(name.startswith('tools/render_hooks/') for name in hooks['tools']))
        self.assertNotIn('ppu_recomp_999.cpp', hooks['changed_outputs'])
        saved = json.loads((output / 'provenance.json').read_text())
        self.assertEqual(saved, record)
        for name, digest in saved['outputs'].items():
            self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), digest)
        self.assertFalse((self.tmp / 'out.partial').exists())

    def test_refuses_and_leaves_nothing_on_a_mismatch(self):
        path = self.base / cohort.PINS['00D116B8']['file']
        path.write_text(path.read_text().replace('synthetic_prologue(ctx, 0x00D116B8);', 'changed(ctx);'))
        self.write_provenance(self.base)
        with self.assertRaisesRegex(HookError, '00D116B8 does not match'):
            prepare.prepare(self.base, self.tmp / 'out', ['configuration', 'cohort'])
        self.assertFalse((self.tmp / 'out').exists())
        self.assertFalse((self.tmp / 'out.partial').exists())

    def test_refuses_a_function_in_another_file(self):
        name = next(iter(configuration.PINS))
        moved = dict(configuration.PINS, **{name: dict(configuration.PINS[name], file='ppu_recomp_999.cpp')})
        with mock.patch.object(configuration, 'PINS', moved), self.assertRaisesRegex(HookError, 'moved to'):
            prepare.prepare(self.base, self.tmp / 'out', ['configuration'])

    def test_refuses_other_lifter_prepared_base_stale_output_and_bad_stage_sets(self):
        output = self.tmp / 'out'
        for stages, message in ((['cohort'], 'requires configuration'), ([], 'choose stages'),
                                (['shadow'], 'unknown: shadow')):
            with self.assertRaisesRegex(HookError, message):
                prepare.prepare(self.base, output, stages)
        output.mkdir()
        (output / 'leftover').write_text('x')
        with self.assertRaisesRegex(HookError, 'fresh output'):
            prepare.prepare(self.base, output, ['configuration'])
        self.write_provenance(self.base, render_adapter_hooks={})
        with self.assertRaisesRegex(HookError, 'already has render adapter hooks'):
            prepare.prepare(self.base, self.tmp / 'other', ['configuration'])
        self.write_provenance(self.base)
        record = json.loads((self.base / 'provenance.json').read_text())
        record['sdk_revision'] = '0' * 40
        (self.base / 'provenance.json').write_text(json.dumps(record))
        with self.assertRaisesRegex(HookError, 'regenerate them with --print-pins'):
            prepare.prepare(self.base, self.tmp / 'other', ['configuration'])

    def test_refuses_sources_changed_since_lifting(self):
        (self.base / 'ppu_recomp_999.cpp').write_text('changed\n')
        with self.assertRaisesRegex(ValueError, 'identity mismatch'):
            prepare.prepare(self.base, self.tmp / 'out', ['configuration'])

    def test_print_pins_reproduces_the_tables(self):
        with mock.patch('builtins.print') as printed:
            prepare.print_pins(self.base, ['cohort'])
        tables = json.loads(printed.call_args.args[0])
        self.assertEqual(tables['cohort']['functions'], dict(sorted(cohort.PINS.items())))
        self.assertEqual(tables['cohort']['dependencies'], dict(sorted(cohort.DEPENDENCIES.items())))


if __name__ == '__main__':
    unittest.main()
