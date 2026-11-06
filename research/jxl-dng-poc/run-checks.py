#!/usr/bin/env python3
"""Run the compiled probe, retaining complete logs and explicit exit statuses."""
import json
import pathlib
import subprocess

root=pathlib.Path(__file__).resolve().parent
cases={
    'selftest':(['--selftest'],0),
    'adobe-integer-nosdk':(['fixtures/adobe-01-jxl-integer.dng'],1),
    'adobe-float-nosdk':(['fixtures/adobe-02-jxl-float.dng'],1),
    'sdk-mode-refusal':(['--stage=opcode','fixtures/adobe-01-jxl-integer.dng'],1),
    'missing-input':(['does-not-exist.dng'],1),
}
summary={}
for name,(arguments,expected) in cases.items():
    p=subprocess.run([str(root/'build-nosdk/dng_probe'),*arguments],cwd=root,
                     stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (root/'logs'/f'{name}.log').write_text(p.stdout)
    summary[name]={'exit':p.returncode,'expected_exit':expected}
    if p.returncode!=expected:
        raise SystemExit(f'{name}: unexpected exit {p.returncode}; inspect log')
    if name=='adobe-integer-nosdk' and ('jxl_selected=1' not in p.stdout or 'unpack: Unsupported file format' not in p.stdout):
        raise SystemExit(f'{name}: expected classified JXL / unsupported unpack evidence missing')
    if name=='adobe-float-nosdk' and 'Unsupported floating-point JPEG XL DNG' not in p.stdout:
        raise SystemExit(f'{name}: missing explicit float diagnostic')
(root/'logs/checks.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
