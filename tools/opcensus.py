#!/usr/bin/env python3
# COPY. The canonical file is tools/opcensus.py in ratlizard/cythera-workbench;
# fix it there and re-copy. This repository is retired, so this copy exists
# only so port/ can run standalone -- do not edit it to diverge.
# Verify with tools/check_copies.sh. Source sha256 57cca4bee875c72005cde04720dc6bd5295e5bcf5cce35116e59a0a1711a38b1.
"""Census the PowerPC opcodes actually used by Cythera's PEF code section.

    python3 tools/opcensus.py build/extract/Cythera.data cythera_symbols.txt

The code section has traceback tables interleaved between functions, so the
symbol file's (address, byte-length) pairs are used to scan only real
instructions. Prints one line per distinct (primary, extended) opcode pair
with its count, which is exactly the interpreter's required scope.
"""
import collections, re, struct, sys

# Primary opcodes that carry a secondary opcode field, and where it lives.
#   19, 31, 63  -> bits 21..30 (10 bits)
#   59          -> bits 26..30 (5 bits, but 10-bit read is fine for grouping)
#   30          -> bits 27..29 (rotate doubleword; 64-bit only)
EXT_10 = {19, 31, 63, 59, 4}

def sections(path):
    b = open(path, 'rb').read()
    nsec = struct.unpack_from('>H', b, 32)[0]
    for i in range(nsec):
        o = 40 + i*28
        _, addr, total, unp, packed, coff = struct.unpack_from('>iIIIII', b, o)
        kind = b[o+24]
        if kind == 0:
            return b[coff:coff+packed]
    raise SystemExit('no code section')

def ranges(symfile):
    out = []
    pat = re.compile(r'^0x([0-9A-Fa-f]+)\s+(\d+)\s+(\d+)\s+(\S+)')
    for line in open(symfile, encoding='utf-8', errors='replace'):
        m = pat.match(line)
        if m:
            out.append((int(m.group(1), 16), int(m.group(2)), m.group(4)))
    return out

def main():
    code = sections(sys.argv[1])
    rs = ranges(sys.argv[2])
    hist = collections.Counter()
    scanned = 0
    for addr, nbytes, _name in rs:
        for off in range(addr, addr + (nbytes & ~3), 4):
            if off + 4 > len(code):
                break
            w = struct.unpack_from('>I', code, off)[0]
            pri = w >> 26
            if pri in EXT_10:
                ext = (w >> 1) & 0x3FF
                hist[(pri, ext)] += 1
            else:
                hist[(pri, None)] += 1
            scanned += 1
    print(f"# scanned {scanned} instructions in {len(rs)} functions "
          f"({len(code)} byte code section)")
    print(f"# {len(hist)} distinct opcode forms")
    print(f"{'pri':>4}{'ext':>6}{'count':>10}")
    for (pri, ext), n in hist.most_common():
        print(f"{pri:>4}{('-' if ext is None else str(ext)):>6}{n:>10}")

main()
