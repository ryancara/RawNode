"""Generate the ordinary 64x48 linear DNG fixture from tests/RawImageTests.cpp.
No real photograph or external converter. JXL and specialised variants omitted.
"""
from pathlib import Path
import struct
import sys

def little(values, width):
    return b''.join((v & ((1 << (8 * width)) - 1)).to_bytes(width, 'little') for v in values)

pixels = bytearray()
for y in range(48):
    for x in range(64):
        rgb = (256+7*x+3*y, 1300+2*x+11*y, 2400+13*x+5*y)
        pixels.extend(little([4095 if (x,y)==(32,24) else v for v in rgb], 2))
tags = []
def tag(number, kind, count, data):
    tags.append((number, kind, count, data))
for n,v in [(254,0),(256,64),(257,48),(273,0),(278,48),(279,len(pixels)),(50714,0)]:
    tag(n,4,1,little([v],4))
for n,v in [(259,1),(262,34892),(274,1),(277,3),(284,1),(50778,21)]:
    tag(n,3,1,little([v],2))
tag(258,3,3,little([16]*3,2)); tag(339,3,3,little([1]*3,2))
tag(50706,1,4,bytes([1,7,1,0])); tag(50707,1,4,bytes([1,3,0,0]))
model=b'RawNode synthetic linear Rec2020\0'; tag(50708,2,len(model),model)
tag(50717,4,3,little([4095]*3,4)); tag(50719,4,2,little([3,2],4)); tag(50720,4,2,little([44,34],4))
matrix=[1716651188,-355670784,-253366281,-666684352,1616481237,15768546,17639857,-42770613,942103121]
tag(50721,10,9,little([x for n in matrix for x in (n,1000000000)],4))
tag(50728,5,3,little([1]*6,4))
tags.sort(); output=bytearray(8+2+12*len(tags)+4); output[:8]=b'II*\0\x08\0\0\0'
struct.pack_into('<H',output,8,len(tags)); strip=0
for i,(number,kind,count,data) in enumerate(tags):
    p=10+i*12; struct.pack_into('<HHI',output,p,number,kind,count)
    if number==273: strip=p+8
    if len(data)<=4: output[p+8:p+8+len(data)]=data
    else:
        struct.pack_into('<I',output,p+8,len(output)); output.extend(data)
        if len(output)%2: output.append(0)
struct.pack_into('<I',output,strip,len(output)); output.extend(pixels)
Path(sys.argv[1]).write_bytes(output)
