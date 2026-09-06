// Drive the wall-clock path the page uses: cw_run with a tick deadline and an
// audio budget, ~3 simulated seconds, then click Open Game's neighbourhood and
// press a key, to prove none of it panics.
import { readFileSync } from 'node:fs';
const wasmBytes = readFileSync(new URL('./cythera_web.wasm', import.meta.url));
let mem; const dec = new TextDecoder();
const env = { cw_log: (p, n) => console.log('[wasm]', dec.decode(new Uint8Array(mem.buffer, p, n))) };
const { instance } = await WebAssembly.instantiate(wasmBytes, { env });
const w = instance.exports; mem = w.memory; w.cw_init();
const game = readFileSync(process.argv[2]);
const p = w.cw_alloc(game.length); new Uint8Array(mem.buffer, p, game.length).set(game);
if (w.cw_boot(p, game.length, Math.floor(Date.now()/1000) + 2082844800, 0, 0) !== 0) process.exit(1);
const VBL = 60.15, RATE = 22050;
let tick0 = w.cw_tick(), audioTotal = 0, steps = 0;
const t0 = performance.now();
for (let f = 0; f < 180; f++) {               // 180 frames at 60 Hz
  const deadline = tick0 + Math.floor((f + 1) / 60 * VBL);
  const samples = Math.floor(RATE / 60);
  let s = 0;
  while (w.cw_tick() < deadline && s < 2_000_000) { s += w.cw_run(2_000_000 - s, deadline, s ? 0 : samples); if (!w.cw_running()) break; }
  if (!s) w.cw_run(0, deadline, samples);
  steps += s; w.cw_audio_drain(); audioTotal += w.cw_audio_len();
  w.cw_render();
  if (f === 60) { w.cw_mouse_move(300, 400); w.cw_mouse_down(300, 400); }
  if (f === 70) w.cw_mouse_up(300, 400);
  if (f === 120) w.cw_key_down(0x35, 27);
  if (f === 125) w.cw_key_up(0x35, 27);
}
console.log(`180 frames: ${steps} instructions, ${((performance.now()-t0)/1000).toFixed(2)} s host, tick ${w.cw_tick()} (deadline ${tick0 + Math.floor(180/60*VBL)}), ${audioTotal} audio samples (expect ~${Math.floor(RATE/60)*180}), running=${w.cw_running()}, menubar=${w.cw_menu_bar_visible()}`);
