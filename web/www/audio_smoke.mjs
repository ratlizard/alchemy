// Does the module produce sound at the start screen? Run the wall-clock path
// to tick ~1500 (the theme queues at ~1120) and count drained samples that
// are not silence (0x80). Usage: node audio_smoke.mjs <archive>
import { readFileSync } from 'node:fs';
const wasmBytes = readFileSync(new URL('./cythera_web.wasm', import.meta.url));
let mem; const dec = new TextDecoder();
const env = { cw_log: (p, n) => console.log('[wasm]', dec.decode(new Uint8Array(mem.buffer, p, n))) };
const { instance } = await WebAssembly.instantiate(wasmBytes, { env });
const w = instance.exports; mem = w.memory; w.cw_init();
const game = readFileSync(process.argv[2]);
const p = w.cw_alloc(game.length); new Uint8Array(mem.buffer, p, game.length).set(game);
if (w.cw_boot(p, game.length, Math.floor(Date.now()/1000) + 2082844800, 0, 0) !== 0) process.exit(1);
let done = 0; while (done < 80_000_000) done += w.cw_run_headless(2_000_000);
w.cw_audio_drain();
const VBL = 60.15, RATE = 22050; const tick0 = w.cw_tick(); let total = 0, loud = 0, firstLoudTick = 0;
for (let f = 0; f < 12 * 60; f++) {
  const deadline = tick0 + Math.floor((f + 1) / 60 * VBL), samples = Math.floor(RATE / 60);
  let s = 0; while (w.cw_tick() < deadline && s < 2_000_000) { s += w.cw_run(2_000_000 - s, deadline, s ? 0 : samples); if (!w.cw_running()) break; }
  if (!s) w.cw_run(0, deadline, samples);
  const n = w.cw_audio_len(); const a = new Uint8Array(mem.buffer, w.cw_audio_drain(), n); total += n;
  for (let i = 0; i < n; i++) if (Math.abs(a[i] - 128) > 4) { loud++; if (!firstLoudTick) firstLoudTick = w.cw_tick(); }
}
console.log(`ticks ${tick0}..${w.cw_tick()}: ${total} samples drained, ${loud} non-silent, first at tick ${firstLoudTick}`);
