"""Mine item-level rules for a recorded SPU job's fixed-size outputs.

For every output word it labels each item by the simplest explanation available (a constant, a copy of
one field of a fetched record at some pipeline lag, or an offset from the output's destination) and fits
a small decision tree over observable features (which records were fetched, and whether record words
are zero, all ones or take one of a few values). Rules are fitted on one recording and can be scored
on others, so a rule that survives held-out recordings is evidence rather than a fit.
Observation only; recordings contain game data and stay out of Git."""
import argparse, json, struct, sys
from collections import Counter, defaultdict
from tools.jobio import parse
from tools.jobio_provenance import items, word_at

RANKS = (0, 1, 2)

def feature_table(its, max_values=6):
    """Boolean features per item as integer bitmasks over items: name -> mask."""
    n = len(its); feats = {}
    def put(name, i):
        feats[name] = feats.get(name, 0) | (1 << i)
    cols = defaultdict(list)  # (size, lag, off) -> values per item index
    for i, it in enumerate(its):
        put('template=%08x' % it['words'][1], i)
        for size in it['fresh']:
            put('fresh(%s)' % (size,), i)
        for size in it['fresh_prev']:
            put('fresh_prev(%s)' % (size,), i)
        for (size, rank), data in it['blocks'].items():
            for off in range(0, len(data) - 3, 4):
                cols[(size, rank, off)].append((i, word_at(data, off)))
    for (size, rank, off), vals in cols.items():
        distinct = Counter(v for _, v in vals)
        tag = '(%s,r%s,+%d)' % (size, rank, off)
        for i, v in vals:
            if v == 0: put('zero' + tag, i)
            elif v == 0xFFFFFFFF: put('ones' + tag, i)
        if len(distinct) <= max_values:
            for v in distinct:
                if v not in (0, 0xFFFFFFFF):
                    for i, x in vals:
                        if x == v: put('eq%08x' % v + tag, i)
    return feats

def candidate_sets(its, p):
    """Map candidate explanation -> bitmask of items it explains, for output word p."""
    cands = defaultdict(int)
    for i, it in enumerate(its):
        v = it['words'][p]
        cands[('const', v)] |= 1 << i
        cands[('dest+', (v - it['dest']) & 0xFFFFFFFF)] |= 1 << i
        for (size, rank), data in it['blocks'].items():
            for off in range(0, len(data) - 3, 4):
                if word_at(data, off) == v:
                    cands[('copy', size, rank, off)] |= 1 << i
    return cands

def label_items(its, p, max_labels=6, prefer_copy=True):
    cands = candidate_sets(its, p); n = len(its)
    remaining = (1 << n) - 1; labels = []; assignment = {}
    # On equal coverage prefer a field copy, then a destination offset, then a constant: a value
    # that is constant in one recording may still be a per-frame input.
    preference = {'copy': 2, 'dest+': 1, 'const': 0} if prefer_copy else {'const': 2, 'dest+': 1, 'copy': 0}
    while remaining and len(labels) < max_labels:
        best = max(cands.items(), key=lambda kv: ((kv[1] & remaining).bit_count(), preference[kv[0][0]]))
        gain = best[1] & remaining
        if gain.bit_count() < 3:
            break
        labels.append(best[0]); assignment[best[0]] = gain; remaining &= ~gain
    return labels, assignment, remaining

def gini(counts, total):
    return 1.0 - sum((c / total) ** 2 for c in counts) if total else 0.0

def fit_tree(classes, feats, universe, depth, min_leaf=3):
    sizes = [(name, (mask & universe).bit_count()) for name, mask in classes.items()]
    total = sum(c for _, c in sizes)
    if total == 0:
        return ('leaf', None, 0)
    majority = max(sizes, key=lambda x: x[1])
    if majority[1] == total or depth == 0:
        return ('leaf', majority[0], total)
    base = gini([c for _, c in sizes], total); best = None
    for name, mask in feats.items():
        yes = universe & mask; no = universe & ~mask
        ny, nn = yes.bit_count(), no.bit_count()
        if ny < min_leaf or nn < min_leaf:
            continue
        gy = gini([(m & yes).bit_count() for m in classes.values()], ny)
        gn = gini([(m & no).bit_count() for m in classes.values()], nn)
        score = base - (ny * gy + nn * gn) / total
        if best is None or score > best[0] + 1e-12:
            best = (score, name, yes, no)
    if best is None or best[0] <= 1e-9:
        return ('leaf', majority[0], total)
    _, name, yes, no = best
    return ('split', name, fit_tree(classes, feats, yes, depth - 1, min_leaf), fit_tree(classes, feats, no, depth - 1, min_leaf))

def classify(tree, feats, i):
    while tree[0] == 'split':
        tree = tree[2] if (feats.get(tree[1], 0) >> i) & 1 else tree[3]
    return tree[1]

def evaluate_label(its, p, label, i):
    """Whether explanation `label` yields the true value of word p for item i."""
    it = its[i]; v = it['words'][p]
    if label is None: return False
    if label[0] == 'const': return v == label[1]
    if label[0] == 'dest+': return v == ((it['dest'] + label[1]) & 0xFFFFFFFF)
    _, size, rank, off = label
    data = it['blocks'].get((size, rank))
    return data is not None and word_at(data, off) == v

def show(tree, indent=''):
    if tree[0] == 'leaf':
        return indent + 'use %s  (%d items)\n' % (tree[1], tree[2])
    return indent + 'if %s:\n' % tree[1] + show(tree[2], indent + '  ') + indent + 'else:\n' + show(tree[3], indent + '  ')

def mine(rec, out_size=464, depth=5):
    its = items(rec, out_size, max(RANKS)); feats = feature_table(its); n = len(its)
    model = {}
    for p in range(out_size // 4):
        labels, assignment, rest = label_items(its, p)
        if not labels:
            model[p] = dict(labels=[], tree=('leaf', None, n), accuracy=0.0); continue
        classes = {l: assignment[l] for l in labels}
        tree = fit_tree(classes, feats, (1 << n) - 1, depth)
        ok = sum(evaluate_label(its, p, classify(tree, feats, i), i) for i in range(n))
        model[p] = dict(labels=labels, tree=tree, accuracy=ok / n)
    return its, feats, model

def score(model, rec, out_size=464):
    """Held-out accuracy of a mined model on another recording."""
    its = items(rec, out_size, max(RANKS)); feats = feature_table(its); n = len(its)
    return {p: sum(evaluate_label(its, p, classify(m['tree'], feats, i), i) for i in range(n)) / n for p, m in model.items()}

def main(argv=None):
    ap = argparse.ArgumentParser(); ap.add_argument('train'); ap.add_argument('test', nargs='*')
    ap.add_argument('--depth', type=int, default=5); ap.add_argument('--words', action='store_true')
    a = ap.parse_args(argv)
    its, feats, model = mine(parse(open(a.train, 'rb').read()), depth=a.depth)
    tests = [score(model, parse(open(t, 'rb').read())) for t in a.test]
    n = len(its)
    train_acc = sum(m['accuracy'] for m in model.values()) / len(model)
    print('items %d, features %d, mean training accuracy %.4f' % (n, len(feats), train_acc))
    if tests:
        mean_test = sum(sum(t.values()) / len(t) for t in tests) / len(tests)
        print('mean held-out accuracy over %d recordings: %.4f' % (len(tests), mean_test))
    weak = [(p, m['accuracy']) for p, m in model.items() if m['accuracy'] < 0.995]
    print('words below 99.5%% training accuracy: %s' % [(p, round(acc, 3)) for p, acc in weak])
    if tests:
        weak_t = [(p, round(min(t[p] for t in tests), 3)) for p in model if min(t[p] for t in tests) < 0.995]
        print('words below 99.5%% on some held-out recording: %s' % weak_t)
    if a.words:
        for p, m in sorted(model.items()):
            print('--- word %d (offset %d) accuracy %.4f' % (p, p * 4, m['accuracy']))
            print(show(m['tree']), end='')

if __name__ == '__main__':
    main()
