// Open the Preferences dialog through the menu on the wall-clock path and
// write the frame. Usage: node prefs_frame.mjs <archive> <out.ppm> [WxH]
import { readFileSync, writeFileSync } from 'node:fs';
const [,, archivePath, out, size] = process.argv; const [SW, SH] = (size || '800x600').split('x').map(Number);
const wasmBytes = readFileSync(new URL('./cythera_web.wasm', import.meta.url));
let mem; const dec = new TextDecoder();
const env = { cw_log: (p, n) => console.log('[wasm]', dec.decode(new Uint8Array(mem.buffer, p, n))) };
const { instance } = await WebAssembly.instantiate(wasmBytes, { env });
const w = instance.exports; mem = w.memory; w.cw_init();
const game = readFileSync(archivePath); const p = w.cw_alloc(game.length); new Uint8Array(mem.buffer, p, game.length).set(game);
if (w.cw_boot(p, game.length, Math.floor(Date.now()/1000) + 2082844800, SW, SH) !== 0) process.exit(1);
let done = 0; while (done < 80_000_000) done += w.cw_run_headless(2_000_000);
const VBL = 60.15; let tick0 = w.cw_tick(), f = 0;
const runSeconds = secs => { const base = w.cw_tick(); for (let i = 1; i <= secs * 60; i++) { const dl = base + Math.floor(i / 60 * VBL); let s = 0; while (w.cw_tick() < dl && s < 2_000_000) { s += w.cw_run(2_000_000 - s, dl, 0); if (!w.cw_running()) return; } w.cw_mix_audio(367); w.cw_audio_drain(); } };
runSeconds(Number(process.argv[6] || 8));
// Click the Preferences plaque (800x600: about v=178 h=585), held a third of a second.
const [pv, ph] = (process.argv[5] || '178,585').split(',').map(Number);
w.cw_mouse_move(pv, ph); runSeconds(0.2); w.cw_mouse_down(pv, ph); runSeconds(0.35); w.cw_mouse_up(pv, ph);
runSeconds(4);
const W = w.cw_width(), H = w.cw_height(); const px = new Uint8Array(mem.buffer, w.cw_render(), W*H*4);
const o = Buffer.alloc(W*H*3); for (let i = 0, j = 0; i < W*H; i++, j += 3) { o[j]=px[i*4]; o[j+1]=px[i*4+1]; o[j+2]=px[i*4+2]; }
writeFileSync(out, Buffer.concat([Buffer.from(`P6\n${W} ${H}\n255\n`), o])); console.log('frame at tick', w.cw_tick(), 'running', w.cw_running());
export {};
