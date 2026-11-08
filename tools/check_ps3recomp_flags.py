"""Keep docs/ps3recomp-flags.md in step with the ps3recomp switches mag-recomp sets.

Extracts every runtime switch this repository sets from its sources, then compares the names,
the literal values and the files that set them with the document's tables. Exits 1 if the
document is missing a switch, lists one nothing sets any more, gives a value or file the sources
no longer have, or lists as "relied on" (left at the runtime default) a switch mag-recomp sets.

    python3 tools/check_ps3recomp_flags.py                    # check the tree
    python3 tools/check_ps3recomp_flags.py --list             # print every switch found and where
    python3 tools/check_ps3recomp_flags.py --runtime local/ps3recomp-runtime
                                                              # also check names against a runtime

What counts as setting a switch:
  * C, C++ and Objective-C under src/: setenv, _putenv_s, _wputenv_s, SetEnvironmentVariableA/W,
    export_variable, set_env and set_env_utf8 called with a literal name; {"NAME", value} pairs
    (the runtime default tables); ObjC @"NAME": value dictionary entries; env["NAME"] = value.
  * Python under tools/ and installer/mag_installer/: x['NAME'] = value, x.update(NAME=...),
    x.setdefault('NAME', ...), dict(..., NAME=...), dictionary literals bound to a name containing
    "env", "-DNAME=value" CMake arguments (string concatenations are evaluated, with the loop
    variables of enclosing `for name in (<literals>)` loops expanded), and the C patterns above
    inside string literals (the packaged supervisors are C, C++ and Objective-C kept in Python).
  * CMake (CMakeLists.txt, cmake/*.cmake): set(NAME value CACHE ...), and compile definitions.

Only names the runtime owns count: environment names starting with one of RUNTIME_PREFIXES or
listed in RUNTIME_NAMES, and CMake cache entries and definitions starting with PS3RECOMP_ (or,
for definitions, a runtime prefix). MAG_* variables are mag-recomp's own and are not listed.

The document's tables have one row per switch, `| `NAME` | kind | value | platforms | set in |
why | opt out |`, with kind env, CMake or define. Rows under a heading containing "relies on"
are runtime defaults mag-recomp leaves alone; rows under a heading containing "not read" are
switches mag-recomp sets that the pinned runtime no longer reads (--runtime checks both).
"""
from __future__ import annotations

import argparse
import ast
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = Path('docs/ps3recomp-flags.md')

RUNTIME_PREFIXES = ('PS3RECOMP_', 'PS3_', 'SPU_', 'SPURS_', 'RSX_', 'PPU_', 'GCM_')
# The runtime reads these although their prefix says otherwise.
RUNTIME_NAMES = {'MAG_NATIVE_CONSUMER'}
# mag-recomp's own variables with a runtime-looking prefix: where the runtime checkout is, and the
# DXC library the native D3D12 renderer (crates/mag-render-native) loads.
NOT_SWITCHES = {'PS3RECOMP_DIR', 'PS3RECOMP_DXC_LIBRARY'}

C_DIRS = ('src',)
C_SUFFIXES = {'.c', '.cc', '.cpp', '.h', '.hpp', '.m', '.mm'}
PY_DIRS = ('tools', 'installer/mag_installer')
CMAKE_GLOBS = ('CMakeLists.txt', 'cmake/*.cmake')

NAME = r'[A-Z][A-Z0-9_]*'
C_SETTERS = ('setenv', '_putenv_s', '_wputenv_s', 'SetEnvironmentVariableA', 'SetEnvironmentVariableW',
             'export_variable', 'set_env', 'set_env_utf8')
_C_STRING = r'(?:L|@)?"((?:\\.|[^"\\\n])*)"'
C_PATTERNS = (
    # A setter call: setenv("NAME", value, ...)
    re.compile(r'\b(?:' + '|'.join(C_SETTERS) + r')\s*\(\s*L?"(' + NAME + r')"\s*,\s*([^,()]*(?:\([^()]*\)[^,()]*)*)'),
    # A {"NAME", value} pair: the default tables and the supervisors' environment maps.
    re.compile(r'\{\s*L?"(' + NAME + r')"\s*,\s*([^{},]*?)\s*\}'),
    # An Objective-C dictionary entry: @"NAME": value
    re.compile(r'@"(' + NAME + r')"\s*:\s*([^,{}]*?)\s*(?=[,}])'),
    # An assignment through a subscript: env["NAME"] = value, env[@"NAME"] = value
    re.compile(r'\[\s*(?:L|@)?"(' + NAME + r')"\s*\]\s*=(?!=)\s*([^;\n]*)'),
)
DEFINE = re.compile(r'^-D(' + NAME + r')=(.*)$', re.S)


@dataclass(frozen=True)
class Setting:
    kind: str           # env, CMake or define
    name: str
    value: str | None   # a literal value, or None when it is computed
    path: str           # repository-relative file that sets it


def runtime_env_name(name: str) -> bool:
    if name in NOT_SWITCHES:
        return False
    return name in RUNTIME_NAMES or any(name.startswith(p) and len(name) > len(p) for p in RUNTIME_PREFIXES)


def runtime_cmake_name(name: str) -> bool:
    return name.startswith('PS3RECOMP_') and name not in NOT_SWITCHES


def strip_c_comments(text: str) -> str:
    """Blank C/C++ comments, keeping string and character literals and line numbers intact."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif c == '/' and text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def c_value(expression: str) -> str | None:
    m = re.fullmatch(r'\s*' + _C_STRING + r'\s*', expression)
    return m.group(1) if m else None


def scan_c_text(text: str, rel: str) -> set[Setting]:
    text = strip_c_comments(text)
    found = set()
    for pattern in C_PATTERNS:
        for m in pattern.finditer(text):
            if runtime_env_name(m.group(1)):
                found.add(Setting('env', m.group(1), c_value(m.group(2)), rel))
    return found


def _flatten_add(node):
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        return _flatten_add(node.left) + _flatten_add(node.right)
    return [node]


def _evaluate(node, bindings):
    """A string for literals, bound loop names and their .upper()/.lower(); otherwise None."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.Name) and node.id in bindings:
        return bindings[node.id]
    if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and not node.args
            and node.func.attr in ('upper', 'lower')):
        inner = _evaluate(node.func.value, bindings)
        return None if inner is None else getattr(inner, node.func.attr)()
    return None


class _PythonScanner(ast.NodeVisitor):
    def __init__(self, rel: str):
        self.rel, self.found, self.bindings = rel, set(), [{}]
        self.in_add = 0

    def env(self, name, value_node):
        if isinstance(name, str) and runtime_env_name(name):
            value = value_node.value if isinstance(value_node, ast.Constant) and isinstance(value_node.value, str) else None
            self.found.add(Setting('env', name, value, self.rel))

    def define(self, text):
        m = DEFINE.match(text)
        if m and runtime_cmake_name(m.group(1)) and '\0' not in m.group(1):
            value = m.group(2)
            self.found.add(Setting('CMake', m.group(1), None if '\0' in value or not value else value, self.rel))

    def visit_For(self, node):
        literals = node.iter.elts if isinstance(node.iter, (ast.Tuple, ast.List)) else None
        if (isinstance(node.target, ast.Name) and literals
                and all(isinstance(e, ast.Constant) and isinstance(e.value, str) for e in literals)):
            outer = self.bindings
            self.bindings = [dict(b, **{node.target.id: e.value}) for b in outer for e in literals]
            for statement in node.body:
                self.visit(statement)
            self.bindings = outer
            for statement in node.orelse:
                self.visit(statement)
        else:
            self.generic_visit(node)

    def visit_Assign(self, node):
        for target in node.targets:
            if isinstance(target, ast.Subscript) and isinstance(target.slice, ast.Constant):
                self.env(target.slice.value, node.value)
            if isinstance(target, ast.Name) and 'env' in target.id.lower() and isinstance(node.value, ast.Dict):
                for key, value in zip(node.value.keys, node.value.values):
                    if isinstance(key, ast.Constant):
                        self.env(key.value, value)
        self.generic_visit(node)

    def visit_Call(self, node):
        func = node.func
        if (isinstance(func, ast.Attribute) and func.attr == 'update') or (isinstance(func, ast.Name) and func.id == 'dict'):
            for keyword in node.keywords:
                if keyword.arg:
                    self.env(keyword.arg, keyword.value)
        if isinstance(func, ast.Attribute) and func.attr == 'setdefault' and len(node.args) == 2 \
                and isinstance(node.args[0], ast.Constant):
            self.env(node.args[0].value, node.args[1])
        self.generic_visit(node)

    def visit_BinOp(self, node):
        if isinstance(node.op, ast.Add) and not self.in_add:
            parts = _flatten_add(node)
            for bindings in self.bindings:
                pieces = [_evaluate(p, bindings) for p in parts]
                if isinstance(pieces[0], str) and pieces[0].startswith('-D'):
                    self.define(''.join('\0' if p is None else p for p in pieces))
            self.in_add += 1
            self.generic_visit(node)
            self.in_add -= 1
        else:
            self.generic_visit(node)

    def visit_Constant(self, node):
        if isinstance(node.value, str):
            if not self.in_add:
                self.define(node.value)
            self.found |= scan_c_text(node.value, self.rel)


def scan_python_text(text: str, rel: str) -> set[Setting]:
    scanner = _PythonScanner(rel)
    scanner.visit(ast.parse(text, filename=rel))
    return scanner.found


def scan_cmake_text(text: str, rel: str) -> set[Setting]:
    text = re.sub(r'#[^\n]*', '', text)
    found = set()
    for m in re.finditer(r'\bset\s*\(\s*(' + NAME + r')\s+("[^"]*"|\S+)\s+CACHE\s+(?:BOOL|STRING|PATH|FILEPATH)\b', text, re.I):
        if runtime_cmake_name(m.group(1)):
            found.add(Setting('CMake', m.group(1), m.group(2).strip('"'), rel))
    for m in re.finditer(r'\b(?:COMPILE_DEFINITIONS|target_compile_definitions\s*\([^)]*?(?:PRIVATE|PUBLIC|INTERFACE))'
                         r'\s+([^)]*)', text, re.I):
        for token in m.group(1).split():
            name, _, value = token.strip('"').partition('=')
            if re.fullmatch(NAME, name) and (runtime_cmake_name(name) or runtime_env_name(name)):
                found.add(Setting('define', name, value or None, rel))
    return found


def _files(root: Path, dirs, suffixes, recursive=True):
    for top in dirs:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob('*') if recursive else base.glob('*')):
            if path.is_file() and path.suffix in suffixes:
                rel = path.relative_to(root).as_posix()
                parts = rel.split('/')
                if 'tests' not in parts and not parts[-1].startswith('test_'):
                    yield rel, path


def scan(root: Path) -> set[Setting]:
    found = set()
    for rel, path in _files(root, C_DIRS, C_SUFFIXES):
        found |= scan_c_text(path.read_text(encoding='utf-8', errors='replace'), rel)
    for rel, path in _files(root, PY_DIRS, {'.py'}, recursive=False):
        if path.name != Path(__file__).name:
            found |= scan_python_text(path.read_text(encoding='utf-8'), rel)
    seen = set()
    for pattern in CMAKE_GLOBS:
        for path in sorted(root.glob(pattern)):
            if path.is_file() and path not in seen:
                seen.add(path)
                found |= scan_cmake_text(path.read_text(encoding='utf-8', errors='replace'), path.relative_to(root).as_posix())
    return found


@dataclass
class Row:
    name: str
    kind: str
    section: str        # set, relied or unread
    cells: list[str]    # the cells after the kind
    line: int


def documented(text: str) -> list[Row]:
    rows, section = [], 'set'
    row = re.compile(r'^\|\s*`(' + NAME + r')`\s*\|\s*(env|CMake|define)\s*\|(.*)$')
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith('## '):
            heading = line.lower()
            section = 'relied' if 'relies on' in heading else 'unread' if 'not read' in heading else 'set'
            continue
        m = row.match(line)
        if m:
            rows.append(Row(m.group(1), m.group(2), section, [c.strip() for c in m.group(3).split('|')], number))
    return rows


def runtime_words(runtime: Path) -> tuple[set[str], set[tuple[str, str]]]:
    """Every upper-case word in the runtime's non-test sources, and the (prefix, suffix) pairs
    of names it builds in CMake, such as PS3RECOMP_FFMPEG_${component}_LIBRARY."""
    words, templates = set(), set()
    suffixes = C_SUFFIXES | {'.py', '.cmake', '.txt', '.inc'}
    for path in runtime.rglob('*'):
        rel = path.relative_to(runtime).parts
        if (path.is_file() and path.suffix in suffixes and 'tests' not in rel and not rel[-1].startswith('test_')
                and (path.suffix != '.txt' or path.name == 'CMakeLists.txt')):
            text = path.read_text(encoding='utf-8', errors='replace')
            words.update(re.findall(r'\b' + NAME + r'\b', text))
            templates.update(re.findall(r'\b(' + NAME + r')\$\{[A-Za-z_][A-Za-z0-9_]*\}([A-Z0-9_]*)', text))
    return words, templates


def named(name: str, words, templates) -> bool:
    return name in words or any(name.startswith(p) and name.endswith(s) and len(name) > len(p) + len(s)
                                for p, s in templates)


def check(root: Path, runtime: Path | None = None):
    settings = scan(root)
    doc_path = root / DOC
    if not doc_path.is_file():
        return [f'{DOC.as_posix()} is missing'], settings
    rows = documented(doc_path.read_text(encoding='utf-8'))
    problems = []
    by_name: dict[str, Row] = {}
    for row in rows:
        if row.name in by_name:
            problems.append(f'{DOC.as_posix()}:{row.line}: {row.name} is listed twice')
        by_name[row.name] = row
    set_names = {s.name: s.kind for s in settings}
    for name in sorted(set_names):
        files = sorted({s.path for s in settings if s.name == name})
        row = by_name.get(name)
        if row is None:
            problems.append(f'{set_names[name]} {name} (set in {", ".join(files)}) is missing from {DOC.as_posix()}')
            continue
        if row.section == 'relied':
            problems.append(f'{DOC.as_posix()}:{row.line}: {name} is listed as relied on but is set in {", ".join(files)}')
            continue
        if row.kind != set_names[name]:
            problems.append(f'{DOC.as_posix()}:{row.line}: {name} is a {set_names[name]} switch, not {row.kind}')
        value_cell, set_in = (row.cells + ['', '', ''])[0], (row.cells + ['', '', ''])[2]
        for value in sorted({s.value for s in settings if s.name == name and s.value is not None}):
            if f'`{value}`' not in value_cell:
                problems.append(f'{DOC.as_posix()}:{row.line}: {name} is set to `{value}`, which its value cell does not give')
        for path in files:
            if f'`{path}`' not in set_in:
                problems.append(f'{DOC.as_posix()}:{row.line}: {name} is set in `{path}`, which its "set in" cell does not name')
    for row in rows:
        if row.section != 'relied' and row.name not in set_names:
            problems.append(f'{DOC.as_posix()}:{row.line}: {row.name} is listed but nothing in mag-recomp sets it')
    if runtime is not None:
        words, templates = runtime_words(runtime)
        for row in rows:
            known = named(row.name, words, templates)
            if row.section == 'unread' and known:
                problems.append(f'{row.name} is listed as not read, but the runtime at {runtime} names it')
            elif row.section != 'unread' and not known:
                problems.append(f'{row.name} is not named anywhere in the runtime at {runtime}')
    return problems, settings


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--root', type=Path, default=ROOT)
    ap.add_argument('--runtime', type=Path, help='a ps3recomp runtime checkout to check the names against')
    ap.add_argument('--list', action='store_true', help='print every switch found and where it is set')
    args = ap.parse_args(argv)
    problems, settings = check(args.root, args.runtime)
    if args.list:
        for s in sorted(settings, key=lambda s: (s.kind, s.name, s.path, s.value or '')):
            print(f'{s.kind}\t{s.name}\t{"<computed>" if s.value is None else s.value}\t{s.path}')
        return 0
    for problem in problems:
        print('FAIL:', problem)
    if problems:
        print(f'{len(problems)} problem(s). Update {DOC.as_posix()} (see its header).')
        return 1
    print(f'OK: {len({s.name for s in settings})} ps3recomp switches match {DOC.as_posix()}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
