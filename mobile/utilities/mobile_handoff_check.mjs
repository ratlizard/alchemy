#!/usr/bin/env node
// Checks the disk handoff, which is a second contract between two pages that
// cannot see each other.
//
//   node utilities/mobile_handoff_check.mjs mobile.html explorer.html
//
// explorer.html's "Send that disk to mobile.html" writes the disk image into
// IndexedDB and opens mobile.html, which reads it back out. Four names have to
// agree for that to work -- the database, its version, the object store and
// the key -- and a browser says nothing at all when they stop agreeing: the
// read returns undefined and the card never appears. That is exactly the
// arrangement mobile_install_check.mjs exists for, and this is the same
// answer: read both pages and fail on drift.
//
// Three other things are pinned here, each of which has a way of going wrong
// that no browser would report.
//
//   - **The query parameter must not be `disk`.** mobile.html has read
//     `?disk=` as the system disk to boot ('Mac OS 7.6' by default) since long
//     before the handoff existed. Putting the handoff on that parameter asks
//     the emulator for a machine called "handoff", which fails inside the
//     iframe where this page cannot see it. It was written that way first.
//
//   - **The two stores must be different.** The remembered archive and a disk
//     on its way to the emulator have opposite lifetimes: one is meant to
//     survive the visit, the other is deleted the moment it is collected.
//     Sharing a store would make the handoff overwrite the archive a visitor
//     had open, or the archive silently deliver itself as a disk.
//
//   - **The version bump must be additive.** A visitor who has been here
//     before is on version 1, which has only the archive store, so the upgrade
//     has to create each store only when it is absent. An upgrade that calls
//     createObjectStore unconditionally throws ConstraintError and takes the
//     remembered archive down with it.
//
// And the file-loading route itself is pinned, because it rests on one
// non-obvious parameter. A file on the device can only reach the emulator
// through infinite-mac's OWN file picker: emulator_load_disk takes a URL that
// infinitemac.org's server fetches, so a blob: is useless to it, and a drop
// belongs to the document being dropped on. On the bare /embed route that
// picker's control is in the DOM at zero size -- screenSize is the string
// "embed", which puts the ScreenFrame in fullscreen-bezel mode, and that mode
// sets --screen-controls-display: none. Passing WIDTHxHEIGHT instead makes it
// an ordinary bezel and the control real.
//
//   - **screenSize must stay in the embed URL.** Drop it and the control goes
//     back to zero size, silently: nothing errors, the button just does
//     nothing on every touch device.
//   - **The iframe must stay offset by the frame inset.** With a bezel the
//     screen sits 11px inside a frame of (w+22, h+22), so the iframe is made
//     frame-sized and pulled up and left by 11 to put the emulated screen back
//     on #scale-target's origin. Without that every touch coordinate is out by
//     11 emulated pixels.
//   - **A tap route must exist beside the drag.** This page is for phones,
//     where there is no drag at all. That was the whole bug in the first
//     version of the card.

import {existsSync, readFileSync} from 'node:fs';
import {pageSource} from './page_scripts.mjs';

const [mobilePath = 'mobile.html', viewerPath = 'explorer.html'] = process.argv.slice(2);
for (const p of [mobilePath, viewerPath]) {
  if (!existsSync(p)) { console.error('missing: ' + p); process.exit(2); }
}

let failures = 0;
const fail = (what, detail) => { failures++; console.log('  FAIL ' + what + (detail ? ' — ' + detail : '')); };
const ok = (what, detail) => console.log('  ok   ' + what + (detail ? ' — ' + detail : ''));
const check = (cond, what, detail) => cond ? ok(what, detail) : fail(what, detail);

const mobile = pageSource(mobilePath);
// pageSource is the JavaScript a page runs; the markup needs the file itself.
const mobileHtml = readFileSync(mobilePath, 'utf8');
const viewer = pageSource(viewerPath);

// A crude "give me this function's body" that does not need a parser: from the
// declaration to the next line that starts at the same indentation.
const sliceFunctionish = (src, decl) => {
  const at = src.indexOf(decl);
  if (at < 0) return null;
  const end = src.indexOf('\n    }', at);
  return end < 0 ? src.slice(at) : src.slice(at, end);
};
const strConst = (src, name) => {
  const m = new RegExp(`\\b${name}\\s*=\\s*'([^']*)'`).exec(src);
  return m ? m[1] : null;
};
const numConst = (src, name) => {
  const m = new RegExp(`\\b${name}\\s*=\\s*(\\d+)`).exec(src);
  return m ? Number(m[1]) : null;
};

// ---- the four names --------------------------------------------------------
const viewerSide = {
  db: strConst(viewer, 'ARCHIVE_DB'),
  version: numConst(viewer, 'ARCHIVE_DB_VERSION'),
  store: strConst(viewer, 'HANDOFF_STORE'),
  key: strConst(viewer, 'HANDOFF_KEY'),
  archiveStore: strConst(viewer, 'ARCHIVE_STORE'),
};
const mobileSide = {
  db: strConst(mobile, 'HANDOFF_DB'),
  version: numConst(mobile, 'HANDOFF_DB_VERSION'),
  store: strConst(mobile, 'HANDOFF_STORE'),
  key: strConst(mobile, 'HANDOFF_KEY'),
};

for (const [side, names] of [['explorer.html', viewerSide], ['mobile.html', mobileSide]])
  for (const [field, value] of Object.entries(names))
    if (value === null || value === undefined)
      fail(`${side} declares ${field}`, 'not found in the page source');
if (failures) { console.log('\nFAIL — the names could not be read; nothing else can be checked'); process.exit(1); }

for (const field of ['db', 'version', 'store', 'key'])
  check(viewerSide[field] === mobileSide[field], `both pages agree on the ${field}`,
        `${JSON.stringify(viewerSide[field])} vs ${JSON.stringify(mobileSide[field])}`);

check(viewerSide.store !== viewerSide.archiveStore,
      'the handoff and the remembered archive are different stores',
      `handoff ${JSON.stringify(viewerSide.store)}, archive ${JSON.stringify(viewerSide.archiveStore)}`);

// ---- the query parameter ---------------------------------------------------
const opened = /window\.open\('mobile\.html\?([^']*)'/.exec(viewer);
check(!!opened, 'explorer.html opens mobile.html with a query string',
      opened ? opened[1] : 'no window.open("mobile.html?…") found');
if (opened) {
  const params = new URLSearchParams(opened[1]);
  const keys = [...params.keys()];
  check(!keys.includes('disk'), 'the handoff does not travel on ?disk=',
        '?disk= already selects the system disk to boot');
  for (const k of keys) {
    const reads = new RegExp(`urlParams\\.get\\('${k}'\\)\\s*===\\s*'${params.get(k)}'`).test(mobile);
    check(reads, `mobile.html reads ?${k}=${params.get(k)}`,
          reads ? null : 'nothing in mobile.html tests that parameter for that value');
  }
}
check(/urlParams\.get\('disk'\)\s*\|\|/.test(mobile),
      'mobile.html still reads ?disk= as the system disk', 'the parameter it was already using');

// ---- the upgrade is additive ----------------------------------------------
const upgrade = /onupgradeneeded\s*=\s*\(\)\s*=>\s*\{([\s\S]*?)\n  \};/.exec(viewer);
check(!!upgrade, 'explorer.html has an onupgradeneeded handler');
if (upgrade) {
  const creates = upgrade[1].match(/createObjectStore\(/g) || [];
  const guards = upgrade[1].match(/objectStoreNames\.contains\(/g) || [];
  check(creates.length >= 2, 'the upgrade creates both stores', creates.length + ' createObjectStore call(s)');
  check(guards.length === creates.length,
        'every createObjectStore is guarded by a contains() test',
        `${creates.length} creates, ${guards.length} guards — an unguarded one throws ConstraintError ` +
        'for a visitor upgrading from version 1');
}

// ---- the handoff is a delivery, not a cache --------------------------------
check(/store\.delete\(HANDOFF_KEY\)/.test(mobile),
      'mobile.html deletes the record once it has it',
      'otherwise every later visit re-mounts a disk the visitor already dealt with');

// ---- nothing pretends the last hop is automatic ----------------------------
check(!/emulator_load_disk['"]?\s*,\s*url:\s*(URL\.createObjectURL|blobUrl|blobURL)/.test(mobile) &&
      !/type:\s*'emulator_load_disk',\s*url:\s*URL\.createObjectURL/.test(mobile),
      'no blob: URL is sent to emulator_load_disk',
      'infinitemac.org fetches that URL server-side; a blob: cannot be reached from there');
check(/id="disk-chip"[^>]*draggable="true"|draggable="true"[^>]*id="disk-chip"/.test(mobileHtml),
      'the card offers a real drag', 'the only route a local file has into the emulator');
check(/indexOf\.call\(e\.dataTransfer\.types[\s\S]{0,80}'Files'/.test(mobile),
      'the chip feature-detects the drag before starting one',
      'a browser that refuses items.add(File) would otherwise drag nothing, silently');

// ---- the file-loading route ------------------------------------------------
check(/searchParams\.set\('screenSize'/.test(mobile),
      'the embed URL asks for an explicit screenSize',
      'without it infinite-mac renders its own Load File control at zero size');
check(/const FRAME_INSET = 11;/.test(mobile),
      'the frame inset is the measured 11px',
      'the screen sits 11px inside a frame of (w+22, h+22)');
check(/iframe\.style\.left = `\$\{-FRAME_INSET\}px`/.test(mobile) &&
      /iframe\.style\.width = `\$\{screenWidth \+ FRAME_INSET \* 2\}px`/.test(mobile),
      'the iframe is offset and sized by it, so the screen lands on the box',
      'otherwise every touch coordinate is out by the inset');
check(/id="disk-card-load"/.test(mobileHtml) && /disk-card-load'\), \(\) => setFileMode\(true\)/.test(mobile),
      'the card offers a tap route as well as the drag',
      'this page is for phones, where there is no drag at all');
check(/#disk-chip\[hidden\] \{ display: none; \}/.test(mobileHtml),
      'the chip\'s hidden attribute actually hides it',
      '#disk-chip sets display:block, which beats the UA [hidden] rule on specificity — ' +
      'a phone was shown a drag it cannot perform');
check(/CAN_DRAG/.test(mobile) && /ontouchstart' in window/.test(mobile),
      'the drag is offered only where one exists', 'and the test fails towards touch');
check(/touchOverlay\.style\.pointerEvents = on \? 'none'/.test(mobile),
      'file mode stands the touch overlay down',
      'it would otherwise swallow every tap meant for the Mac\'s own control');

/* Three things about file mode that a browser reports in no way at all, and
   which between them made the control unreachable AND unreadable on the first
   attempt. Each is a CSS rule about someone else's element, so nothing here
   throws when they are wrong -- the control simply is not where it is aimed,
   or is painted over, or is nine pixels tall. */
check(/scaleTarget\.style\.transform = 'none'/.test(mobile) &&
      /panZoomContainer\.style\.willChange = 'auto'/.test(mobile),
      'file mode clears the transforms on the iframe\'s ancestors',
      'position:fixed is positioned against a transformed ancestor, not the viewport, and ' +
      'will-change:transform alone is enough to make one — the strip was scaled twice and ' +
      'landed 270px above where it was aimed');
check(/emulatorWrapper\.style\.zIndex = '2500'/.test(mobile),
      'file mode raises #emulator-wrapper, not just the iframe',
      '#emulator-wrapper has z-index 1 and is therefore a stacking context: the iframe\'s own ' +
      'z-index cannot lift it above a panel outside that context, so the panel pointing at the ' +
      'control was painted over it');
check(/clipPath = `inset\(/.test(mobile),
      'file mode clips to the control row rather than shrinking the whole frame',
      'fitting the whole 640x1385 screen into a phone made the control about nine pixels tall');
check(/Math\.max\(1\.4, Math\.min\(3\.2, vw \/ FILE_STRIP_WIDTH\)\)/.test(mobile),
      'and magnifies it, never shrinks it', 'the floor is above 1, and the cap keeps Load File on screen');
check(/id="file-banner-zoom"/.test(mobileHtml),
      'there is a way through if the row ever moves',
      'the strip depends on measured constants of someone else\'s UI, so the whole-emulator ' +
      'view is kept as an escape hatch');

/* The zip route is a third contract between the two pages: explorer.html names
   the zip so that infinite-mac unpacks it into a folder of that name, and
   mobile.html's typed installer looks for exactly that folder. Nothing errors
   when they drift — the script simply reports that it found nothing. */
{
  const viewerZip = strConst(viewer, 'ZIP_FOLDER_NAME');
  const mobileZip = strConst(mobile, 'ZIP_FOLDER_NAME');
  check(viewerZip && viewerZip === mobileZip, 'both pages agree on the zip folder name',
        `${JSON.stringify(viewerZip)} vs ${JSON.stringify(mobileZip)}`);
  check(new RegExp("dlBlob\\(built\\.blob, ZIP_FOLDER_NAME \\+ '\\.zip'\\)").test(viewer),
        'the zip is named after that folder',
        'infinite-mac names the unpacked folder after the file');
  const zipBody = sliceFunctionish(viewer, 'async function buildForkZip');
  check(/'\.rsrc\/' \+ archiveName/.test(viewer) && /'\.finf\/' \+ archiveName/.test(viewer),
        'the zip carries .rsrc/ and .finf/ at its root',
        "infinite-mac's uploadsFromFile only recognises a zip whose entry names START with those");
  check(/const b = new Uint8Array\(32\)/.test(viewer),
        'Finder info is the full 32 bytes', 'FInfo is 16 and FXInfo another 16');
  const zipScript = sliceFunctionish(mobile, 'function buildZipInstallScript');
  check(!!zipScript && /'on error'/.test(zipScript) &&
        (zipScript.match(/'try'/g) || []).length <= (zipScript.match(/'on error'/g) || []).length,
        'the typed zip installer has on error for every try',
        'without it the compile fails with `Expected "on" but found "end"`');
  check(!!zipScript && /with replacing/.test(zipScript),
        'and replaces silently', 'otherwise the Finder asks and nobody is there to answer');
}

/* File mode clips the iframe down to the control row, so while it is up the
   Mac's screen is not drawn at all. If it does not end by itself, choosing a
   file appears to do nothing whatsoever -- the upload runs behind an opaque
   panel and the only way out is a button nobody should have to hunt for. That
   shipped once. */
check(/window\.addEventListener\('focus', noteFileModeFocus\)/.test(mobile) &&
      /window\.addEventListener\('blur', noteFileModeBlur\)/.test(mobile),
      'file mode ends when the picker hands focus back',
      'a page cannot see inside the file dialog, but it can see that it lost and regained focus');
check(/visibilitychange/.test(mobile),
      'and when the page comes back from the background',
      'a phone browser backgrounds the page rather than blurring it');
check(/fileModeTimer = setTimeout/.test(mobile),
      'with a timer as the floor',
      'a browser that reports neither must still come back to the screen on its own');

console.log(failures ? `\nFAIL — ${failures} problem(s)` : '\nok — the handoff, file-loading and zip contracts hold');
process.exit(failures ? 1 : 0);
