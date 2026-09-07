// Render what the page would play to a WAV, so a change to the synthesised
// instruments, or a substitute tune, can be listened to rather than described.
// Boots the module, optionally installs one substitute, plays the start-screen
// theme on the wall-clock path for N seconds, and writes 22,050 Hz 8-bit mono
// -- the format the guest's own sound channels carry.
//
//   node render_wav.mjs <archive> <seconds> <out.wav> [XXXXXXXX.mid|.wav]
//
// The checksum comes from the substitute's file name. The game's eleven tunes
// and their checksums are listed in index.html as GAME_TUNES; the start-screen
// theme, the only one that plays without entering the game, is FB7C80EC.
import { readFileSync, writeFileSync } from 'node:fs';
const [,, archivePath, secsArg, out, midiPath] = process.argv;
const SECS = Number(secsArg);
const wasmBytes = readFileSync(new URL('./cythera_web.wasm', import.meta.url));
let mem; const dec = new TextDecoder();
const env = { cw_log: (p, n) => { const m = dec.decode(new Uint8Array(mem.buffer, p, n)); if (!m.startsWith('[TRAP]')) process.stderr.write('[wasm] ' + m + '\n'); } };
const { instance } = await WebAssembly.instantiate(wasmBytes, { env });
const w = instance.exports; mem = w.memory; w.cw_init();
const game = readFileSync(archivePath);
const p = w.cw_alloc(game.length); new Uint8Array(mem.buffer, p, game.length).set(game);
if (w.cw_boot(p, game.length, Math.floor(Date.now()/1000) + 2082844800, 640, 480) !== 0) process.exit(1);
if (midiPath) {
  const { readFileSync: rf } = await import('node:fs');
  const midi = new Uint8Array(rf(midiPath));
  const cs = parseInt(midiPath.split('/').pop().slice(0,8), 16);
  const mp = w.cw_alloc(midi.length); new Uint8Array(mem.buffer, mp, midi.length).set(midi);
  console.error('installed kind', w.cw_tune_install(cs, mp, midi.length));
}
let done = 0; while (done < 80_000_000) done += w.cw_run_headless(2_000_000);
w.cw_audio_drain();
const VBL = 60.15, RATE = 22050; const tick0 = w.cw_tick(); const chunks = [];
for (let f = 0; f < SECS * 60; f++) {
  const deadline = tick0 + Math.floor((f + 1) / 60 * VBL), samples = Math.round(RATE / 60);
  let s = 0; while (w.cw_tick() < deadline && s < 2_000_000) { const r = w.cw_run(2_000_000 - s, deadline, 0); if (!r || !w.cw_running()) break; s += r; }
  w.cw_mix_audio(samples);
  const ptr = w.cw_audio_drain(), n = w.cw_audio_len();
  if (n) chunks.push(Buffer.from(new Uint8Array(mem.buffer, ptr, n)));
}
const pcm = Buffer.concat(chunks);
const hdr = Buffer.alloc(44);
hdr.write('RIFF', 0); hdr.writeUInt32LE(36 + pcm.length, 4); hdr.write('WAVE', 8);
hdr.write('fmt ', 12); hdr.writeUInt32LE(16, 16); hdr.writeUInt16LE(1, 20); hdr.writeUInt16LE(1, 22);
hdr.writeUInt32LE(RATE, 24); hdr.writeUInt32LE(RATE, 28); hdr.writeUInt16LE(1, 32); hdr.writeUInt16LE(8, 34);
hdr.write('data', 36); hdr.writeUInt32LE(pcm.length, 40);
writeFileSync(out, Buffer.concat([hdr, pcm]));
let loud = 0; for (let i = 0; i < pcm.length; i += 16) if (Math.abs(pcm[i] - 128) > 4) loud++;
console.log(`${out}: ${pcm.length} samples (${(pcm.length/RATE).toFixed(1)} s), ${loud} of ${Math.ceil(pcm.length/16)} checks non-silent`);
