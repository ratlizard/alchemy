// Node runner for the same module the page loads: boots the archive and runs
// the guest headless for N instructions, reporting the executor's rate under
// V8. Usage: node bench.mjs <archive> [instructions] [frame.ppm]
import { readFileSync, writeFileSync } from 'node:fs';
const [,, archivePath, insArg, framePath] = process.argv;
const target = Number(insArg || 85_000_000);
const wasmBytes = readFileSync(new URL('./cythera_web.wasm', import.meta.url));
let mem;
const dec = new TextDecoder();
const env = { cw_log: (p, n) => console.log('[wasm]', dec.decode(new Uint8Array(mem.buffer, p, n))) };
const t0 = performance.now();
const { instance } = await WebAssembly.instantiate(wasmBytes, { env });
const w = instance.exports; mem = w.memory;
w.cw_init();
console.log(`instantiated in ${(performance.now() - t0).toFixed(0)} ms, module ${wasmBytes.length} bytes`);
const game = readFileSync(archivePath);
const p = w.cw_alloc(game.length);
new Uint8Array(mem.buffer, p, game.length).set(game);
const t1 = performance.now();
const rc = w.cw_boot(p, game.length, Math.floor(Date.now() / 1000) + 2082844800);
if (rc !== 0) { console.log('boot failed:', dec.decode(new Uint8Array(mem.buffer, w.cw_error_ptr(), w.cw_error_len()))); process.exit(1); }
const W = w.cw_width(), H = w.cw_height();
console.log(`booted in ${(performance.now() - t1).toFixed(0)} ms, screen ${W}x${H}, ${w.cw_instructions_per_tick()} instructions/tick`);
let done = 0; const t2 = performance.now(); let lastReport = t2;
while (done < target) {
  done += w.cw_run_headless(Math.min(2_000_000, target - done));
  if (!w.cw_running()) { console.log('guest halted at', done); break; }
  const now = performance.now();
  if (now - lastReport > 5000) { console.log(`  ${(done / 1e6).toFixed(0)}M, ${((now - t2) / 1000).toFixed(1)} s, ${(done / ((now - t2) / 1000) / 1e6).toFixed(2)} M/s, tick ${w.cw_tick()}`); lastReport = now; }
}
const secs = (performance.now() - t2) / 1000;
console.log(`ran ${done} instructions in ${secs.toFixed(2)} s = ${(done / secs / 1e6).toFixed(2)} M instructions/s, tick ${w.cw_tick()}`);
if (framePath) {
  const px = new Uint8Array(mem.buffer, w.cw_render(), W * H * 4);
  const out = Buffer.alloc(W * H * 3);
  for (let i = 0, j = 0; i < W * H; i++, j += 3) { out[j] = px[i * 4]; out[j + 1] = px[i * 4 + 1]; out[j + 2] = px[i * 4 + 2]; }
  writeFileSync(framePath, Buffer.concat([Buffer.from(`P6\n${W} ${H}\n255\n`), out]));
  console.log('frame written to', framePath);
}
