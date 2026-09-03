#include "mac/pict.h"

#include <cstring>
#include <sstream>

namespace cyt {
namespace {

// Fixed data length per PICT opcode. Negative values need special handling:
//   -1  a two-byte length precedes the data
//   -2  a Point plus a Pascal string (the text operations)
//   -3  the same, with a four-byte position
//   -4  a one-byte offset plus a Pascal string
//   -5  two one-byte offsets plus a Pascal string
//  -10  a BitsRect or BitsRgn: parsed rather than skipped
//  -11  a DirectBitsRect or DirectBitsRgn
constexpr s16 kUnknownOp = -99;

s16 opSize(u16 op) {
  static const s16 table[] = {
    /*0x00*/  0, -1,  8,  2,  1,  2,  4,  4,  2,  8,  8,  4,  4,  2,  4,  4,
    /*0x10*/  8,  1, -1, -1, -1,  2,  2,  0,  0,  0,  6,  6,  0,  6,  0,  6,
    /*0x20*/  8,  4,  6,  2, -2, -2, -2, -2, -3, -4, -5, -4, -2, -2, -2, -2,
    /*0x30*/  8,  8,  8,  8,  8,  8,  8,  8,  0,  0,  0,  0,  0,  0,  0,  0,
    /*0x40*/  8,  8,  8,  8,  8,  8,  8,  8,  0,  0,  0,  0,  0,  0,  0,  0,
    /*0x50*/  8,  8,  8,  8,  8,  8,  8,  8,  0,  0,  0,  0,  0,  0,  0,  0,
    /*0x60*/ 12, 12, 12, 12, 12, 12, 12, 12,  4,  0,  0,  0,  4,  0,  0,  0,
    /*0x70*/ -1, -1, -1, -1, -1, -1, -1, -1,  0,  0,  0,  0,  0,  0,  0,  0,
    /*0x80*/ -1, -1, -1, -1, -1, -1, -1, -1,  0,  0,  0,  0,  0,  0,  0,  0,
    /*0x90*/-10,-10, -1, -1, -1, -1, -1, -1,-10,-10,-11,-11, kUnknownOp,
            kUnknownOp, kUnknownOp, kUnknownOp,
  };
  if (op < sizeof(table) / sizeof(table[0])) return table[op];
  if (op == 0x00A0) return 2;
  if (op == 0x00A1) return -1;    // long comment
  if (op == 0x00FF) return 0;     // end of picture
  if (op == 0x0C00) return 24;    // header comment
  // Reserved ranges have documented shapes; the ones Cythera never emits are
  // treated as unknown so a surprise is reported rather than silently skipped.
  return kUnknownOp;
}

struct Reader {
  Mem& m;
  GuestAddr base;
  u32 len;
  u32 p = 0;
  bool bad = false;

  bool have(u32 n) const { return u64(p) + n <= len; }
  u8 u8v() { if (!have(1)) { bad = true; return 0; } return m.r8(base + p++); }
  u16 u16v() {
    if (!have(2)) { bad = true; return 0; }
    u16 v = m.r16(base + p);
    p += 2;
    return v;
  }
  s16 s16v() { return s16(u16v()); }
  u32 u32v() {
    if (!have(4)) { bad = true; return 0; }
    u32 v = m.r32(base + p);
    p += 4;
    return v;
  }
  void skip(u32 n) { if (!have(n)) { bad = true; p = len; } else p += n; }
  Rect rectv() {
    Rect r;
    r.top = s16v(); r.left = s16v(); r.bottom = s16v(); r.right = s16v();
    return r;
  }
};

// PackBits: a control byte either repeats the next byte or copies a literal run.
bool unpackBits(Reader& rd, u32 packedLen, u32 expected, std::vector<u8>* out) {
  out->clear();
  out->reserve(expected);
  const u32 end = rd.p + packedLen;
  while (out->size() < expected && rd.p < end && !rd.bad) {
    u8 flag = rd.u8v();
    if (flag & 0x80) {
      u32 n = 257u - flag;
      u8 v = rd.u8v();
      out->insert(out->end(), n, v);
    } else {
      u32 n = u32(flag) + 1;
      for (u32 i = 0; i < n; ++i) out->push_back(rd.u8v());
    }
  }
  out->resize(expected, 0);
  rd.p = end < rd.len ? end : rd.len;
  return !rd.bad;
}

}  // namespace

DecodedPict decodePict(Mem& m, GuestAddr data, u32 length) {
  DecodedPict out;
  Reader rd{m, data, length};

  // Picture header: a two-byte size then the picture frame. Version 2 pictures
  // then carry a version opcode, which the opcode loop handles.
  rd.skip(2);
  rd.rectv();

  while (rd.p < rd.len && !rd.bad) {
    if (rd.p & 1) rd.skip(1);          // opcodes are word-aligned
    u16 op = rd.u16v();
    if (op == 0x00FF) break;

    const bool bitsOp = (op == 0x0090 || op == 0x0091 || op == 0x0098 ||
                         op == 0x0099 || op == 0x009A || op == 0x009B);
    if (!bitsOp) {
      s16 sz = opSize(op);
      if (sz == kUnknownOp) {
        std::ostringstream s;
        s << "unsupported PICT opcode 0x" << std::hex << op;
        out.error = s.str();
        return out;
      }
      if (sz >= 0) {
        rd.skip(u32(sz));
      } else if (sz == -1) {
        if (op == 0x0001) {
          // Clipping region: a length that includes its own two bytes.
          u16 n = rd.u16v();
          rd.skip(n >= 2 ? u32(n - 2) : 0);
        } else if (op == 0x00A1) {
          rd.skip(2);
          u16 n = rd.u16v();
          rd.skip(n);
        } else {
          u16 n = rd.u16v();
          rd.skip(n);
        }
      } else if (sz == -2 || sz == -3 || sz == -4 || sz == -5) {
        rd.skip(sz == -3 ? 4u : (sz == -5 ? 2u : 1u));
        u8 n = rd.u8v();
        rd.skip(n);
      }
      continue;
    }

    // ---- the CopyBits family ----------------------------------------------
    const bool direct = (op == 0x009A || op == 0x009B);
    const bool masked = (op == 0x0091 || op == 0x0099 || op == 0x009B);
    if (direct) rd.skip(4);            // pmBaseAddr placeholder

    const u16 rowBytesField = rd.u16v();
    const bool isPixMap = (rowBytesField & 0x8000) != 0 || direct;
    const u32 rowBytes = rowBytesField & 0x3FFF;
    const Rect bounds = rd.rectv();

    u16 packType = 0, pixelSize = 1, cmpCount = 1;
    Palette palette;
    if (isPixMap) {
      rd.skip(2);                      // pmVersion
      packType = rd.u16v();
      rd.skip(4);                      // packSize
      rd.skip(8);                      // hRes, vRes
      rd.skip(2);                      // pixelType
      pixelSize = rd.u16v();
      cmpCount = rd.u16v();
      rd.skip(2);                      // cmpSize
      rd.skip(4);                      // planeBytes
      rd.skip(4);                      // pmTable
      rd.skip(4);                      // pmReserved
      if (!direct) {
        rd.skip(4);                    // ctSeed
        const u16 flags = rd.u16v();
        const u32 count = u32(rd.u16v()) + 1;
        palette.entries.assign(256, Rgb{});
        for (u32 i = 0; i < count && i <= 256 && !rd.bad; ++i) {
          u32 idx = rd.u16v();
          Rgb c{rd.u16v(), rd.u16v(), rd.u16v()};
          if (flags & 0x8000) idx = i;
          palette.entries[idx & 0xFF] = c;
        }
      }
    }
    rd.skip(8 + 8 + 2);                // srcRect, dstRect, mode
    if (masked) {
      // A region follows, whose first word is its total length.
      u16 n = rd.u16v();
      rd.skip(n >= 2 ? u32(n - 2) : 0);
    }

    const s16 w = bounds.width(), h = bounds.height();
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
      out.error = "PICT image bounds are implausible";
      return out;
    }

    // Rows are PackBits-compressed unless rowBytes is small or the pack type
    // says otherwise; the length prefix is a byte for narrow rows, else a word.
    const bool packed = rowBytes >= 8 && packType != 1 && packType != 2;
    // Only one of the two buffers is used, decided by whether the picture is
    // direct colour or indexed.
    out.direct = direct;
    if (direct) out.rgb.assign(size_t(w) * size_t(h), Rgb{});
    else out.pixels.assign(size_t(w) * size_t(h), 0);

    for (s16 y = 0; y < h && !rd.bad; ++y) {
      std::vector<u8> row;
      if (!packed) {
        row.resize(rowBytes);
        for (u32 i = 0; i < rowBytes; ++i) row[i] = rd.u8v();
      } else {
        u32 n = (rowBytes > 250) ? u32(rd.u16v()) : u32(rd.u8v());
        if (direct && packType == 3) {
          // Sixteen-bit direct pixels repeat two bytes at a time.
          const u32 end = rd.p + n;
          row.reserve(size_t(w) * 2);
          while (row.size() < size_t(w) * 2 && rd.p < end && !rd.bad) {
            u8 flag = rd.u8v();
            if (flag & 0x80) {
              u32 rep = 257u - flag;
              u8 a = rd.u8v(), b = rd.u8v();
              for (u32 k = 0; k < rep; ++k) { row.push_back(a); row.push_back(b); }
            } else {
              u32 cnt = u32(flag) + 1;
              for (u32 k = 0; k < cnt * 2; ++k) row.push_back(rd.u8v());
            }
          }
          rd.p = end < rd.len ? end : rd.len;
        } else {
          const u32 expect = direct ? u32(w) * cmpCount : rowBytes;
          unpackBits(rd, n, expect, &row);
        }
      }

      u8* dst = out.pixels.data() + size_t(y) * size_t(w);
      // An eight-bit component becomes a sixteen-bit one by repetition, so that
      // 0xFF is 0xFFFF exactly rather than 0xFF00.
      auto wide = [](u32 v) { return u16((v & 0xFF) * 0x0101); };
      if (direct && packType == 4) {
        // Planar RGB: the row holds every red, then every green, then every
        // blue, with an alpha plane first when there are four components.
        Rgb* drgb = out.rgb.data() + size_t(y) * size_t(w);
        const u32 off = (cmpCount == 3) ? 0 : u32(w);
        for (s16 x = 0; x < w; ++x) {
          u32 r = off + u32(x) < row.size() ? row[off + u32(x)] : 0;
          u32 g = off + u32(w) + u32(x) < row.size() ? row[off + u32(w) + u32(x)] : 0;
          u32 bb = off + 2 * u32(w) + u32(x) < row.size()
                       ? row[off + 2 * u32(w) + u32(x)] : 0;
          drgb[x] = Rgb{wide(r), wide(g), wide(bb)};
        }
      } else if (direct && packType == 3) {
        // Chunky 16-bit, five bits per component.
        Rgb* drgb = out.rgb.data() + size_t(y) * size_t(w);
        for (s16 x = 0; x < w; ++x) {
          u32 i = u32(x) * 2;
          u16 v = u16(i + 1 < row.size() ? (u16(row[i]) << 8 | row[i + 1]) : 0);
          u32 r = ((v >> 10) & 31) * 255 / 31;
          u32 g = ((v >> 5) & 31) * 255 / 31;
          u32 bb = (v & 31) * 255 / 31;
          drgb[x] = Rgb{wide(r), wide(g), wide(bb)};
        }
      } else if (pixelSize == 8) {
        for (s16 x = 0; x < w; ++x)
          dst[x] = u32(x) < row.size() ? row[u32(x)] : 0;
      } else if (pixelSize == 1 || pixelSize == 2 || pixelSize == 4) {
        const u32 perByte = 8u / pixelSize;
        const u32 mask = (1u << pixelSize) - 1;
        s16 x = 0;
        for (u32 i = 0; i < row.size() && x < w; ++i) {
          for (u32 k = 0; k < perByte && x < w; ++k, ++x) {
            const u32 shift = 8 - pixelSize * (k + 1);
            u32 v = (row[i] >> shift) & mask;
            // One-bit pictures are black-on-white: a set bit means black.
            dst[x] = (pixelSize == 1) ? u8(v ? 255 : 0) : u8(v);
          }
        }
      } else {
        std::ostringstream s;
        s << "unsupported PICT pixel size " << pixelSize
          << " (pack type " << packType << ")";
        out.error = s.str();
        return out;
      }
    }

    if (pixelSize == 1 && palette.empty()) {
      // Give a monochrome picture an explicit two-entry meaning.
      palette.entries.assign(256, Rgb{});
      palette.entries[0] = Rgb{0xFFFF, 0xFFFF, 0xFFFF};
      palette.entries[255] = Rgb{0, 0, 0};
    }
    out.width = w;
    out.height = h;
    out.palette = std::move(palette);
    out.ok = !rd.bad;
    if (!out.ok) out.error = "PICT data ended early";
    return out;
  }

  out.error = out.error.empty() ? "PICT contained no image" : out.error;
  return out;
}

}  // namespace cyt
