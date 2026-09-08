/* mac-bytes.js — reading classic Mac bytes.
 *
 * Shared by cythera_data_viewer.html and resource_fork_browser.html. Both had
 * their own copy of every function here, and the Mac Roman table existed three
 * times in two files.
 *
 * LOAD ORDER: this file first. mac-containers.js, mac-media.js and
 * mac-export.js all use what is declared here.
 *
 * These are CLASSIC scripts, deliberately -- no `type="module"`, no `import`,
 * no `export`. A module script is fetched with CORS, and a page opened from
 * `file://` has an opaque origin, so a module would fail to load off a USB
 * stick or a download folder. A classic <script src> does not. Every page here
 * has to keep working when it is double-clicked rather than served, so:
 *
 *   - declare things at top level and let them be globals,
 *   - never add `type="module"` to a <script> tag in these pages,
 *   - keep the load order in the HTML matching the dependency order above.
 *
 * utilities/verify_viewer.mjs fails the build if a module script appears.
 */

/* ---- big-endian readers ------------------------------------------------ */
/* Everything in a resource fork, a BinHex header and a Delver archive is
 * big-endian, which is the opposite of what DataView defaults to, so these
 * read the bytes by hand rather than inviting a `true` in the wrong place. */
function u8(b, i) { return b[i]; }
function i8(b, i) { const v = b[i]; return v > 127 ? v - 256 : v; }
function u16be(b, i) { return (b[i] << 8) | b[i + 1]; }
function i16be(b, i) { const v = u16be(b, i); return v > 32767 ? v - 65536 : v; }
/* The high byte is multiplied rather than shifted: `b[i] << 24` is a signed
 * 32-bit operation in JavaScript, so a length of 0x80000000 or more comes back
 * negative and every bounds check that follows silently passes. */
function u32be(b, i) { return ((b[i] * 0x1000000) + (b[i + 1] << 16) + (b[i + 2] << 8) + b[i + 3]) >>> 0; }

/* A four-character type or creator code, with unprintable bytes shown as '?'
 * rather than dropped -- 'DelS' and '????' should not look alike. */
function fourcc(b, p) {
  let s = '';
  for (let i = 0; i < 4; i++) {
    const c = b[p + i];
    s += (c >= 32 && c < 127) ? String.fromCharCode(c) : '?';
  }
  return s;
}

function latin1(bytes) { return new TextDecoder('latin1').decode(bytes); }

/* ---- Mac Roman --------------------------------------------------------- */
/* Code points for bytes 0x80-0xFF. 0xF8FF is the Apple logo, which is in the
 * private use area and will render as a box on most systems -- that is
 * correct, there is nowhere else for it to go. */
const MACROMAN_HIGH = [
  0xC4,0xC5,0xC7,0xC9,0xD1,0xD6,0xDC,0xE1,0xE0,0xE2,0xE4,0xE3,0xE5,0xE7,0xE9,0xE8,
  0xEA,0xEB,0xED,0xEC,0xEE,0xEF,0xF1,0xF3,0xF2,0xF4,0xF6,0xF5,0xFA,0xF9,0xFB,0xFC,
  0x2020,0xB0,0xA2,0xA3,0xA7,0x2022,0xB6,0xDF,0xAE,0xA9,0x2122,0xB4,0xA8,0x2260,0xC6,0xD8,
  0x221E,0xB1,0x2264,0x2265,0xA5,0xB5,0x2202,0x2211,0x220F,0x3C0,0x222B,0xAA,0xBA,0x3A9,0xE6,0xF8,
  0xBF,0xA1,0xAC,0x221A,0x192,0x2248,0x2206,0xAB,0xBB,0x2026,0xA0,0xC0,0xC3,0xD5,0x152,0x153,
  0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0xF7,0x25CA,0xFF,0x178,0x2044,0x20AC,0x2039,0x203A,0xFB01,0xFB02,
  0x2021,0xB7,0x201A,0x201E,0x2030,0xC2,0xCA,0xC1,0xCB,0xC8,0xCD,0xCE,0xCF,0xCC,0xD3,0xD4,
  0xF8FF,0xD2,0xDA,0xDB,0xD9,0x131,0x2C6,0x2DC,0xAF,0x2D8,0x2D9,0x2DA,0xB8,0x2DD,0x2DB,0x2C7
];

function decodeMacRoman(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i++) {
    const b = bytes[i];
    s += String.fromCodePoint(b < 0x80 ? b : MACROMAN_HIGH[b - 0x80]);
  }
  return s;
}

/* A Pascal string: one length byte, then that many characters. */
function pstring(bytes, off) {
  const p = off || 0;
  const len = bytes[p];
  return decodeMacRoman(bytes.subarray(p + 1, p + 1 + len));
}

/* The exact inverse of decodeMacRoman, for writing. The reverse map is built
 * from MACROMAN_HIGH itself rather than transcribed, so the two cannot drift:
 * any string made of characters Mac Roman can express round-trips byte for
 * byte. A character outside the repertoire becomes '?' -- the archive writer
 * that needs this encodes titles read FROM Mac Roman bytes in the first
 * place, so hitting that case means the caller has a bug, not the table. */
let MACROMAN_REVERSE = null;
function encodeMacRoman(s) {
  if (!MACROMAN_REVERSE) {
    MACROMAN_REVERSE = new Map();
    for (let i = 0; i < 128; i++) MACROMAN_REVERSE.set(MACROMAN_HIGH[i], 0x80 + i);
  }
  const out = new Uint8Array(s.length);
  let n = 0;
  for (const ch of s) {
    const cp = ch.codePointAt(0);
    out[n++] = cp < 0x80 ? cp : (MACROMAN_REVERSE.get(cp) ?? 0x3F);
  }
  return out.subarray(0, n);
}

/* ---- CRC-32 ------------------------------------------------------------ */
/* One table, used by both the ZIP writer and the PNG writer -- they want the
 * same polynomial, and each file used to carry its own copy of it. */
const CRC32_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(bytes) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) c = CRC32_TABLE[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

/* ---- Odds and ends ----------------------------------------------------- */
function fmtBytes(n) {
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(0) + ' KB';
  return (n / 1024 / 1024).toFixed(1) + ' MB';
}

/* Safe for a file name on every platform, and short enough to survive a zip
 * viewer. Control bytes are stripped as well as the reserved punctuation:
 * a Mac resource name really can contain a newline. */
function safeFileName(s) {
  return String(s)
    .replace(/[\\/:*?"<>|\x00-\x1f]/g, '_')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, 72);
}
