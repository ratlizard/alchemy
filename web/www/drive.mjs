// Drive the game on the wall-clock path from a list of actions and write
// frames, so a dialog can be reached and looked at without a browser.
// Usage: node drive.mjs <archive> <save-dir|-> <out-dir> action...
//   wait S | click V H | move V H | key MAC CH | menu TITLE ITEMREGEX | frame NAME
// The save dir is a desktop-store folder holding data.fork, resource.fork
// and metadata.json; it is imported before the game starts.
import { readFileSync, writeFileSync } from 'node:fs';
const [,, archivePath, saveDir, outDir, ...actions] = process.argv;
const wasmBytes = readFileSync(new URL('./cythera_web.wasm', import.meta.url));
let mem; const dec = new TextDecoder(), enc = new TextEncoder();
const env = { cw_log: (p, n) => console.log('[wasm]', dec.decode(new Uint8Array(mem.buffer, p, n))) };
const { instance } = await WebAssembly.instantiate(wasmBytes, { env });
const w = instance.exports; mem = w.memory; w.cw_init();
const alloc = u8 => { const p = w.cw_alloc(u8.length); new Uint8Array(mem.buffer, p, u8.length).set(u8); return p; };
const game = readFileSync(archivePath);
if (w.cw_load(alloc(game), game.length, Math.floor(Date.now()/1000) + 2082844800, 800, 600) !== 0) process.exit(1);
// Import every desktop-store folder under the save dir (each holds
// metadata.json and the two forks), so the disk looks as it did to the game.
import { readdirSync, statSync, existsSync } from 'node:fs';
function importTree(dir) {
  if (existsSync(dir + '/metadata.json')) {
    const meta = JSON.parse(readFileSync(dir + '/metadata.json', 'utf8'));
    const data = existsSync(dir + '/data.fork') ? readFileSync(dir + '/data.fork') : Buffer.alloc(0);
    const rsrc = existsSync(dir + '/resource.fork') ? readFileSync(dir + '/resource.fork') : Buffer.alloc(0);
    const path = enc.encode(meta.path);
    console.log('import', meta.path, data.length, rsrc.length, w.cw_import(alloc(path), path.length, meta.file_type, meta.creator, meta.finder_flags, meta.created_date, meta.modified_date, data.length ? alloc(data) : 0, data.length, rsrc.length ? alloc(rsrc) : 0, rsrc.length));
    return;
  }
  for (const e of readdirSync(dir)) if (statSync(dir + '/' + e).isDirectory()) importTree(dir + '/' + e);
}
if (saveDir !== '-') importTree(saveDir);
w.cw_start();
let done = 0; while (done < 80_000_000) done += w.cw_run_headless(2_000_000);
const VBL = 60.15;
function wait(secs) {
  const base = w.cw_tick(); let idle = 0;
  for (let i = 1; i <= Math.round(secs * 60); i++) {
    const dl = base + Math.floor(i / 60 * VBL); let s = 0;
    while (w.cw_tick() < dl && s < 2_000_000) { const r = w.cw_run(2_000_000 - s, dl, 0); if (!w.cw_running()) return; if (!r) break; s += r; }
    w.cw_mix_audio(367); w.cw_audio_drain();
    if (!s) { if (++idle > 600) break; } else idle = 0;
  }
}
function frame(name) {
  const W = w.cw_width(), H = w.cw_height(); const px = new Uint8Array(mem.buffer, w.cw_render(), W*H*4);
  const o = Buffer.alloc(W*H*3); for (let i = 0, j = 0; i < W*H; i++, j += 3) { o[j]=px[i*4]; o[j+1]=px[i*4+1]; o[j+2]=px[i*4+2]; }
  writeFileSync(`${outDir}/${name}.ppm`, Buffer.concat([Buffer.from(`P6\n${W} ${H}\n255\n`), o]));
  console.log(`frame ${name} at tick ${w.cw_tick()}`);
}
for (let i = 0; i < actions.length;) {
  const a = actions[i++];
  if (a === 'wait') wait(Number(actions[i++]));
  else if (a === 'move') { const v = +actions[i++], h = +actions[i++]; w.cw_mouse_move(v, h); }
  else if (a === 'click') { const v = +actions[i++], h = +actions[i++]; w.cw_mouse_move(v, h); wait(0.15); w.cw_mouse_down(v, h); wait(0.3); w.cw_mouse_up(v, h); wait(0.5); }
  else if (a === 'key') { const k = +actions[i++], c = +actions[i++]; w.cw_key_down(k, c); wait(0.1); w.cw_key_up(k, c); wait(0.3); }
  else if (a === 'menu') { const title = actions[i++], re = new RegExp(actions[i++]); const menus = JSON.parse(dec.decode(new Uint8Array(mem.buffer, w.cw_menus(), w.cw_menus_len()))); const m = menus.find(m => m.title === title); const it = m && m.items.find(x => re.test(x.text)); console.log('menu', title, it && it.text, '->', it ? w.cw_menu_select(m.id, it.n) : 'missing'); wait(0.5); }
  else if (a === 'down') { const v = +actions[i++], h = +actions[i++]; w.cw_mouse_move(v, h); w.cw_mouse_down(v, h); }
  else if (a === 'up') { const v = +actions[i++], h = +actions[i++]; w.cw_mouse_up(v, h); }
  // Headless steps: ticks from the instruction count, and a Standard File
  // dialog is answered by the runner (the first file of a matching type in
  // the default directory) instead of being tracked as a real dialog.
  else if (a === 'hwait') { let n = Number(actions[i++]) * 1_000_000, d = 0; while (d < n) { const r = w.cw_run_headless(Math.min(2_000_000, n - d)); if (!r || !w.cw_running()) break; d += r; } }
  else if (a === 'audio') { const secs = Number(actions[i++]); const base = w.cw_tick(); let total = 0, loud = 0; for (let k = 1; k <= secs * 60; k++) { const dl = base + Math.floor(k / 60 * VBL); let s = 0; while (w.cw_tick() < dl && s < 2_000_000) { const r = w.cw_run(2_000_000 - s, dl, 0); if (!r || !w.cw_running()) break; s += r; } w.cw_mix_audio(367); const n = w.cw_audio_len(); const buf = new Uint8Array(mem.buffer, w.cw_audio_drain(), n); total += n; for (let j = 0; j < n; j += 8) if (Math.abs(buf[j] - 128) > 4) loud++; } console.log(`audio over ${secs} s: ${total} samples, ${loud} loud checks, tick ${w.cw_tick()}`); }
  else if (a === 'debug') console.log('sound path:', dec.decode(new Uint8Array(mem.buffer, w.cw_audio_debug(), w.cw_audio_debug_len())));
  else if (a === 'frame') frame(actions[i++]);
  else { console.log('unknown action', a); break; }
}
