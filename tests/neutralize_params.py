"""Hides every H.264 SPS/PPS after the first ones in an MPEG-TS file.

Their NAL unit type is changed to 0 ("unspecified"), which decoders ignore,
so the file keeps its exact layout but carries parameter sets only at the
start, like some broadcasts. Used to test that exported segments get their
parameter sets re-inserted.

usage: neutralize_params.py INPUT.ts OUTPUT.ts
"""
import sys

data = bytearray(open(sys.argv[1], 'rb').read())
seen = set()
changed = 0
i = data.find(b'\x00\x00\x01')
while i >= 0 and i + 3 < len(data):
    header = data[i + 3]
    nal_type = header & 0x1f
    if nal_type in (7, 8) and header & 0x80 == 0:
        if nal_type in seen:
            data[i + 3] = header & 0xe0  # keep nal_ref_idc, type 0
            changed += 1
        seen.add(nal_type)
    i = data.find(b'\x00\x00\x01', i + 3)
open(sys.argv[2], 'wb').write(data)
print(f'{changed} parameter sets hidden')
