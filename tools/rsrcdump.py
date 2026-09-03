#!/usr/bin/env python3
# COPY. The canonical file is tools/rsrcdump.py in ratlizard/cythera-workbench;
# fix it there and re-copy. This repository is retired, so this copy exists
# only so port/ can run standalone -- do not edit it to diverge.
# Verify with tools/check_copies.sh. Source sha256 3f3160db6c20b62714c92ab8016a8d31ab6e18d78fe29abfc47669a6c0caef69.
"""Inventory a classic Mac resource fork: types, counts, total sizes."""
import struct, sys, collections

def main():
    b = open(sys.argv[1], 'rb').read()
    dat_off, map_off, dat_len, map_len = struct.unpack_from('>IIII', b, 0)
    type_off = map_off + struct.unpack_from('>H', b, map_off + 24)[0]
    name_off = map_off + struct.unpack_from('>H', b, map_off + 26)[0]
    ntypes = struct.unpack_from('>H', b, type_off)[0] + 1
    rows = []
    for i in range(ntypes):
        o = type_off + 2 + i*8
        rtype, cnt, ref_off = struct.unpack_from('>4sHH', b, o)
        cnt += 1
        total = 0
        ids = []
        for j in range(cnt):
            r = type_off + ref_off + j*12
            rid, nm_off, attr_and_data = struct.unpack_from('>hhI', b, r)
            doff = dat_off + (attr_and_data & 0x00FFFFFF)
            total += struct.unpack_from('>I', b, doff)[0]
            ids.append(rid)
        rows.append((rtype.decode('mac-roman', 'replace'), cnt, total,
                     min(ids), max(ids)))
    print(f"data fork region: {dat_len} bytes; map: {map_len} bytes; "
          f"{ntypes} types")
    print(f"{'type':<6}{'count':>6}{'bytes':>10}   id range")
    for t, c, tot, lo, hi in sorted(rows, key=lambda r: -r[2]):
        print(f"{t:<6}{c:>6}{tot:>10}   {lo} .. {hi}")
    print(f"{'TOTAL':<6}{sum(r[1] for r in rows):>6}{sum(r[2] for r in rows):>10}")

main()
