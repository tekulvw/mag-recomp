"""Refuse incomplete Group12 diagnostics; return each root's own bounded census."""
import argparse
import json
import re
from pathlib import Path


def derive(events, root=1):
    def require(ok, why):
        if not ok:
            raise ValueError(why)
    require(type(root) is int and 1 <= root <= 6, 'root bound')
    require(not any(e.get('event') == 'overflow' for e in events), 'association overflow')
    sessions = [e for e in events if e.get('event') == 'session']
    require(len(sessions) == 1 and sessions[0].get('pid', 0) > 0 and sessions[0].get('session'), 'session')
    def rows(kind):
        return [e for e in events if e.get('event') == 'native_program_identity_water_' + kind and e.get('root') == root]
    require(not rows('refused'), 'water diagnostic refused')
    intervals, done = rows('interval'), rows('complete')
    require(len(intervals) == len(done) == 1, 'unique complete water census')
    b, z = intervals[0], done[0]
    require(b.get('complete') is True and z.get('complete') is True, 'source/publication incomplete')
    n = b['selected']
    require(type(n) is int and 0 <= n <= 16 and z['selected'] == z['published'] == n, 'selected/publication count')
    require(1 <= b['fragments'] <= 512 and 0 <= b['begin_operation'] < b['end_operation'] < b['operations'] <= 65536, 'budgets/order')
    require((b['source_marker_kind'], b['source_marker_method'], b['source_marker_value']) == (1, 0x1bcc, 0), 'typed crossed12 marker')
    require(0 < b['source_group13_call'] <= 0x1ffffffc and b['source_group13_call'] % 4 == 0, 'crossed13 frontier')
    counts = [b['geometry_operations'], b['draw_operations'], b['inherited_draw_operations']]
    require(all(type(c) is int and 0 <= c < b['operations'] for c in counts), 'operation counts')
    require(sum(counts[:2]) + n <= b['end_operation'] - b['begin_operation'] - 1 and counts[2] <= counts[1], 'interval count extent')
    require(counts[2] == 0, 'draw program inherited before water binding; no selector attribution')
    fs, cs = rows('fragment'), rows('control')
    require(len(fs) == len(cs) == n and len({e['index'] for e in fs}) == len({e['index'] for e in cs}) == n and {e['index'] for e in fs} == {e['index'] for e in cs}, 'unique complete selected identities')
    allocations = [e for e in events if e.get('event') == 'native_fp' and e.get('root') == root]
    require(sorted(e['index'] for e in allocations) == list(range(b['fragments'])) and all(e['phase'] == b['source_phase'] for e in allocations), 'own complete allocations/phase')
    receipts = [e for e in events if e.get('event') == 'gpu_receipt' and e.get('root') == root]
    require(len(receipts) == 1 and receipts[0].get('ticket', 0) > 0 and receipts[0].get('report_generation', 0) > 0, 'own genuine receipt')
    am, cm = {e['index']: e for e in allocations}, {e['index']: e for e in cs}
    selected = []
    for f in fs:
        i = f['index']; c = cm[i]
        require(i in am, 'selected allocation')
        a = am[i]
        require(c['status'] == 'present' and type(c['value']) is int and 0 <= c['value'] <= 0xffffffff, 'known published control')
        require(re.fullmatch(r'[0-9A-F]{16}', f['structural_hash']) and int(f['structural_hash'], 16) and 16 <= f['code_bytes'] <= min(a['bytes'], 4096) and f['code_bytes'] % 16 == 0, 'program identity/extent')
        require(a['location'] == 1 and a['offset'] > 0 and a['offset'] % 128 == 0, 'own mapped allocation')
        selected.append(dict(fragment=i, structural_hash=f['structural_hash'], shader_control=f'{c["value"]:08X}', selector=f'{f["structural_hash"]}:{c["value"]:08X}', offset=a['offset'], location=a['location'], allocation_bytes=a['bytes']))
    return dict(root=root, source_phase=b['source_phase'], call_interval=[b['begin_operation'], b['end_operation']], geometry_operations=counts[0], draw_operations=counts[1], empty=not n and not any(counts), selectors=selected, scope='source-group12 candidates/census; actual producer draw and pixel association still required')


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('association', type=Path); p.add_argument('--root', type=int, default=1); a = p.parse_args()
    print(json.dumps(derive([json.loads(line) for line in a.association.read_text().splitlines()], a.root), indent=2))
