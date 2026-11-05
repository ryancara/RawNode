#!/usr/bin/env python3
"""Generate the minimal XMP-disabled compatibility translation unit for SDK 2768."""
import difflib
import pathlib
import sys

source=pathlib.Path(sys.argv[1])
output=pathlib.Path(sys.argv[2])
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
output.write_text(modified)
output.with_suffix('.patch').write_text(''.join(difflib.unified_diff(
    original.splitlines(True),modified.splitlines(True),
    fromfile='dng_sdk/source/dng_jxl.cpp',tofile='dng_sdk/source/dng_jxl.cpp')))
