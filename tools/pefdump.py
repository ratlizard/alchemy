#!/usr/bin/env python3
# COPY. The canonical file is tools/pefdump.py in ratlizard/cythera-workbench;
# fix it there and re-copy. This repository is retired, so this copy exists
# only so port/ can run standalone -- do not edit it to diverge.
# Verify with tools/check_copies.sh. Source sha256 5a32fff98c8b93293f70c7c63952da53eda4047e2943d9ab369a4062eb73400f.
"""Dump a PEF (Preferred Executable Format) container's structure.

    python3 tools/pefdump.py build/extract/Cythera.data [--symbols]

Prints the container/section headers and the loader section's import
libraries and symbols -- i.e. the exact host API surface the binary needs.
"""
import struct, sys, collections

KIND = {0:'code',1:'unpackedData',2:'patternData',3:'constant',
        4:'loader',5:'debug',6:'execData',7:'exception',8:'traceback'}
SYMCLASS = {0:'code',1:'data',2:'tvect',3:'toc',4:'glue'}

def u32(b,o): return struct.unpack_from('>I',b,o)[0]
def u16(b,o): return struct.unpack_from('>H',b,o)[0]

class Pef:
    def __init__(self, blob):
        self.b = blob
        tag1, tag2, arch, ver, self.stamp, odv, oiv, self.cur = \
            struct.unpack_from('>4s4s4sIIIII', blob, 0)
        assert tag1 == b'Joy!' and tag2 == b'peff', 'not a PEF container'
        self.arch = arch.decode()
        self.nsec, self.ninst = struct.unpack_from('>HH', blob, 32)
        self.sections = []
        for i in range(self.nsec):
            o = 40 + i*28
            name_off, addr, total, unpacked, packed, coff = \
                struct.unpack_from('>iIIIII', blob, o)
            kind, share, align, _ = struct.unpack_from('>BBBB', blob, o+24)
            self.sections.append(dict(
                idx=i, name_off=name_off, addr=addr, total=total,
                unpacked=unpacked, packed=packed, coff=coff,
                kind=kind, kindname=KIND.get(kind, str(kind)),
                share=share, align=align))

    def loader(self):
        return next((s for s in self.sections if s['kind'] == 4), None)

def parse_loader(b, base):
    h = {}
    (h['mainSection'], h['mainOffset'], h['initSection'], h['initOffset'],
     h['termSection'], h['termOffset'], h['importedLibraryCount'],
     h['totalImportedSymbolCount'], h['relocSectionCount'],
     h['relocInstrOffset'], h['loaderStringsOffset'],
     h['exportHashOffset'], h['exportHashTablePower'],
     h['exportedSymbolCount']) = struct.unpack_from('>iIiIiIIIIIIIII', b, base)
    return h

def cstr(b, o):
    e = b.index(b'\0', o)
    return b[o:e].decode('mac-roman', 'replace')

def main():
    path = sys.argv[1]
    want_syms = '--symbols' in sys.argv
    blob = open(path, 'rb').read()
    p = Pef(blob)
    print(f"PEF  arch={p.arch}  sections={p.nsec} (instantiated {p.ninst})  "
          f"version=0x{p.cur:08x}")
    print(f"{'#':>2} {'kind':<14}{'addr':>10}{'total':>10}{'unpacked':>10}"
          f"{'packed':>10}{'fileoff':>10}")
    for s in p.sections:
        print(f"{s['idx']:>2} {s['kindname']:<14}{s['addr']:>10}{s['total']:>10}"
              f"{s['unpacked']:>10}{s['packed']:>10}{s['coff']:>10}")

    ld = p.loader()
    if not ld:
        return 0
    base = ld['coff']
    h = parse_loader(blob, base)
    strs = base + h['loaderStringsOffset']
    print(f"\nloader: entry=sec{h['mainSection']}+0x{h['mainOffset']:x}  "
          f"init=sec{h['initSection']}+0x{h['initOffset']:x}  "
          f"term=sec{h['termSection']}+0x{h['termOffset']:x}")
    print(f"imports: {h['importedLibraryCount']} libraries, "
          f"{h['totalImportedSymbolCount']} symbols; "
          f"exports: {h['exportedSymbolCount']}; "
          f"reloc sections: {h['relocSectionCount']}")

    # Imported library table follows the 56-byte loader header.
    libs = []
    lo = base + 56
    for i in range(h['importedLibraryCount']):
        o = lo + i*24
        name_off, old_imp, cur_ver, count, first, opt, _r = \
            struct.unpack_from('>IIIIIBB', blob, o)
        libs.append(dict(name=cstr(blob, strs + name_off), count=count,
                         first=first, ver=cur_ver, opt=opt))

    # Imported symbol table: one u32 per symbol, class in the high byte.
    sym_base = lo + h['importedLibraryCount']*24
    syms = []
    for i in range(h['totalImportedSymbolCount']):
        v = u32(blob, sym_base + i*4)
        syms.append((SYMCLASS.get((v >> 24) & 0x0f, str(v >> 24)),
                     cstr(blob, strs + (v & 0x00ffffff))))

    print(f"\n{'library':<28}{'symbols':>8}  version")
    for L in libs:
        print(f"{L['name']:<28}{L['count']:>8}  0x{L['ver']:08x}"
              + ("  (weak)" if L['opt'] & 0x40 else ""))

    if want_syms:
        for L in libs:
            print(f"\n===== {L['name']}  ({L['count']} symbols) =====")
            chunk = syms[L['first']:L['first']+L['count']]
            for cls, nm in sorted(chunk, key=lambda t: t[1]):
                print(f"  {cls:<6}{nm}")
    else:
        by = collections.Counter(c for c, _ in syms)
        print("\nsymbol classes:", dict(by))
    return 0

if __name__ == '__main__':
    sys.exit(main())
