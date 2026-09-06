// Boot to the start screen, print the guest's menus as the page sees them,
// choose one enabled File item through the runner's own MenuSelect path, and
// prove the guest keeps running. Usage: node menus_smoke.mjs <archive>
import { readFileSync } from 'node:fs';
const wasmBytes = readFileSync(new URL('./cythera_web.wasm', import.meta.url));
let mem; const dec = new TextDecoder();
const env = { cw_log: (p, n) => console.log('[wasm]', dec.decode(new Uint8Array(mem.buffer, p, n))) };
const { instance } = await WebAssembly.instantiate(wasmBytes, { env });
const w = instance.exports; mem = w.memory; w.cw_init();
const game = readFileSync(process.argv[2]);
const p = w.cw_alloc(game.length); new Uint8Array(mem.buffer, p, game.length).set(game);
if (w.cw_boot(p, game.length, Math.floor(Date.now()/1000) + 2082844800, 0, 0) !== 0) process.exit(1);
let done = 0; while (done < 100_000_000) done += w.cw_run_headless(2_000_000);
const menus = JSON.parse(dec.decode(new Uint8Array(mem.buffer, w.cw_menus(), w.cw_menus_len())));
for (const m of menus) console.log(`${m.inBar ? 'bar ' : 'sub '}${m.id} "${m.title}"${m.enabled ? '' : ' (disabled)'}: ${m.items.map(i => i.separator ? '—' : `${i.text}${i.key ? '⌘' + i.key : ''}${i.enabled ? '' : '(off)'}`).join(' | ')}`);
const file = menus.find(m => m.title === 'File');
const item = file && file.items.find(i => i.enabled && !i.separator && /Prefer/.test(i.text)) || file && file.items.find(i => i.enabled && !i.separator);
console.log('selecting', file && file.id, item && item.text, '->', w.cw_menu_select(file.id, item.n));
done = 0; while (done < 20_000_000) done += w.cw_run_headless(2_000_000);
console.log('running after selection:', w.cw_running(), 'tick', w.cw_tick());
