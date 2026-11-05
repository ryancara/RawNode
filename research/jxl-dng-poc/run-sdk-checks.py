#!/usr/bin/env python3
"""Sequential original-file SDK experiments; logs include numerical data and RSS."""
import json
from pathlib import Path
import subprocess
import time

root=Path(__file__).resolve().parent
binary=root/'build-sdk/dng_probe'
integer='fixtures/adobe-01-jxl-integer.dng'
floating='fixtures/adobe-02-jxl-float.dng'
cases=[
    ('sdk-selftest',['--selftest']),
    ('integer-default',[integer]),
    ('float-default',[floating]),
    ('integer-reference',['--reference-unpack',integer]),
    ('integer-opcode-reference',['--stage=opcode','--reference-unpack',integer]),
    ('integer-stage2-reference',['--stage=stage2','--reference-unpack',integer]),
    ('integer-stage3-reference',['--stage=stage3','--reference-unpack',integer]),
    ('integer-highlight1',['--stage=opcode','--highlight=1',integer]),
    ('float-reference',['--keep-float','--reference-unpack',floating]),
    ('float-stage2-reference',['--keep-float','--stage=stage2','--reference-unpack',floating]),
    ('jxl-host-disabled',['--no-sdk',integer]),
]
summary={}
failed=[]
for name,args in cases:
    start=time.perf_counter()
    result=subprocess.run([str(binary),*args],cwd=root,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (root/'logs'/f'{name}.log').write_text(result.stdout)
    lines=[line for line in result.stdout.splitlines() if any(key in line for key in
        ('unpack_ms=','sdk_reference','bitmap=','peak_rss_bytes=','ERROR:','unpacked_float channel='))]
    summary[name]={'arguments':args,'exit':result.returncode,'elapsed_s':time.perf_counter()-start,'evidence':lines}
    (root/'logs/sdk-checks.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(name,'exit',result.returncode,'seconds',round(summary[name]['elapsed_s'],3),flush=True)
    if result.returncode and name!='jxl-host-disabled':
        print(result.stdout[-2200:],flush=True)
    expected=1 if name=='jxl-host-disabled' else 0
    if result.returncode!=expected:failed.append(name)
    if name not in ('sdk-selftest','jxl-host-disabled') and 'sdk_processed=1' not in result.stdout:failed.append(name+': no SDK evidence')
if failed:raise SystemExit('Unexpected results: '+', '.join(failed))
