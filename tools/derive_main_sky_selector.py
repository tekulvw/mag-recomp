"""Refuse incomplete diagnostic metadata; derive exact bounded group13 selectors."""
import argparse
import json
import re
from pathlib import Path

def derive(events):
    def require(ok, why):
        if not ok:
            raise ValueError(why)
    require(not any(e.get('event') in ('overflow', 'native_program_identity_refused') for e in events), 'refused/overflowed log')
    sessions = [e for e in events if e.get('event') == 'session']
    require(len(sessions) == 1 and sessions[0].get('pid', 0) > 0 and sessions[0].get('session'), 'session')
    def rows(kind):
        return [e for e in events if e.get('event') == 'native_program_identity_' + kind and e.get('root') == 1]
    begin, end = rows('begin'), rows('prepared')
    require(len(begin) == len(end) == 1 and end[0].get('complete') is True, 'complete root1 metadata')
    b, z = begin[0], end[0]
    n, count = b['fragments'], b['entries']
    require(1 <= n <= 512 and 1 <= count <= 1024 and 1 <= b['operations'] <= 65536, 'budgets')
    require(z['fragments'] == n and z['entries'] == count, 'summary counts')
    fs, order, cs = rows('fragment'), rows('order'), rows('control')
    require(len(fs) == len(cs) == n and len(order) == count, 'entry counts')
    require(sorted(e['index'] for e in fs) == sorted(e['index'] for e in cs) == list(range(n)), 'unique complete fragment/control indices')
    require(all(re.fullmatch(r'[0-9A-F]{16}', e['structural_hash']) and int(e['structural_hash'], 16) and 16 <= e['code_bytes'] <= 4096 and e['code_bytes'] % 16 == 0 for e in fs), 'program identity')
    require(all(e['status'] in ('present', 'inherited') and 0 <= e['value'] <= 0xffffffff for e in cs), 'control parse')
    require(all(e['kind'] in (2, 6) for e in order), 'operation kind')
    positions = [e['operation'] for e in order]
    require(positions == sorted(set(positions)) and 0 <= positions[0] and positions[-1] < b['operations'], 'source order')
    require(sorted(e['index'] for e in order if e['kind'] == 6) == list(range(n)), 'complete fragment traversal')
    calls = [e for e in order if e['kind'] == 2]
    require(all(0 < e['target'] <= 0x1ffffffc and e['target'] % 4 == 0 for e in calls), 'call targets')
    targets = [b['source_group13_call'], b['source_group15_call'], b['source_group15_second_call']]
    require(len(set(targets)) == 3 and all(t > 0 and t % 4 == 0 for t in targets), 'source call aliases')
    matches = [i for i, e in enumerate(calls) if e['target'] == targets[0]]
    require(len(matches) == 1, 'unique crossed13 call')
    i = matches[0]
    require(i + 2 < len(calls) and [e['target'] for e in calls[i+1:i+3]] == targets[1:], 'crossed15 end frontier')
    lo, hi = calls[i]['operation'], calls[i+1]['operation']
    selected = [e['index'] for e in order if e['kind'] == 6 and lo < e['operation'] < hi]
    require(selected, 'empty group13 interval')
    require(any(e.get('event') == 'gpu_receipt' and e.get('root') == 1 and e.get('ticket', 0) > 0 and e.get('report_generation', 0) > 0 for e in events), 'genuine root1 receipt')
    allocations = [e for e in events if e.get('event') == 'native_fp' and e.get('root') == 1]
    require(sorted(e['index'] for e in allocations) == list(range(n)) and all(e['phase'] == b['source_phase'] for e in allocations), 'actual root allocations/phase')
    fm, cm = {e['index']:e for e in fs}, {e['index']:e for e in cs}
    require(all(cm[j]['status'] == 'present' for j in selected), 'group13 must full-bind control')
    return dict(root=1,source_phase=b['source_phase'],call_interval=[lo,hi],selectors=[dict(fragment=j,structural_hash=fm[j]['structural_hash'],shader_control=f'{cm[j]["value"]:08X}',selector=f'{fm[j]["structural_hash"]}:{cm[j]["value"]:08X}') for j in selected])

def derive_for_root(events, root):
    """Join a captured root's own bounded source interval to its actual FP spans."""
    # Root-one full source order remains an independent completeness anchor.
    derive(events)
    def require(ok, why):
        if not ok:
            raise ValueError(why)
    require(root in range(1, 7), 'captured root bound')
    def rows(kind):
        return [e for e in events if e.get('event') == 'native_program_identity_sky_' + kind and e.get('root') == root]
    intervals, done = rows('interval'), rows('complete')
    require(len(intervals) == len(done) == 1, 'unique captured-root summary')
    b, z = intervals[0], done[0]
    require(b['complete'] is True and z['complete'] is True, 'captured-root source/publication complete')
    n = b['selected']
    require(1 <= n <= 16 and z['selected'] == z['published'] == n, 'captured-root candidate count')
    require(1 <= b['fragments'] <= 512 and 1 <= b['entries'] <= 1024 and 0 <= b['begin_operation'] < b['end_operation'] < b['operations'] <= 65536, 'captured-root budgets/order')
    targets = [b['source_group13_call'], b['source_group15_call'], b['source_group15_second_call']]
    require(len(set(targets)) == 3 and all(0 < t <= 0x1ffffffc and t % 4 == 0 for t in targets), 'captured-root call aliases')
    fs, cs = rows('fragment'), rows('control')
    require(len(fs) == len(cs) == n and len({e['index'] for e in fs}) == n and {e['index'] for e in fs} == {e['index'] for e in cs}, 'captured-root unique complete identities')
    cm = {e['index']:e for e in cs}
    allocations = [e for e in events if e.get('event') == 'native_fp' and e.get('root') == root]
    require(sorted(e['index'] for e in allocations) == list(range(b['fragments'])), 'captured-root complete allocations')
    am = {e['index']:e for e in allocations}
    receipt = [e for e in events if e.get('event') == 'gpu_receipt' and e.get('root') == root]
    require(len(receipt) == 1 and receipt[0].get('ticket', 0) > 0 and receipt[0].get('report_generation', 0) > 0, 'captured-root genuine receipt')
    result = []
    for f in fs:
        i = f['index']; c = cm[i]; a = am[i]
        require(c['status'] == 'present' and 0 <= c['value'] <= 0xffffffff, 'captured-root known published control')
        require(re.fullmatch(r'[0-9A-F]{16}', f['structural_hash']) and int(f['structural_hash'], 16) and 16 <= f['code_bytes'] <= min(a['bytes'], 4096) and f['code_bytes'] % 16 == 0, 'captured-root program')
        require(a['phase'] == b['source_phase'] and a['location'] == 1 and a['offset'] > 0 and a['offset'] % 128 == 0, 'captured-root source allocation')
        result.append(dict(fragment=i,structural_hash=f['structural_hash'],shader_control=f'{c["value"]:08X}',selector=f'{f["structural_hash"]}:{c["value"]:08X}',offset=a['offset'],location=a['location'],allocation_bytes=a['bytes']))
    return dict(root=root,source_phase=b['source_phase'],call_interval=[b['begin_operation'],b['end_operation']],selectors=result)

if __name__ == '__main__':
    p = argparse.ArgumentParser();p.add_argument('association', type=Path);p.add_argument('--root',type=int);a = p.parse_args()
    events=[json.loads(line) for line in a.association.read_text().splitlines()]
    print(json.dumps(derive_for_root(events,a.root) if a.root is not None else derive(events), indent=2))
