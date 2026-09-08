// The patch route, over the real files, without a browser.
//
//   node patch_smoke.mjs [path to a Cythera Data data fork] [addons dir]
//
// Three things it proves, none of which page_smoke.mjs can see:
//
// 1. THE VENDORED SCRIPTS LOAD IN THE ORDER THE PAGE LOADS THEM. delv/*.js are
//    plain scripts defining globals, and the page's tags put them in dependency
//    order. Loading them the same way here fails if that order is wrong or if a
//    fifth file from grimoire's js/ became necessary.
//
// 2. THE MODULE EXPORTS THE PATCH ROUTE NEEDS EXIST. cw_vfs_stage and its
//    accessors are called only from a browser, so a rename in lib.rs would
//    otherwise surface as a TypeError on a phone.
//
// 3. unwrapPatch REACHES THE PATCH INSIDE WHAT PEOPLE ACTUALLY DOWNLOAD. The
//    one published patch is a .hqx wrapping a .sit; the .sit cannot be
//    decompressed here, and the message has to say so and name what is inside,
//    because that is the file most people will pick first. The bare patch, and
//    the same patch inside each single-file wrapper, must all come through.
//
// The merge itself is grimoire's and is checked there (utilities/patch_check.mjs,
// which compares against the unpatched rebuild). This does not repeat that.

import { readFileSync, existsSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const here = dirname(fileURLToPath(import.meta.url));
const [dataPath = join(process.env.TMPDIR || '/tmp', 'Cythera Data.data'),
       addonDir = join(here, '../../../cythera-reference/community/addons')] = process.argv.slice(2);

let failures = 0;
const fail = (what, why) => { failures++; console.error(`FAIL ${what}: ${why}`); };
const ok = (what, detail) => console.log(`  ok   ${what}${detail ? '  — ' + detail : ''}`);

// ---- 1. the vendored scripts, loaded as the page loads them ----------------
const ORDER = ['mac-bytes.js', 'mac-containers.js', 'mac-stuffit.js', 'delv-archive.js'];
const html = readFileSync(join(here, 'index.html'), 'utf8');
const tagged = [...html.matchAll(/<script src="delv\/([^"]+)"><\/script>/g)].map(m => m[1]);
if (tagged.join(',') !== ORDER.join(','))
  fail('script tags', `the page loads ${JSON.stringify(tagged)}, this check expects ${JSON.stringify(ORDER)}`);
else ok('the page loads the four vendored files in order', tagged.join(', '));

const ctx = vm.createContext({ console, TextDecoder, TextEncoder });
for (const name of ORDER) {
  const p = join(here, 'delv', name);
  if (!existsSync(p)) { fail('vendored ' + name, 'missing'); continue; }
  try { vm.runInContext(readFileSync(p, 'utf8'), ctx, { filename: name }); }
  catch (e) { fail('loading ' + name, e.message); }
}
const has = n => vm.runInContext(`typeof ${n}`, ctx) === 'function';
for (const n of ['mergeDelverPatch', 'delverArchiveSpec', 'writeDelverArchive',
                 'looksLikeBinHex', 'binhexDecode', 'binhexSplitForks',
                 'looksLikeStuffIt', 'parseStuffItArchive', 'macBinaryForks'])
  if (!has(n)) fail('after loading delv/', `${n} is not defined`);
if (!failures) ok('they define what the page calls', '9 functions');

// ---- 2. the module exports the page calls ---------------------------------
const wasmPath = join(here, 'cythera_web.wasm');
if (!existsSync(wasmPath)) {
  console.log('  skip  cythera_web.wasm is not built, so its exports are unchecked');
} else {
  const mod = await WebAssembly.compile(readFileSync(wasmPath));
  const names = new Set(WebAssembly.Module.exports(mod).map(e => e.name));
  const want = ['cw_vfs_stage', 'cw_vfs_find', 'cw_found_ptr', 'cw_found_len', 'cw_import',
                'cw_staged_data_ptr', 'cw_staged_data_len', 'cw_staged_rsrc_ptr', 'cw_staged_rsrc_len',
                'cw_staged_type', 'cw_staged_creator', 'cw_staged_flags',
                'cw_staged_created', 'cw_staged_modified', 'cw_staged_clear'];
  const missing = want.filter(n => !names.has(n));
  if (missing.length) fail('module exports', missing.join(', ') + ' not exported');
  else ok('the module exports the patch route', `${want.length} functions`);
}

// ---- 3. unwrapPatch over the real files ------------------------------------
// Taken out of the page rather than copied, so this cannot drift from it.
const unwrapSrc = html.match(/function unwrapPatch\(fileName, u8\) \{[\s\S]*?\n\}/);
if (!unwrapSrc) fail('unwrapPatch', 'not found in index.html');
else {
  vm.runInContext(unwrapSrc[0], ctx, { filename: 'unwrapPatch' });
  const unwrap = (name, u8) => { ctx.__n = name; ctx.__b = u8; return vm.runInContext('unwrapPatch(__n, __b)', ctx); };

  const hqx = join(addonDir, '614_MagpiePumpkinPatch.sit.hqx');
  if (!existsSync(hqx)) console.log('  skip  the Pumpkin Patch is not in the add-ons');
  else {
    // The published file: BinHex around StuffIt. It must be refused, and the
    // message must name the patch inside so a person knows what to extract.
    let msg = '';
    try { unwrap('614_MagpiePumpkinPatch.sit.hqx', new Uint8Array(readFileSync(hqx))); }
    catch (e) { msg = e.message; }
    if (!msg) fail('the published .hqx', 'it was accepted, and its payload is a StuffIt archive');
    else if (!/StuffIt/.test(msg) || !/Pumpkin Patch/.test(msg))
      fail('the published .hqx', 'the message does not name StuffIt and the patch inside: ' + msg);
    else ok('the published .hqx is refused, naming what is inside', msg.slice(0, 96));
  }

  // The bare patch, if a previous run of grimoire's check left one unpacked.
  const unpacked = join(process.env.TMPDIR || '/tmp', 'cythera_patch_check', 'pumpkin', 'Patches', 'Pumpkin Patch');
  if (!existsSync(unpacked) || !existsSync(dataPath)) {
    console.log('  skip  no unpacked patch and archive to merge (run grimoire\'s patch_check.mjs first)');
  } else {
    const patch = new Uint8Array(readFileSync(unpacked));
    const bare = unwrap('Pumpkin Patch', patch);
    if (bare.bytes.length !== patch.length) fail('the bare patch', 'it was unwrapped as something');
    else ok('the bare patch passes through untouched', bare.bytes.length.toLocaleString() + ' B');

    // The same patch inside MacBinary: the wrapper the page's own export
    // writes, so a patch exported from one browser and given to another works.
    const mb = new Uint8Array(128 + ((patch.length + 127) & ~127));
    const name = 'Pumpkin Patch';
    mb[1] = name.length;
    for (let i = 0; i < name.length; i++) mb[2 + i] = name.charCodeAt(i);
    const dv = new DataView(mb.buffer);
    dv.setUint32(65, 0x44656c50); // 'DelP'
    dv.setUint32(69, 0x44656c70); // 'Delp'
    dv.setUint32(83, patch.length);
    mb.set(patch, 128);
    const un = unwrap('whatever.bin', mb);
    if (un.name !== name || un.bytes.length !== patch.length)
      fail('a MacBinary patch', `got ${JSON.stringify(un.name)} of ${un.bytes.length} bytes`);
    else ok('a MacBinary wrapper is unwrapped', `${un.name}, ${un.bytes.length.toLocaleString()} B`);

    ctx.__base = new Uint8Array(readFileSync(dataPath));
    ctx.__patch = patch;
    const m = vm.runInContext('(() => { const m = mergeDelverPatch(__base, __patch); return {r: m.replaced.length, s: m.skipped.length, n: m.bytes.length}; })()', ctx);
    if (m.r !== 12) fail('the merge through the vendored copy', `replaced ${m.r}, expected 12`);
    else ok('the vendored copy merges the real patch', `${m.r} replaced, ${m.s} skipped, ${m.n.toLocaleString()} B out`);
  }
}

console.log(failures ? `\n${failures} failure(s)` : '\npatch route ok');
process.exit(failures ? 1 : 0);
