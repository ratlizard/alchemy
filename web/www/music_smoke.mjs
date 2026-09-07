// Does a tune library reach the game through the page's own code?
//
// Two halves, because they fail differently. The zip reader is lifted out of
// index.html and run over a zip built here with one stored entry and one
// deflated one, so a change to it is caught without a browser. Then the
// module is booted, the MIDI is installed for the tune it replaces, and the
// audio is compared with the same seconds of the game's own music: a
// substitute that installs but never reaches the mixer would otherwise look
// like a pass.
//
// Usage: node music_smoke.mjs <archive> <FB7C80EC.mid>
import { readFileSync } from 'node:fs';

const [, , archivePath, midiPath] = process.argv;
if (!archivePath || !midiPath) {
  console.error('usage: node music_smoke.mjs <archive> <XXXXXXXX.mid>');
  process.exit(2);
}
const midi = new Uint8Array(readFileSync(midiPath));
const checksumName = midiPath.split('/').pop();
const checksum = parseInt(checksumName.slice(0, 8), 16);

// --- the page's zip reader, run here rather than reimplemented
const page = readFileSync(new URL('./index.html', import.meta.url), 'utf8');
const from = page.indexOf('const MUSIC_NAME =');
const start = page.indexOf('async function inflateRaw');
const end = page.indexOf('// Hand one file to the module');
if (from < 0 || start < 0 || end < 0) { console.error('could not find the zip reader in index.html'); process.exit(1); }
const src = page.slice(from, page.indexOf('\n', from)) + '\n' + page.slice(start, end);
const dec = new TextDecoder();
let warnings = 0;
const say = m => { warnings++; console.log('  page said:', m); };
const { unzip, MUSIC_NAME } = await import(
  'data:text/javascript;base64,' +
  Buffer.from(`export const __f = (dec, say) => { ${src}; return { unzip, MUSIC_NAME }; };`).toString('base64')
).then(m => m.__f(dec, say));

// A zip with the same file twice: stored, and deflated.
async function buildZip(entries) {
  const enc = new TextEncoder(), locals = [], central = [];
  let offset = 0;
  for (const { name, bytes, deflate } of entries) {
    const nameBytes = enc.encode(name);
    let payload = bytes, method = 0;
    if (deflate) {
      const s = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate-raw'));
      payload = new Uint8Array(await new Response(s).arrayBuffer());
      method = 8;
    }
    const lh = new Uint8Array(30 + nameBytes.length);
    const lv = new DataView(lh.buffer);
    lv.setUint32(0, 0x04034b50, true); lv.setUint16(4, 20, true); lv.setUint16(8, method, true);
    lv.setUint32(18, payload.length, true); lv.setUint32(22, bytes.length, true);
    lv.setUint16(26, nameBytes.length, true);
    lh.set(nameBytes, 30);
    const ch = new Uint8Array(46 + nameBytes.length);
    const cv = new DataView(ch.buffer);
    cv.setUint32(0, 0x02014b50, true); cv.setUint16(10, method, true);
    cv.setUint32(20, payload.length, true); cv.setUint32(24, bytes.length, true);
    cv.setUint16(28, nameBytes.length, true); cv.setUint32(42, offset, true);
    ch.set(nameBytes, 46);
    locals.push(lh, payload); central.push(ch);
    offset += lh.length + payload.length;
  }
  const centralSize = central.reduce((n, c) => n + c.length, 0);
  const eocd = new Uint8Array(22), ev = new DataView(eocd.buffer);
  ev.setUint32(0, 0x06054b50, true);
  ev.setUint16(8, entries.length, true); ev.setUint16(10, entries.length, true);
  ev.setUint32(12, centralSize, true); ev.setUint32(16, offset, true);
  return Buffer.concat([...locals, ...central, eocd].map(a => Buffer.from(a)));
}

const zip = await buildZip([
  { name: 'library/' + checksumName, bytes: midi, deflate: false },
  { name: 'library/deflated/' + checksumName, bytes: midi, deflate: true },
  { name: 'library/readme.txt', bytes: new TextEncoder().encode('not a tune'), deflate: true },
]);
const entries = await unzip(new Uint8Array(zip));
if (!entries || entries.length !== 2) { console.error(`FAIL: expected 2 tune entries, got ${entries && entries.length}`); process.exit(1); }
for (const e of entries) {
  if (e.bytes.length !== midi.length || !e.bytes.every((b, i) => b === midi[i])) {
    console.error(`FAIL: ${e.name} did not come back byte for byte`); process.exit(1);
  }
}
console.log(`ok: the zip reader returned ${entries.length} tune files, stored and deflated, byte for byte, and passed over readme.txt`);

// --- the module: does an installed tune actually change what is played?
const wasmBytes = readFileSync(new URL('./cythera_web.wasm', import.meta.url));
let mem;
const env = { cw_log: (p, n) => { const m = dec.decode(new Uint8Array(mem.buffer, p, n)); if (m.startsWith('[TUNE]')) console.log(' ', m); } };
const game = readFileSync(archivePath);

async function themeAudio(install) {
  const { instance } = await WebAssembly.instantiate(wasmBytes, { env });
  const w = instance.exports; mem = w.memory; w.cw_init();
  const p = w.cw_alloc(game.length); new Uint8Array(mem.buffer, p, game.length).set(game);
  if (w.cw_boot(p, game.length, Math.floor(Date.now() / 1000) + 2082844800, 640, 480) !== 0) { console.error('FAIL: boot'); process.exit(1); }
  if (install) {
    const mp = w.cw_alloc(midi.length); new Uint8Array(mem.buffer, mp, midi.length).set(midi);
    const kind = w.cw_tune_install(checksum, mp, midi.length);
    if (kind !== 1) { console.error(`FAIL: the module read ${checksumName} as kind ${kind}, expected 1 (MIDI)`); process.exit(1); }
    if (w.cw_tune_count() !== 1) { console.error('FAIL: count'); process.exit(1); }
  }
  let done = 0; while (done < 80_000_000) done += w.cw_run_headless(2_000_000);
  w.cw_audio_drain();
  const VBL = 60.15, RATE = 22050, tick0 = w.cw_tick(), chunks = [];
  for (let f = 0; f < 20 * 60; f++) {
    const deadline = tick0 + Math.floor((f + 1) / 60 * VBL);
    let s = 0; while (w.cw_tick() < deadline && s < 2_000_000) { const r = w.cw_run(2_000_000 - s, deadline, 0); if (!r || !w.cw_running()) break; s += r; }
    w.cw_mix_audio(Math.round(RATE / 60));
    const ptr = w.cw_audio_drain(), n = w.cw_audio_len();
    if (n) chunks.push(Buffer.from(new Uint8Array(mem.buffer, ptr, n)));
  }
  return { audio: Buffer.concat(chunks), w };
}

const own = await themeAudio(false);
const swapped = await themeAudio(true);
const loud = b => { let n = 0; for (let i = 0; i < b.length; i += 8) if (Math.abs(b[i] - 128) > 4) n++; return n; };
if (!loud(swapped.audio)) { console.error('FAIL: the substituted tune played silence'); process.exit(1); }
const same = own.audio.length === swapped.audio.length && own.audio.equals(swapped.audio);
if (same) { console.error('FAIL: installing the tune changed nothing that reached the mixer'); process.exit(1); }
console.log(`ok: ${checksumName} installed as MIDI and changed the music — ${loud(own.audio)} non-silent checks of the game's own, ${loud(swapped.audio)} of the substitute`);

// --- and clearing puts the game's own music back
swapped.w.cw_tune_clear();
if (swapped.w.cw_tune_count() !== 0) { console.error('FAIL: clear left tunes installed'); process.exit(1); }
console.log('ok: cleared');
