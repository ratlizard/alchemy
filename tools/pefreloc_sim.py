#!/usr/bin/env python3
# COPY. The canonical file is tools/pefreloc_sim.py in ratlizard/cythera-workbench;
# fix it there and re-copy. This repository is retired, so this copy exists
# only so port/ can run standalone -- do not edit it to diverge.
# Verify with tools/check_copies.sh. Source sha256 e4f7b99597ada344a17f617def6c6a08ce7f781f35989753a69abdd75972e59e.
"""Simulate PEF relocation for Cythera and validate the result.

Confirms the relocation opcode decoding before it is committed to C++: every
relocated word must land inside the code section, the data section, or the
synthetic import-vector region, and the instruction stream must be consumed
exactly. Run as:  python3 tools/pefreloc_sim.py build/extract/Cythera.data
"""
import collections, struct, sys

CODE_BASE = 0x00100000
DATA_BASE = 0x00800000
SHIM_BASE = 0x0F000000     # one 8-byte fake transition vector per import

def main():
    b = open(sys.argv[1], 'rb').read()
    nsec = struct.unpack_from('>H', b, 32)[0]
    secs = []
    for i in range(nsec):
        o = 40 + i*28
        _, addr, total, unp, packed, coff = struct.unpack_from('>iIIIII', b, o)
        secs.append(dict(i=i, total=total, unp=unp, packed=packed,
                         coff=coff, kind=b[o+24]))
    code = next(s for s in secs if s['kind'] == 0)
    data = next(s for s in secs if s['kind'] == 2)
    ld   = next(s for s in secs if s['kind'] == 4)
    base = ld['coff']
    (mainSec, mainOff, initSec, initOff, termSec, termOff, nlib, nsym,
     nrel, relOff, strOff, ehOff, ehPow, nexp) = struct.unpack_from(
        '>iIiIiIIIIIIIII', b, base)

    # ---- expand the pattern-initialised data section -------------------------
    mem = bytearray(data['total'])
    src = base = ld['coff']
    p = data['coff']
    out = 0
    def argval():
        nonlocal p
        v = 0
        while True:
            byte = b[p]; p += 1
            v = (v << 7) | (byte & 0x7F)
            if not (byte & 0x80):
                return v
    end = data['coff'] + data['packed']
    while p < end:
        op = b[p]; p += 1
        kind = op >> 5
        cnt = op & 0x1F
        if kind == 0:                      # zero
            n = cnt if cnt else argval()
            out += n
        elif kind == 1:                    # blockCopy
            n = cnt if cnt else argval()
            mem[out:out+n] = b[p:p+n]; p += n; out += n
        elif kind == 2:                    # repeatBlock
            n = cnt if cnt else argval()
            rpt = argval() + 1
            blk = b[p:p+n]; p += n
            for _ in range(rpt):
                mem[out:out+n] = blk; out += n
        elif kind == 3:                    # blockCopy + repeatedZero
            cs = cnt if cnt else argval()
            rc = argval(); rpt = argval() + 1
            common = b[p:p+cs]; p += cs
            for k in range(rpt):
                mem[out:out+cs] = common; out += cs
                out += rc
            mem[out:out+cs] = common; out += cs
        elif kind == 4:                    # blockCopy + repeatedBlock
            cs = cnt if cnt else argval()
            rc = argval(); rpt = argval() + 1
            common = b[p:p+cs]; p += cs
            for k in range(rpt):
                mem[out:out+cs] = common; out += cs
                mem[out:out+rc] = b[p:p+rc]; p += rc; out += rc
            mem[out:out+cs] = common; out += cs
        else:
            raise SystemExit(f'unknown pattern opcode {op:#x} at {p-1:#x}')
    print(f"pattern data: expanded {data['packed']} -> {out} bytes "
          f"(declared unpacked {data['unp']}, total {data['total']})")

    # ---- relocate -----------------------------------------------------------
    imports = [SHIM_BASE + 8*i for i in range(nsym)]
    sec_addr = {0: CODE_BASE, 1: DATA_BASE}
    relhdr = base + 56 + nlib*24 + nsym*4
    stats = collections.Counter()
    for k in range(nrel):
        sidx, _r, cnt, first = struct.unpack_from('>HHII', b, relhdr + k*12)
        start = base + relOff + first
        words = [struct.unpack_from('>H', b, start + 2*j)[0] for j in range(cnt)]
        sec_start = sec_addr[sidx]
        rel = sec_start
        imp = 0
        sC, sD = sec_addr[0], sec_addr[1]
        i = 0
        def bump(delta):
            nonlocal rel
            off = rel - sec_start
            cur = struct.unpack_from('>I', mem, off)[0]
            struct.pack_into('>I', mem, off, (cur + delta) & 0xFFFFFFFF)
            rel += 4
        while i < len(words):
            w = words[i]; i += 1
            op = w >> 9
            if op <= 0x1F:                       # BySectDWithSkip
                skip = (w >> 8) & 0x3F
                n = w & 0xFF
                rel += skip * 4
                for _ in range(n): bump(sD)
                stats['BySectDWithSkip'] += 1
            elif op == 0x20:                     # BySectC
                for _ in range((w & 0x1FF) + 1): bump(sC)
                stats['BySectC'] += 1
            elif op == 0x21:                     # BySectD
                for _ in range((w & 0x1FF) + 1): bump(sD)
                stats['BySectD'] += 1
            elif op == 0x22:                     # TVector12
                for _ in range((w & 0x1FF) + 1):
                    off = rel - sec_start
                    c = struct.unpack_from('>I', mem, off)[0]
                    d = struct.unpack_from('>I', mem, off+4)[0]
                    struct.pack_into('>I', mem, off,   (c + sC) & 0xFFFFFFFF)
                    struct.pack_into('>I', mem, off+4, (d + sD) & 0xFFFFFFFF)
                    rel += 12
                stats['TVector12'] += 1
            elif op == 0x23:                     # TVector8
                for _ in range((w & 0x1FF) + 1):
                    off = rel - sec_start
                    c = struct.unpack_from('>I', mem, off)[0]
                    d = struct.unpack_from('>I', mem, off+4)[0]
                    struct.pack_into('>I', mem, off,   (c + sC) & 0xFFFFFFFF)
                    struct.pack_into('>I', mem, off+4, (d + sD) & 0xFFFFFFFF)
                    rel += 8
                stats['TVector8'] += 1
            elif op == 0x24:                     # VTable8
                for _ in range((w & 0x1FF) + 1):
                    bump(sD); rel += 4
                stats['VTable8'] += 1
            elif op == 0x25:                     # ImportRun
                for _ in range((w & 0x1FF) + 1):
                    bump(imports[imp]); imp += 1
                stats['ImportRun'] += 1
            elif op == 0x30:                     # SmByImport
                idx = w & 0x1FF
                bump(imports[idx]); imp = idx + 1
                stats['SmByImport'] += 1
            elif op == 0x31:                     # SmSetSectC
                sC = sec_addr[w & 0x1FF]; stats['SmSetSectC'] += 1
            elif op == 0x32:                     # SmSetSectD
                sD = sec_addr[w & 0x1FF]; stats['SmSetSectD'] += 1
            elif op == 0x33:                     # SmBySection
                bump(sec_addr[w & 0x1FF]); stats['SmBySection'] += 1
            elif 0x40 <= op <= 0x47:             # IncrPosition
                rel += (w & 0xFFF) + 1
                stats['IncrPosition'] += 1
            elif 0x48 <= op <= 0x4F:             # SmRepeat
                blk = ((w >> 8) & 0xF) + 1
                rpt = (w & 0xFF) + 1
                # Re-run the preceding blk words rpt more times.
                seg = words[i-1-blk:i-1]
                words[i:i] = seg * rpt
                stats['SmRepeat'] += 1
            else:
                raise SystemExit(f'unhandled reloc opcode {op:#04x} '
                                 f'(word {w:04x}) at index {i-1}')
        print(f"section {sidx}: consumed {i} words (header said {cnt}); "
              f"relocAddress ended at {rel:#x} "
              f"(section spans {sec_start:#x}..{sec_start+data['total']:#x}); "
              f"importIndex={imp}/{nsym}")
    print("opcode usage:", dict(stats))

    # ---- validate -----------------------------------------------------------
    # Every word the relocator touched should now be a plausible address. Scan
    # the whole data section and classify every 4-byte word to spot damage.
    buckets = collections.Counter()
    for off in range(0, (len(mem) & ~3), 4):
        v = struct.unpack_from('>I', mem, off)[0]
        if v == 0: buckets['zero'] += 1
        elif CODE_BASE <= v < CODE_BASE + code['total']: buckets['->code'] += 1
        elif DATA_BASE <= v < DATA_BASE + data['total']: buckets['->data'] += 1
        elif SHIM_BASE <= v < SHIM_BASE + 8*nsym: buckets['->import'] += 1
        elif v < 0x10000: buckets['small'] += 1
        else: buckets['other'] += 1
    print("data-section word classification:", dict(buckets.most_common()))

    # The entry point must be a transition vector: code in code, TOC in data.
    ep = sec_addr[mainSec] + mainOff if mainSec >= 0 else 0
    off = ep - DATA_BASE
    ec, et = struct.unpack_from('>II', mem, off)
    print(f"entry tvector @ {ep:#x}: code={ec:#x} toc={et:#x}  "
          f"code_ok={CODE_BASE <= ec < CODE_BASE+code['total']}  "
          f"toc_ok={DATA_BASE <= et < DATA_BASE+data['total']}")
    open('build/data_section_relocated.bin','wb').write(mem)

main()
