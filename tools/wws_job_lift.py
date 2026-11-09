#!/usr/bin/env python3
"""Lift every WWS SPU job module embedded in the retail engine ELF (private output).

The job manager streams each module from its main-memory image into the job
code buffer at local store 0x4000, header included. This tool finds the module
images, bounds each module's code by control-flow reachability from its header
entry -- following direct branches, plus optional measured interpreter entries
and, with --static-indirect (opt-in until accepted in game), the
indirect-branch targets a static pass proves (computed jumps, explicit return
addresses, constant-address pointer loads and bounded jump tables) -- lifts
that interval with the SDK lifter, and emits a registration
file that registers each module as a verified streamed region
(spu_overlay_register_verified_region) by default unless MAG_COMPILED_JOBS is
explicitly set to a value other than 1. Original bytes and generated code stay
in the private output directory; the tool itself holds no title bytes.
"""
import argparse, json, pathlib, re, struct, subprocess, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from verified_spu_jobs import digest, sdk_identity, fresh_output, copy_baseline, executable_sections

LOAD_BASE = 0x4000
HEADER = 0x30
MAGIC = b'\xC0\xDE\xC0\xDE'
FIRST_IMAGE_ID = 200
PREFIX = 'mag_wwsjob_{ea:08x}_'
REGISTRY = 'jobmods_residency_register.c'
LS_SIZE = 0x40000


def elf_segments(elf):
    if elf[:4] != b'\x7fELF' or elf[4] != 2 or elf[5] != 2:
        raise ValueError('expected a big-endian ELF64 engine image')
    phoff = struct.unpack_from('>Q', elf, 0x20)[0]
    phes, phn = struct.unpack_from('>HH', elf, 0x36)
    out = []
    for i in range(phn):
        typ, _, off, va, _, filesz, _, _ = struct.unpack_from('>IIQQQQQQ', elf, phoff + i * phes)
        if typ == 1 and filesz:
            out.append((va, off, filesz))
    return out


def find_modules(elf):
    """Module images: 16-byte aligned header, magic at +0x20, load base at +0x28."""
    modules = []
    for va, off, size in elf_segments(elf):
        blob = elf[off:off + size]
        at = blob.find(MAGIC)
        while at != -1:
            h = at - 0x20
            if h >= 0 and h % 16 == 0 and h + HEADER <= len(blob) and \
                    struct.unpack_from('>I', blob, h + 0x28)[0] == LOAD_BASE:
                entry, image = struct.unpack_from('>II', blob, h + 0x10)
                if HEADER <= entry < image <= LS_SIZE - LOAD_BASE and h + image <= len(blob):
                    modules.append(dict(ea=va + h, entry=LOAD_BASE + entry, size=image,
                                        raw=bytes(blob[h:h + image])))
            at = blob.find(MAGIC, at + 4)
    modules.sort(key=lambda m: m['ea'])
    return modules


def fnv64(data):
    """The hash SPU_INTERP_ATTRIBUTION reports for ls4000_hash (its seed is not
    the standard FNV-1a offset basis; keep it identical to the report)."""
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


# SPU branch encodings (Cell BE SPU ISA): RI16 branches by 9-bit opcode,
# indirect branches and stops by 11-bit opcode.
RELATIVE = {0x064: 'jump', 0x066: 'call', 0x040: 'cond', 0x042: 'cond', 0x044: 'cond', 0x046: 'cond'}
ABSOLUTE = {0x060: 'jump', 0x062: 'call'}
INDIRECT_END = {0x1A8, 0x1AA}            # bi, iret: no fallthrough
INDIRECT_CONT = {0x1A9, 0x1AB, 0x128, 0x129, 0x12A, 0x12B}   # bisl/bisled, conditional bi*


def _direct_flow(pc, w):
    """(direct target or None, kind, falls through) of the word at pc."""
    op9, op11 = w >> 23, w >> 21
    i16 = (w >> 7) & 0xFFFF
    if op9 in RELATIVE:
        kind = RELATIVE[op9]
        return (pc + (_sext(i16, 16) << 2)) & (LS_SIZE - 1), kind, kind != 'jump'
    if op9 in ABSOLUTE:
        kind = ABSOLUTE[op9]
        return (i16 << 2) & (LS_SIZE - 1), kind, kind != 'jump'
    return None, None, op11 not in INDIRECT_END


def reachable_end(raw, entries, lo, hi, indirect=False):
    """Highest reachable instruction + 4. Direct branches are always followed;
    with `indirect` (opt-in), so are the indirect-branch targets discover_code
    proves."""
    if indirect:
        return discover_code(raw, entries, lo, hi)[0]
    seen, work = set(), [e for e in entries if lo <= e < hi]
    while work:
        pc = work.pop()
        while lo <= pc < hi and pc not in seen and not pc & 3:
            seen.add(pc)
            target, _, falls = _direct_flow(pc, _word(raw, pc))
            if target is not None:
                work.append(target)
            if not falls:
                break
            pc += 4
    if not seen:
        raise ValueError('no reachable code')
    return max(seen) + 4


# ---------------------------------------------------------------------------
# Static discovery of indirect-branch targets.
#
# A forward constant propagation over the module's reachable code tracks, per
# instruction, which registers hold a value that is known at lift time:
#   ('k', c)      the 32-bit constant c (il/ila/ilhu/ilh/iohl, ai/a/sf/ori on
#                 constants, and the link of a branch-and-set-link to the next
#                 instruction, which is how position-independent code reads its
#                 own address);
#   ('q', a)      the quadword loaded from the 16-byte aligned module address a;
#   ('m', a, d)   the word of the module image at a, plus d, in the preferred
#                 slot (a quadword load rotated by a known byte count).
# These uses of tracked values name code:
#   * the target register of bi/bisl/bisled/biz/binz/bihz/bihnz;
#   * $r0, the link register, when control leaves through a branch that does
#     not set it: the code built an explicit return address for the callee;
#   * a bounded jump table feeding `bi` (see _jump_table): every entry;
#   * nothing else -- in particular a constant that is merely in range is never
#     taken as code.
# A candidate is accepted only if every instruction reachable from it through
# direct control flow is a defined, non-zero SPU instruction inside the module
# that no known-address load or store (or jump table) touches. Accepted
# candidates are explored like the header entry, to a fixed point. After the
# fixed point, candidates whose code overlaps data found later are banned, newly
# matched jump tables are added, and the pass is repeated until neither changes.
#
# Calls follow the SPU ABI: registers 80-127 survive a call, the rest do not,
# and a callee starts with nothing known.
# ---------------------------------------------------------------------------

# Opcodes the SPU ISA defines, keyed by the width of the opcode field.
_RRR = {0x8, 0xB, 0xC, 0xD, 0xE, 0xF}                                     # op4
_RI18 = {0x08, 0x09, 0x21}                                                # op7
_RI16 = {0x040, 0x041, 0x042, 0x044, 0x046, 0x047, 0x060, 0x061, 0x062,  # op9
         0x064, 0x065, 0x066, 0x067, 0x081, 0x082, 0x083, 0x0C1}
_RI10 = {0x04, 0x05, 0x06, 0x0C, 0x0D, 0x14, 0x15, 0x16, 0x1C, 0x1D,    # op8
         0x24, 0x34, 0x44, 0x45, 0x46, 0x4C, 0x4D, 0x4E, 0x4F, 0x5C,
         0x5D, 0x5E, 0x5F, 0x74, 0x75, 0x7C, 0x7D, 0x7E, 0x7F}
_RI8 = {0x1D8, 0x1D9, 0x1DA, 0x1DB}                                       # op10
_RR = {0x000, 0x001, 0x002, 0x003, 0x00C, 0x00D, 0x00F, 0x10C, 0x10D,   # op11
       0x140, 0x201, 0x040, 0x041, 0x042, 0x048, 0x049, 0x053, 0x0C0,
       0x0C1, 0x0C2, 0x0C8, 0x0C9, 0x0D3, 0x340, 0x341, 0x342, 0x343,
       0x346, 0x34E, 0x058, 0x059, 0x05A, 0x05B, 0x05C, 0x05D, 0x05E,
       0x05F, 0x078, 0x079, 0x07A, 0x07B, 0x07C, 0x07D, 0x07E, 0x07F,
       0x1CC, 0x1CD, 0x1CF, 0x1D8, 0x1D9, 0x1DB, 0x1DC, 0x1DD, 0x1DF,
       0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x1F8, 0x1F9, 0x1FB, 0x1FC, 0x1FD,
       0x1FF, 0x1B0, 0x1B1, 0x1B2, 0x1B4, 0x1B5, 0x1B6, 0x1B8, 0x1B9,
       0x2A5, 0x2A6, 0x2AE, 0x2B4, 0x2B6, 0x1F0, 0x1D4, 0x1D5, 0x1D6,
       0x1D7, 0x1C4, 0x144, 0x1A8, 0x1A9, 0x1AA, 0x1AB, 0x128, 0x129,
       0x12A, 0x12B, 0x1AC, 0x241, 0x249, 0x2C1, 0x2C9, 0x240, 0x248,
       0x250, 0x253, 0x258, 0x2C0, 0x2C8, 0x2D0, 0x2D8, 0x3C0, 0x3C8,
       0x3D0, 0x3D8, 0x3C4, 0x3C5, 0x3C6, 0x3C7, 0x3CC, 0x3CE, 0x2C2,
       0x2C3, 0x2C4, 0x2C5, 0x2C6, 0x2CA, 0x2CB, 0x2CC, 0x2CD, 0x2CE,
       0x35C, 0x35D, 0x35E, 0x35F, 0x3C2, 0x3C3, 0x3CA, 0x3CB, 0x3B8,
       0x3B9, 0x398, 0x3BA, 0x3BF, 0x3D4}
# Formats that write no register: stores, branches without link, hints,
# channel/SPR writes, halts, stops, no-ops and syncs.
_NO_WRITE9 = {0x041, 0x047, 0x040, 0x042, 0x044, 0x046, 0x060, 0x064}
_NO_WRITE8 = {0x24, 0x4F, 0x5F, 0x7F}
_NO_WRITE11 = {0x000, 0x001, 0x002, 0x003, 0x10C, 0x10D, 0x140, 0x201, 0x144,
               0x1A8, 0x1AA, 0x128, 0x129, 0x12A, 0x12B, 0x1AC, 0x258, 0x2D8,
               0x3D8, 0x3BA}
_CALLEE_SAVED = range(80, 128)
_PADDING = {0x201, 0x001}                 # nop, lnop (op11)
_MASK = 0xFFFFFFFF


def _sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def _word(raw, pc):
    return struct.unpack_from('>I', raw, pc - LOAD_BASE)[0]


def is_defined(w):
    """True if the SPU ISA defines an instruction with this encoding."""
    return (w >> 28 in _RRR or w >> 25 in _RI18 or w >> 23 in _RI16 or w >> 24 in _RI10
            or w >> 22 in _RI8 or w >> 21 in _RR)


def _writes(w):
    """The register a defined instruction writes, or None."""
    if w >> 28 in _RRR:
        return (w >> 21) & 0x7F
    if w >> 25 in (0x08, 0x09) or w >> 23 in _NO_WRITE9 or w >> 24 in _NO_WRITE8 \
            or w >> 21 in _NO_WRITE11:
        return None
    return w & 0x7F


def _after_call(state, link):
    return {r: v for r, v in state.items() if r in _CALLEE_SAVED and r != link}


def _evaluate(pc, w, state):
    """The value the instruction at pc leaves in its destination register, or
    None when it is not known; also the quadword it reads or writes, if known."""
    get = state.get
    rt, ra, rb = w & 0x7F, (w >> 7) & 0x7F, (w >> 14) & 0x7F
    i16, i10, i7 = (w >> 7) & 0xFFFF, _sext((w >> 14) & 0x3FF, 10), rb
    op7, op8, op9, op11 = w >> 25, w >> 24, w >> 23, w >> 21
    k = lambda r: get(r, (None,))[1] if get(r, (None,))[0] == 'k' else None
    if op7 == 0x21:                                     # ila
        return ('k', (w >> 7) & 0x3FFFF), None
    if op9 == 0x081:                                    # il
        return ('k', _sext(i16, 16) & _MASK), None
    if op9 == 0x082:                                    # ilhu
        return ('k', i16 << 16), None
    if op9 == 0x083:                                    # ilh
        return ('k', (i16 << 16) | i16), None
    if op9 == 0x0C1:                                    # iohl
        return (('k', k(rt) | i16) if k(rt) is not None else None), None
    if op9 in (0x061, 0x067, 0x041, 0x047):             # lqa lqr stqa stqr
        a = (i16 << 2) if op9 in (0x061, 0x041) else pc + (_sext(i16, 16) << 2)
        a &= LS_SIZE - 16
        return (('q', a) if op9 in (0x061, 0x067) else None), a
    if op8 in (0x34, 0x24):                             # lqd stqd
        if k(ra) is None:
            return None, None
        a = (k(ra) + (i10 << 4)) & (LS_SIZE - 16)
        return (('q', a) if op8 == 0x34 else None), a
    if op11 in (0x1C4, 0x144):                          # lqx stqx
        if k(ra) is None or k(rb) is None:
            return None, None
        a = (k(ra) + k(rb)) & (LS_SIZE - 16)
        return (('q', a) if op11 == 0x1C4 else None), a
    v = get(ra)
    if op8 == 0x1C and v:                               # ai
        if v[0] == 'k':
            return ('k', (v[1] + i10) & _MASK), None
        if v[0] == 'm':
            return ('m', v[1], (v[2] + i10) & _MASK), None
    if op8 == 0x04 and v:                               # ori
        if i10 == 0:
            return v, None
        if v[0] == 'k':
            return ('k', (v[1] | i10) & _MASK), None
    if op11 == 0x0C0 and v and get(rb):                 # a
        x, y = (v, get(rb)) if v[0] != 'k' else (get(rb), v)
        if y[0] == 'k' and x[0] == 'k':
            return ('k', (x[1] + y[1]) & _MASK), None
        if y[0] == 'k' and x[0] == 'm':
            return ('m', x[1], (x[2] + y[1]) & _MASK), None
    if op11 == 0x040 and k(ra) is not None and k(rb) is not None:   # sf
        return ('k', (k(rb) - k(ra)) & _MASK), None
    if op11 in (0x1DC, 0x1FC, 0x1FF) and v:             # rotqby rotqbyi shlqbyi
        n = (k(rb) if op11 == 0x1DC else i7)
        if n is None:
            return None, None
        if op11 != 0x1DC and i7 == 0:
            return v, None
        n &= 15
        if op11 != 0x1FF and v[0] == 'q' and not n & 3:
            return ('m', v[1] + n, 0), None
    return None, None


def _value_target(raw, value):
    """The code address a tracked value names, if it names one."""
    if value is None:
        return None
    if value[0] == 'k':
        return value[1]
    image = len(raw) + LOAD_BASE
    if value[0] == 'q':
        value = ('m', value[1], 0)
    if value[0] == 'm' and LOAD_BASE <= value[1] and value[1] + 4 <= image:
        return (_word(raw, value[1]) + value[2]) & _MASK
    return None


def discover_code(raw, entries, lo, hi, pointers=()):
    """(reachable end, statically proven indirect targets) of the module code
    reachable from `entries`. See the block comment above. `pointers` are
    further candidates (see data_code_pointers), validated like the others."""
    banned, tables = set(), {}
    while True:
        state_at, targets, closures, data, joins = _discover(raw, entries, lo, hi, banned, tables,
                                                             pointers)
        late = {t for t in targets if closures[t] & data}
        banned |= late
        found = {}
        for pc in sorted(state_at):
            if pc not in tables and _word(raw, pc) >> 21 == 0x1A8:
                table = _jump_table(raw, pc, state_at, joins, lo, hi, data)
                if table is not None:
                    found[pc] = table
        if not late and not found:
            return max(state_at) + 4, sorted(targets)
        tables.update(found)


def _closure(raw, start, lo, hi, data, trusted):
    """Addresses reachable from start through direct control flow, stopping at
    trusted code; None unless every one is a defined, non-zero instruction
    inside [lo, hi) that no known-address access touches."""
    seen, stack = set(), [start]
    while stack:
        pc = stack.pop()
        while pc not in seen and pc not in trusted:
            if not (lo <= pc < hi) or pc & 3:
                return None
            w = _word(raw, pc)
            if not w or not is_defined(w) or pc in data:
                return None
            seen.add(pc)
            target, _, falls = _direct_flow(pc, w)
            if target is not None and lo <= target < hi:
                stack.append(target)
            if not falls:
                break
            pc += 4
    return seen


def _run(raw, pc, lo, window=32):
    """Addresses before pc, nearest first, of the straight-line code that
    falls through to it: conditional branches are allowed, calls and
    unconditional transfers end it."""
    q = pc - 4
    while q >= lo and pc - q <= 4 * window:
        w = _word(raw, q)
        _, kind, falls = _direct_flow(q, w)
        if not falls or kind == 'call' or w >> 21 in (0x1A9, 0x1AB):
            return
        yield q
        q -= 4


def _writer(raw, pc, reg, lo):
    """(address, word) of the nearest instruction in the straight-line run
    before pc that writes reg, or None."""
    for q in _run(raw, pc, lo):
        w = _word(raw, q)
        if _writes(w) == reg:
            return q, w
    return None


def _jump_table(raw, pc, state_at, joins, lo, hi, data):
    """Targets of the bounded jump table that feeds `bi` at pc, or None.

    The idiom, all in one straight-line run ending at the bi (any order where
    the data dependencies allow, other instructions interleaved):
        shli   off, idx, 2
        clgti  c, idx, N            ; idx > N takes the default
        a      addr, off, base      ; base: a known constant
        lqx    q, base, off
        rotqby t, q, addr           ; t = word at base + 4*idx
       [a      t, t, rel]           ; relative table: rel a known constant
        brnz   c, default
        bi     t
    The table has exactly N+1 entries, every one of which must be a valid code
    address inside the module; otherwise nothing is taken."""
    w = _word(raw, pc)
    reg = lambda word, shift: (word >> shift) & 0x7F
    st = state_at[pc]
    known_at = lambda at: state_at.get(at, {})
    k = lambda at, r: known_at(at).get(r, (None,))[1] if known_at(at).get(r, (None,))[0] == 'k' else None
    target_reg = reg(w, 7)
    d = _writer(raw, pc, target_reg, lo)
    if d is None:
        return None
    rel, rot = 0, d
    if d[1] >> 21 == 0x0C0:                                   # a t, x, rel
        for x, y in ((reg(d[1], 7), reg(d[1], 14)), (reg(d[1], 14), reg(d[1], 7))):
            r = _writer(raw, d[0], x, lo)
            if r is not None and r[1] >> 21 == 0x1DC and k(d[0], y) is not None:
                rel, rot = k(d[0], y), r
                break
        else:
            return None
    if rot[1] >> 21 != 0x1DC:                                 # rotqby t, q, addr
        return None
    lq = _writer(raw, rot[0], reg(rot[1], 7), lo)
    ad = _writer(raw, rot[0], reg(rot[1], 14), lo)
    if lq is None or ad is None or lq[1] >> 21 != 0x1C4 or ad[1] >> 21 != 0x0C0:
        return None
    pair = {reg(lq[1], 7), reg(lq[1], 14)}
    if pair != {reg(ad[1], 7), reg(ad[1], 14)} or len(pair) != 2:
        return None
    known = [r for r in pair if k(lq[0], r) is not None]
    if len(known) != 1:
        return None
    base_reg, off_reg = known[0], (pair - set(known)).pop()
    first = min(lq[0], ad[0])
    if any(_writer(raw, lq[0], r, lo) != _writer(raw, ad[0], r, lo) for r in pair):
        return None
    base = k(lq[0], base_reg)
    sh = _writer(raw, first, off_reg, lo)
    if sh is None or sh[1] >> 21 != 0x07B or reg(sh[1], 14) != 2:   # shli off, idx, 2
        return None
    idx = reg(sh[1], 7)
    source = _writer(raw, sh[0], idx, lo)
    bound = start = None
    for q in _run(raw, pc, lo):                               # clgti c, idx, N
        c = _word(raw, q)
        if c >> 24 == 0x5C and reg(c, 7) == idx and _writer(raw, q, idx, lo) == source:
            test = c & 0x7F
            if any(_word(raw, b) >> 23 == 0x042 and _word(raw, b) & 0x7F == test
                   and _writer(raw, b, test, lo) == (q, c) for b in range(q + 4, pc, 4)):
                bound, start = _sext((c >> 14) & 0x3FF, 10), q    # brnz c, default
                break
    if bound is None or not 0 <= bound < 1024 or base & 3:
        return None
    start = min(start, sh[0], rot[0], d[0], first)
    # Nothing may enter the run between its first matched instruction and the
    # bi, and the dataflow must have reached all of it.
    if any(start < j <= pc for j in joins) or any(q not in state_at for q in range(start, pc, 4)):
        return None
    if base < LOAD_BASE or base + 4 * (bound + 1) > LOAD_BASE + len(raw):
        return None
    words = range(base, base + 4 * (bound + 1), 4)
    out = {}
    for a in words:
        t = (_word(raw, a) + rel) & _MASK
        if _closure(raw, t, lo, hi, data | set(words), state_at) is None:
            return None
        out[t] = st
    return out, set(words)


def _discover(raw, entries, lo, hi, banned, tables, pointers=()):
    inside = lambda a: lo <= a < hi and not a & 3
    state_at, data, valid, targets, closures, joins = {}, set(), set(), {}, {}, set()
    work = [(e, {}) for e in entries if inside(e)]
    for _, words in tables.values():
        data |= words

    def candidate(target, state, kind):
        if target is None or target in banned:
            return
        if target in targets:
            work.append((target, state))
            return
        if not inside(target):
            return
        seen = _closure(raw, target, lo, hi, data, valid | state_at.keys())
        if seen is None:
            banned.add(target)
            return
        valid.update(seen)
        targets[target], closures[target] = kind, seen
        work.append((target, state))

    for target in pointers:
        candidate(target, {}, 'pointer')

    while work:
        pc, st = work.pop()
        while inside(pc):
            old = state_at.get(pc)
            if old is not None:
                merged = {r: v for r, v in old.items() if st.get(r) == v}
                if len(merged) == len(old):
                    break
                st = merged
            state_at[pc] = st
            w = _word(raw, pc)
            value, access = _evaluate(pc, w, st)
            if access is not None:
                data.update(range(access, access + 16, 4))
            dest = _writes(w)
            target, kind, falls = _direct_flow(pc, w)
            op11 = w >> 21
            ra = (w >> 7) & 0x7F
            leaves = kind in ('jump', 'cond') or op11 in (0x1A8, 0x128, 0x129, 0x12A, 0x12B)
            if leaves and not (op11 == 0x1A8 and ra == 0):
                # $r0 holds an address the code built itself: the return point.
                candidate(_value_target(raw, st.get(0)) if st.get(0, ('',))[0] == 'k' else None,
                          _after_call(st, None), 'continuation')
            if op11 in (0x1A8, 0x128, 0x129, 0x12A, 0x12B):           # bi, bi[h]z/nz
                candidate(_value_target(raw, st.get(ra)), st, 'jump')
                for t in tables.get(pc, ({},))[0]:
                    candidate(t, st, 'table')
            elif op11 in (0x1A9, 0x1AB):                             # bisl, bisled
                candidate(_value_target(raw, st.get(ra)), {}, 'call')
            if kind == 'call':
                if target == pc + 4:
                    st = dict(st)
                    st[w & 0x7F] = ('k', pc + 4)
                    pc += 4
                    continue
                joins.add(target)
                work.append((target, {}))
                st = _after_call(st, w & 0x7F)
            elif op11 in (0x1A9, 0x1AB):
                st = _after_call(st, w & 0x7F)
            elif target is not None:
                joins.add(target)
                work.append((target, st))
            if kind != 'call' and op11 not in (0x1A9, 0x1AB) and dest is not None:
                if value is None:
                    if dest in st:
                        st = {r: v for r, v in st.items() if r != dest}
                else:
                    st = dict(st)
                    st[dest] = value
            if not falls:
                break
            pc += 4
    if not state_at:
        raise ValueError('no reachable code')
    return state_at, targets, closures, data, joins | set(targets)


def wrapper_elf(raw, code_end, entry):
    """Minimal SPU ELF32: one PT_LOAD of the whole image at 0x4000 and an
    executable .text section covering [0x4030, code_end)."""
    shstr = b'\0.text\0.shstrtab\0'
    ehsize, phsize, shsize = 52, 32, 40
    image_off = 0x100
    shstr_off = image_off + len(raw)
    sh_off = (shstr_off + len(shstr) + 3) & ~3
    eh = bytearray(b'\x7fELF\x01\x02\x01' + b'\0' * 9)
    eh += struct.pack('>HHIIIIIHHHHHH', 2, 23, 1, entry, ehsize, sh_off, 0,
                      ehsize, phsize, 1, shsize, 3, 2)
    ph = struct.pack('>IIIIIIII', 1, image_off, LOAD_BASE, LOAD_BASE, len(raw), len(raw), 7, 16)
    text = struct.pack('>IIIIIIIIII', 1, 1, 6, LOAD_BASE + HEADER, image_off + HEADER,
                       code_end - LOAD_BASE - HEADER, 0, 0, 16, 0)
    strtab = struct.pack('>IIIIIIIIII', 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    out = bytearray(eh + ph)
    out += b'\0' * (image_off - len(out)) + raw + shstr
    out += b'\0' * (sh_off - len(out)) + b'\0' * shsize + text + strtab
    return bytes(out)


def attribution_entries(paths):
    """{ls4000_hash: {entry,...}} from SPU_INTERP_ATTRIBUTION reports."""
    found = {}
    for path in paths:
        lines = pathlib.Path(path).read_text().splitlines()
        cols = next((l.split('\t') for l in lines if l.startswith('wall_ns\t')), None)
        if not cols:
            raise ValueError('not an interpreter attribution report: ' + str(path))
        for line in lines[lines.index('\t'.join(cols)) + 1:]:
            row = dict(zip(cols, line.split('\t')))
            if len(row) == len(cols):
                found.setdefault(row['ls4000_hash'], set()).add(int(row['entry'], 16))
    return found


def emit_registry(modules):
    lines = ['/* PRIVATE generated expected guest bytes; do not commit. */',
             '#include "spu_overlay_residency.h"', '#include <stdio.h>', '#include <stdlib.h>',
             '#include <string.h>', 'extern void spu_begin_image(int);']
    for m in modules:
        lines.append(f"extern void {m['prefix']}spu_recomp_register(void);")
        code = m['raw'][HEADER:m['code_end'] - LOAD_BASE]
        lines.append(f"static const uint8_t job_{m['image_id']}_code[] = {{")
        lines += ['  ' + ','.join(f'0x{x:02x}' for x in code[k:k + 24]) + ',' for k in range(0, len(code), 24)]
        lines.append('};')
    lines += ['__attribute__((constructor)) static void mag_register_streamed_jobs(void) {',
              '  const char* enabled = getenv("MAG_COMPILED_JOBS");',
              '  if (enabled && strcmp(enabled, "1")) return;']
    for m in modules:
        n = m['code_end'] - LOAD_BASE - HEADER
        lines.append(f"  {{ const spu_overlay_code_segment seg = {{0x{LOAD_BASE + HEADER:X}u, {n}u, job_{m['image_id']}_code}};")
        lines.append(f"    spu_begin_image({m['image_id']}); {m['prefix']}spu_recomp_register();")
        # The span is the lifted code alone. Jobs DMA their own data into the
        # rest of the module image, and any transfer into the span from another
        # source drops the resident owner -- after which every indirect branch
        # of the running job falls to the interpreter.
        span = m['code_end'] - LOAD_BASE
        lines.append(f"    if (!spu_overlay_register_verified_region(0x{m['ea']:08X}u, 0x{LOAD_BASE:X}u, 0x{span:X}u, {m['image_id']}, &seg, 1)) {{")
        lines.append('      fputs("MAG streamed job registration failed\\n", stderr); abort(); } }')
    lines += ['  spu_begin_image(0);', '}']
    return '\n'.join(lines) + '\n'


def generated_census(source, prefix):
    functions = len(re.findall(r'^void ' + prefix + r'spu_func_', source, re.M))
    unsupported = bool(re.search(r'unsupported|UNSUPPORTED|\.word', source))
    return functions, unsupported


def require_complete(records):
    failed = [str(record['image_id']) for record in records if record['status'] != 'lifted']
    if failed:
        raise ValueError('incomplete streamed job coverage: ' + ', '.join(failed))


def data_code_pointers(raw, lo, end):
    """Opt-in candidates: words of the module image at or past `end` (its data)
    whose value is a function boundary inside [lo, end) -- an aligned address
    whose previous instruction, skipping nop/lnop padding, cannot fall through.
    This is a data heuristic: such a word is only probably a code pointer."""
    found = set()
    for at in range(end, LOAD_BASE + len(raw) - 3, 4):
        value = _word(raw, at)
        if not lo <= value < end or value & 3:
            continue
        before = value - 4
        while before >= lo and _word(raw, before) >> 21 in _PADDING:
            before -= 4
        if before >= lo and not _direct_flow(before, _word(raw, before))[2]:
            found.add(value)
    return sorted(found)


def module_plans(raw, roots, lo, hi, static=False, data_pointers=False):
    """Bounds to try in order. By default only the direct-branch bound. With
    `static`, first the statically extended bound, and with `data_pointers` as
    well, before that the one extended by data code pointers; each is offered
    only when it differs from the next, and the direct bound stays last as the
    fallback."""
    if data_pointers and not static:
        raise ValueError('data code pointers require static indirect discovery')
    direct = ('direct', reachable_end(raw, roots, lo, hi), [])
    if not static:
        return [direct]
    end, static = discover_code(raw, roots, lo, hi)
    plans = [direct] if end == direct[1] and not static else [('static', end, static), direct]
    if data_pointers:
        pointers = data_code_pointers(raw, lo, end)
        for _ in range(8):
            wide_end, wide = discover_code(raw, roots, lo, hi, pointers)
            again = data_code_pointers(raw, lo, wide_end)
            if again == pointers:
                if wide_end != end or wide != static:
                    plans.insert(0, ('static+data', wide_end, wide))
                break
            pointers = again
    return plans


def lift_module(m, code_end, measured, static, lifter, work, output, merge_chunks, lifter_args=()):
    """Lift [0x4030, code_end) of one module with the measured and statically
    proven entries; returns (function count, command, extra entries)."""
    lo = LOAD_BASE + HEADER
    m['code_end'] = code_end
    extra = sorted({e for e in measured if e < code_end} | set(static))
    wrapper = wrapper_elf(m['raw'], code_end, m['entry'])
    if executable_sections(wrapper) != [(lo, m['raw'][HEADER:code_end - LOAD_BASE])]:
        raise ValueError('wrapper executable coverage')
    elf_path = work / f"job_{m['ea']:08x}.elf"
    elf_path.write_bytes(wrapper)
    dest = output / f"mag_wwsjob_{m['image_id']}"
    cmd = [sys.executable, str(lifter), '--auto-functions', str(elf_path), '--code-end', hex(code_end),
           '--symbol-prefix', m['prefix'], '--output', str(dest)]
    if extra:
        cmd += ['--extra-funcs', ','.join(hex(e) for e in extra)]
    if merge_chunks:
        cmd += ['--merge-chunks']
    cmd += list(lifter_args)
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    (work / f"lift_{m['ea']:08x}.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise ValueError('lifter failed')
    for f in [dest / 'spu_recomp.c', dest / 'spu_recomp.h']:
        f.write_text(f.read_text().replace('../../runtime/spu/', ''))
    count, unsupported = generated_census((dest / 'spu_recomp.c').read_text(), m['prefix'])
    if unsupported or not count:
        for f in dest.iterdir():
            f.unlink()
        dest.rmdir()
        raise ValueError('unsupported instructions' if unsupported else 'no functions')
    return count, cmd, extra


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--engine-elf', type=pathlib.Path, required=True)
    identity = ap.add_mutually_exclusive_group(required=True)
    identity.add_argument('--engine-elf-sha256')
    identity.add_argument('--engine-profile', type=pathlib.Path,
                          help='reviewed engine SPU profile containing elf_sha256')
    ap.add_argument('--sdk', type=pathlib.Path, required=True)
    ap.add_argument('--sdk-revision', required=True)
    ap.add_argument('--baseline', type=pathlib.Path, required=True, help='existing audited SPU directory')
    ap.add_argument('--output', type=pathlib.Path, required=True)
    ap.add_argument('--attribution', type=pathlib.Path, action='append', default=[],
                    help='SPU_INTERP_ATTRIBUTION report(s) whose entries become extra lift entries')
    ap.add_argument('--static-indirect', action='store_true',
                    help='opt-in: extend each module by the indirect-branch targets a static pass '
                         'proves (computed jumps, explicit return addresses, constant-address '
                         'pointer loads, bounded jump tables); a module whose extended lift fails '
                         'is retried with the direct-branch bound. Without it the bound follows '
                         'direct branches and measured entries only')
    ap.add_argument('--data-code-pointers', action='store_true',
                    help='further opt-in heuristic, requires --static-indirect: also take words in '
                         'a module\'s data that hold the address of a function boundary inside its '
                         'code as entries (validated, with fallback to the --static-indirect bound)')
    ap.add_argument('--merge-chunks', action='store_true',
                    help='lift each contiguous run between call targets as one host function, so '
                         'branches inside it are gotos instead of drain-loop transfers')
    ap.add_argument('--lifter-arg', action='append', default=[], metavar='ARG',
                    help='opt-in: pass ARG through to the SDK lifter for every module (repeat for '
                         'more, e.g. --lifter-arg=--promote-regs); each module\'s recorded command '
                         'shows it. Without it the lifter command is unchanged')
    ap.add_argument('--require-complete', action='store_true',
                    help='fail after recording results if any discovered job remains interpreted')
    a = ap.parse_args()
    if a.data_code_pointers and not a.static_indirect:
        ap.error('--data-code-pointers requires --static-indirect')
    preparer_digest = digest(pathlib.Path(__file__))
    profile_digest = digest(a.engine_profile) if a.engine_profile else None
    if a.engine_profile:
        a.engine_elf_sha256 = json.loads(a.engine_profile.read_text())['elf_sha256']
    if not re.fullmatch('[0-9a-f]{64}', a.engine_elf_sha256):
        raise ValueError('engine ELF SHA256 must be an exact lowercase identity')
    if digest(a.engine_elf) != a.engine_elf_sha256:
        raise ValueError('engine ELF identity')
    pin = sdk_identity(a.sdk, a.sdk_revision)
    lifter = a.sdk / 'tools/spu_lifter.py'
    if 'spu_overlay_register_verified_region' not in (a.sdk / 'runtime/spu/spu_overlay_residency.h').read_text():
        raise ValueError('SDK lacks verified streamed regions')
    base = json.loads((a.baseline / 'provenance.json').read_text())
    base_digest = digest(a.baseline / 'provenance.json')
    if base.get('status') != 'complete':
        raise ValueError('baseline must be complete')
    for rel, h in base['outputs'].items():
        if digest(a.baseline / rel) != h:
            raise ValueError('baseline drift ' + rel)
    if (a.baseline / REGISTRY).exists():
        raise ValueError('baseline already has streamed job registration')
    extras = attribution_entries(a.attribution)
    modules = find_modules(a.engine_elf.read_bytes())
    if not modules:
        raise ValueError('no job modules found')
    output = fresh_output(a.output)
    copy_baseline(a.baseline, output)
    (output / 'provenance.json').write_text(json.dumps(dict(base, status='preparing'), indent=2) + '\n')
    work = output / 'wws-job-inputs'
    work.mkdir(mode=0o700)
    lifted, records = [], []
    for index, m in enumerate(modules):
        m['image_id'] = FIRST_IMAGE_ID + index
        m['prefix'] = PREFIX.format(ea=m['ea'])
        lo, hi = LOAD_BASE + HEADER, LOAD_BASE + m['size']
        measured = sorted(e for e in extras.get('%016x' % fnv64(m['raw'][:64]), ()) if lo <= e < hi)
        record = dict(ea=hex(m['ea']), size=hex(m['size']), entry=hex(m['entry']),
                      image_id=m['image_id'], raw_sha256=digest_bytes(m['raw']))
        roots = [m['entry'], *measured]
        try:
            plans = module_plans(m['raw'], roots, lo, hi, a.static_indirect, a.data_code_pointers)
        except ValueError as e:
            plans = []
            record.update(status='interpreted', reason=str(e))
        for basis, code_end, static in plans:
            try:
                count, cmd, extra = lift_module(m, code_end, measured, static, lifter, work, output,
                                                a.merge_chunks, a.lifter_arg)
            except ValueError as e:
                if a.static_indirect:
                    record.setdefault('rejected', []).append(
                        dict(discovery=basis, code_end=hex(code_end), reason=str(e)))
                record.update(status='interpreted', reason=str(e))
                continue
            record.pop('reason', None)
            # Without --static-indirect the record is exactly the direct-bound one.
            discovery = dict(discovery=basis) if a.static_indirect else {}
            static_record = dict(static_entries=[hex(e) for e in static]) if a.static_indirect else {}
            record.update(status='lifted', **discovery, code_end=hex(code_end), functions=count,
                          extra_entries=[hex(e) for e in extra], **static_record, command=cmd[1:])
            lifted.append(m)
            break
        records.append(record)
    (output / REGISTRY).write_text(emit_registry(lifted))
    if sdk_identity(a.sdk, a.sdk_revision) != pin or digest(a.baseline / 'provenance.json') != base_digest:
        raise ValueError('input drift')
    if digest(pathlib.Path(__file__)) != preparer_digest:
        raise ValueError('preparer source drift')
    base['wws_job_extension'] = {'engine_elf_sha256': a.engine_elf_sha256, 'SDK_pin': pin,
                                 'preparer_sha256': preparer_digest,
                                 'lifter_sha256': digest(lifter), 'runtime_toggle': 'default ON; MAG_COMPILED_JOBS=0 explicitly disables',
                                 'attribution_sha256': [digest(p) for p in a.attribution], 'modules': records}
    if a.engine_profile:
        if digest(a.engine_profile) != profile_digest:
            raise ValueError('engine profile drift')
        base['wws_job_extension']['engine_profile_sha256'] = profile_digest
    if a.require_complete and any(r['status'] != 'lifted' for r in records):
        base['status'] = 'incomplete'
    base['outputs'] = {str(f.relative_to(output)): digest(f) for f in sorted(output.rglob('*'))
                       if f.suffix in ('.c', '.h', '.cpp')}
    (output / 'provenance.json').write_text(json.dumps(base, indent=2) + '\n')
    print(json.dumps({'output': str(output), 'modules': len(modules), 'lifted': len(lifted),
                      'interpreted': [r for r in records if r['status'] != 'lifted']}, indent=2))
    if a.require_complete:
        require_complete(records)


def digest_bytes(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


if __name__ == '__main__':
    main()
