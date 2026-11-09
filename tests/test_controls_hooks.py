"""Controls hook stage on synthetic lifted sources: seams, guards and the adapter's event list."""
import re
import unittest
from pathlib import Path

from tests.test_render_hooks import synthetic_lift
from tools.render_hooks import common, controls, prepare

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'src/game_controls/mag_game_controls_hooks.h'


def hooked_body(name):
    files, pins, dependencies = synthetic_lift(controls)
    source = files[pins[name]['file']]
    return common.bodies(common.instrument_source(source, controls, pins, dependencies), {name})[name][0]


class ControlsStage(unittest.TestCase):
    def test_every_event_is_an_adapter_event(self):
        enum = re.search(r'enum class MagControlsHook : uint8_t \{(.*?)\};', HEADER.read_text(), re.S)[1]
        declared = set(re.findall(r'[A-Za-z]+', enum))
        used = {event for pair in controls.SCOPES.values() for event in pair} | set(controls.EARLY.values())
        used |= {event for points in controls.POINTS.values() for event, *_ in points}
        self.assertLessEqual(used, declared)

    def test_instrumented_functions_are_exactly_the_pinned_ones(self):
        self.assertEqual(set(controls.SCOPES) | set(controls.EARLY) | set(controls.POINTS), set(controls.PINS))
        self.assertFalse(set(controls.SCOPES) & set(controls.EARLY))

    def test_scope_wraps_the_whole_function(self):
        body = hooked_body('003A2C84')
        first = body.split('\n')
        self.assertEqual(first[0], common.signature('003A2C84'))
        self.assertIn('MagGameControlsHookScope mag_game_controls_scope(MagControlsHook::HandlerEnter, '
                      'MagControlsHook::HandlerExit, ctx);', first[2])

    def test_early_result_returns_before_the_original_body(self):
        body = hooked_body('004E42BC')
        hook = body.index('if (mag_game_controls_hook(MagControlsHook::AssistEligibility, ctx)) return;')
        self.assertLess(hook, body.index('synthetic_prologue'))

    def test_recognition_continues_at_its_label_and_keeps_the_original_call(self):
        body = hooked_body('003A2C84')
        hook = body.index('if (mag_game_controls_hook(MagControlsHook::ControlRecognize, ctx)) goto loc_003A2E58;')
        self.assertLess(hook, body.index(controls.call('003A2DA0', '0039FBE4')))
        self.assertIn('\nloc_003A2E58:\n', body)

    def test_points_sit_on_the_named_side_of_their_statement(self):
        for name, points in controls.POINTS.items():
            body = hooked_body(name)
            for event, placement, statement, _ in points:
                with self.subTest(event=event):
                    hook, at = body.index(f'MagControlsHook::{event}, ctx)'), body.index(statement)
                    self.assertEqual(hook < at, placement == 'before')

    def test_missing_continuation_label_is_refused(self):
        name = '003A2C84'
        body = hooked_body(name)
        stripped = controls.strip_body(body, name).replace('\nloc_003A2E58:\n', '\n')
        with self.assertRaisesRegex(common.HookError, 'continuation loc_003A2E58'):
            controls.instrument_body(stripped, name)

    def test_shared_update_tick_matches_the_render_stages(self):
        for stage in (prepare.STAGES['update-loop'], prepare.STAGES['cohort']):
            self.assertEqual(stage.PINS['00013C54'], controls.PINS['00013C54'])
            self.assertIn('00013C54', stage.SHARED)


if __name__ == '__main__':
    unittest.main()
