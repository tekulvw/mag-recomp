"""Windows headers define min and max as macros unless NOMINMAX comes first, and then std::min and
std::max no longer compile ("expected unqualified-id"). The Windows player build is the only one
that sees those headers, and CI does not build it, so check the sources: a file that includes a
Windows header and uses std::min or std::max defines NOMINMAX before its first Windows header."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
WINDOWS_HEADER = re.compile(r'^\s*#\s*include\s*<(windows|winsock2|ws2tcpip|mstcpip|winsock)\.h>', re.M | re.I)
NOMINMAX = re.compile(r'^\s*#\s*define\s+NOMINMAX\b', re.M)
STD_MINMAX = re.compile(r'\bstd::(min|max)\b')


class WindowsMinMaxTests(unittest.TestCase):
    def test_nominmax_before_windows_headers(self):
        missing = []
        for path in sorted((ROOT / 'src').rglob('*')):
            if path.suffix not in ('.c', '.cc', '.cpp', '.h', '.hpp') or not path.is_file():
                continue
            text = path.read_text(errors='replace')
            header = WINDOWS_HEADER.search(text)
            if not header or not STD_MINMAX.search(text):
                continue
            define = NOMINMAX.search(text)
            if not define or define.start() > header.start():
                missing.append(str(path.relative_to(ROOT)))
        self.assertEqual(missing, [], 'define NOMINMAX before the first Windows header in these files')


if __name__ == '__main__':
    unittest.main()
