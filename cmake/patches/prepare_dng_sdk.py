#!/usr/bin/env python3
"""Generate the minimal XMP-disabled compatibility translation unit for SDK 2768."""
import difflib
import hashlib
import pathlib
import sys

source=pathlib.Path(sys.argv[1])
output=pathlib.Path(sys.argv[2])
if hashlib.sha256(source.read_bytes()).hexdigest() != '915831cdad621c6de54795aa011a797f90ad212d3dafc3171ed8e0608053ad1b':
    raise SystemExit('Unknown SDK dng_jxl.cpp: review the XMP compatibility guards')
original=source.read_text()
begin='\t\tif (includeXMP && metadata && metadata->GetXMP ())'
end='\t\t\t} // xmp'
if original.count(begin)!=1 or original.count(end)!=1:
    raise SystemExit('Unexpected SDK source: XMP encoder guard needs review')
modified=original.replace(begin,'\t\t#if qDNGUseXMP\n'+begin).replace(end,end+'\n\t\t#endif // qDNGUseXMP')
begin='void dng_jxl_decoder::ProcessXMPBox ('
start=modified.index(begin)
brace=modified.index('\n\t{',start)+len('\n\t{')
stop=modified.index('\n/*****************************************************************************/',brace)
close=modified.rfind('\n\t}',brace,stop)
modified=modified[:close]+'\n\t#endif // qDNGUseXMP\n'+modified[close:]
modified=modified[:brace]+'\n\t#if qDNGUseXMP\n'+modified[brace:]
if not output.exists() or output.read_text() != modified:
    output.write_text(modified)
output.with_suffix('.patch').write_text(''.join(difflib.unified_diff(
    original.splitlines(True),modified.splitlines(True),
    fromfile='dng_sdk/source/dng_jxl.cpp',tofile='dng_sdk/source/dng_jxl.cpp')))
