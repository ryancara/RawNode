#!/usr/bin/env python3
"""Compare separate serial/parallel 0.22.3 probes; never link two LibRaw copies."""
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys

serial, parallel, image = (str(Path(p).resolve()) for p in sys.argv[1:])
environment = dict(os.environ, OMP_NUM_THREADS='3', OMP_DYNAMIC='FALSE')
report = {'threads': 3, 'runs': {}, 'scope': 'Single-host diagnostics, not a controlled benchmark'}
expected = None
for repetition in range(3):
    for name, binary in [('serial', serial), ('openmp', parallel)]:
        p = subprocess.run([binary, image], capture_output=True, text=True,
                           env=environment, check=True)
        fingerprints = re.findall(r'(camera_processed|linear_rec2020) fingerprint_fnv64=(\d+)', p.stdout)
        if not fingerprints or expected is not None and fingerprints != expected:
            raise SystemExit('Ordinary RAW pixel fingerprint changed')
        expected = fingerprints
        timing = re.search(r'bitmap=.*unpack_ms=([\d.]+) total_after_identify_ms=([\d.]+)', p.stdout)
        report['runs'].setdefault(name, []).append({'unpack_ms': float(timing[1]),
            'total_after_identify_ms': float(timing[2]), 'fingerprints': fingerprints})
report['matching_fingerprints'] = True
report['median_total_ms'] = {name: statistics.median(r['total_after_identify_ms'] for r in runs)
                             for name, runs in report['runs'].items()}
print(json.dumps(report, indent=2))
