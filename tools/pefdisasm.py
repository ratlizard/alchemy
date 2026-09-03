#!/usr/bin/env python3
# COPY. The canonical file is tools/pefdisasm.py in ratlizard/cythera-workbench;
# fix it there and re-copy. This repository is retired, so this copy exists
# only so port/ can run standalone -- do not edit it to diverge.
# Verify with tools/check_copies.sh. Source sha256 2c7457376c3c9d6c1d6a64b90e3f6f865299cd6575547d22b334c6f353afad5a.
"""Disassemble a range of the PEF code section.

    python3 tools/pefdisasm.py <Cythera.data> <start> [end|+length]

Addresses are offsets within the code section -- the same ones
cythera_symbols.txt lists and the interpreter's CYT_WATCH_PC takes. Symbols
are printed as labels where they land, and branch displacements are resolved
to absolute code-section addresses so reading control flow needs no mental
arithmetic.
"""
import os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pefdump import Pef

def _llvm_mc():
    """Find llvm-mc. Homebrew keeps it keg-only under llvm@NN/bin, off PATH."""
    import glob
    import shutil
    found = shutil.which('llvm-mc')
    if found:
        return found
    for pat in ('/opt/homebrew/opt/llvm*/bin/llvm-mc',
                '/usr/local/opt/llvm*/bin/llvm-mc'):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]
    return 'llvm-mc'          # let the failure name it


LLVM_MC = _llvm_mc()


def code_section(path):
    p = Pef(open(path, 'rb').read())
    for s in p.sections:
        if s['kindname'] == 'code':
            return p.b[s['coff']:s['coff'] + s['unpacked']]
    raise SystemExit("no code section in " + path)


HERE = os.path.dirname(os.path.abspath(__file__))


def symbols():
    # reference/ is where this repository keeps it; the repository root is
    # honoured too, for a copy dropped in beside the port's scripts.
    for cand in (os.path.join(HERE, '..', 'reference', 'cythera_symbols.txt'),
                 os.path.join(HERE, '..', 'cythera_symbols.txt')):
        if os.path.exists(cand):
            path = cand
            break
    else:
        path = os.path.join(HERE, '..', 'cythera_symbols.txt')
    out = {}
    if os.path.exists(path):
        for line in open(path):
            m = re.match(r'\s*0x([0-9A-Fa-f]+)\s+\d+\s+\d+\s+\S+\s+(.*\S)', line)
            if m:
                out[int(m.group(1), 16)] = m.group(2)
    # Calls to imported routines go through cross-TOC glue, so a branch lands on
    # an unnamed stub rather than on "GetNewDialog". port/build/glue.txt pairs
    # every stub with its import; regenerate it with
    #   port/build/cythera <data fork> --dump-glue port/build/glue.txt
    glue = os.path.join(HERE, '..', 'port', 'build', 'glue.txt')
    if os.path.exists(glue):
        for line in open(glue):
            addr, _, name = line.strip().partition(' ')
            if name:
                out.setdefault(int(addr, 16), name + ' [import]')
    else:
        sys.stderr.write(
            "note: no port/build/glue.txt, so calls to imported routines will\n"
            "      show as bare addresses. Regenerate it with\n"
            "        port/build/cythera <data fork> --dump-glue "
            "port/build/glue.txt\n")
    return out


# llvm-mc prints branch displacements, not targets, and in two shapes: a bare
# "b .+136" and a conditional "bt 2, .+152".
DISP = re.compile(r'^(.*?)\.([+-]\d+)\s*$')


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    data, start = sys.argv[1], int(sys.argv[2], 0)
    if len(sys.argv) > 3:
        arg = sys.argv[3]
        end = start + int(arg[1:], 0) if arg.startswith('+') else int(arg, 0)
    else:
        end = start + 0x80
    code = code_section(data)
    syms = symbols()
    words = code[start:end]
    hexin = ' '.join(f'0x{b:02x}' for b in words)
    got = subprocess.run([LLVM_MC, '--disassemble', '--triple=powerpc'],
                         input=hexin, capture_output=True, text=True)
    lines = [l.strip() for l in got.stdout.splitlines() if l.strip()]
    for i, insn in enumerate(lines):
        addr = start + i * 4
        if addr in syms:
            print(f"\n{addr:08x} <{syms[addr]}>:")
        m = DISP.match(insn)
        if m:
            target = addr + int(m.group(2))
            label = syms.get(target)
            insn = (m.group(1) + f"0x{target:x}" +
                    (f" <{label}>" if label else ""))
        word = int.from_bytes(words[i * 4:i * 4 + 4], 'big')
        print(f"{addr:08x}  {word:08x}  {insn}")
    if got.stderr.strip():
        sys.stderr.write(got.stderr)


if __name__ == '__main__':
    main()
