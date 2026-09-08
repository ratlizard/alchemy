/* mac-stuffit.js -- read the table of contents of a StuffIt archive, and
   take out the files that were stored rather than compressed.
   =========================================================================

   Cythera's installer is mirrored as a StuffIt archive -- archive.org's
   `cythera.sit` and old.mac.gdn's `Cythera_Installer.sit` are StuffIt 5,
   Ambrosia's own `Cythera_1.0.4_Installer.sit` is the classic `SIT!`
   format -- and in every one of them the file that matters, the installer
   application's 6.8 MB data fork, is STORED: method 0, no compression at
   all. Only the application's 263 KB resource fork is compressed, and the
   page has no use for it. So this file reads both catalogs and hands back
   stored forks, and says plainly which method a compressed fork would need.
   It is not a decompressor. StuffIt's methods 13 (LZ+Huffman, what the
   1.0.1 and 1.0.2 installers use for both forks) and 15 (Arsenic) are
   documented in The Unarchiver's XADMaster and in benletchford/stuffit-rs
   if a page ever needs them; nothing here does.

   WHERE THE FORMAT CAME FROM

   Copied, as CLAUDE.md's licensing note allows for infrastructure with no
   oracle role: the two header layouts are those in `stuffit-rs` 0.1.5
   (Ben Letchford, MIT OR Apache-2.0), which in turn cites The Unarchiver's
   `XADStuffItParser.m` and `XADStuffIt5Parser.m`. Checked against the six
   archives above by `utilities/vise_check.mjs`: fork lengths as listed by
   `lsar`, and the stored data fork byte-identical to the one inside
   Bryce Schroeder's Cythera.bin.

   LOAD ORDER: after mac-bytes.js (u16be, u32be, fourcc, decodeMacRoman).
   A classic script, like the rest of js/: no import, no export, globals. */

const SIT5_SIGNATURE = 'StuffIt (c)199';        // the 80-byte banner starts so
const SIT_METHOD_NAMES = {
  0: 'stored', 1: 'RLE', 2: 'LZW', 3: 'Huffman', 5: 'LZAH', 6: 'fixed Huffman',
  8: 'MW', 13: 'LZ+Huffman', 14: 'Installer', 15: 'Arsenic'
};

function looksLikeStuffIt(bytes) {
  if (!bytes || bytes.length < 96) return false;
  if (fourcc(bytes, 0) === 'SIT!' && fourcc(bytes, 10) === 'rLau') return true;
  return latin1(bytes.subarray(0, SIT5_SIGNATURE.length)) === SIT5_SIGNATURE && bytes[82] === 5;
}

/* The catalog, with nothing decompressed. Returns
 *
 *   { format, entries: [{ path, name, isFolder, type, creator, finderFlags,
 *                         dataLen, dataPackedLen, dataMethod, dataOffset,
 *                         rsrcLen, rsrcPackedLen, rsrcMethod, rsrcOffset }] }
 *
 * where dataOffset / rsrcOffset are where each fork's bytes sit in the
 * archive, stored or not. */
function parseStuffItArchive(bytes) {
  if (!looksLikeStuffIt(bytes)) throw new Error('not a StuffIt archive');
  return fourcc(bytes, 0) === 'SIT!' ? parseStuffItClassic(bytes) : parseStuffIt5(bytes);
}

/* ---- SIT! (StuffIt 1.5.1 through 4) ------------------------------------
 * A 22-byte archive header, then 112-byte entry headers each followed by
 * that entry's resource fork bytes and then its data fork bytes. Folders
 * are entries whose method byte is 0x20 (start) or 0x21 (end). */
function parseStuffItClassic(bytes) {
  const total = Math.min(u32be(bytes, 6), bytes.length);
  const entries = [];
  const path = [];
  let at = 22;
  while (at + 112 <= total) {
    const h = bytes.subarray(at, at + 112);
    const rsrcMethodByte = h[0], dataMethodByte = h[1];
    const nameLen = Math.min(h[2], 31);
    const name = decodeMacRoman(h.subarray(3, 3 + nameLen));
    at += 112;
    if (dataMethodByte === 0x20 || rsrcMethodByte === 0x20) { path.push(name); continue; }
    if (dataMethodByte === 0x21 || rsrcMethodByte === 0x21) { path.pop(); continue; }
    const rsrcLen = u32be(h, 84), dataLen = u32be(h, 88);
    const rsrcPackedLen = u32be(h, 92), dataPackedLen = u32be(h, 96);
    entries.push({
      path: path.concat([name]).join('/'), name, isFolder: false,
      type: fourcc(h, 66), creator: fourcc(h, 70), finderFlags: u16be(h, 74),
      rsrcLen, rsrcPackedLen, rsrcMethod: rsrcMethodByte & 0x0F, rsrcOffset: at,
      dataLen, dataPackedLen, dataMethod: dataMethodByte & 0x0F, dataOffset: at + rsrcPackedLen,
      encrypted: !!((dataMethodByte | rsrcMethodByte) & 0x10)
    });
    at += rsrcPackedLen + dataPackedLen;
  }
  return { format: 'StuffIt (classic)', entries };
}

/* ---- StuffIt 5 -----------------------------------------------------------
 * An 80-byte banner, a small header, and a linked list of entries whose
 * offsets are XORed with 0xA5A5A5A5 unless the archive says otherwise. The
 * entry header is 48 bytes plus the name, an optional comment, then a
 * metadata block carrying the Finder identity and, when bit 0 of its first
 * word is set, the resource fork's lengths. The forks follow: resource fork
 * first, then data fork. */
function parseStuffIt5(bytes) {
  const flags = bytes[83];
  const plain = !!(flags & 0x10);
  const off = v => (plain ? v : (v ^ 0xA5A5A5A5)) >>> 0;
  let at = off(u32be(bytes, 88));
  let remaining = u16be(bytes, 92);
  const entries = [];
  const dirs = new Map();
  /* Where to carry on after each folder ends. A folder's header gives the
     offset of its first child, and the walk jumps there -- so without
     somewhere to put the folder's OWN next-entry pointer, the walk descends
     into the first folder it meets and never comes back up. That is not
     hypothetical: `Cythera Installed Folder with Preferences & License.sit`
     listed 12 entries of 51, everything in it stopping at the end of the
     first folder, and the six single-file archives this page was written
     against have no nested folders at all so nothing noticed for a year.
     One stack entry per open folder, popped by the folder's end marker. */
  const resume = [];
  while (remaining > 0 && at + 48 <= bytes.length) {
    const start = at;
    if (u32be(bytes, at) !== 0xA5A5A5A5) throw new Error('StuffIt 5 entry marker missing at 0x' + at.toString(16));
    const version = bytes[at + 4];
    const headerSize = u16be(bytes, at + 6);
    const eflags = bytes[at + 9];
    const nextOff = u32be(bytes, at + 22);
    const dirOff = off(u32be(bytes, at + 26));
    const nameLen = u16be(bytes, at + 30);
    const dataLen = u32be(bytes, at + 34), dataPackedLen = u32be(bytes, at + 38);
    const isFolder = !!(eflags & 0x40);
    const dataMethod = isFolder ? 0 : bytes[at + 46];
    const childCount = isFolder ? u16be(bytes, at + 46) : 0;
    at += 48;
    const name = decodeMacRoman(bytes.subarray(at, at + nameLen));
    at += nameLen;
    if (isFolder && nameLen === 0) {            // end-of-folder marker
      // The stack first, the marker's own pointer as a fallback, and never a
      // jump to zero: a folder with nothing after it at its level has a null
      // next-entry pointer, and following it would land on the header at
      // offset 0 and throw on the marker check.
      const back = resume.length ? resume.pop() : 0;
      const to = back || nextOff;
      if (to) { at = to; continue; }
      break;
    }
    if (at < start + headerSize) {              // a comment
      const commentLen = u16be(bytes, at);
      at += 4 + commentLen;
    }
    const meta = u16be(bytes, at);
    const type = fourcc(bytes, at + 4), creator = fourcc(bytes, at + 8);
    const finderFlags = u16be(bytes, at + 12);
    at += 14 + (version === 1 ? 22 : 18);
    let rsrcLen = 0, rsrcPackedLen = 0, rsrcMethod = 0;
    const hasRsrc = !isFolder && !!(meta & 1);
    if (hasRsrc) {
      rsrcLen = u32be(bytes, at); rsrcPackedLen = u32be(bytes, at + 4);
      rsrcMethod = bytes[at + 12];
      const passLen = bytes[at + 13];
      at += 14;
      if ((eflags & 0x20) && passLen) at += passLen;
    }
    const parent = dirs.get(dirOff) || '';
    const path = parent ? parent + '/' + name : name;
    if (isFolder) {
      dirs.set(start, path);
      entries.push({ path, name, isFolder: true, type, creator, finderFlags,
                     dataLen: 0, dataPackedLen: 0, dataMethod: 0, dataOffset: 0,
                     rsrcLen: 0, rsrcPackedLen: 0, rsrcMethod: 0, rsrcOffset: 0 });
      remaining += childCount;
      if (dataLen && dataLen !== 0xFFFFFFFF) { resume.push(nextOff); at = dataLen; }   // first child
    } else {
      entries.push({ path, name, isFolder: false, type, creator, finderFlags,
                     rsrcLen, rsrcPackedLen, rsrcMethod, rsrcOffset: at,
                     dataLen, dataPackedLen, dataMethod, dataOffset: at + rsrcPackedLen,
                     encrypted: !!(eflags & 0x20) });
      at += rsrcPackedLen + dataPackedLen;
    }
    remaining--;
  }
  return { format: 'StuffIt 5', entries };
}

/* One fork of one entry, if it was stored. `which` is 'data' or 'rsrc'.
 * Throws, naming the method, for a fork that would need decompressing. */
function stuffItStoredFork(bytes, entry, which) {
  const len = which === 'data' ? entry.dataLen : entry.rsrcLen;
  if (!len) return new Uint8Array(0);
  const method = which === 'data' ? entry.dataMethod : entry.rsrcMethod;
  const packedLen = which === 'data' ? entry.dataPackedLen : entry.rsrcPackedLen;
  const offset = which === 'data' ? entry.dataOffset : entry.rsrcOffset;
  if (method !== 0 || packedLen !== len)
    throw new Error('"' + entry.name + '" ' + which + ' fork is compressed with StuffIt method ' + method +
                    ' (' + (SIT_METHOD_NAMES[method] || 'unknown') + '), which this page does not decompress');
  if (entry.encrypted) throw new Error('"' + entry.name + '" is encrypted');
  if (offset + len > bytes.length) throw new Error('"' + entry.name + '" runs past the end of the archive');
  return bytes.subarray(offset, offset + len);
}
