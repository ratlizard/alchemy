// Run the wall-clock path (cw_run with a tick deadline) for N simulated
// seconds at a given screen size and write the frame, so what the page would
// show can be looked at without a browser. Usage: node realtime_frame.mjs <archive> <WxH> <seconds> <out.ppm>
import { readFileSync, writeFileSync } from 'node:fs';
const [,, archivePath, size, secsArg, out] = process.argv;
const [SW, SH] = size.split('x').map(Number), SECS = Number(secsArg);
const wasmBytes = readFileSync(new URL('./cythera_web.wasm', import.meta.url));
let mem; const dec = new TextDecoder();
const env = { cw_log: (p, n) => console.log('[wasm]', dec.decode(new Uint8Array(mem.buffer, p, n))) };
const { instance } = await WebAssembly.instantiate(wasmBytes, { env });
const w = instance.exports; mem = w.memory; w.cw_init();
const game = readFileSync(archivePath);
const p = w.cw_alloc(game.length); new Uint8Array(mem.buffer, p, game.length).set(game);
if (w.cw_boot(p, game.length, Math.floor(Date.now()/1000) + 2082844800, SW, SH) !== 0) process.exit(1);
const W = w.cw_width(), H = w.cw_height();
const VBL = 60.15, RATE = 22050; const tick0 = w.cw_tick(); let steps = 0; const t0 = performance.now();
for (let f = 0; f < SECS * 60; f++) {
  const deadline = tick0 + Math.floor((f + 1) / 60 * VBL), samples = Math.floor(RATE / 60);
  let s = 0; while (w.cw_tick() < deadline && s < 2_000_000) { s += w.cw_run(2_000_000 - s, deadline, s ? 0 : samples); if (!w.cw_running()) break; }
  if (!s) w.cw_run(0, deadline, samples);
  steps += s; w.cw_audio_drain();
}
const px = new Uint8Array(mem.buffer, w.cw_render(), W * H * 4);
let nonblack = 0; for (let i = 0; i < W * H; i++) if (px[i*4] | px[i*4+1] | px[i*4+2]) nonblack++;
console.log(`${W}x${H}: ${SECS} s simulated in ${((performance.now()-t0)/1000).toFixed(1)} s host, ${steps} instructions, tick ${w.cw_tick()}, running=${w.cw_running()}, nonblack pixels ${nonblack}`);
if (out) { const o = Buffer.alloc(W*H*3); for (let i = 0, j = 0; i < W*H; i++, j += 3) { o[j]=px[i*4]; o[j+1]=px[i*4+1]; o[j+2]=px[i*4+2]; } writeFileSync(out, Buffer.concat([Buffer.from(`P6\n${W} ${H}\n255\n`), o])); }
