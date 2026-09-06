// Time the module through a save load on the catch-up path, one call at a
// time, so a call that blocks for seconds is seen here and not on a phone.
// Boots at the phone's portrait size, imports a desktop-store save folder,
// settles six paced seconds, clicks Onward through the headless path (a
// click delivered on the paced path does not register under Node — the
// phone's does; the difference is not understood), then makes capped
// catch-up calls and prints their durations, the guest clock they skipped
// and the audio they left. Usage: node load_timing.mjs <archive> <save-dir>
import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';
const [,, archivePath, saveDir] = process.argv;
const wasmBytes = readFileSync(new URL('./cythera_web.wasm', import.meta.url));
let mem; const dec = new TextDecoder(), enc = new TextEncoder();
const env = { cw_log: (p, n) => {} };
const { instance } = await WebAssembly.instantiate(wasmBytes, { env });
const w = instance.exports; mem = w.memory; w.cw_init();
const alloc = u8 => { const p = w.cw_alloc(u8.length); new Uint8Array(mem.buffer, p, u8.length).set(u8); return p; };
const game = readFileSync(archivePath);
if (w.cw_load(alloc(game), game.length, Math.floor(Date.now()/1000) + 2082844800, 640, 1018) !== 0) process.exit(1);
function importTree(dir) { if (existsSync(dir + '/metadata.json')) { const meta = JSON.parse(readFileSync(dir + '/metadata.json', 'utf8')); const data = existsSync(dir + '/data.fork') ? readFileSync(dir + '/data.fork') : Buffer.alloc(0); const rsrc = existsSync(dir + '/resource.fork') ? readFileSync(dir + '/resource.fork') : Buffer.alloc(0); const path = enc.encode(meta.path); w.cw_import(alloc(path), path.length, meta.file_type, meta.creator, meta.finder_flags, meta.created_date, meta.modified_date, data.length ? alloc(data) : 0, data.length, rsrc.length ? alloc(rsrc) : 0, rsrc.length); return; } for (const e of readdirSync(dir)) if (statSync(dir + '/' + e).isDirectory()) importTree(dir + '/' + e); }
importTree(saveDir); w.cw_start();
let done = 0; while (done < 80_000_000) done += w.cw_run_headless(2_000_000);
w.cw_audio_drain(); w.cw_audio_drain();
// settle 6 s of paced time like the page, then click headlessly
const VBL = 60.15; let base = w.cw_tick();
for (let i = 1; i <= 360; i++) { const dl = base + Math.floor(i / 60 * VBL); let s = 0; while (w.cw_tick() < dl && s < 2_000_000) { const r = w.cw_run(2_000_000 - s, dl, 0); if (!r) break; s += r; } w.cw_mix_audio(367); w.cw_audio_drain(); }
w.cw_mouse_move(387, 240); w.cw_run_headless(2_000_000); w.cw_mouse_down(387, 240); for (let k = 0; k < 4; k++) w.cw_run_headless(2_000_000); w.cw_mouse_up(387, 240);
console.log('click delivered at tick', w.cw_tick(), 'memory', mem.buffer.byteLength >> 20, 'MB');
let tick0 = w.cw_tick(), total = 0; const T = performance.now();
for (let k = 0; k < 200; k++) {
  const t = performance.now(), before = w.cw_tick(); const r = w.cw_run_catchup(1_000_000, w.cw_tick() + 120); const ms = performance.now() - t;
  w.cw_audio_drain(); const n = w.cw_audio_len(); total += n;
  if (ms > 80 || k % 50 === 0) console.log(`call ${k}: ${ms.toFixed(0)} ms, ${r} steps, ticks ${before}->${w.cw_tick()} (+${w.cw_tick() - before}), audio ${n} samples, memory ${mem.buffer.byteLength >> 20} MB`);
}
console.log(`80 calls in ${((performance.now() - T)/1000).toFixed(1)} s, ticks ${tick0}->${w.cw_tick()}, audio total ${total} samples (${(total/22050/60).toFixed(1)} min)`);
console.log('sound path:', dec.decode(new Uint8Array(mem.buffer, w.cw_audio_debug(), w.cw_audio_debug_len())));
