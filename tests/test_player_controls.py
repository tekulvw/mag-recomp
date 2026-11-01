"""Portable input contracts with synthetic events; no game data or window."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PlayerControlsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('C++20 compiler required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='mag-controls-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name)/'controls'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(ROOT/'src'), str(ROOT/'tests/player_controls.cpp'),
                        str(ROOT/'src/player_controls.cpp'), '-o', str(cls.binary)], check=True)

    def test_device_policy_and_look_arbitration(self):
        subprocess.run([str(self.binary), 'policy'], check=True)

    def test_mouse_displacement_and_mode_gains(self):
        subprocess.run([str(self.binary), 'mouse'], check=True)

    def test_controller_transfer_function(self):
        subprocess.run([str(self.binary), 'controller'], check=True)

    def test_current_frame_and_once_only_consumption(self):
        subprocess.run([str(self.binary), 'frames'], check=True)

    def test_binding_aggregation_and_source_filter(self):
        subprocess.run([str(self.binary), 'bindings'], check=True)

    def test_focus_release_and_fresh_activation(self):
        subprocess.run([str(self.binary), 'focus'], check=True)

    def test_disconnect_preserves_other_binding_and_rejects_late_input(self):
        subprocess.run([str(self.binary), 'device'], check=True)

    def test_captured_motion_with_zero_net_displacement(self):
        subprocess.run([str(self.binary), 'motion'], check=True)

    def test_primary_secondary_validation_preserves_active_inputs(self):
        subprocess.run([str(self.binary), 'validation'], check=True)

    def test_hold_and_toggle_bindings_across_sources(self):
        subprocess.run([str(self.binary), 'toggle'], check=True)

    def test_raw_controller_axes_and_lifecycle(self):
        subprocess.run([str(self.binary), 'axes'], check=True)

    def test_disconnect_preserves_queued_cross_source_order(self):
        subprocess.run([str(self.binary), 'device-order'], check=True)

    def test_physical_poll_and_semantic_attachment_are_separate(self):
        subprocess.run([str(self.binary), 'sample-phases'], check=True)

    def test_skipped_interpretation_and_focus_invalidation(self):
        subprocess.run([str(self.binary), 'sample-lifecycle'], check=True)

    def test_sealed_input_is_not_reinterpreted_in_a_new_context(self):
        subprocess.run([str(self.binary), 'sample-contexts'], check=True)

    def test_source_revocation_between_poll_and_interpretation(self):
        subprocess.run([str(self.binary), 'sample-revocation'], check=True)

    def test_capture_loss_cancels_sealed_mouse_policy_inputs(self):
        subprocess.run([str(self.binary), 'sample-mouse-revocation'], check=True)

    def test_mode_permission_revocation_precedes_next_sample(self):
        subprocess.run([str(self.binary), 'configure-policy'], check=True)

    def test_native_conditioned_neutral_is_attached_without_another_poll(self):
        subprocess.run([str(self.binary), 'native-neutral'], check=True)

    def test_raw_fractional_mouse_motion_is_not_quantized(self):
        subprocess.run([str(self.binary), 'precise-mouse'], check=True)

    def test_window_and_device_producers_preserve_each_others_state(self):
        subprocess.run([str(self.binary), 'capture-producers'], check=True)

    def test_keyboard_and_mouse_disconnect_release_only_their_source(self):
        subprocess.run([str(self.binary), 'source-disconnect'], check=True)

    def test_action_transitions_preserve_physical_timestamps_and_short_pulses(self):
        subprocess.run([str(self.binary), 'action-time'], check=True)


if __name__ == '__main__':
    unittest.main()
