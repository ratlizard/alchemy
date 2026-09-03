#!/usr/bin/env node
// Checks the one-tap mod install, which is a contract between two pages.
//
//   node utilities/mobile_install_check.mjs mobile.html explorer.html
//
// explorer.html writes a disk image holding an edited Cythera Data and an
// AppleScript called "Install and Play". mobile.html types those two names at
// the emulated Finder to select them, and then sends Command-O and Command-R.
// Neither page can see the other, and nothing in a browser complains when a
// name on one side stops matching the name on the other -- it just selects
// nothing and the install quietly does nothing at all. That is the same
// arrangement mobile_undither_check.mjs exists for, and this is the same
// answer: read both, and fail on drift.
//
// It also reads the script itself, because three things about it were settled
// by compiling it in a real Mac OS 7.6 and would be easy to undo by tidying:
//
//   - `try` needs its `on error` clause in this AppleScript. Without it the
//     compile fails with `Expected "on" but found "end"`, which happens after
//     everything else has already worked, so it looks like the install broke
//     rather than the script.
//   - `duplicate ... with replacing` is what makes the replace silent. Take it
//     away and the Finder asks, and there is nobody there to answer.
//   - the script names the disk it is sitting on, so that name has to be the
//     one writeHfsImage was given.
//
// And it builds a disk the way the page does, so that "the script is on the
// disk, as TEXT with creator ToyS" is checked rather than assumed -- the
// creator is the whole mechanism, since it is what makes the Finder hand the
// file to Script Editor.

import {existsSync, readFileSync} from 'node:fs';
import vm from 'node:vm';
import {collectPageScripts, pageSource} from './page_scripts.mjs';

const [mobilePath = 'mobile.html', viewerPath = 'explorer.html'] = process.argv.slice(2);
for (const p of [mobilePath, viewerPath]) {
  if (!existsSync(p)) { console.error('missing: ' + p); process.exit(2); }
}

let failures = 0;
const fail = (what, detail) => { failures++; console.log('  FAIL ' + what + (detail ? ' — ' + detail : '')); };
const ok = (what, detail) => console.log('  ok   ' + what + (detail ? ' — ' + detail : ''));
const check = (cond, what, detail) => cond ? ok(what, detail) : fail(what, detail);

const mobile = pageSource(mobilePath);
const viewer = pageSource(viewerPath);

// ---- what each page says the names are -------------------------------------
function constOf(source, name) {
  const m = new RegExp(`\\bconst ${name}\\s*=\\s*'([^']*)'`).exec(source);
  return m ? m[1] : null;
}
const viewerNames = {
  volume: constOf(viewer, 'DISK_VOLUME_NAME'),
  script: constOf(viewer, 'DISK_SCRIPT_NAME'),
  archive: constOf(viewer, 'DISK_ARCHIVE_NAME'),
};
const mobileNames = {
  volume: constOf(mobile, 'INSTALL_DISK_NAME'),
  script: constOf(mobile, 'INSTALL_SCRIPT_NAME'),
  diskPrefix: constOf(mobile, 'INSTALL_DISK_PREFIX'),
  scriptPrefix: constOf(mobile, 'INSTALL_SCRIPT_PREFIX'),
};
for (const [where, names] of [[viewerPath, viewerNames], [mobilePath, mobileNames]]) {
  const missing = Object.entries(names).filter(([, v]) => !v).map(([k]) => k);
  if (missing.length) { fail(`${where} names`, 'could not read ' + missing.join(', ')); }
}
if (failures) { console.log('\nFAIL — the names could not be read; nothing else can be checked'); process.exit(1); }

check(viewerNames.volume === mobileNames.volume, 'both pages call the disk the same thing',
      `${viewerPath}: "${viewerNames.volume}", ${mobilePath}: "${mobileNames.volume}"`);
check(viewerNames.script === mobileNames.script, 'both pages call the script the same thing',
      `"${viewerNames.script}"`);

// The Finder's type-select needs a prefix, not the whole name.
const startsWith = (name, prefix) => name.toLowerCase().startsWith(prefix.toLowerCase());
check(startsWith(viewerNames.volume, mobileNames.diskPrefix),
      'what the page types selects the disk', `"${mobileNames.diskPrefix}" of "${viewerNames.volume}"`);
check(startsWith(viewerNames.script, mobileNames.scriptPrefix),
      'what the page types selects the script', `"${mobileNames.scriptPrefix}" of "${viewerNames.script}"`);
// A prefix that is also a prefix of the other name would select the wrong one.
check(!startsWith(viewerNames.volume, mobileNames.scriptPrefix) &&
      !startsWith(viewerNames.script, mobileNames.diskPrefix),
      'and neither prefix could select the other');

// ---- the sequence of keystrokes --------------------------------------------
/* The order is the whole design: close the windows so the desktop has the
   keyboard, select the disk, open it, select the script, open it, run it.
   Any other order types into the wrong place. */
const stepsSrc = /const INSTALL_STEPS = \[([\s\S]*?)\n {4}\];/.exec(mobile);
if (!stepsSrc) fail('INSTALL_STEPS', 'not found in ' + mobilePath);
else {
  const steps = [];
  for (const m of stepsSrc[1].matchAll(/\['(\w+)',\s*([^\]]*)\]/g)) {
    steps.push([m[1], m[2].trim()]);
  }
  const shape = steps.filter(([k]) => k !== 'note' && k !== 'wait').map(([k, a]) => `${k}:${a.split(',')[0].trim()}`);
  const want = [
    // Return dismisses the Finder's modal desktop-rebuild alert, which
    // otherwise holds the keyboard and swallows everything after it; Escape
    // cancels the rename that Return starts when there was no alert.
    "key:'Enter'",
    "key:'Escape'",
    "cmd:'KeyW'",
    'type:INSTALL_DISK_PREFIX',
    "cmd:'KeyO'",
    'type:INSTALL_SCRIPT_PREFIX',
    "cmd:'KeyO'",
    "cmd:'KeyR'",
  ];
  check(JSON.stringify(shape) === JSON.stringify(want),
        'the keystrokes go in the order the Finder needs', shape.join(' → '));
  const closes = steps.find(([k, a]) => k === 'cmd' && a.startsWith("'KeyW'"));
  check(closes && Number(closes[1].split(',')[1]) >= 4,
        'enough Command-W to reach the desktop', closes ? closes[1] : 'none');
  // Nothing may aim at a pixel: that is the whole point of the design.
  check(!/\['(click|move|point)'/.test(stepsSrc[1]),
        'and nothing in the sequence points at the screen');
}

// ---- the script the viewer writes ------------------------------------------
/* buildInstallScript and its three constants are lifted out of the page and
   run on their own. Evaluating the whole viewer would need a DOM it has no use
   for here, and slicing by name means a rename fails this check rather than
   silently testing nothing. */
function sliceFunction(source, name) {
  const at = source.indexOf(`function ${name}(`);
  if (at < 0) return null;
  let depth = 0, i = source.indexOf('{', at);
  for (let j = i; j < source.length; j++) {
    if (source[j] === '{') depth++;
    else if (source[j] === '}' && --depth === 0) return source.slice(at, j + 1);
  }
  return null;
}
const builder = sliceFunction(viewer, 'buildInstallScript');
if (!builder) {
  fail('buildInstallScript', 'not found in ' + viewerPath);
} else {
  const sandbox = {console, TextDecoder, TextEncoder, Uint8Array, String, Number,
                   Array, Map, Set, Math, JSON, Error, Date, isNaN, parseInt};
  sandbox.window = sandbox;
  vm.createContext(sandbox);
  const {sources} = collectPageScripts(viewerPath);
  for (const name of ['js/mac-bytes.js', 'js/mac-hfs.js']) {
    const s = sources.find(x => x.name === name);
    if (!s) { fail('viewer loads ' + name); continue; }
    vm.runInContext(s.code, sandbox, {filename: name});
  }
  vm.runInContext(
    `const DISK_VOLUME_NAME = ${JSON.stringify(viewerNames.volume)};\n` +
    `const DISK_SCRIPT_NAME = ${JSON.stringify(viewerNames.script)};\n` +
    `const DISK_ARCHIVE_NAME = ${JSON.stringify(viewerNames.archive)};\n` + builder, sandbox);
  const peek = vm.runInContext('(n) => eval(n)', sandbox);
  const script = peek('buildInstallScript')(viewerNames.archive);

  check(script.includes(`disk "${viewerNames.volume}"`),
        'the script names the disk it is on', `disk "${viewerNames.volume}"`);
  check(script.includes(`file "${viewerNames.archive}"`),
        'and the archive it is installing', `file "${viewerNames.archive}"`);
  check(/\btry\b/.test(script) && /\bon error\b/.test(script),
        'try has its on error clause, which this AppleScript requires');
  check(/duplicate .* with replacing/.test(script),
        'the copy replaces without asking');
  check(/\bopen theGame\b/.test(script), 'and the game is launched');
  check(!script.includes('\n'), 'lines end with CR, as a classic Mac text file does');
  check(script.split('\r').filter(l => l.startsWith('--')).length >= 3,
        'the script explains itself to whoever opens it',
        script.split('\r').filter(l => l.startsWith('--')).length + ' comment lines');

  // Build the disk the way the page does and read the script back off it.
  const enc = peek('encodeMacRoman');
  /* The page's own entry list, counted in its source rather than mirrored
     here: a fixture that says two while the page writes three would pass and
     prove nothing. */
  // buildEditedDiskImage, not downloadEditedDiskImage: the build was split out
  // when a second caller appeared (the handoff to mobile.html), and both
  // callers have to be writing the same two-file disk.
  const exportBody = sliceFunction(viewer, 'buildEditedDiskImage');
  const entriesSrc = /entries: \[([\s\S]*?)\n {6}\],/.exec(exportBody || '');
  const entryCount = entriesSrc ? (entriesSrc[1].match(/\{ name:/g) || []).length : -1;
  check(entryCount === 2, 'the page writes two files onto the disk', entryCount + ' entries');

  const image = peek('writeHfsImage')({
    volumeName: viewerNames.volume,
    entries: [
      {name: viewerNames.archive, type: 'DelS', creator: 'Delv', data: enc('archive')},
      {name: viewerNames.script, type: 'TEXT', creator: 'ToyS', data: enc(script)},
    ],
  });
  // Walk the catalog for the script's record: type and creator are the
  // mechanism, not decoration.
  const u16 = (b, i) => (b[i] << 8) | b[i + 1];
  const u32 = (b, i) => ((b[i] * 0x1000000) + (b[i + 1] << 16) + (b[i + 2] << 8) + b[i + 3]) >>> 0;
  const alBlSt = u16(image, 1024 + 28), alSz = u32(image, 1024 + 20);
  const catAt = alBlSt * 512 + u16(image, 1024 + 150) * alSz;
  const catalog = image.subarray(catAt, catAt + u32(image, 1024 + 146));
  const found = new Map();
  for (let node = u32(catalog, 14 + 10); node !== 0; ) {
    const at = node * 512;
    for (let i = 0; i < u16(catalog, at + 10); i++) {
      const start = at + u16(catalog, at + 512 - (i + 1) * 2);
      const keyLen = catalog[start];
      const name = Array.from(catalog.subarray(start + 7, start + 7 + catalog[start + 6]))
        .map(c => String.fromCharCode(c)).join('');
      const d = start + ((1 + keyLen + 1) & ~1);
      if (catalog[d] === 2) {
        found.set(name, {
          type: String.fromCharCode(...catalog.subarray(d + 4, d + 8)),
          creator: String.fromCharCode(...catalog.subarray(d + 8, d + 12)),
        });
      }
    }
    node = u32(catalog, at);
  }
  /* Two files, no more. A third pushes the catalog past one 512-byte leaf,
     and a multi-leaf volume raises the Finder's modal desktop-file alert on
     every mount -- which holds the keyboard and stops the install dead. The
     end-to-end run that found this is why the Read Me is gone and its text
     lives in the script's comments. */
  check(found.size === 2, 'the disk carries two files, so its catalog fits one leaf node',
        [...found.keys()].join(', '));

  const onDisk = found.get(viewerNames.script);
  check(!!onDisk, 'the script is on the exported disk', [...found.keys()].join(', '));
  check(onDisk && onDisk.type === 'TEXT' && onDisk.creator === 'ToyS',
        'as TEXT/ToyS, which is what opens it in Script Editor',
        onDisk ? onDisk.type + '/' + onDisk.creator : 'absent');
  check(!!found.get(viewerNames.archive), 'and so is the archive, under the name the script uses',
        viewerNames.archive);
}

// ---- the disk has to be installable, not just well-formed ------------------
/* A Delver scenario with no resource fork installs perfectly and then will not
   open: Cythera dies with "Unable to open RT", which names nothing a person
   could trace back to the viewer. Measured in the emulator against the real
   1.0.4 Cythera Data -- identical data fork, fork present and absent -- so the
   viewer says so at export time, and this is what keeps it saying so. */
{
  const warner = sliceFunction(viewer, 'missingForkWarning');
  if (!warner) {
    fail('missingForkWarning', 'not found in ' + viewerPath);
  } else {
    const box = {};
    vm.createContext(box);
    vm.runInContext(warner, box);
    const call = vm.runInContext('missingForkWarning', box);
    check(call(new Uint8Array(4)) === null, 'an archive with its fork exports without complaint');
    const warning = call(new Uint8Array(0));
    check(typeof warning === 'string' && warning.length > 40,
          'and one without it says so', warning ? warning.slice(0, 60) + '…' : 'nothing');
    check(warning && warning.includes('Unable to open RT'),
          'naming the error the game actually gives, so the two can be connected');
    // Both exports carry the fork, and both must warn -- a .bin installed by
    // hand fails in exactly the same way as a .dsk installed by the script.
    for (const fn of ['buildEditedDiskImage', 'downloadEditedMacBinary']) {
      const body = sliceFunction(viewer, fn);
      check(!!body && body.includes('missingForkWarning'), fn + ' checks for the fork');
    }
    // The warning has to reach a person on both routes out of the build, and
    // the handoff is the one that could quietly drop it -- it opens another
    // tab, so an unmentioned problem is one the visitor never sees.
    for (const fn of ['downloadEditedDiskImage', 'sendEditedDiskToMobile']) {
      const body = sliceFunction(viewer, fn);
      check(!!body && body.includes('forkWarning'), fn + ' passes the fork warning on');
    }
  }
}

/* The patch script mobile.html types has to clear the same AppleScript traps
   as the one explorer.html writes onto a disk -- it is the same dialect and
   the same 7.6. `try` without `on error` was written twice now, and it fails
   at compile time, after every other step has worked, so it reads as the
   patch being wrong rather than the script. */
{
  // patchPassLines, not buildPatchScript: a patch is typed in passes now, and
  // each pass is a whole script of its own.
  const patchBody = sliceFunction(mobile, 'patchPassLines');
  check(!!patchBody, 'mobile.html builds a patch script');
  if (patchBody) {
    const find = sliceFunction(mobile, 'patchPassLines') + mobile.slice(
      mobile.indexOf('const PATCH_FIND_GAME'), mobile.indexOf('function patchPassLines'));
    const tries = (find.match(/'try'/g) || []).length;
    const onErrors = (find.match(/'on error'/g) || []).length;
    check(tries > 0 && onErrors >= tries,
          'every try in the typed script has its on error',
          `${tries} try, ${onErrors} on error — without it the compile fails with ` +
          '`Expected "on" but found "end"`');
    check(/starting at ' \+ \(r\.offset \+ 1\)/.test(patchBody),
          'the patch offset is sent one-based',
          'write ... starting at is 1-based in this AppleScript, measured in a real Mac OS 7.6; ' +
          'a 0-based offset writes to the byte before the one meant');
    check(/read fd from ' \+ \(r\.offset \+ 1\)/.test(patchBody) && /set ok to false/.test(patchBody),
          'it checks the old bytes before writing any',
          'a patch is only valid against the file it was diffed from');
    /* The bytes are typed as hex and decoded in the script. Spelling them as
       `(ASCII character N)` costs 46 typed characters per patched byte and put
       one portrait at two hours; hex costs two. The keyboard is the whole cost
       of this route, so this is the difference between usable and not. */
    check(/my unhex\("' \+ hexOf\(r\.now\)/.test(patchBody) &&
          /my unhex\("' \+ hexOf\(r\.old\)/.test(patchBody),
          'bytes are typed as hex, not spelled out',
          'two typed characters per byte instead of forty-six');
    const prelude = mobile.slice(mobile.indexOf('const PATCH_HEX_PRELUDE'),
                                 mobile.indexOf('const PATCH_MAX_SCRIPT'));
    check(/set end of L to/.test(prelude) && !/set d to d & /.test(prelude),
          'the decoder accumulates into a list, not a string',
          'AppleScript strings are immutable, so concatenating in a loop is quadratic — ' +
          'on a 68K that is seconds against minutes');
    check(/ASCII number/.test(prelude), 'and reads nybbles arithmetically',
          'rather than `offset of c in ...`, which a missing scripting addition could defeat');
  }
  {
    const split = sliceFunction(mobile, 'buildPatchScripts');
    check(!!split && /PATCH_MAX_SCRIPT/.test(split), 'a long patch is split into passes',
          "Script Editor's field is a classic TextEdit record, capped at 32,767 bytes");
    check(!!split && /batch\.length && patchPassLines/.test(split),
          'and a run is never split across passes',
          'the read, the check and the write for one run have to be in the same script');
    // typeAndRun, not runPatch: opening Script Editor and typing a script is
    // shared with the zip installer now, so the select-all lives there.
    const runner = sliceFunction(mobile, 'typeAndRun');
    check(!!runner && /tapCommand\('KeyA'\)/.test(runner),
          'each pass after the first selects all before typing',
          'otherwise the next pass is appended to the last one');
  }
}

console.log(failures ? `\nFAIL — ${failures} problem(s)` : '\ninstall check: clean');
process.exit(failures ? 1 : 0);
