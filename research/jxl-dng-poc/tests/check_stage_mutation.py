#!/usr/bin/env python3
"""Restore the reviewed stage bug in a temporary translation unit; require failure.

GCC/Clang + CMake Makefiles audit helper. Configure the SDK build with
-DCMAKE_EXPORT_COMPILE_COMMANDS=ON. Never edits the checkout or its build objects.
"""
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import shutil

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
entry = next(e for e in json.loads((build / 'compile_commands.json').read_text())
             if Path(e['file']).resolve() == root / 'src/probe.cpp')
with tempfile.TemporaryDirectory(prefix='jxl-stage-mutation-') as directory:
    scratch = Path(directory)
    shutil.copytree(root / 'src', scratch / 'src')
    shutil.copytree(root / 'tests', scratch / 'tests')
    source = scratch / 'src/probe.cpp'
    text = source.read_text()
    condition = 'if(o.stage=="auto" && hasOpcode2)'
    flag = 'LIBRAW_RAWOPTIONS_DNG_STAGE2 | LIBRAW_RAWOPTIONS_DNG_ALLOWSIZECHANGE'
    assert text.count(condition) == 1 and text.count(flag) == 1
    source.write_text(text.replace(condition, 'if(o.stage=="auto")').replace(
        flag, 'LIBRAW_RAWOPTIONS_DNG_STAGE23_IFPRESENT_JPGJXL'))
    command = shlex.split(entry['command'])
    for option in ('-o', '-MF', '-MT'):
        if option in command:
            i = command.index(option)
            if option == '-o':
                command[i + 1] = str(scratch / 'probe.o')
            else:
                del command[i:i + 2]
    command = [a for a in command if a != '-MD']
    command[command.index(entry['file'])] = str(source)
    subprocess.run(command, cwd=entry['directory'], check=True)
    link = shlex.split((build / 'CMakeFiles/dng_probe.dir/link.txt').read_text())
    link = [a for a in link if not a.startswith('-Wl,--dependency-file=')]
    obj = next(a for a in link if a.endswith('/src/probe.cpp.o'))
    link[link.index(obj)] = str(scratch / 'probe.o')
    link[link.index('-o') + 1] = str(scratch / 'mutant')
    subprocess.run(link, cwd=build, check=True)
    result = subprocess.run([str(scratch / 'mutant'), '--jxl-selftest'],
                            capture_output=True, text=True)
    print(result.stdout)
    print(result.stderr)
    if result.returncode != 1 or 'Independent normalized/polynomial pixel oracle failed' not in result.stderr:
        raise SystemExit('Mutation did not fail the intended numerical regression')
    print('PASS: restored conditional stage bug is caught by independent pixel expectations')
