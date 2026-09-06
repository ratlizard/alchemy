// Exercise the save path under Node: load, import a file as the page would
// restore one, start, run to the start screen, scan for changed files, read
// them back, acknowledge, and prove a second scan offers nothing.
// Usage: node saves_smoke.mjs <archive>
import { readFileSync } from 'node:fs';
const wasmBytes = readFileSync(new URL('./cythera_web.wasm', import.meta.url));
let mem; const dec = new TextDecoder(), enc = new TextEncoder();
const env = { cw_log: (p, n) => console.log('[wasm]', dec.decode(new Uint8Array(mem.buffer, p, n))) };
const { instance } = await WebAssembly.instantiate(wasmBytes, { env });
const w = instance.exports; mem = w.memory; w.cw_init();
const alloc = u8 => { const p = w.cw_alloc(u8.length); new Uint8Array(mem.buffer, p, u8.length).set(u8); return p; };
const game = readFileSync(process.argv[2]);
if (w.cw_load(alloc(game), game.length, Math.floor(Date.now()/1000) + 2082844800, 640, 480) !== 0) process.exit(1);
// the game's folder, from Cythera Data
const n = enc.encode('Cythera Data'); const np = alloc(n); console.log('found Cythera Data:', w.cw_vfs_find(np, n.length));
const folder = dec.decode(new Uint8Array(mem.buffer, w.cw_found_ptr(), w.cw_found_len())).replace(/[^/]*$/, '');
console.log('game folder:', JSON.stringify(folder));
// import a fake character file beside it
const path = enc.encode(folder + 'Smoke Test'); const data = new Uint8Array(2176).fill(0x5a);
const fourcc = s => [...s].reduce((a, c) => (a << 8 | c.charCodeAt(0)) >>> 0, 0);
console.log('import:', w.cw_import(alloc(path), path.length, fourcc('DelP'), fourcc('Delv'), 256, 3000000000, 3000000000, alloc(data), data.length, 0, 0));
console.log('start:', w.cw_start());
let done = 0; while (done < 85_000_000) done += w.cw_run_headless(2_000_000);
const report = () => { const k = w.cw_save_scan(); const names = []; for (let i = 0; i < k; i++) names.push(dec.decode(new Uint8Array(mem.buffer, w.cw_save_path_ptr(i), w.cw_save_path_len(i))) + ` (${String.fromCharCode(...[24,16,8,0].map(s => w.cw_save_type(i) >>> s & 255))}, ${w.cw_save_data_len(i)}+${w.cw_save_rsrc_len(i)})`); return names; };
const first = report(); console.log('scan 1:', first);
w.cw_save_ack();
console.log('scan 2 after ack:', report());
const importedUnchanged = !first.some(s => s.includes('Smoke Test'));
console.log(importedUnchanged ? 'ok: the imported file was not offered back unchanged' : 'FAIL: imported file offered back');
const licence = first.some(s => /License|thaumaturgy/.test(s));
console.log(licence ? 'FAIL: licence or log offered' : 'ok: licence and log excluded');
