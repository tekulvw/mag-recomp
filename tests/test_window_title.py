"""The engine's default window title is shared by every platform backend, so it names no platform."""
import re
import unittest
from pathlib import Path

MAIN = Path(__file__).resolve().parents[1] / 'src' / 'main.cpp'


class WindowTitleTest(unittest.TestCase):
    def test_default_title_is_platform_neutral(self):
        source = MAIN.read_text(encoding='utf-8')
        defaults = re.findall(r'if \(!title \|\| !\*title\) title = "([^"]*)";', source)
        self.assertEqual(defaults, ['MAG v1.0'])
        for platform in ('macOS', 'Windows', 'Linux', 'Metal', 'Vulkan', 'D3D12'):
            self.assertNotIn(platform, defaults[0])


if __name__ == '__main__':
    unittest.main()
