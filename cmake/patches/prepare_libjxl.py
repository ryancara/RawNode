#!/usr/bin/env python3
"""Apply the one-line encoder initialization fix verified by UBSan on 0.11.2."""
import hashlib
from pathlib import Path
import sys

source = Path(sys.argv[1]) / 'lib/jxl/enc_ans.h'
original = '9832a022d086300f2b11a14bd45b486ff7c201fe113e4c989488347eccb2bb7e'
patched = '6ea884da63e6293520692c02ac429ecfeb8d6794ced9cd039d590b2cef0b9c83'
data = source.read_bytes()
actual = hashlib.sha256(data).hexdigest()
if actual == original:
    data = data.replace(b'  bool use_prefix_code;', b'  bool use_prefix_code = false;')
    if hashlib.sha256(data).hexdigest() != patched:
        raise SystemExit('Unexpected libjxl compatibility result')
    source.write_bytes(data)
elif actual != patched:
    raise SystemExit('Refusing to change an unknown or modified libjxl enc_ans.h')
print('libjxl 0.11.2 encoder initialization patch verified:', patched)
