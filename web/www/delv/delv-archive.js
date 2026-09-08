/* delv-archive.js -- the Cythera archive itself: its master index, its
 * resources, the encryption laid over them, and the record formats stored
 * inside.
 *
 * This file, delv-graphics.js and delv-script.js were the Delver-specific half
 * of cythera_data_viewer.html's inline script, which had reached 9,800 lines.
 * They came out because at that size the code that decodes a 1999 game and the
 * code that draws a web page had become indistinguishable, and because the two
 * delvmod cross-checks -- the only things here that can tell a wrong decoder
 * from a right one -- had nothing to point at but "the page".
 *
 * WHAT THIS IS NOT. It is not a library, and calling it one would mislead
 * whoever reads it next. `fileBytes` and `masterIndexGlobal` below are the
 * open archive, and getResourceBytes() reads them as ambient globals rather
 * than taking them as arguments; the tables memoised in here are still dropped
 * by resetDerivedCaches() over in the page. A classic script shares one global
 * scope with the document, so extracting these files changed none of that and
 * was not meant to. What it buys is that the domain code can be read on its
 * own and that a harness can name the file it is checking. Threading the
 * archive through as a parameter is a different and much larger job.
 *
 * CLASSIC SCRIPT, deliberately -- no `type="module"`, no import, no export.
 * A module is fetched with CORS, and a page opened from file:// has an opaque
 * origin, so a module would not load at all when the page is double-clicked
 * rather than served. utilities/page_scripts.mjs throws if it finds one.
 *
 * LOAD ORDER: after js/mac-bytes.js, whose u16be/u32be/decodeMacRoman this
 * uses, after js/mac-containers.js and js/mac-vise.js (extractDelverArchive
 * unwraps a container or an installer), and before delv-graphics.js and
 * delv-script.js.
 *
 * The encryption lists near the bottom are delvmod's, not this project's, and
 * utilities/delv_crosscheck.mjs re-reads them out of delvmod's Python on every
 * run. The comment above smartDecrypt says what guessing them cost.
 */

let fileBytes = null;
let masterIndexGlobal = null;
function parseDelverStringTable(p) {
  if (!p || p.length < 6) return null;
  const hdr = u16be(p, 0);
  if ((hdr & 0xF000) !== 0x9000) return null;
  const count = hdr & 0x0FFF;
  const out = [];
  for (let i = 0; i < count; i++) {
    const e = 2 + i * 4;
    if (e + 4 > p.length) break;
    const off = u16be(p, e+2);
    if (off >= p.length) { out.push(''); continue; }
    let end = off; while (end < p.length && p[end] !== 0) end++;
    out.push(decodeMacRoman(p.subarray(off, end)));
  }
  return out;
}
// A cursor over the archive. The readers themselves live in js/mac-bytes.js,
// so this only keeps the position: `pstring` decodes Mac Roman rather than
// Latin-1, which is what the archive's title actually is -- the two agree on
// ASCII and disagree on every accented character.
class BinReader {
  constructor(bytes) { this.data = bytes; this.pos = 0; }
  seek(p) { this.pos = p; }
  u8() { return this.data[this.pos++]; }
  u16() { const v = u16be(this.data, this.pos); this.pos += 2; return v; }
  u32() { const v = u32be(this.data, this.pos); this.pos += 4; return v; }
  pstring(off) { return pstring(this.data, off !== undefined ? off : this.pos); }
}

// Called tens of thousands of times while the cross-reference and script-text
// indexes are built, so it reads the two words directly rather than allocating
// a BinReader per call. It also validates the pair now: slice() clamps, so a
// corrupt length used to yield a short buffer that looked like a real (but
// truncated) resource instead of an error.
function getResourceBytes(resid) {
  const subn = Math.floor(resid / 0x100) - 1;
  const n = resid % 0x100;
  const mi = masterIndexGlobal && masterIndexGlobal[subn];
  if (!mi || !mi[0]) return null;
  const [subOff, subLen] = mi;
  if (n*8 + 8 > subLen) return null;
  const p = subOff + n*8;
  const roff = ((fileBytes[p]*0x1000000) + (fileBytes[p+1]<<16) + (fileBytes[p+2]<<8) + fileBytes[p+3]) >>> 0;
  const rlen = ((fileBytes[p+4]*0x1000000) + (fileBytes[p+5]<<16) + (fileBytes[p+6]<<8) + fileBytes[p+7]) >>> 0;
  if (!roff || roff + rlen > fileBytes.length) return null;
  return fileBytes.slice(roff, roff+rlen);
}

// How many entries a subindex actually declares. The builders below used to
// ask for all 256 ids in every populated subindex -- 8,704 lookups where 1,600
// exist -- because the count was never consulted.
function subindexCount(subn) {
  const mi = masterIndexGlobal && masterIndexGlobal[subn];
  return mi && mi[0] ? Math.min(256, Math.floor(mi[1] / 8)) : 0;
}

function parseCompositionWord(t) {
  const resid = 0x8E00 + ((t >> 4) & 0xFF);
  const tileInSheet = t & 0x0F;
  const segment = (t >> 12) & 0xF;
  return {resid, tileInSheet, segment};
}

// BinHex 4.0, MacBinary and AppleSingle/AppleDouble are decoded by
// js/mac-containers.js, which resource_fork_browser.html loads as well -- both
// pages had their own copy, and each had fixed a bug the other still had.

// Where the master index is, and how long it is, per delvmod's
// archive.load_header/load_index: an (offset,length) pair at 0x80, and the
// index entries start 8 bytes into it -- the first pair describes the index
// itself. Cythera Data says 0x80/0x800, so the entries run 0x88..0x87F and
// there are length/8 - 1 = 255 of them.
//
// This used to be hardcoded as "256 pairs at 0x88", which invented a subindex
// 255: that entry sits at 0x880, one slot PAST the end of the index, so the
// reader was taking the first subindex's own data as an index entry and
// getting off=0xA07F5000 len=0xFFFF0000. It was bounds-checked away rather
// than explained. There is no subindex 255 -- those bytes were never anything
// but what follows the index.
function delverMasterIndexExtent(bytes) {
  if (!bytes || bytes.length < 0x88) return null;
  const off = u32be(bytes, 0x80), len = u32be(bytes, 0x84);
  // 16 bytes is the smallest index that could hold one entry after its own
  // self-describing pair, and the whole thing has to be inside the file.
  if (off < 0x80 || len < 16 || len % 8 !== 0 || off + len > bytes.length) return null;
  // Nothing past the index can be a subindex, so this is also the floor every
  // entry's offset has to clear.
  return { off, len, first: off + 8, count: Math.min(256, len / 8 - 1), dataStart: off + len };
}

// Does this buffer actually hold a Delver archive? The answer is read from
// the structure the archive is built out of, and from nothing else: a title,
// a master index whose every entry is either empty or a whole number of
// 8-byte records inside the file, and inside every subindex it names, resource
// entries that are likewise either empty or inside the file. That is the
// shape delvmod's load_header/load_index walks, and it is what tells a data
// fork from a resource fork, from BinHex ASCII, from an unrelated file.
//
// It used to demand eight populated subindexes as well, which was a count
// standing in for a check. The shipped Cythera Data has 34, so eight looked
// safe; a Cythera SAVED GAME has six (the party's records, its combat-AI
// scripts, the prop list of the zone the player stands in, one portrait, and
// two subindexes the scenario never has), and a patch file -- Magpie's Pumpkin Patch, twelve
// tile sheets -- has two. Every one of them was refused, so the page could
// not open a player file at all: utilities/addons_check.mjs opens all seven
// third-party archives in the community's add-ons, and all seven were on its
// refused list. The rule above accepts all seven and the shipped archive, and refuses
// every other file in the same corpus: the application's PEF data fork, both
// resource forks, the BinHex texts, the installers, JPEGs, an .rtf, a TEXT
// file. What made the eight seem necessary was that the entries were only
// COUNTED: a stray value that happened to pass was tolerated, so the count
// had to be high enough that stray values could not reach it. Requiring that
// no entry be stray is the stronger test and needs no threshold.
//
// The player name is the pstring at 0x20 -- delvmod's `player_name`, empty in
// the scenario file -- so a caller can say what it opened.
function describeDelverArchive(bytes) {
  if (!bytes || bytes.length < 0x888)
    return { ok: false, reason: 'only ' + (bytes ? bytes.length : 0) + ' bytes, too small to hold a master index' };
  // Byte 0 is the length of the archive's title, which is the first thing in
  // the file. Checking it matters more than it looks: without it a MacBinary
  // or AppleSingle file WRAPPING a real archive passes, because the wrapper
  // pushes everything 128 (or 38) bytes along and enough of the shifted
  // garbage at 0x88 still reads as plausible offsets.
  const tlen = bytes[0];
  let title = '';
  if (tlen >= 1 && tlen <= 63) {
    let printable = true;
    for (let i = 1; i <= tlen; i++) if (bytes[i] < 0x20 || bytes[i] === 0x7F) { printable = false; break; }
    if (printable) title = decodeMacRoman(bytes.subarray(1, 1 + tlen));
  }
  if (!title) return { ok: false, title: '', reason: 'no title string at byte 0' };
  const mi = delverMasterIndexExtent(bytes);
  if (!mi) return { ok: false, title, reason: 'no usable master index (offset,length) pair at 0x80' };
  const player = (bytes[0x20] >= 1 && bytes[0x20] <= 31) ? pstring(bytes, 0x20) : '';
  let populated = 0;
  for (let i = 0; i < mi.count; i++) {
    const off = u32be(bytes, mi.first + i*8), len = u32be(bytes, mi.first + i*8 + 4);
    if (!off && !len) continue;
    if (!(off >= mi.dataStart && len > 0 && len % 8 === 0 && off + len <= bytes.length))
      return { ok: false, title, player, populated, reason: 'master index entry ' + i + ' at 0x' +
        (mi.first + i*8).toString(16).toUpperCase() + ' is neither empty nor a subindex inside the file' };
    // Every entry of the subindex, too: a resource is (offset,length) and
    // both lie past the index and inside the file, or the entry is empty.
    const n = Math.min(256, len / 8);
    for (let k = 0; k < n; k++) {
      const roff = u32be(bytes, off + k*8), rlen = u32be(bytes, off + k*8 + 4);
      if (!roff && !rlen) continue;
      if (roff < mi.dataStart || roff + rlen > bytes.length)
        return { ok: false, title, player, populated, reason: 'subindex ' + i + ' entry ' + k +
          ' points outside the file' };
    }
    populated++;
  }
  if (!populated)
    return { ok: false, title, player, populated, reason: 'the master index at 0x' + mi.first.toString(16).toUpperCase() +
      ' names no subindex at all' };
  return { ok: true, title, player, populated };
}

function extractDelverArchive(bytes, opts) {
  opts = opts || {};
  const notes = [];
  const direct = describeDelverArchive(bytes);
  if (direct.ok) return { bytes, via: 'data fork', info: direct };
  notes.push('read as-is, ' + direct.reason);

  // The whole game at once: Cythera's installer is an Installer VISE
  // archive (js/mac-vise.js) holding the archive, the application, the
  // Combat AI scripts and the documentation, each with both forks. The
  // archive is the one file of type 'DelS' in it. What comes back carries
  // the installer too, so the page can keep the other files.
  const installer = sniffViseInstaller(bytes, opts.pick);
  if (installer) {
    const arc = installer.archive;
    const entry = arc.entries.find(e => e.type === 'DelS');
    if (!entry) throw new Error('That is an ' + arc.versionName + ' installer' +
      (installer.container && installer.container.name ? ' ("' + installer.container.name + '")' : '') +
      ' with ' + arc.entries.length + ' files and no Delver archive among them.');
    const got = viseExtract(arc, entry);
    const d = describeDelverArchive(got.data);
    if (!d.ok) throw new Error('"' + entry.name + '" inside the installer is not a Delver archive: ' + d.reason);
    const label = arc.versionName + ' installer' +
      (installer.container && installer.container.name ? ' "' + installer.container.name + '"' : '') +
      (installer.container ? ' in ' + installer.container.kind : '');
    return { bytes: got.data, via: label, info: d,
             forks: { kind: label, name: entry.name, type: entry.type, creator: entry.creator,
                      data: got.data, rsrc: got.rsrc },
             installer: { archive: arc, container: installer.container, entry, crcOk: got.crcOk,
                          installers: installer.installers, picked: installer.picked } };
  }

  // sniffMacContainer knows the order these have to be tried in, and why.
  const forks = sniffMacContainer(bytes);
  const kind = forks ? forks.kind : '';

  if (forks) {
    for (const which of ['data', 'rsrc']) {
      const buf = forks[which];
      if (!buf || !buf.length) continue;
      const d = describeDelverArchive(buf);
      if (d.ok) return { bytes: buf, via: kind + ' ' + (which === 'data' ? 'data fork' : 'resource fork'), info: d, forks };
      notes.push(kind + ' ' + which + ' fork, ' + d.reason);
    }
    const t = (forks.type || '').trim(), c = (forks.creator || '').trim();
    throw new Error('That is a ' + kind + ' file' + (forks.name ? ' holding "' + forks.name + '"' : '') +
      (t ? " (type '" + t + "', creator '" + c + "')" : '') +
      (t === 'APPL' ? ' — the Cythera application, not its data.' : '.') +
      " The archives this tool reads are “Cythera Data” (type 'DelS', creator 'Delv') and a Cythera saved game (type 'DelP'). [" + notes.join('; ') + ']');
  }
  throw new Error('Not a Delver archive: ' + notes.join('; ') +
    '. Expected "Cythera Data" itself, a .hqx / MacBinary / AppleSingle wrapper around it, ' +
    'or the Cythera installer (.sit or Cythera.bin).');
}

function bitsOfSingle(data, size, index) {
  let result = 0;
  for (let i=0; i<size; i++) {
    const bitPos = index + i; const byteI = bitPos >> 3; const bitI = bitPos & 7;
    const byte = byteI < data.length ? data[byteI] : 0;
    const bit = (byte >> (7-bitI)) & 1;
    result = (result << 1) | bit;
  }
  return result >>> 0;
}

function ncbitsOf(data, fields) {
  let result = 0;
  for (const [size, index] of fields) { result = (result * Math.pow(2,size)) + bitsOfSingle(data, size, index); }
  return result;
}

// BUGFIX: Calculated the accurate final required byte bound rather than estimating blindly to avoid 
// an out-of-bounds byte index error grabbing extra garbage payload bits.
function bitsOf(data, size, index) {
  if (size === 0) return 0;
  const byteIndex0 = Math.floor(index/8);
  const bitIndex = index % 8;
  const lastByteNeeded = Math.floor((index + size - 1) / 8);
  const bitSize = size % 8;
  let byteIndex = byteIndex0;
  let truncated = false;
  let result = (data[byteIndex] !== undefined ? data[byteIndex] : 0) & (0xFF >> bitIndex);
  while (byteIndex < lastByteNeeded) {
    byteIndex += 1;
    if (data.length === byteIndex) { result <<= 8; truncated = true; break; }
    result = (result << 8) | (data[byteIndex] !== undefined ? data[byteIndex] : 0);
  }
  // When the requested range runs off the end of the buffer we shifted in a
  // padding byte above and must shift it back out, matching the reference
  // implementation. Without this the value comes back 256x too large.
  result >>= truncated ? 8 : (8 - ((index + size) % 8)) % 8;
  return result;
}

// --- Structured "Delver atom Array" parser -------------------------------
// Ported from delv/script.py (Array.demarshal / read_atom). Character
// Names (0x0201), Sign/Scroll/Quest/Book/Bookshelf Text, Ring and
// Gravestone Inscriptions all share this exact format: a 2-byte header
// (top nibble = typecode, must be 9; low 12 bits = entry count) followed
// by N 4-byte reference records. Each record's high bit marks it as a
// "dref": byte0 (minus 0x80, high byte of a resid) + byte1 (low byte of
// resid) + bytes 2-3 (uint16 offset). For this resource family the
// encoded resid is bogus/self-referential (see CharacterNameArray's
// override_dref) -- the string data lives at `offset` within this SAME
// resource's bytes, not in the referenced resid. We just read offset and
// grab the NUL-terminated MacRoman string living there.
const DELVER_TEXT_ARRAY_RESIDS = new Set([
  0x0201, 0x0218, 0x0219, 0x021A, 0x021B, 0x021D, 0x021F, 0x0220
]);
function parseDelverTextArray(data) {
  if (data.length < 2) return null;
  const header = u16be(data, 0);
  const typecode = (header >> 12) & 0xF;
  const count = header & 0x0FFF;
  if (typecode !== 9) return null; // not an Array-type resource
  const entries = [];
  let p = 2;
  for (let n = 0; n < count && p + 4 <= data.length; n++) {
    const b0 = data[p];
    // Per the wiki's Word page every atom is a 4-byte word, so a non-dref
    // atom advances by 4 like any other. Skipping 1 byte desynced the rest
    // of the array behind it.
    if (b0 < 0x80) { p += 4; continue; } // inline atom, not a dref (rare)
    const off = u16be(data, p+2);
    p += 4;
    if (off >= data.length) { entries.push({ index: n, offset: off, str: '(offset out of range)' }); continue; }
    let end = off;
    while (end < data.length && data[end] !== 0) end++;
    const str = decodeMacRoman(data.slice(off, end));
    entries.push({ index: n, offset: off, str });
  }
  return entries;
}

// --- Map (subindex 127 / 0x80xx) structured header parser -----------------
// Ported from delv/level.py Map.load_from_bfile(). The layout below matches
// the wiki's Cythera Map page field for field -- including the four cardinal
// exit ports, which that page does document by name and in order (North,
// East, South, West; uint16 each) at exactly these offsets. This parser
// agrees with the wiki here rather than adding anything to it.
function parseDelverMap(data) {
  if (data.length < 32) return null;
  const width = u16be(data, 0), height = u16be(data, 2), unknown = u16be(data, 4);
  const roofLayerSize = u16be(data, 6), roofUnderlayerSize = u16be(data, 8);
  const hEdge = data[10], vEdge = data[11];
  const exitN = u16be(data, 12), exitE = u16be(data, 14), exitS = u16be(data, 16), exitW = u16be(data, 18);
  // Sanity bounds -- reject anything that isn't a plausible Map header.
  // The format's own ceiling is 4096x4096 (prop coordinates are 12-bit), per
  // the wiki, so that is the limit enforced here. Cythera's own maps are far
  // smaller -- the largest is 64x64 -- but a hand-edited or fan-made archive
  // is entitled to the full range, and the old 320-per-axis guard would have
  // rejected it outright while the comment claimed 1024. A total-tile cap
  // still applies so a bad decrypt guess cannot trigger a runaway allocation.
  if (width === 0 || height === 0 || width > 4096 || height > 4096) return null;
  if (width * height > 4194304) return null;
  // 12 bytes padding at offset 20-31, then roof data, then map_data
  const roofWords = 0x20*(roofLayerSize + roofUnderlayerSize);
  const roofStart = 32;
  const mapStart = roofStart + roofWords*2;
  const mapEnd = mapStart + width*height*2;
  // The header must actually be followed by enough bytes to hold the
  // declared map_data grid -- otherwise this almost certainly is not a
  // real Map resource (e.g. wrong subindex, or auto-decrypt guessed wrong).
  if (mapStart < 0 || mapEnd > data.length + 4) return null;
  return {
    width, height, unknown, roofLayerSize, roofUnderlayerSize,
    horizontalEdgePropagation: hEdge, verticalEdgePropagation: vEdge,
    exitZoneportNorth: exitN, exitZoneportEast: exitE,
    exitZoneportSouth: exitS, exitZoneportWest: exitW,
    roofDataOffset: roofStart, mapDataOffset: mapStart,
    mapDataLength: width*height
  };
}

// --- PropList (subindex 128 / 0x81xx) 16-byte record parser --------------
// Ported from delv/level.py PropList.load_from_bfile(), with one correction.
//
// delvmod names the uint16 at +8/+9 `propref` and the uint32 at +10..13
// `storeref`. That is backwards. The wiki's subindex 128 page has StoreRef as
// the 2 bytes at +8/+9, and it has evidence: those 2 bytes match the symbol
// keys in 0xF015 exactly. Checked against this archive -- in prop list 0x8102
// (Odemia) the water trough at (47,44) has 00 07 at +8/+9 and F015 contains
// {0x0007, "Od_Trough1"}; the blacksmith's bellows at (45,46) has 00 06 and
// F015 has {0x0006, "Od_Bellows1"}. Reading a uint32 at +10 straddled the
// boundary and picked up nothing meaningful.
//
// The wiki's field table also lists an "Other Prop reference, uint32?" which
// would occupy +8..+11 and therefore overlap StoreRef. It is zero in every
// record in the scenario file -- prop-to-prop references are made at runtime
// -- so there is nothing here to tell the two readings apart. Both are
// reported below, with the 6 bytes after StoreRef kept as raw tail.
function parseDelverPropList(data) {
  const records = [];
  const recSize = 16;
  for (let p = 0; p + recSize <= data.length; p += recSize) {
    const flags = data[p];
    // read_xy24: 3 bytes packing 12-bit x, 12-bit y
    const raw = (data[p+1] << 16) | u16be(data, p+2);
    const x = raw >> 12, y = raw & 0x0FFF;
    // ...except when the prop is not on the floor at all. delvmod's level.py
    // (textual_location / inside_something) and the wiki's subindex 128 page
    // agree that the location word doubles as a containment link: flags & 0x10
    // means a character is holding it and the low 16 bits are that character's
    // number, flags & 0x08 means it is inside another prop in this same list at
    // index (low16 - 0x100), and both bits together mean the character has it
    // equipped. This viewer read those bytes as x,y unconditionally, so every
    // contained prop in the shipped archive was being placed at coordinates
    // that mean nothing: 985 of the 14,485 records -- 866 inside another prop,
    // 119 carried by a character, 32 of those equipped -- plus 5 deleted
    // records, which carry the same bits and are excluded here. 0x01 is
    // "okay to take".
    const carried = !!(flags & 0x10), inside = !!(flags & 0x08);
    const holder = raw & 0xFFFF;
    const aw = u16be(data, p+4);
    const aspectRot = (aw >> 10) & 0x3F, proptype = aw & 0x03FF;
    const d3 = u16be(data, p+6);
    const storeref = u16be(data, p+8);
    const otherprop = u32be(data, p+8);
    const u = u16be(data, p+14);
    let tail = '';
    for (let k = p+10; k < p+16; k++) tail += data[k].toString(16).padStart(2,'0');
    records.push({
      index: records.length, flags, x, y, aspect: aspectRot & 0x1F,
      rotated: aspectRot & 0x20, proptype, d1: d3 >> 8, d2: d3 & 0xFF,
      d3, storeref, otherprop, tail, u,
      onMap: !carried && !inside && flags !== 0xFF,
      carriedBy: carried ? holder : null,
      container: (!carried && inside) ? holder - 0x100 : null,
      equipped: carried && inside,
      takeable: !!(flags & 0x01)
    });
  }
  return records;
}

// --- Schedule List (resource 0xF00B) parser -------------------------------
// Ported from delv/schedule.py ScheduleList. 0x100 uint16 lengths, one per
// possible character, followed by that many EIGHT-byte entries each, packed
// in order. Eight is what the wiki's Schedules page says and what the loop
// below has always read -- the old "7-byte" comment was the thing that was
// wrong, not the code.
//   hour u8 | flags u8 (ends up as character field 0x15) | script u16
//   | level u8 | position xy24
function parseDelverScheduleList(data) {
  if (data.length < 512) return null;
  const lengths = [];
  for (let i = 0; i < 256; i++) lengths.push(u16be(data, i*2));
  let p = 512;
  const schedules = [];
  for (let ci = 0; ci < 256; ci++) {
    const len = lengths[ci];
    const entries = [];
    for (let n = 0; n < len && p + 8 <= data.length; n++) {
      const hour = data[p], mode = data[p+1];
      const scripting = u16be(data, p+2);
      const level = data[p+4];
      // read_xy24: 3 bytes packing 12-bit x, 12-bit y
      const raw = (data[p+5] << 16) | u16be(data, p+6);
      const x = raw >> 12, y = raw & 0x0FFF;
      p += 8;
      entries.push({ hour, mode, scripting, level, x, y });
    }
    if (len) schedules.push({ character: ci, entries });
  }
  return schedules;
}

function decryptResource(data, resid) {
  // Delver Archive resource cipher, taken verbatim from the reference
  // delv.archive.decrypt() implementation. The resource's own ID is used
  // as the "prokey" seed for a rolling multiplicative PRNG whose low byte
  // is XORed against each byte of ciphertext.
  const out = new Uint8Array(data.length);
  let key = (resid ^ (resid >> 8)) & 0xFFFF;
  const m = ((resid & 0x3F) << 2) + 1;
  const b = resid >> 6;
  for (let i = 0; i < data.length; i++) {
    key = (key * m + b) & 0xFFFF;
    out[i] = data[i] ^ (key & 0xFF);
  }
  return out;
}

function printableRatio(data) {
  if (!data.length) return 0;
  let count = 0;
  for (let i = 0; i < data.length; i++) {
    const b = data[i];
    if (b >= 32 && b <= 126) count++;
  }
  return count / data.length;
}

function byteEntropy(data) {
  if (!data.length) return 0;
  const counts = new Array(256).fill(0);
  for (let i = 0; i < data.length; i++) counts[data[i]]++;
  let h = 0;
  for (let i = 0; i < 256; i++) {
    if (!counts[i]) continue;
    const p = counts[i] / data.length;
    h -= p * Math.log2(p);
  }
  return h;
}

// Subindices holding pure binary structured data. These are NEVER encrypted,
// and running the speculative decryptor on them corrupts the bytes -- which
// previously made 36 of 42 Map headers unparseable and pushed prop
// coordinates far outside the map bounds (so props silently drew off-canvas).
/* Which subindexes are encrypted is not something to guess at.
 *
 * The archive format does not flag it, so this file used to decrypt every
 * resource speculatively and keep whichever version looked more like
 * plaintext. That heuristic had already grown three escalating special cases
 * (all-zero payloads, a container-structure test, a named-script test), each
 * one added after it got a specific resource wrong -- which is what it looks
 * like when a guess is standing in for knowledge.
 *
 * delvmod states it outright. From its Scenario class, in
 * sources/github_delvmod/code/delv/archive.py:
 *
 *   known_encrypted = [1,2,4,7,8,9,10,11,12,13,14,15,16,19,20,23,24,25,26,27,29,47]
 *   known_clear     = [0,3,127,128,131,135,137,141,142,143,144,187,239,254]
 *   single_known    = {0x0210: False}
 *
 * Checked against the whole archive, the heuristic disagreed with that table
 * on 18 of 1,558 resources, always by leaving an encrypted resource
 * undecrypted. 0x1415 is the clearest of them: its decrypted form contains
 * "Sewers" and readable structure, its stored form is noise, and the scores
 * were 0.238 raw against 0.223 decrypted -- a coin flip, lost.
 *
 * The heuristic below is still here, and still needed: it now runs only for
 * subindexes the table has no opinion about, which is exactly what a modded
 * archive with new subindexes would be.
 *
 * utilities/delv_crosscheck.mjs re-reads these three lists out of the Python
 * and fails if the copies here drift from them. */
const DELV_ENCRYPTED_SUBN = new Set([1,2,4,7,8,9,10,11,12,13,14,15,16,19,20,23,24,25,26,27,29,47]);
const DELV_CLEAR_SUBN = new Set([0,3,127,128,131,135,137,141,142,143,144,187,239,254]);
const DELV_CLEAR_RESID = new Set([0x0210]);
/* The two subindexes only a PLAYER FILE has, and they are kept apart from the
   three tables above on purpose: those are delvmod's, compared against
   delvmod's own source by `delv_crosscheck.mjs`, and adding to them would
   turn the oracle into a mirror. This one is ours, and it is read out of the
   executable rather than guessed.

   Every segment the engine writes into a player file goes through
   `TCachedSegFiles::SaveSegment` / `TSegFile::SaveSegment`, which do not
   encrypt: `SaveLevelProps` writes 0x81zz, 0x82zz and 0xF306, `SaveGlobals`
   writes 0xF009 and 0xF00E, `THeap::Save` writes 0xF307 and 0xF308,
   `TToDo::SaveToDo` 0x0401, `TStatusWindow::SaveMacros` 0x0404,
   `CreatePlayer` 0x8800 and `TDelverApp::SaveToFile` 0x0400. The one routine
   that encrypts, `SaveEncryptedSegment`, has exactly one caller —
   `TInterp::DoInterpAt`, the script interpreter writing a script resource
   back — so in a player file the script subindexes are encrypted and nothing
   else is. 129 (0x82zz, the map memory) and 242 (0xF3zz, the script heap)
   never occur in a scenario, so naming them clear here costs the scenario
   nothing.

   It mattered: both are zero-filled early in a game, the heuristic below
   scores zeros worse than the noise they would decrypt to, and a saved game's
   map memory and heap were being served as garbage. */
const DELV_PLAYER_CLEAR_SUBN = new Set([129, 242]);

function smartDecrypt(data, resid) {
  const subn = Math.floor(resid / 0x100) - 1;
  if (DELV_CLEAR_RESID.has(resid) || DELV_CLEAR_SUBN.has(subn) || DELV_PLAYER_CLEAR_SUBN.has(subn)) {
    return { data: data, wasDecrypted: false, rawScore: 0, decScore: 0, exempt: true, known: true };
  }
  if (DELV_ENCRYPTED_SUBN.has(subn)) {
    return { data: decryptResource(data, resid), wasDecrypted: true, rawScore: 0, decScore: 0, known: true };
  }
  // Beyond here the archive is telling us nothing and neither is delvmod, so
  // decrypt speculatively and keep whichever version looks more like
  // plaintext: higher printable-ASCII ratio, lower entropy.
  const decrypted = decryptResource(data, resid);
  // A resource that decrypts to nothing but zero bytes is an empty
  // placeholder, and that is a certainty, not a guess -- the odds of the
  // keystream matching arbitrary ciphertext across every byte are nil.
  // The scoring heuristic cannot see this: all-zeros has no printable
  // characters at all, so ciphertext noise beats it and 0x033F, 0x0500 and
  // 0x0540 were all being shown as raw garbage.
  let allZero = decrypted.length > 0;
  for (let i = 0; i < decrypted.length; i++) if (decrypted[i] !== 0) { allZero = false; break; }
  let rawZero = data.length > 0;
  for (let i = 0; i < data.length; i++) if (data[i] !== 0) { rawZero = false; break; }
  if (allZero && !rawZero) return { data: decrypted, wasDecrypted: true, rawScore: 0, decScore: 0, allZero: true };
  /* And the same certainty the other way round, which was missing until
     7 September 2026. A resource that is ALREADY nothing but zero bytes
     cannot be ciphertext: the keystream is never all zeros, so no plaintext
     encrypts to this. The scoring heuristic cannot see that either -- zeros
     have no printable characters, so the noise it would decrypt to wins --
     and a saved game is where it showed: three of a player file's own
     subindexes (0x82zz map memory, 0xF307 and 0xF308, the script heap) are
     zero-filled in an early save and were all being served as garbage. */
  if (rawZero) return { data: data, wasDecrypted: false, rawScore: 0, decScore: 0, allZero: true };
  const rawScore = printableRatio(data) - byteEntropy(data) / 32;
  const decScore = printableRatio(decrypted) - byteEntropy(decrypted) / 32;
  // Byte statistics alone get it wrong for small script resources: 0x1050 and
  // 0x1914 happen to begin with 0x81 in their encrypted form, which reads as a
  // function header and wins on printable ratio. Now that the container format
  // is understood, ask which candidate actually parses -- structure beats
  // statistics whenever exactly one of the two is well formed.
  if (typeof dvmPlausibleContainer === 'function') {
    // Subindex 3's named scripts are plaintext but look like nothing to the
    // container test, and their bytecode has few printable bytes, so the score
    // heuristic was "decrypting" all fourteen into noise. A leading Pascal
    // name is decisive structure -- keystream output does not spell "Defend".
    const rawNamed = (typeof dvmNamedScript === 'function') && dvmNamedScript(data);
    const decNamed = (typeof dvmNamedScript === 'function') && dvmNamedScript(decrypted);
    if (rawNamed && !decNamed) return { data: data, wasDecrypted: false, rawScore: 0, decScore: 0, byStructure: true };
    if (decNamed && !rawNamed) return { data: decrypted, wasDecrypted: true, rawScore: 0, decScore: 0, byStructure: true };
    const rawOk = dvmPlausibleContainer(data, resid);
    const decOk = dvmPlausibleContainer(decrypted, resid);
    if (decOk && !rawOk) return { data: decrypted, wasDecrypted: true, rawScore, decScore, byStructure: true };
    if (rawOk && !decOk) return { data: data, wasDecrypted: false, rawScore, decScore, byStructure: true };
  }
  if (decScore > rawScore) {
    return { data: decrypted, wasDecrypted: true, rawScore, decScore };
  }
  return { data: data, wasDecrypted: false, rawScore, decScore };
}

function extractPascalStrings(data) {
  // Classic Pascal-style strings: a single length-prefix byte followed by
  // that many printable bytes. The cap here used to be 63, which silently
  // chopped Cythera's dialogue -- many lines run well past 100 characters
  // (one is 160). A Pascal length byte allows up to 255.
  let entries = [];
  let i = 0;
  while (i < data.length) {
    const len = data[i];
    if (len > 2 && len <= 255 && i + 1 + len <= data.length) {
      let isPrintable = true;
      let str = '';
      for (let j = 0; j < len; j++) {
        const code = data[i + 1 + j];
        if (code >= 32 && code <= 126) {
          str += String.fromCharCode(code);
        } else if (code === 13 || code === 10) {
          str += '\n';
        } else {
          isPrintable = false;
          break;
        }
      }
      // A real Pascal string ends where it ends. If the byte straight after
      // the run is still printable text, the "length" was almost certainly a
      // letter from a C string and this reading cuts a word in half -- which
      // is exactly how "a glowing triangle" became "a glowing tria".
      const after = i + 1 + len;
      const runsOn = after < data.length && data[after] >= 32 && data[after] <= 126;
      if (isPrintable && !runsOn) {
        entries.push({ offset: i, str, kind: 'pascal' });
        i += len + 1;
        continue;
      }
    }
    i++;
  }
  return entries;
}

function extractCStrings(data, minLen = 3) {
  // Many Cythera text/script resources actually use plain NUL-terminated
  // C-style strings rather than Pascal strings. Scan for runs of printable
  // ASCII bytes bounded by 0x00 bytes (or the ends of the buffer).
  let entries = [];
  let cur = '';
  let start = 0;
  let ok = true;
  for (let i = 0; i <= data.length; i++) {
    const b = i < data.length ? data[i] : 0;
    if (b === 0) {
      if (ok && cur.length >= minLen) {
        entries.push({ offset: start, str: cur, kind: 'cstr' });
      }
      cur = '';
      start = i + 1;
      ok = true;
    } else if (b >= 32 && b <= 126) {
      cur += String.fromCharCode(b);
    } else {
      ok = false;
    }
  }
  return entries;
}

function extractReadableStrings(data, resid) {
  // If the resource is a Delver container, its own structure says where the
  // strings are, and that beats any scan. The byte-level scans below are the
  // fallback for data that is not a container at all.
  if (resid !== undefined) {
    const owned = dvmStringObjects(data, resid);
    if (owned.length) {
      return owned.map(e =>
        '0x' + e.offset.toString(16).padStart(4, '0') + '  ' + JSON.stringify(e.str)
      ).join('\n\n');
    }
  }
  const pascal = extractPascalStrings(data);
  const cstrs = extractCStrings(data);
  const all = pascal.concat(cstrs).sort((a, b) => a.offset - b.offset);
  if (!all.length) return null;
  return all.map(e =>
    '0x' + e.offset.toString(16).padStart(4, '0') + '  [' + e.kind + ']  "' + e.str + '"'
  ).join('\n');
}

function hexDump(data) {
  let out = '';
  const limit = Math.min(data.length, 2048);
  for (let i = 0; i < limit; i += 16) {
    let hex = i.toString(16).padStart(4, '0') + '  ';
    let ascii = '';
    for (let j = 0; j < 16; j++) {
      if (i + j < data.length) {
        const b = data[i + j];
        hex += b.toString(16).padStart(2, '0') + ' ';
        ascii += (b >= 32 && b <= 126) ? String.fromCharCode(b) : '.';
      } else {
        hex += '   ';
      }
    }
    out += hex + ' |' + ascii + '|\n';
  }
  if (data.length > limit) out += '... (truncated, ' + data.length + ' bytes total)\n';
  return out;
}

/* ---- Writing ----------------------------------------------------------- */
/* Everything above reads an archive; from here down is the other direction.
 *
 * writeDelverArchive() mirrors delvmod's Archive.to_file() operation for
 * operation, deliberately: header pstrings at 0 and 0x20, the three unknown
 * bytes, the (offset,length) pair that describes the master index itself,
 * 0x800 bytes of padding, then every resource in subindex order and slot
 * order, then one 256-entry subindex table per populated subindex, then the
 * master index pairs filled in last. Being byte-identical to the reference
 * implementation is the point -- utilities/delv_write_check.mjs builds
 * archives with delvmod and fails if this function's output differs by one
 * byte, which is the same arrangement that keeps decompressDCG honest.
 * If you change the layout here, you are no longer writing Delver archives,
 * you are writing something delvmod happens to be able to read.
 *
 * Two behaviours worth calling out because they look like bugs and are
 * delvmod's own, kept for equivalence:
 *   - A zero-length resource is dropped entirely (delvmod's `if res:` uses
 *     __len__, so an empty resource is falsy and never written).
 *   - Every subindex table is written as 256 entries / 2048 bytes and the
 *     master index records length 2048, whatever the table's own length
 *     said in the file this data came from.
 */

// The cipher is a keystream XOR, so it is its own inverse: encrypting IS
// decrypting. The alias exists so call sites say what they mean.
const encryptResource = decryptResource;

function writeDelverArchive(spec) {
  const mio = spec.masterIndexOffset ?? 0x80;
  const masterLen = spec.masterIndexLength ?? 0x800;
  // Group resources into subindex slot arrays, dropping empties (see above).
  const slots = new Array(256).fill(null);
  for (const r of (spec.resources || [])) {
    if (!r.data || !r.data.length) continue;
    const subn = Math.floor(r.resid / 0x100) - 1;
    const n = r.resid % 0x100;
    if (subn < 0 || subn > 255) throw new Error('resid out of range: 0x' + r.resid.toString(16));
    if (subn >= masterLen / 8 - 1) throw new Error('subindex ' + subn + ' does not fit a master index of length 0x' + masterLen.toString(16));
    if (!slots[subn]) slots[subn] = new Array(256).fill(null);
    slots[subn][n] = r;
  }

  // A zero-initialised growable buffer; the gaps delvmod creates by seeking
  // forward (between the header fields, and the whole pad) stay zero here
  // by construction.
  let buf = new Uint8Array(0x20000);
  let end = 0;
  const ensure = (need) => {
    if (need > buf.length) {
      const nb = new Uint8Array(Math.max(need, buf.length * 2));
      nb.set(buf);
      buf = nb;
    }
    if (need > end) end = need;
  };
  const w8 = (v, at) => { ensure(at + 1); buf[at] = v & 0xFF; };
  const w32 = (v, at) => {
    ensure(at + 4);
    buf[at] = (v >>> 24) & 0xFF; buf[at + 1] = (v >>> 16) & 0xFF;
    buf[at + 2] = (v >>> 8) & 0xFF; buf[at + 3] = v & 0xFF;
  };
  const wbytes = (b, at) => { ensure(at + b.length); buf.set(b, at); };
  const wpstring = (s, at) => {
    const eb = encodeMacRoman(s || '');
    w8(eb.length, at);
    wbytes(eb, at + 1);
  };

  wpstring(spec.scenarioTitle ?? 'Cythera: Fate of Alaric', 0);
  wpstring(spec.playerName ?? '', 0x20);
  w8(spec.unknown40 ?? 0x13, 0x40);
  w8(spec.unknown42 ?? 2, 0x42);
  w8(spec.unknown48 ?? 2, 0x48);
  w32(mio, mio);
  w32(masterLen, mio + 4);

  // delvmod pads a fixed 0x800 after the header regardless of the master
  // index length it just wrote, so the first resource lands at mio + 0x808.
  let pos = mio + 8 + 0x800;
  ensure(pos);

  // Pass 1: resource data, subindex-ascending, slot-ascending. Encryption is
  // applied on the way out; spec data is always plaintext.
  const placed = new Map();
  for (let subn = 0; subn < 256; subn++) {
    const sl = slots[subn];
    if (!sl) continue;
    for (let n = 0; n < 256; n++) {
      const r = sl[n];
      if (!r) continue;
      const stored = r.encrypted ? encryptResource(r.data, r.resid) : r.data;
      placed.set(r, [pos, r.data.length]);
      wbytes(stored, pos);
      pos += stored.length;
    }
  }

  // Pass 2: one 256-entry table per populated subindex, remembering where
  // each went for the master index.
  const master = [];
  for (let subn = 0; subn < 256; subn++) {
    const sl = slots[subn];
    if (!sl) continue;
    master.push([subn, pos]);
    for (let n = 0; n < 256; n++) {
      const at = placed.get(sl[n]);
      w32(at ? at[0] : 0, pos);
      w32(at ? at[1] : 0, pos + 4);
      pos += 8;
    }
  }

  // Pass 3: the master index, back at the start. Empty subindexes stay as
  // the zeros the pad already holds, which is exactly the (0,0) delvmod
  // writes for them.
  for (const [subn, off] of master) {
    const at = mio + 8 + subn * 8;
    w32(off, at);
    w32(0x800, at + 4);
  }
  return buf.slice(0, end);
}

/* The parse that writeDelverArchive is the inverse of: an archive buffer in,
 * a writer spec out, with every resource as plaintext plus the encryption
 * verdict smartDecrypt reached for it. writeDelverArchive(delverArchiveSpec(b))
 * is the round trip the harness proves byte-exact against delvmod, and it is
 * the seam a future save-modified-archive feature edits in the middle of. */
function delverArchiveSpec(bytes) {
  const mi = delverMasterIndexExtent(bytes);
  if (!mi) return null;
  const spec = {
    scenarioTitle: pstring(bytes, 0),
    playerName: pstring(bytes, 0x20),
    unknown40: bytes[0x40], unknown42: bytes[0x42], unknown48: bytes[0x48],
    masterIndexOffset: mi.off, masterIndexLength: mi.len,
    resources: []
  };
  for (let subn = 0; subn < mi.count; subn++) {
    const p0 = mi.first + subn * 8;
    const subOff = u32be(bytes, p0), subLen = u32be(bytes, p0 + 4);
    if (!subOff || subOff + subLen > bytes.length) continue;
    const nmax = Math.min(256, Math.floor(subLen / 8));
    for (let n = 0; n < nmax; n++) {
      const p = subOff + n * 8;
      const roff = u32be(bytes, p), rlen = u32be(bytes, p + 4);
      if (!roff || !rlen || roff + rlen > bytes.length) continue;
      const resid = (subn + 1) * 0x100 + n;
      const raw = bytes.slice(roff, roff + rlen);
      const dec = smartDecrypt(raw, resid);
      // fileOffset/fileLength are where this resource's CIPHERTEXT sits in the
      // file it was read from. Nothing needed them until the patch route: the
      // cipher is a position-indexed keystream XOR, so editing plaintext
      // changes exactly the same byte positions in the file, and knowing where
      // the resource starts turns an edit into a handful of (offset, byte)
      // pairs instead of a five-megabyte archive.
      spec.resources.push({ resid, data: dec.data, encrypted: dec.wasDecrypted,
                            fileOffset: roff, fileLength: rlen });
    }
  }
  return spec;
}

/* ---- applying a Magpie patch ----------------------------------------------
   The closest thing Cythera has to an add-on system, and it is not a plug-in
   folder: nothing in the game reads one. A Magpie patch is a Delver Archive
   carrying the same scenario header as `Cythera Data` and holding only the
   resources to replace -- the Pumpkin Patch is 57,662 bytes against the
   archive's 5.6 MB, twelve tile sheets (subindex 141, 55 of their 192 tiles
   redrawn: jack-o'-lanterns, autumn foliage, blood on the blades) and one
   resource of Magpie's own. Magpie merged it into the data file on disk in 2000; this does the same
   merge in memory, so the browser player can boot a patched archive without
   anything being written back to a real file.

   THE MERGE IS BY RESOURCE ID AND NOTHING ELSE. Take the base archive's spec,
   replace each resource whose id the patch also carries, re-serialize. The
   cipher is keyed by the resource id and indexed from the start of the
   resource (see `decryptResource`), not by where the resource sits in the
   file, so moving a resource to a new offset is safe and the plaintext the
   patch supplies re-encrypts to the same bytes wherever it lands.

   WHAT IT REFUSES, and why each one is a real file someone will try:
   - a patch for another scenario: the title at offset 0 must match, or the
     resource ids mean something else entirely;
   - a saved game: `DelP` is the type of both a player file and a patch, and
     five of the twelve community add-ons are player files. A non-empty player
     name at 0x20 is the difference, and those go through the character import
     instead;
   - anything that is not a Delver Archive at all.

   A resource id the patch holds that the base does not is REPORTED AND
   SKIPPED rather than added, and so is one whose encryption verdict the two
   sides disagree about. The one real patch carries exactly one such
   resource, 0xFFFF in a subindex the game's archive does not have, which is
   Magpie's own bookkeeping and not game content. Adding ids the base lacks is
   how a patch would grow the archive into shapes nothing here has ever seen;
   the count comes back so a caller can say so. */
function mergeDelverPatch(baseBytes, patchBytes) {
  const base = delverArchiveSpec(baseBytes);
  if (!base) throw new Error('the game archive is not a Delver Archive');
  const patch = delverArchiveSpec(patchBytes);
  if (!patch) throw new Error('that file is not a Delver Archive, so it is not a Magpie patch');
  if (patch.playerName)
    throw new Error('that is a saved game (' + patch.playerName + '), not a patch — import it as a character instead');
  if (patch.scenarioTitle !== base.scenarioTitle)
    throw new Error('that patch is for ' + JSON.stringify(patch.scenarioTitle) +
                    ', and this game is ' + JSON.stringify(base.scenarioTitle));
  if (!patch.resources.length) throw new Error('that patch holds no resources');

  const byId = new Map(base.resources.map(r => [r.resid, r]));
  const replaced = [], skipped = [], disagreed = [];
  for (const r of patch.resources) {
    const target = byId.get(r.resid);
    if (!target) { skipped.push(r.resid); continue; }
    // Both sides ran the same smartDecrypt on the same id. When they reach
    // different verdicts one of the two plaintexts is not plaintext, and the
    // writer re-encrypts from the base's verdict -- so writing this resource
    // would put garbage in the archive. Leave the original in place and say
    // so. DELV_CLEAR_SUBN covers subindex 141, where the one real patch
    // works, so this is a guard rather than a path anything has taken.
    if (!!r.encrypted !== !!target.encrypted) { disagreed.push(r.resid); continue; }
    target.data = r.data;
    replaced.push(r.resid);
  }
  if (!replaced.length)
    throw new Error('none of that patch\'s ' + patch.resources.length +
                    ' resource(s) could be applied to the game archive');
  return { bytes: writeDelverArchive(base), replaced, skipped, disagreed,
           title: patch.scenarioTitle };
}

/* ---- editing a map -------------------------------------------------------
   Two writers for the map editor in index.html, and they are deliberately the
   smallest two that could work.

   A map resource is a header, a roof block and then one big-endian tile word
   per square, so **painting terrain is a patch, not a re-serialization**:
   `writeDelverMapTiles` copies the resource and writes two bytes per square
   changed. Nothing else in the resource is touched, and that is the property
   `delv_write_check.mjs` requires -- every other byte identical, the changed
   squares reading back as asked. A whole-map serializer would have to
   reproduce the roof block and the twelve bytes of padding at 20..31 that
   nothing in this project understands, and would risk all of it to move one
   tile.

   The prop list is the other way round: `writeDelverPropList` above already
   re-serializes the whole list and is proven the exact inverse of the parser
   over all 14,485 records in the shipped archive, so adding and removing go
   through it. `makeDelverPropRecord` exists so the page never has to invent
   the fields a fresh record needs -- particularly `tail`, six bytes the
   parser keeps as hex and nothing here understands.

   REMOVING A PROP DOES NOT REMOVE THE RECORD, and that is the important part.
   A record can be inside another prop in the same list, and the link is that
   prop's INDEX -- so splicing an entry out silently re-points every
   containment link after it. The file's own answer is a record whose flags
   are 0xFF: delvmod's `show_in_map` drops one (`if self.flags == 0xFF: return
   False`), this file's parser drops one, and the shipped archive carries
   exactly five. So Erase writes 0xFF and leaves the record where it is, every
   index intact. */
function writeDelverMapTiles(data, m, edits) {
  if (!m || !edits || !edits.length) return data;
  const out = data.slice();
  for (const e of edits) {
    const x = e.x | 0, y = e.y | 0;
    if (x < 0 || y < 0 || x >= m.width || y >= m.height) continue;
    const o = m.mapDataOffset + (x + y * m.width) * 2;
    if (o + 1 >= out.length) continue;
    out[o] = (e.tile >> 8) & 0xFF;
    out[o + 1] = e.tile & 0xFF;
  }
  return out;
}
// A prop record with every field a fresh one needs and nothing invented: the
// six tail bytes are zero, which is what an unplaced record in the archive
// has, and the flags default to 0 -- on the map, not takeable.
function makeDelverPropRecord(p) {
  return {
    flags: p.flags === undefined ? 0 : (p.flags & 0xFF),
    x: p.x & 0xFFF, y: p.y & 0xFFF,
    aspect: (p.aspect || 0) & 0x1F, rotated: p.rotated ? 0x20 : 0,
    proptype: p.proptype & 0x3FF,
    d3: (p.d3 || 0) & 0xFFFF, storeref: (p.storeref || 0) & 0xFFFF,
    tail: '000000000000'
  };
}
// The record a square shows, topmost first: the list is drawn in order, so
// the LAST record on a square is the one on top of the pile.
function delverPropsAtSquare(records, x, y) {
  return records.filter(r => r.onMap && r.x === x && r.y === y).reverse();
}

/* The inverse of parseDelverPropList, record for record. It can be exact
 * because the parse is lossless even where it looks lossy: x and y jointly
 * carry all 24 bits of the location word (holder links included -- a carried
 * prop's holder number is just what those bits mean, not different bits),
 * and `tail` keeps bytes 10..15 verbatim, covering delvmod's storeref u32
 * and trailing u16 that the parser only summarises. So editing a record is:
 * parse the list, change fields, write it back -- and an untouched list
 * writes back byte-identical, which delv_write_check.mjs proves over every
 * prop list in the real archive. Records are 16 bytes; a list whose length
 * is not a multiple of 16 keeps its trailing fragment only if the caller
 * re-appends it (none in the shipped archive has one). */
/* ---- the character records, 0xF009 ---------------------------------------
   512 fixed 32-byte records, one per character index -- the same index that
   names them in 0x0201, talks for them in 0x18nn and schedules them in
   0xF00B. In `Cythera Data` they are where everybody starts; in a **saved
   game** they are the live state of the world, which is why they are the
   whole of what a save editor edits.

   The field map is delvmod's wiki page for F009, which was worked out by
   diffing the shipped table against saves taken either side of a change
   (Hector before and after joining the party is the worked example there).
   Two fields are not from it: **nutrition at byte 27**, read out of the
   executable -- `TGameViewer::DoTicks` takes one off byte 27 of this record
   every game hour (the trace is in the private workbench's `doc/game-clock.md`,
   and the sheet's Hunger section is drawn from it) -- and byte 28's
   neighbours, left unnamed. In I.M.Cheater, the community's cheated save,
   the hero's byte 27 is 24 (a full stomach) and byte 28 is 0xFF (255
   training points), which is exactly what the file is famous for.

   THE UNKNOWN BYTES ARE KEPT AS BYTES, not as a hex tail. `parseDelverPropList`
   keeps its six unexplained bytes in a `tail` string because they are
   contiguous; here what is not understood is scattered through the record --
   6..7, 22..26, 29..31, and a second appearance word at 20..21 that is
   usually but NOT always the same as the one at 4..5 (the hero's differ in
   I.M.Cheater: aspect 13 against 0). So each record keeps `raw`, its own 32
   bytes, and the writer starts from that copy and lays the named fields back
   over it. That makes `write(parse(x))` exactly `x` for any record, named
   fields or not, and it makes an edit touch only the bytes the edit is
   about -- which for a file whose format is two thirds understood is the
   only honest way to write one back.

   `delv_write_check.mjs` requires the round trip byte for byte over the
   shipped table and over a saved game's when one is present. */
const DELV_CHAR_RECORD = 32;
function parseDelverCharacterRecords(data) {
  const out = [];
  for (let i = 0; i + DELV_CHAR_RECORD <= data.length; i += DELV_CHAR_RECORD) {
    const raw = data.slice(i, i + DELV_CHAR_RECORD);
    const ap = (raw[4] << 8) | raw[5];
    const xy = (raw[1] << 16) | (raw[2] << 8) | raw[3];
    out.push({
      index: out.length, raw,
      zone: raw[0], x: xy >> 12, y: xy & 0xFFF,
      proptype: ap & 0x3FF, aspect: ap >> 10,
      state: raw[8],                       // C0/D0/80 on the placed, 00 otherwise
      body: raw[9], reflex: raw[10], mind: raw[11],
      xp: (raw[12] << 8) | raw[13],
      health: raw[14], healthMax: raw[15],
      magic: raw[16], magicMax: raw[17],
      party: raw[18],                      // 0 until Hector joins, 5 after
      level: raw[19],
      nutrition: raw[27], training: raw[28]
    });
  }
  // Both tables in hand are an exact multiple of 32, but a fragment at the
  // end would be lost silently, so it rides along on the array and the writer
  // puts it back -- the prop-list writer's rule made safe rather than
  // written down.
  out.tail = data.slice(out.length * DELV_CHAR_RECORD);
  return out;
}
// True where a record is in use at all: an unused slot is 32 zero bytes, and
// a slot that is only a zone byte (the tail of the shipped table) is not a
// character either.
function delverCharacterInUse(r) { return !!(r && (r.proptype || r.healthMax || r.body)); }
function writeDelverCharacterRecords(records) {
  const tail = records.tail || new Uint8Array(0);
  const out = new Uint8Array(records.length * DELV_CHAR_RECORD + tail.length);
  out.set(tail, records.length * DELV_CHAR_RECORD);
  for (let i = 0; i < records.length; i++) {
    const r = records[i], p = i * DELV_CHAR_RECORD;
    out.set(r.raw, p);
    out[p] = r.zone & 0xFF;
    const xy = ((r.x & 0xFFF) << 12) | (r.y & 0xFFF);
    out[p+1] = (xy >> 16) & 0xFF; out[p+2] = (xy >> 8) & 0xFF; out[p+3] = xy & 0xFF;
    const ap = ((r.aspect & 0x3F) << 10) | (r.proptype & 0x3FF);
    out[p+4] = (ap >> 8) & 0xFF; out[p+5] = ap & 0xFF;
    out[p+8] = r.state & 0xFF;
    out[p+9] = r.body & 0xFF; out[p+10] = r.reflex & 0xFF; out[p+11] = r.mind & 0xFF;
    out[p+12] = (r.xp >> 8) & 0xFF; out[p+13] = r.xp & 0xFF;
    out[p+14] = r.health & 0xFF; out[p+15] = r.healthMax & 0xFF;
    out[p+16] = r.magic & 0xFF; out[p+17] = r.magicMax & 0xFF;
    out[p+18] = r.party & 0xFF;
    out[p+19] = r.level & 0xFF;
    out[p+27] = r.nutrition & 0xFF; out[p+28] = r.training & 0xFF;
  }
  return out;
}

function writeDelverPropList(records) {
  const out = new Uint8Array(records.length * 16);
  for (let i = 0; i < records.length; i++) {
    const r = records[i], p = i * 16;
    out[p] = r.flags & 0xFF;
    const raw = ((r.x & 0xFFF) << 12) | (r.y & 0xFFF);
    out[p+1] = (raw >> 16) & 0xFF; out[p+2] = (raw >> 8) & 0xFF; out[p+3] = raw & 0xFF;
    const aw = (((r.aspect & 0x1F) | (r.rotated ? 0x20 : 0)) << 10) | (r.proptype & 0x3FF);
    out[p+4] = (aw >> 8) & 0xFF; out[p+5] = aw & 0xFF;
    out[p+6] = (r.d3 >> 8) & 0xFF; out[p+7] = r.d3 & 0xFF;
    out[p+8] = (r.storeref >> 8) & 0xFF; out[p+9] = r.storeref & 0xFF;
    for (let k = 0; k < 6; k++) out[p+10+k] = parseInt(r.tail.substr(k*2, 2), 16) || 0;
  }
  return out;
}
