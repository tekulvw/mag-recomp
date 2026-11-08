"""The Windows supervisor compiles from a config without packaged runtime defaults."""
import unittest
from mag_installer import native_package_windows as windows


class WindowsRuntimeDefaultsTests(unittest.TestCase):
    def test_runtime_defaults_have_a_fallback_for_older_configs(self):
        # Smoke builds write a config without MAG_RUNTIME_DEFAULTS; the supervisor must still compile.
        source = windows._SUPERVISOR
        self.assertLess(source.index('#ifndef MAG_RUNTIME_DEFAULTS'), source.index('= MAG_RUNTIME_DEFAULTS;'))
        self.assertIn('#define MAG_RUNTIME_DEFAULTS {{NULL, NULL}}', source)
        loop = source[source.index('= MAG_RUNTIME_DEFAULTS;'):]
        self.assertIn('defaults[i][0]', loop.split('\n')[1])  # the loop stops at the NULL entry


if __name__ == '__main__':
    unittest.main()
