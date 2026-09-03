#!/usr/bin/env python3
# COPY. The canonical file is tools/screen_to_png.py in ratlizard/cythera-workbench;
# fix it there and re-copy. This repository is retired, so this copy exists
# only so port/ can run standalone -- do not edit it to diverge.
# Verify with tools/check_copies.sh. Source sha256 bfda90c40bba7882b64381c20e3cfdb05b5e9cfd266796d3a030e27fd882ceb7.
"""Turn a framebuffer dump from `cythera --dump-screen` into a PNG.

    python3 tools/screen_to_png.py screen.bin screen.png

The dump is: width, height and rowBytes as big-endian words, then the indexed
pixels, then a 256-entry RGB palette.
"""
import struct
import sys

from PIL import Image


def main():
    raw = open(sys.argv[1], 'rb').read()
    w, h, row = struct.unpack_from('>HHH', raw, 0)
    pixels = raw[6:6 + row * h]
    palette = raw[6 + row * h:6 + row * h + 768]
    img = Image.new('P', (w, h))
    # Rows are padded to rowBytes, so each one is trimmed to the real width.
    img.putdata(b''.join(pixels[y * row:y * row + w] for y in range(h)))
    img.putpalette(palette)
    img.convert('RGB').save(sys.argv[2])
    print(f"{sys.argv[2]}: {w}x{h}")


main()
