#!/usr/bin/env python3
"""Decode a BinHex 4.0 (.hqx) file into its data and resource forks.

    python3 utilities/binhex_decode.py "reference/game/Cythera Data.hqx" [outdir]

Writes <outdir>/<name>.data and <outdir>/<name>.rsrc and prints the header.
Defaults outdir to the input file's directory.
"""
import os
import struct
import sys

# BinHex 4.0 packs 3 bytes into 4 characters of this 64-character alphabet.
ALPHA = '!"#$%&\'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr'
REV = {c: i for i, c in enumerate(ALPHA)}


# Python 3.11 dropped binascii.a2b_hqx/rledecode_hqx, so both stages are done
# here. The archives are ~11M characters, which rules out a per-character loop:
# the unpacking goes through bytes.translate and strided slices, and the RLE
# expansion walks run to run with find() rather than byte to byte.
_XLATE = bytearray(255 for _ in range(256))
for _i, _c in enumerate(ALPHA):
    _XLATE[ord(_c)] = _i
_XLATE = bytes(_XLATE)
_JUNK = bytes(b for b in range(256) if chr(b) not in ALPHA)


def debinhex(raw):
    """Undo the 6-bit packing and the 0x90 run-length encoding.

    Takes the raw bytes of a .hqx file, returns the decoded header+forks blob.
    """
    start = raw.index(b':')
    body = raw[start + 1:]
    body = body[:body.index(b':')]

    # Each surviving byte becomes its 6-bit value; everything else disappears.
    vals = body.translate(_XLATE, _JUNK)
    n4 = len(vals) - (len(vals) % 4)
    a, b, c, d = vals[0:n4:4], vals[1:n4:4], vals[2:n4:4], vals[3:n4:4]

    packed = bytearray(len(a) * 3)
    packed[0::3] = bytes((x << 2 | y >> 4) & 0xFF for x, y in zip(a, b))
    packed[1::3] = bytes((y << 4 | z >> 2) & 0xFF for y, z in zip(b, c))
    packed[2::3] = bytes((z << 6 | w) & 0xFF for z, w in zip(c, d))

    # Trailing 6-bit values that did not fill a whole group still carry bits.
    rest = vals[n4:]
    if len(rest) >= 2:
        bits = nbits = 0
        for v in rest:
            bits = (bits << 6) | v
            nbits += 6
            if nbits >= 8:
                nbits -= 8
                packed.append((bits >> nbits) & 0xFF)

    out = bytearray()
    pos = 0
    while True:
        hit = packed.find(0x90, pos)
        if hit < 0:
            out += packed[pos:]
            break
        out += packed[pos:hit]
        if hit + 1 >= len(packed):
            out.append(0x90)
            break
        n = packed[hit + 1]
        if n == 0:
            out.append(0x90)            # 0x90 0x00 is a literal 0x90
        elif out:
            out.extend(out[-1:] * (n - 1))
        pos = hit + 2
    return bytes(out)


def split_forks(d):
    p = 0
    nlen = d[p]; p += 1
    name = d[p:p + nlen].decode('mac-roman', 'replace'); p += nlen
    p += 1                                        # version byte
    ftype = d[p:p + 4]; p += 4
    fcreator = d[p:p + 4]; p += 4
    flags = struct.unpack('>H', d[p:p + 2])[0]; p += 2
    dlen = struct.unpack('>I', d[p:p + 4])[0]; p += 4
    rlen = struct.unpack('>I', d[p:p + 4])[0]; p += 4
    p += 2                                        # header CRC
    data = d[p:p + dlen]; p += dlen
    p += 2                                        # data CRC
    rsrc = d[p:p + rlen]
    return {
        'name': name, 'type': ftype, 'creator': fcreator, 'flags': flags,
        'data': data, 'rsrc': rsrc, 'dlen': dlen, 'rlen': rlen,
    }


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    src = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(src) or '.'

    with open(src, 'rb') as fh:
        info = split_forks(debinhex(fh.read()))

    stem = os.path.join(outdir, os.path.splitext(os.path.basename(src))[0])
    with open(stem + '.data', 'wb') as fh:
        fh.write(info['data'])
    with open(stem + '.rsrc', 'wb') as fh:
        fh.write(info['rsrc'])

    print(f"name:     {info['name']}")
    print(f"type:     {info['type']!r}  creator: {info['creator']!r}")
    print(f"data fork: {info['dlen']} bytes -> {stem}.data")
    print(f"rsrc fork: {info['rlen']} bytes -> {stem}.rsrc")
    return 0


if __name__ == '__main__':
    sys.exit(main())
