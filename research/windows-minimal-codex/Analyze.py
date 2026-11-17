"""Verify downloaded native observations and retain a compact durable summary."""
import collections
import hashlib
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2])
sources = ['runner', 'ordinary', 'ProbeUtf8', 'ProbeScoped', 'ProbeUtf8Ctype',
           'new-permissions-other-runner', 'new-permissions-other-ordinary',
           'volume-ntfs', 'volume-fat32', 'volume-exfat']
summary = {'tested_commit': '0b6912455e6afcddd1e43b8f64e832a7d78e79fb',
           'run': 44109872879, 'sources': {}, 'rehashed_snapshots': 0,
           'note': 'Passing checks include demonstrations of unsafe unchanged baseline behavior.'}
for source in sources:
    rows = [json.loads(line) for line in (root / (source + '.jsonl')).read_text(
        encoding='utf-8-sig', errors='replace').splitlines() if line.startswith('{')]
    checks = [row for row in rows if row['type'] == 'check']
    assert all(row['pass'] for row in checks), source
    cases = collections.defaultdict(dict)
    verified = 0
    for row in rows:
        if 'case' not in row:
            continue
        case = cases[row['case']]
        if row['type'] == 'info':
            case[row['key']] = row['value']
        elif row['type'] == 'state':
            if row['exists'] and not row['read_error'] and row['bytes_complete'] and row['sha256']:
                assert hashlib.sha256(bytes.fromhex(row['bytes_hex'])).hexdigest() == row['sha256']
                verified += 1
            if row['role'] in ['destination', 'grade A', 'grade B', 'external target', 'other link']:
                case.setdefault('states', []).append({key: row[key] for key in [
                    'phase', 'role', 'path', 'exists', 'attributes', 'file_id',
                    'sha256', 'security_error', 'tag_error']})
    summary['sources'][source] = {'checks': len(checks), 'failed': 0,
                                  'rehashed': verified, 'cases': dict(cases)}
    summary['rehashed_snapshots'] += verified
summary['total_checks'] = sum(item['checks'] for item in summary['sources'].values())
output.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({key: summary[key] for key in ['total_checks', 'rehashed_snapshots']}))
