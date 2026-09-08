/* mac-containers.js — getting a classic Mac file's two forks out of whatever
 * it was wrapped in to survive the trip.
 *
 * Needs mac-bytes.js. Load it first.
 *
 * A Mac file has a data fork and a resource fork. Everything that carried one
 * across a network or onto a non-Mac disk had to flatten that into a single
 * stream, and this decodes the three wrappers that actually turn up:
 *
 *   BinHex 4.0 (.hqx)      7-bit text, from FTP and Usenet
 *   MacBinary (.bin)       binary, from BBSs and FTP
 *   AppleSingle/Double     binary, from A/UX, NFS and Mac OS X
 *
 * Both pages had their own BinHex and MacBinary decoders, and each had solved
 * a problem the other had not: one found the payload correctly in a file with
 * comment lines, the other handled a file with no resource fork. This is the
 * union of the two, plus AppleSingle, which neither had.
 *
 * Every decoder returns the same shape, or null:
 *
 *   { kind, name, type, creator, data, rsrc }
 *
 * `data` and `rsrc` are Uint8Arrays; `rsrc` may be empty (a file with no
 * resource fork is normal, an application with no data fork is normal too).
 */

/* ---- BinHex 4.0 -------------------------------------------------------- */
/* 3 bytes packed into 4 characters of a 64-character alphabet, then a 0x90
 * run-length pass. The alphabet skips characters that were unreliable on the
 * mail and news systems this had to cross, which is why it is not simply
 * base64 and why it looks like a cat walked over the keyboard. */
const HQX_ALPHA = '!"#$%&\'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr';
const HQX_REV = (() => {
  const t = new Uint8Array(256).fill(255);
  for (let i = 0; i < HQX_ALPHA.length; i++) t[HQX_ALPHA.charCodeAt(i)] = i;
  return t;
})();

/* "The file contains two colons" matched plenty of binary resource forks, and
 * this test now runs before the raw-fork one, so a false positive stops a real
 * fork from opening at all. Require either the banner, or a file that is
 * plausible 7-bit text AND has a run of the alphabet after its first colon. */
function looksLikeBinHex(bytes) {
  const head = latin1(bytes.subarray(0, Math.min(bytes.length, 4096)));
  if (head.includes('must be converted with BinHex') || /BinHex\s*4\.0/i.test(head)) return true;
  for (let i = 0; i < Math.min(bytes.length, 4096); i++) {
    const b = bytes[i];
    if (b > 0x7E || (b < 0x20 && b !== 9 && b !== 10 && b !== 13)) return false;
  }
  const c = head.indexOf(':');
  if (c < 0) return false;
  const probe = head.slice(c + 1, c + 65).replace(/[\r\n]/g, '');
  return probe.length >= 32 && /^[!-r]+$/.test(probe) && !/[.:;<=>?\\^_{|}~]/.test(probe);
}

function binhexDecode(bytes) {
  /* The payload opens at the first ':' AFTER the banner, not the first ':' in
   * the file. Copies mirrored from old FTP and BBS archives routinely carry
   * comment lines above the banner -- a URL, a "Date:" header, an uploader's
   * signature -- and starting at the first colon truncates the payload to
   * whatever followed one of those. */
  const head = latin1(bytes.subarray(0, Math.min(bytes.length, 8192)));
  const banner = head.search(/BinHex\s*4\.0/i);
  let start = -1;
  for (let i = banner > 0 ? banner : 0; i < bytes.length; i++) if (bytes[i] === 0x3A) { start = i; break; }
  if (start < 0) throw new Error('no BinHex ":" start marker');
  let end = bytes.length;
  for (let i = start + 1; i < bytes.length; i++) if (bytes[i] === 0x3A) { end = i; break; }

  /* 6-bit values; anything outside the alphabet (newlines, padding, the stray
   * spaces some mailers insert) simply disappears. */
  const vals = new Uint8Array(end - start - 1);
  let n = 0;
  for (let i = start + 1; i < end; i++) {
    const v = HQX_REV[bytes[i]];
    if (v !== 255) vals[n++] = v;
  }
  const n4 = n - (n % 4);
  const packed = new Uint8Array((n4 >> 2) * 3 + 3);
  let p = 0;
  for (let i = 0; i < n4; i += 4) {
    const a = vals[i], b = vals[i + 1], c = vals[i + 2], d = vals[i + 3];
    packed[p++] = ((a << 2) | (b >> 4)) & 0xFF;
    packed[p++] = ((b << 4) | (c >> 2)) & 0xFF;
    packed[p++] = ((c << 6) | d) & 0xFF;
  }
  let bits = 0, nbits = 0;                       // a partial final group
  for (let i = n4; i < n; i++) {
    bits = (bits << 6) | vals[i]; nbits += 6;
    if (nbits >= 8) { nbits -= 8; packed[p++] = (bits >> nbits) & 0xFF; }
  }

  /* Run-length: `b 0x90 n` is b repeated n times, and `0x90 0x00` is a literal
   * 0x90. A plain JS array here meant about seven million boxed Numbers for
   * Cythera Data, which is enough to kill a tab on a phone. */
  let cap = p + (p >> 1) + 64, out = new Uint8Array(cap), o = 0;
  const ensure = need => {
    if (o + need <= cap) return;
    while (o + need > cap) cap *= 2;
    const grown = new Uint8Array(cap); grown.set(out.subarray(0, o)); out = grown;
  };
  for (let i = 0; i < p;) {
    const b = packed[i];
    if (b !== 0x90) { ensure(1); out[o++] = b; i++; continue; }
    if (i + 1 >= p) { ensure(1); out[o++] = 0x90; break; }
    const cnt = packed[i + 1];
    if (cnt === 0) { ensure(1); out[o++] = 0x90; }
    else if (o > 0) { const last = out[o - 1]; ensure(cnt - 1); for (let k = 1; k < cnt; k++) out[o++] = last; }
    i += 2;
  }
  return out.subarray(0, o);
}

function binhexSplitForks(d) {
  let p = 0;
  const nameLen = d[p++];
  if (nameLen < 1 || nameLen > 63 || p + nameLen + 17 > d.length)
    throw new Error('BinHex header is malformed');
  const name = decodeMacRoman(d.subarray(p, p + nameLen)); p += nameLen;
  p += 1;                                        // version
  const type = fourcc(d, p); p += 4;
  const creator = fourcc(d, p); p += 4;
  p += 2;                                        // finder flags
  const dataLen = u32be(d, p); p += 4;
  const rsrcLen = u32be(d, p); p += 4;
  p += 2;                                        // header CRC
  if (p + dataLen + 2 + rsrcLen + 2 > d.length)
    throw new Error('BinHex fork lengths exceed the decoded payload');
  const data = d.subarray(p, p + dataLen); p += dataLen + 2;   // data CRC
  return { kind: 'BinHex 4.0', name, type, creator, data, rsrc: d.subarray(p, p + rsrcLen) };
}

/* ---- MacBinary I/II ---------------------------------------------------- */
/* Bytes 74 and 82 are reserved and must be zero. Checking them is what stops
 * an ordinary binary file that happens to start with a zero byte from being
 * mistaken for a container and "extracted" into nonsense. */
function macBinaryForks(b) {
  if (b.length < 128 || b[0] !== 0 || b[74] !== 0 || b[82] !== 0) return null;
  const nameLen = b[1];
  if (nameLen < 1 || nameLen > 63) return null;
  const dataLen = u32be(b, 83), rsrcLen = u32be(b, 87);
  if (128 + dataLen > b.length) return null;
  const rsrcStart = 128 + Math.ceil(dataLen / 128) * 128;   // forks are 128-byte aligned
  const hasRsrc = rsrcLen > 0 && rsrcStart + rsrcLen <= b.length;
  if (!hasRsrc && dataLen === 0) return null;               // nothing in it either way
  return {
    kind: 'MacBinary',
    name: decodeMacRoman(b.subarray(2, 2 + nameLen)),
    type: fourcc(b, 65), creator: fourcc(b, 69),
    data: b.subarray(128, 128 + dataLen),
    rsrc: hasRsrc ? b.subarray(rsrcStart, rsrcStart + rsrcLen) : new Uint8Array(0)
  };
}

/* ---- AppleSingle / AppleDouble ----------------------------------------- */
/* A magic number and a table of numbered entries. AppleDouble holds only the
 * parts that a foreign filesystem could not store, so its "data fork" entry is
 * usually absent -- the data lives in the plain file sitting next to it. */
function appleSingleForks(b) {
  if (b.length < 26) return null;
  const magic = u32be(b, 0);
  if (magic !== 0x00051600 && magic !== 0x00051607) return null;
  const out = {
    kind: magic === 0x00051600 ? 'AppleSingle' : 'AppleDouble',
    name: '', type: '', creator: '', data: new Uint8Array(0), rsrc: new Uint8Array(0)
  };
  const n = u16be(b, 24);
  for (let i = 0; i < n; i++) {
    const p = 26 + i * 12;
    if (p + 12 > b.length) break;
    const id = u32be(b, p), off = u32be(b, p + 4), len = u32be(b, p + 8);
    if (off + len > b.length) continue;
    const seg = b.subarray(off, off + len);
    if (id === 1) out.data = seg;
    else if (id === 2) out.rsrc = seg;
    else if (id === 3) out.name = decodeMacRoman(seg);
    else if (id === 9 && len >= 8) { out.type = fourcc(seg, 0); out.creator = fourcc(seg, 4); }
  }
  return out;
}

/* One entry point: what is this file, and what is inside it?
 *
 * ORDER MATTERS. BinHex is tested first because its 7-bit text can, by
 * coincidence, satisfy the looser MacBinary shape test. AppleSingle comes
 * before MacBinary for the same reason in reverse: AppleSingle has a real
 * magic number, MacBinary is recognised only by a few zeroed reserved bytes,
 * and an AppleSingle header happens to satisfy those.
 *
 * Returns null when the bytes are not wrapped in anything -- which is the
 * normal case for a bare resource fork or a bare Delver archive. */
function sniffMacContainer(bytes) {
  if (!bytes || !bytes.length) return null;
  if (looksLikeBinHex(bytes)) return binhexSplitForks(binhexDecode(bytes));
  return appleSingleForks(bytes) || macBinaryForks(bytes);
}

/* ---- Writing MacBinary ------------------------------------------------- */
/* The one container this file can produce as well as open. MacBinary II is
 * the format that carries BOTH forks in one flat file and that emulators
 * accept by drag-and-drop, which is exactly what an edited archive needs --
 * a bare .data download loses the resource fork, and BinHex would mean
 * writing the RLE/6-bit encoder for no added carrying capacity.
 *
 * The layout is the mirror of macBinaryForks above: version-0 byte, Pascal
 * name, type/creator, fork lengths at 83/87, forks 128-byte aligned after
 * the header. The MacBinary II extras are the version pair at 122/123 (129 =
 * "II", both as written-by and minimum-to-read) and the CRC-16/XMODEM of
 * bytes 0..123 at 124 -- the CRC is what lets a strict reader tell a real
 * MacBinary II file from 128 bytes of coincidence, so it is computed, not
 * zeroed. Dates at 91/95 are seconds since the Mac epoch, 1904-01-01. */
function crc16xmodem(bytes, start, end) {
  let crc = 0;
  for (let i = start; i < end; i++) {
    crc ^= bytes[i] << 8;
    for (let b = 0; b < 8; b++) crc = ((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1) & 0xFFFF;
  }
  return crc;
}

function writeMacBinary(f) {
  const name = encodeMacRoman((f.name || 'Untitled').slice(0, 31));
  const type = encodeMacRoman(((f.type || '????') + '    ').slice(0, 4));
  const creator = encodeMacRoman(((f.creator || '????') + '    ').slice(0, 4));
  const data = f.data || new Uint8Array(0);
  const rsrc = f.rsrc || new Uint8Array(0);
  const dataPad = Math.ceil(data.length / 128) * 128;
  const rsrcPad = Math.ceil(rsrc.length / 128) * 128;
  const out = new Uint8Array(128 + dataPad + rsrcPad);
  const w32 = (v, at) => { out[at] = (v >>> 24) & 0xFF; out[at+1] = (v >>> 16) & 0xFF; out[at+2] = (v >>> 8) & 0xFF; out[at+3] = v & 0xFF; };
  out[1] = name.length;
  out.set(name, 2);
  out.set(type, 65);
  out.set(creator, 69);
  w32(data.length, 83);
  w32(rsrc.length, 87);
  const macNow = Math.floor(Date.now() / 1000) + 2082844800;   // 1904 epoch
  w32(macNow, 91);
  w32(macNow, 95);
  out[122] = 129; out[123] = 129;                              // MacBinary II
  const crc = crc16xmodem(out, 0, 124);
  out[124] = crc >> 8; out[125] = crc & 0xFF;
  out.set(data, 128);
  out.set(rsrc, 128 + dataPad);
  return out;
}
