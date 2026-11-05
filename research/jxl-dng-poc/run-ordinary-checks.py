#!/usr/bin/env python3
"""Compare no-host decoding between plain and SDK-enabled LibRaw 0.22.3."""
import json
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parent
fixtures={'nef':'fixtures/iss030e122639.NEF',
          'ordinary-dng':'sources/dng_sdk_1_7_1/sample_files/04_PGTM2_per_profile.dng'}
report={}
for name,path in fixtures.items():
    outputs={}
    for build in ('build-nosdk','build-sdk'):
        p=subprocess.run([str(root/build/'dng_probe'),str(root/path)],
                         stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        (root/'logs'/f'compat-{name}-{build}.log').write_text(p.stdout)
        lines=[line for line in p.stdout.splitlines() if 'fingerprint_fnv64=' in line
               or line.startswith('bitmap=') or 'host_attached=' in line]
        outputs[build]={'exit':p.returncode,'evidence':lines}
        if p.returncode or 'jxl_selected=0 host_attached=0' not in p.stdout:
            raise SystemExit(f'{name}/{build}: ordinary decoding failed or SDK host attached')
    fingerprints={b:[x for x in r['evidence'] if 'fingerprint_fnv64=' in x] for b,r in outputs.items()}
    equal=fingerprints['build-sdk']==fingerprints['build-nosdk'] and bool(fingerprints['build-sdk'])
    report[name]={'outputs':outputs,'matching_fingerprints':equal}
    (root/'logs/ordinary-compatibility.json').write_text(json.dumps(report,indent=2)+'\n')
    if not equal:raise SystemExit(f'{name}: fingerprint mismatch')
    print(name,'matching processed-camera and float-RGBA fingerprints')
