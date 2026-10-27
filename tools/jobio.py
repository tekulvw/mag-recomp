"""Reader and summary for SPU job I/O recordings (SDK runtime/diagnostics/job_io_recorder).

A recording holds the local store and registers at entry, every MFC transfer and
atomic line operation with its data, and the local store and registers at exit.
Recordings contain original game data and stay out of Git; this module and its
tests contain format handling only."""
import argparse, struct, sys
from collections import Counter
MAGIC = 0x4A4F4249
GET, PUT, GETLLAR, PUTLLC, PUTLLUC, ZERO = 1, 2, 3, 4, 5, 6
NAMES = {GET: 'GET', PUT: 'PUT', GETLLAR: 'GETLLAR', PUTLLC: 'PUTLLC', PUTLLUC: 'PUTLLUC', ZERO: 'ZERO'}

def parse(data):
    if len(data) < 48:
        raise ValueError('short file')
    magic, version, tag, pc, ls_size, gpr_size, truncated, event_bytes = struct.unpack_from('<8I', data, 0)
    if magic != MAGIC or version not in (1, 2):
        raise ValueError('not a recognized job I/O recording')
    start_ns, end_ns = struct.unpack_from('<2Q', data, 32)
    at = 48
    need = at + ls_size + gpr_size + event_bytes + ls_size + gpr_size
    if len(data) != need:
        raise ValueError('size mismatch: have %d, header implies %d' % (len(data), need))
    entry_ls = data[at:at + ls_size]; at += ls_size
    entry_gpr = data[at:at + gpr_size]; at += gpr_size
    events = []; stop = at + event_bytes
    while at < stop:
        kind, cmd, lsa, ea, size, epc, extra = struct.unpack_from('<7I', data, at); at += 28
        payload = b'' if kind == ZERO else data[at:at + size]
        at += 0 if kind == ZERO else size
        events.append(dict(kind=kind, cmd=cmd, lsa=lsa, ea=ea, size=size, pc=epc, extra=extra, data=payload))
    if at != stop:
        raise ValueError('event stream overran its declared length')
    exit_ls = data[at:at + ls_size]; at += ls_size
    exit_gpr = data[at:at + gpr_size]
    return dict(format_version=version, full_job_replay=version == 1 and not truncated,
                atomic_observations_omitted=version == 2,
                tag=tag, entry_pc=pc, truncated=bool(truncated), start_ns=start_ns, end_ns=end_ns,
                entry_ls=entry_ls, entry_gpr=entry_gpr, events=events, exit_ls=exit_ls, exit_gpr=exit_gpr)

def changed_ranges(a, b, gap=16):
    """Byte ranges where two equal-length buffers differ, merging runs closer than gap."""
    out = []; i = 0; n = len(a)
    while i < n:
        if a[i] == b[i]:
            i += 1; continue
        j = i + 1; last = i
        while j < n and j - last <= gap:
            if a[j] != b[j]:
                last = j
            j += 1
        out.append((i, last + 1)); i = last + 1
    return out

def summary(rec):
    kinds = Counter(); sizes = Counter(); bytes_by = Counter()
    for e in rec['events']:
        kinds[NAMES[e['kind']]] += 1; sizes[(NAMES[e['kind']], e['size'])] += 1; bytes_by[NAMES[e['kind']]] += e['size']
    return dict(format_version=rec['format_version'], full_job_replay=rec['full_job_replay'],
                atomic_observations_omitted=rec['atomic_observations_omitted'],
                entry_pc=rec['entry_pc'], tag=rec['tag'], truncated=rec['truncated'],
                duration_ms=(rec['end_ns'] - rec['start_ns']) / 1e6, events=len(rec['events']),
                by_kind=dict(kinds), bytes_by_kind=dict(bytes_by),
                size_histogram={'%s:%d' % k: v for k, v in sorted(sizes.items())},
                ls_changed_ranges=len(changed_ranges(rec['entry_ls'], rec['exit_ls'])))

def main(argv=None):
    ap = argparse.ArgumentParser(); ap.add_argument('recording'); ap.add_argument('--events', action='store_true')
    a = ap.parse_args(argv)
    with open(a.recording, 'rb') as f:
        rec = parse(f.read())
    import json
    print(json.dumps(summary(rec), indent=1))
    if a.events:
        for e in rec['events']:
            print('%-7s cmd=%02x lsa=%05x ea=%08x size=%-6d extra=%d' % (NAMES[e['kind']], e['cmd'] & 0xFF, e['lsa'], e['ea'], e['size'], e['extra']))

if __name__ == '__main__':
    main()
