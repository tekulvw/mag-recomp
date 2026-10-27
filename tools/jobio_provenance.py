"""Black-box relation finder between the inputs and outputs of recorded SPU job runs.

Groups the 464-byte (or another fixed size) outputs of a job I/O recording into items, records
which optional input records each item fetched (its shape), and for every output word and shape
finds the simplest explanation: a constant, a copy of a fixed field of a fetched record, or a
fixed offset from the output's own destination address. It reports only observable relations;
it reads no original code. Recordings contain game data and stay out of Git."""
import argparse, json, struct, sys
from collections import Counter, defaultdict
from tools.jobio import parse, GET, PUT

RECORD_SIZES = (112, 192, 544, 656, 736, 1040, 1152, 1184, 1904, 2240)

def items(rec, out_size=464, max_rank=2):
    """One dict per output. Records are cached by the job, so an item without a fresh fetch of a kind
    uses an older one; sources are therefore ranked by recency within each kind.
    blocks[(size, r)] is the r-th most recent record of that size fetched before this output
    (r = 0 newest). `fresh` holds the records fetched since the previous output and `fresh_prev`
    those fetched between the two outputs before."""
    ev = rec['events']; last = -1; out = []; recent = defaultdict(list); prev_fresh = {}
    for i, e in enumerate(ev):
        if e['kind'] == PUT and e['size'] == out_size:
            fetched = {}; order = []
            for x in ev[last + 1:i]:
                if x['kind'] == GET and x['size'] in RECORD_SIZES:
                    fetched[x['size']] = x['data']; order.append(x['size']); recent[x['size']].append(x['data'])
            blocks = {}
            for size, lst in recent.items():
                for r in range(max_rank + 1):
                    if len(lst) > r:
                        blocks[(size, r)] = lst[-1 - r]
            out.append(dict(event=i, shape=tuple(order), blocks=blocks, fresh=fetched, fresh_prev=prev_fresh,
                            dest=e['ea'], words=struct.unpack('>%dI' % (out_size // 4), e['data'])))
            prev_fresh = fetched; last = i
    return out

def word_at(data, off):
    return struct.unpack_from('>I', data, off)[0]

def explain(group, p, min_fraction=0.99):
    """Simplest rule explaining output word p across the items of one shape group, or None."""
    n = len(group)
    vals = Counter(it['words'][p] for it in group)
    top, count = vals.most_common(1)[0]
    if count >= min_fraction * n:
        return ('const', top, count / n)
    cands = Counter()
    for it in group:
        v = it['words'][p]
        seen = set()
        for (size, rank), data in it['blocks'].items():
            for off in range(0, len(data) - 3, 4):
                if word_at(data, off) == v and (size, rank, off) not in seen:
                    seen.add((size, rank, off)); cands[(size, rank, off)] += 1
    if cands:
        (size, rank, off), c = cands.most_common(1)[0]
        if c >= min_fraction * n:
            return ('copy', (size, rank, off), c / n)
    deltas = Counter((it['words'][p] - it['dest']) & 0xFFFFFFFF for it in group)
    d, c = deltas.most_common(1)[0]
    if c >= min_fraction * n:
        return ('dest+', d if d < 1 << 31 else d - (1 << 32), c / n)
    return None

def derive(rec, out_size=464, min_group=8):
    its = items(rec, out_size)
    groups = defaultdict(list)
    for it in its:
        groups[(it['words'][1], tuple(sorted(it['fresh'])))].append(it)
    rules = {}; covered = 0; total = 0
    for key, g in groups.items():
        if len(g) < min_group:
            continue
        for p in range(out_size // 4):
            r = explain(g, p)
            rules[(key, p)] = r
            total += len(g)
            if r:
                covered += len(g)
    return its, groups, rules, covered, total

def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('recording'); ap.add_argument('--out-size', type=int, default=464)
    ap.add_argument('--min-group', type=int, default=8)
    a = ap.parse_args(argv)
    with open(a.recording, 'rb') as f:
        rec = parse(f.read())
    its, groups, rules, covered, total = derive(rec, a.out_size, a.min_group)
    big = sum(len(g) for g in groups.values() if len(g) >= a.min_group)
    unexplained = Counter(p for (key, p), r in rules.items() if r is None)
    print(json.dumps(dict(items=len(its), shapes=len(groups), items_in_analysed_shapes=big,
                          word_item_pairs_explained=covered, word_item_pairs=total,
                          explained_fraction=round(covered / total, 4) if total else None,
                          words_unexplained_in_some_shape={str(p): c for p, c in sorted(unexplained.items())}), indent=1))

if __name__ == '__main__':
    main()
