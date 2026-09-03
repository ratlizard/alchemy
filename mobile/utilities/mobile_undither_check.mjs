#!/usr/bin/env node
// Checks mobile.html's undither shader against the viewer it was taken from.
//
//   node utilities/mobile_undither_check.mjs mobile.html explorer.html
//
// explorer.html owns this filter. Its UD block is the settled
// result of the tuning done in cythera_graphics_undither.html, and mobile.html
// runs the same filter on the emulator's live screen -- as three GPU passes,
// because the viewer's implementation costs 5.8 seconds for a 640x480 frame
// and 16 seconds at phone resolution. Two copies of one filter is exactly the
// arrangement where a tuning change lands in one of them and not the other.
//
// So this reads the settings and the detector's constants out of the viewer
// and fails if the shader has drifted from them. What it cannot do here is
// compare output: that needs a GL context. The port was measured against the
// viewer's own undither() in headless Chromium, over a checkerboard-dithered
// gradient crossed by one-pixel linework, with stray repair and the 2x
// supersample switched off in the reference to match what the shader carries:
//
//   mean |difference| 0.035 of 255, worst case 1, borders included.
//
// Re-measure that after any change to the maths, not just this.

import {readFileSync, existsSync} from 'node:fs';
import {pageSource} from './page_scripts.mjs';

const [mobilePath = 'mobile.html', viewerPath = 'explorer.html'] = process.argv.slice(2);
for (const p of [mobilePath, viewerPath]) {
  if (!existsSync(p)) { console.error('missing: ' + p); process.exit(2); }
}
const mobile = readFileSync(mobilePath, 'utf8');
// The viewer is read through pageSource() rather than as a file, because the
// undither block it owns now lives in js/delv-graphics.js and this check reads
// UD, LINE_REACH and detect() out of it by name. Reading the HTML text alone
// found nothing and exited 2 the moment the filter moved -- which is the
// failure page_scripts.mjs exists to prevent, and the reason the rule here is
// to ask what a page runs rather than what its own file contains.
const viewer = pageSource(viewerPath);

let failures = 0;
const fail = (what, detail) => { failures++; console.log('  FAIL ' + what + (detail ? ' — ' + detail : '')); };
const ok = (what, detail) => console.log('  ok   ' + what + (detail ? ' — ' + detail : ''));
const check = (cond, what, detail) => cond ? ok(what, detail) : fail(what, detail);

// ---- the viewer's settings -------------------------------------------------
const udSrc = (() => {
  const at = viewer.indexOf('const UD = {');
  if (at < 0) { console.error('no UD block in ' + viewerPath); process.exit(2); }
  return viewer.slice(at, viewer.indexOf('};', at));
})();
const ud = {};
for (const m of udSrc.matchAll(/(\w+)\s*:\s*("[^"]*"|[\d.]+|true|false)/g)) {
  ud[m[1]] = m[2] === 'true' ? true : m[2] === 'false' ? false
           : m[2].startsWith('"') ? m[2].slice(1, -1) : Number(m[2]);
}
console.log(`  viewer UD: sens ${ud.sens}, strength ${ud.strength}, detail ${ud.detail}, ` +
            `filter ${ud.filter}, passes ${ud.passes}, radius ${ud.radius}`);

// ---- what the page carries -------------------------------------------------
const settingsSrc = /const UNDITHER = \{([^}]*)\}/.exec(mobile);
if (!settingsSrc) { fail('settings block', 'no `const UNDITHER = {...}` in ' + mobilePath); }
const settings = {};
if (settingsSrc) for (const m of settingsSrc[1].matchAll(/(\w+)\s*:\s*([\d.]+)/g)) settings[m[1]] = Number(m[2]);

for (const key of ['sens', 'strength', 'detail']) {
  check(settings[key] === ud[key], `${key} matches the viewer`, `${settings[key]} vs ${ud[key]}`);
}

// ---- the shader source -----------------------------------------------------
function shader(name) {
  const at = mobile.indexOf(`const ${name} = \``);
  if (at < 0) return null;
  return mobile.slice(mobile.indexOf('`', at) + 1, mobile.indexOf('`;', at));
}
const inside = shader('GLSL_INSIDE');
const shaders = {};
for (const name of ['VS', 'FS_MEAN', 'FS_DETECT', 'FS_DETAIL']) {
  const src = shader(name);
  if (src === null) { fail('shader ' + name, 'not found'); continue; }
  shaders[name] = inside ? src.replace(/\$\{GLSL_INSIDE\}/g, inside) : src;
}
// A `${}` left unresolved compiles to nothing but a syntax error at run time,
// on a path only reached when someone turns the filter on.
const unresolved = Object.entries(shaders).filter(([, src]) => /\$\{/.test(src)).map(([n]) => n);
check(unresolved.length === 0, 'every shader interpolates', unresolved.join(', ') || '4 shaders');

const detect = shaders.FS_DETECT || '';

// ---- the detector's constants, as the viewer states them -------------------
// detect() in the viewer: a residual below 0.5 is nothing; coherence is turned
// into a weight by (thr - coh) * 8; the weight is faded in over a residual
// magnitude of 1.5 to 5.0; the line reach is 3 pixels along each axis.
const viewerDetect = viewer.slice(viewer.indexOf('function detect('), viewer.indexOf('function checkerNotch('));
const wanted = [
  [/const LINE_REACH\s*=\s*(\d+)/.exec(viewer)?.[1], /k\s*<=\s*(\d+)/.exec(detect)?.[1], 'line reach'],
  [/\(thr-coh\)\*(\d+)/.exec(viewerDetect.replace(/\s/g, ''))?.[1],
   /u_sens\s*-\s*coh\)\s*\*\s*([\d.]+)/.exec(detect)?.[1]?.replace(/\.0$/, ''), 'coherence-to-weight slope'],
];
for (const [want, got, label] of wanted) {
  check(want !== undefined && String(want) === String(got), label, `viewer ${want}, shader ${got}`);
}
const smoothViewer = /smoothstep\(([\d.]+),([\d.]+),m\)/.exec(viewerDetect.replace(/\s/g, ''));
const smoothShader = /smoothstep\(([\d.]+),\s*([\d.]+),\s*m\)/.exec(detect);
check(smoothViewer && smoothShader && smoothViewer[1] === smoothShader[1] && smoothViewer[2] === smoothShader[2],
      'the magnitude ramp', smoothViewer && smoothShader ? `viewer ${smoothViewer[1]}-${smoothViewer[2]}, shader ${smoothShader[1]}-${smoothShader[2]}` : 'not found');
check(/m\s*>=\s*0\.5/.test(detect) && /mj\s*<\s*0\.5/.test(detect), 'the residual floor of 0.5');
check(ud.diagonals === false && /a\s*<\s*2/.test(detect), 'two axes, as the viewer has diagonals off');

// ---- the smoother ----------------------------------------------------------
// checkerNotch is half the pixel and half the mean of its four orthogonal
// neighbours. It ignores its guide argument, which is why the viewer's
// `passes: 3` computes the same answer three times over.
check(ud.filter === 'notch', 'the viewer still uses the notch smoother', String(ud.filter));
check(/0\.5\s*\*\s*c\s*\+\s*0\.5\s*\*/.test(detect), 'the notch is half and half');
check(/for\s*\(int e = 0; e < 4; e\+\+\)/.test(detect), 'over four orthogonal neighbours');

// ---- the stages the page deliberately leaves out ---------------------------
// mobile.html explains why it does not carry these. If the viewer ever turns
// one off, that explanation is stale and the shader may be able to gain it.
check(ud.stray > 0, 'the viewer still does stray-colour repair (the page explains why it does not)',
      'stray ' + ud.stray);
check(ud.upscale === 2 && ud.supersample === true,
      'the viewer still supersamples (the page explains why it does not)',
      `upscale ${ud.upscale}, supersample ${ud.supersample}`);
check(/deliberately not here/.test(mobile), 'the page still explains both');

console.log(failures ? `\nFAIL — ${failures} problem(s)` : '\nundither check: clean');
process.exit(failures ? 1 : 0);
