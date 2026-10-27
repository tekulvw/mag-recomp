#!/usr/bin/env python3
"""Splice a lifter fix into an existing generated PPU lift, function by function.

The generated engine lift carries reviewed post-lift transforms, so a lifter
fix cannot simply regenerate it. Instead lift the same inputs twice with the
SDK lifter immediately before and after the fix (--before / --after). Every
function whose body differs between those two, or that only the fixed lifter
emits, is the fix's exact footprint. Each changed function must still be
byte-identical in the existing lift to its --before body (so no transform
touched it and the lifter versions agree there); then the --after bodies
replace them, added functions go into one new translation unit, and their
declarations and function-table entries are appended. Everything else stays
byte-identical. The output is a fresh directory with an updated provenance.

  tools/ppu_function_splice.py --base <generated dir> --before <lift> --after <lift>
      --output <fresh generated dir> --sdk <sdk> --before-revision <rev> --after-revision <rev>
"""
import argparse, hashlib, json, pathlib, re, shutil, subprocess, sys

FUNC = re.compile(r'^void (func_[0-9A-F]{8})\(ppu_context\* ctx\) \{\n.*?^\}\n', re.M | re.S)
ENTRY = re.compile(r'^    \{ 0x([0-9A-F]{8})ULL, (func_[0-9A-F]{8}), "func_[0-9A-F]{8}" \},\n', re.M)


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def functions(directory):
    """{name: (file, body)} over every ppu_recomp_*.cpp."""
    out = {}
    for path in sorted(directory.glob('ppu_recomp_*.cpp')):
        for m in FUNC.finditer(path.read_text()):
            if m[1] in out:
                raise ValueError('duplicate definition ' + m[1])
            out[m[1]] = (path.name, m[0])
    return out


def table(directory):
    entries = {}
    for path in sorted(directory.glob('ppu_recomp_*.cpp')):
        for m in ENTRY.finditer(path.read_text()):
            entries[m[2]] = (path.name, m[0])
    return entries


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('base', 'before', 'after', 'output', 'sdk'):
        ap.add_argument('--' + name, type=pathlib.Path, required=True)
    ap.add_argument('--before-revision', required=True)
    ap.add_argument('--after-revision', required=True)
    ap.add_argument('--reason', required=True)
    ap.add_argument('--only', default='', help='comma-separated function names: splice just these '
                    '(each must equal its --before body under the base lift\'s halt guard)')
    a = ap.parse_args()
    if a.output.exists():
        raise SystemExit('output must be fresh')
    base_record = json.loads((a.base / 'provenance.json').read_text())
    for name, h in base_record['outputs'].items():
        if digest(a.base / name) != h:
            raise SystemExit('base drift: ' + name)
    before, after, base = functions(a.before), functions(a.after), functions(a.base)
    removed = sorted(set(before) - set(after))
    changed = sorted(n for n in before if n in after and before[n][1] != after[n][1])
    added = sorted(set(after) - set(before))
    # The base lift guards every unresolved instruction site with a halt
    # (tools/lift.py post-processing); spliced bodies get the same guard.
    guard = ' do { fprintf(stderr, "[mag] unsupported lifted code in %s\\n", __func__); _Exit(86); } while (0);'
    def guarded(body):
        return re.sub(r'(/\* TODO: [^*]*\*/);', lambda m: m[1] + guard, body)
    footprint = dict(changed=changed, added=added, removed=removed)
    if a.only:
        only = [x.strip() for x in a.only.split(',') if x.strip()]
        if any(n not in changed for n in only):
            raise SystemExit('--only names a function the fix does not change')
        changed, added = only, []
        for n in changed:
            if base[n][1] != guarded(before[n][1]):
                raise SystemExit(f'{n}: existing lift differs from the guarded --before body')
    else:
        for n in changed:
            if n not in base or base[n][1] != before[n][1]:
                raise SystemExit(f'{n}: existing lift differs from the --before lift; splice is not exact')
    if any(n in base for n in added):
        raise SystemExit('an added function already exists in the base lift')
    after_table, base_table = table(a.after), table(a.base)
    missing = [n for n in added if n not in after_table]
    if missing:
        raise SystemExit('added functions lack table entries: ' + ', '.join(missing))
    # Fresh copy, then edit the copy.
    a.output.mkdir(parents=True)
    for p in a.base.iterdir():
        if p.is_file():
            shutil.copy2(p, a.output / p.name)
    texts = {}
    spliced = {}
    for n in changed:
        f = base[n][0]
        t = texts.setdefault(f, (a.output / f).read_text())
        assert t.count(base[n][1]) == 1, n
        spliced[n] = guarded(after[n][1])
        texts[f] = t.replace(base[n][1], spliced[n])
    for f, t in texts.items():
        (a.output / f).write_text(t)
    new_tu = None
    if added:
        index = 1 + max(int(p.stem.rsplit('_', 1)[1]) for p in a.output.glob('ppu_recomp_*.cpp'))
        new_tu = f'ppu_recomp_{index:03d}.cpp'
        head = (a.base / 'ppu_recomp_000.cpp').read_text().split('\nvoid func_', 1)[0]
        body = ''.join(guarded(after[n][1]) + '\n' for n in added)
        (a.output / new_tu).write_text(head.rstrip('\n') + '\n\n' + body)
        header = (a.output / 'ppu_recomp.h').read_text()
        decls = ''.join(f'void {n}(ppu_context* ctx);\n' for n in added)
        anchor = header.rfind('void func_')
        line_end = header.index('\n', anchor) + 1
        (a.output / 'ppu_recomp.h').write_text(header[:line_end] + decls + header[line_end:])
        tf = next(iter({v[0] for v in base_table.values()}))
        t = (a.output / tf).read_text()
        rows = ''.join(after_table[n][1] for n in added)
        t = t.replace('    { 0, NULL, NULL }\n', rows + '    { 0, NULL, NULL }\n', 1)
        count = re.search(r'function_table_count = (\d+);', t)
        t = t.replace(count[0], f'function_table_count = {int(count[1]) + len(added)};')
        (a.output / tf).write_text(t)
    record = dict(base_record)
    record['outputs'] = {p.name: digest(p) for p in sorted(a.output.iterdir())
                         if p.suffix in ('.c', '.cpp', '.h')}
    record.setdefault('function_splices', []).append(dict(
        reason=a.reason, base_provenance_sha256=digest(a.base / 'provenance.json'),
        before_revision=a.before_revision, after_revision=a.after_revision,
        changed=changed, added=added, new_translation_unit=new_tu, lifter_footprint=footprint,
        body_sha256={n: hashlib.sha256(spliced.get(n, after[n][1]).encode()).hexdigest() for n in changed + added}))
    (a.output / 'provenance.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(dict(changed=changed, added=added, new_translation_unit=new_tu), indent=1))


if __name__ == '__main__':
    main()
