#!/usr/bin/env node
// Checks mobile.html against the emulator it embeds.
//
//   node utilities/mobile_api_check.mjs mobile.html infinite-mac
//
// The page is a touch shell around an infinitemac.org embed, and everything it
// sends -- postMessage payloads, key codes, iframe query parameters, emulator
// speed values -- is defined by that project, not by this one. Nothing checked
// it, and three things had drifted: the "pause" query parameter (it is
// "paused", so the emulator never started paused), emulator_mute and
// emulator_set_volume (no such messages; the button did nothing), and a speed
// control that displayed raw numbers as though they were multipliers.
//
// So this reads the four files in the checked-out repo that define the
// contract and fails if the page steps outside it:
//
//   src/embed-types.ts                 message types and their properties
//   src/defs/run-def.ts                iframe query parameters
//   src/emulator/common/key-codes.ts   key codes (ADB map: Basilisk II)
//   src/emulator/common/emulators.ts   speed values and their names
//
// The page is executed, not scanned: it runs in a stub DOM and every message
// it actually posts is captured. A message built at runtime cannot hide from
// that the way it can from a regular expression.

import {readFileSync, existsSync} from 'node:fs';
import vm from 'node:vm';
import {pageSource} from './page_scripts.mjs';

const [htmlPath = 'mobile.html', repoPath = 'infinite-mac'] = process.argv.slice(2);
for (const p of [htmlPath, repoPath]) {
  if (!existsSync(p)) { console.error('missing: ' + p); process.exit(2); }
}
const read = p => readFileSync(`${repoPath}/${p}`, 'utf8');

// ---- the contract, read out of the emulator's own source -------------------
const embedTypes = read('src/embed-types.ts');
function parseEventUnion(name) {
  const start = embedTypes.indexOf(`export type ${name} =`);
  if (start < 0) throw new Error(`${name} not found in embed-types.ts`);
  const end = embedTypes.indexOf('\n\n', start);
  const body = embedTypes.slice(start, end < 0 ? undefined : end);
  const variants = new Map();
  // Each variant is `{ type: "emulator_x"; prop: T; ... }`.
  for (const block of body.split('|')) {
    const m = /type:\s*"([^"]+)"/.exec(block);
    if (!m) continue;
    const props = new Set(['type']);
    for (const pm of block.matchAll(/^\s{10}(\w+)\??:/gm)) props.add(pm[1]);
    variants.set(m[1], props);
  }
  return variants;
}
const CONTROL_EVENTS = parseEventUnion('EmbedControlEvent');
const NOTIFICATION_EVENTS = parseEventUnion('EmbedNotificationEvent');

const runDef = read('src/defs/run-def.ts');
const QUERY_PARAMS = new Set(
  [...runDef.matchAll(/searchParams\.(?:get|getAll|has)\("([^"]+)"\)/g)].map(m => m[1]));

const keyCodesSrc = read('src/emulator/common/key-codes.ts');
function parseKeyMap(name) {
  const start = keyCodesSrc.indexOf(`export const ${name}`);
  if (start < 0) throw new Error(`${name} not found`);
  const end = keyCodesSrc.indexOf('\n};', start);
  const body = keyCodesSrc.slice(start, end);
  return new Set([...body.matchAll(/"([^"]+)":\s*0x[0-9a-f]+/g)].map(m => m[1]));
}
// The Quadra 650 is a Basilisk II machine, and ui.ts falls through to the ADB
// map for everything that is not Previous or Mini vMac.
const ADB_KEYS = parseKeyMap('JS_CODE_TO_ADB_KEYCODE');

const emulatorsSrc = read('src/emulator/common/emulators.ts');
const MACEMU_SPEEDS = (() => {
  const start = emulatorsSrc.indexOf('const MACEMU_SPEED_CONFIG');
  const end = emulatorsSrc.indexOf('};', start);
  const body = emulatorsSrc.slice(start, end);
  return new Map([...body.matchAll(/\[(-?\d+),\s*"([^"]+)"\]/g)].map(m => [Number(m[1]), m[2]]));
})();

console.log(`  contract: ${CONTROL_EVENTS.size} control messages, ${NOTIFICATION_EVENTS.size} notifications, ` +
            `${QUERY_PARAMS.size} query parameters, ${ADB_KEYS.size} key codes, ${MACEMU_SPEEDS.size} speeds`);

// ---- run the page ----------------------------------------------------------
const html = readFileSync(htmlPath, 'utf8');
const js = pageSource(htmlPath);

let failures = 0;
const fail = (what, detail) => { failures++; console.log('  FAIL ' + what + (detail ? ' — ' + detail : '')); };
const ok = (what, detail) => console.log('  ok   ' + what + (detail ? ' — ' + detail : ''));

function makeEl(tag, id) {
  const listeners = new Map();
  const el = {
    tagName: String(tag).toUpperCase(), id: id || '', tabIndex: -1, title: '', hidden: false,
    style: {}, innerHTML: '', textContent: '', value: '', src: '', width: 0, height: 0,
    className: '', children: [], dataset: {},
    classList: {
      _s: new Set(),
      add(...c) { c.forEach(x => this._s.add(x)); },
      remove(...c) { c.forEach(x => this._s.delete(x)); },
      contains(c) { return this._s.has(c); },
      toggle(c, on) { const v = on === undefined ? !this._s.has(c) : on; if (v) this._s.add(c); else this._s.delete(c); return v; },
    },
    appendChild(c) { el.children.push(c); return c; },
    removeChild(c) { const i = el.children.indexOf(c); if (i >= 0) el.children.splice(i, 1); return c; },
    setAttribute(k, v) { if (k === 'class') el.className = String(v); el.dataset['attr_' + k] = String(v); },
    getAttribute(k) { return k === 'class' ? el.className : (el.dataset['attr_' + k] ?? null); },
    addEventListener(t, fn) { (listeners.get(t) || listeners.set(t, []).get(t)).push(fn); },
    removeEventListener(t, fn) { const l = listeners.get(t) || []; const i = l.indexOf(fn); if (i >= 0) l.splice(i, 1); },
    dispatch(t, ev) { for (const fn of (listeners.get(t) || []).slice()) fn(Object.assign({type: t, preventDefault() {}, stopPropagation() {}}, ev)); },
    listenerCount(t) { return (listeners.get(t) || []).length; },
    focus() {}, blur() {}, click() { el.dispatch('click', {}); }, remove() {},
    setPointerCapture() {}, releasePointerCapture() {},
    getBoundingClientRect: () => ({left: 10, top: 10, right: 170, bottom: 190, width: 160, height: 180}),
    getContext: () => null, offsetWidth: 160, offsetHeight: 180,
    querySelector: sel => el._q(sel)[0] || null,
    querySelectorAll: sel => el._q(sel),
    _q: sel => (el.children.filter(c => c.className.split(/\s+/).includes(String(sel).replace(/^\./, '')))),
  };
  return el;
}

function run({webgl = true} = {}) {
  const registry = new Map();
  const byClass = new Map();
  for (const m of html.matchAll(/\bid="([^"]+)"/g)) registry.set(m[1], makeEl('div', m[1]));

  // Windows and their parts, which the page finds with querySelector.
  for (const winId of ['hud-dpad', 'hud-actions']) {
    const win = registry.get(winId);
    for (const cls of ['drag-handle', 'resize-handle', 'mode-toggle', 'btn-grid', 'gesture-area']) {
      const child = makeEl('div');
      child.className = cls;
      win.appendChild(child);
    }
  }
  // The buttons that carry key codes, taken from the real markup.
  const codeButtons = [...html.matchAll(/class="btn[^"]*"[^>]*data-code="([^"]+)"/g)].map(m => m[1])
    .concat([...html.matchAll(/data-code="([^"]+)"[^>]*class="btn[^"]*"/g)].map(m => m[1]));
  const btnEls = [...new Set(codeButtons)].map(code => {
    const b = makeEl('div');
    b.className = 'btn';
    b.setAttribute('data-code', code);
    return b;
  });
  byClass.set('.btn[data-code]', btnEls);
  byClass.set('.hud-window', ['hud-dpad', 'hud-actions'].map(id => registry.get(id)));
  byClass.set('.action-btn', []);
  byClass.set('.dir-btn', []);

  const posted = [];
  const iframe = registry.get('mac-iframe');
  const rawPosted = [];
  iframe.contentWindow = {postMessage: (msg, origin) => { posted.push({msg, origin}); }};

  const glCanvas = registry.get('gl-canvas');
  const glCalls = [];
  glCanvas.getContext = kind => {
    if (kind !== 'webgl' || !webgl) return null;
    const gl = new Proxy({
      VERTEX_SHADER: 1, FRAGMENT_SHADER: 2, COMPILE_STATUS: 3, LINK_STATUS: 4,
      ARRAY_BUFFER: 5, STATIC_DRAW: 6, TEXTURE_2D: 7, FLOAT: 8, RGBA: 9,
      UNSIGNED_BYTE: 10, NEAREST: 11, CLAMP_TO_EDGE: 12, TRIANGLES: 13,
      TEXTURE_MIN_FILTER: 14, TEXTURE_MAG_FILTER: 15, TEXTURE_WRAP_S: 16, TEXTURE_WRAP_T: 17,
      getShaderParameter: () => true, getProgramParameter: () => true,
      getShaderInfoLog: () => '', getProgramInfoLog: () => '',
      getAttribLocation: () => 0, getUniformLocation: () => ({}),
    }, {
      get(t, k) {
        if (k in t) return t[k];
        return (...args) => { glCalls.push(String(k)); return {}; };
      },
    });
    return gl;
  };
  registry.get('gesture-canvas').getContext = () => new Proxy({}, {get: () => () => ({})});

  const winListeners = new Map();
  const docListeners = new Map();
  const store = new Map();
  const session = new Map();
  // A clock the checks drive. Nothing fires unless a check asks it to, as
  // before -- but the page now coalesces pointer moves to one a frame and
  // spaces its button transitions in milliseconds, so there has to be a clock
  // to ask. See the note on BUTTON_GAP_MS in the page.
  // A clock that starts at zero is not a browser's clock, and the page reads
  // Date.now() to space its button transitions: with now === 0 the very first
  // press finds "no time has passed since the last one" and waits out the gap,
  // which a real page never does. Start where a browser would.
  const clock = {now: Date.parse('2026-01-01T00:00:00Z'), queue: []};
  const schedule = (fn, ms) => { clock.queue.push({fn, at: clock.now + (ms || 0), id: clock.queue.length + 1}); return clock.queue.length; };
  const cancel = id => { const t = clock.queue.find(t => t.id === id); if (t) t.cancelled = true; };
  const sandbox = {
    console: {log() {}, warn() {}, error() {}},
    Math, JSON, Object, Array, String, Number, Boolean, Set, Map, Error, TypeError,
    Date: Object.assign(function (...a) { return new Date(...a); }, {now: () => clock.now}),
    RegExp, Promise, isNaN, isFinite, parseInt, parseFloat, Infinity, NaN, undefined,
    URL, URLSearchParams, TextEncoder, TextDecoder, Uint8Array, Float32Array, Array_,
    setTimeout: schedule, clearTimeout: cancel,
    setInterval: () => 0, clearInterval() {},
    requestAnimationFrame: fn => schedule(fn, 16), cancelAnimationFrame: cancel,
    prompt: () => null,
    localStorage: {
      getItem: k => (store.has(k) ? store.get(k) : null),
      setItem: (k, v) => store.set(k, String(v)),
      removeItem: k => store.delete(k),
    },
    sessionStorage: {
      getItem: k => (session.has(k) ? session.get(k) : null),
      setItem: (k, v) => session.set(k, String(v)),
      removeItem: k => session.delete(k),
    },
    // No service worker here, so the page's cross-origin-isolation dance
    // resolves immediately and goes straight on to booting the emulator.
    // mobile_coi_check.mjs is what drives that path with one present.
    navigator: {userAgent: 'node'},
    // The page asks whether this device has a hover-capable pointer, to decide
    // whether a drag exists at all. Answering "no" here is the honest stub:
    // node is not a mouse, and it exercises the touch branch, which is the one
    // that matters and the one that was wrong.
    matchMedia: () => ({matches: false, addEventListener() {}, addListener() {}}),
    crossOriginIsolated: false,
    location: {search: '', href: 'file:///mobile.html', hash: ''},
    innerWidth: 390, innerHeight: 844,
    document: {
      getElementById: id => registry.get(id) || registry.set(id, makeEl('div', id)).get(id),
      createElement: t => makeEl(t),
      querySelector: sel => (byClass.get(sel) || [])[0] || null,
      querySelectorAll: sel => byClass.get(sel) || [],
      addEventListener(t, fn) { (docListeners.get(t) || docListeners.set(t, []).get(t)).push(fn); },
      removeEventListener() {},
      documentElement: makeEl('html'),
      body: makeEl('body'),
      hidden: false,
      fullscreenElement: null,
    },
    addEventListener(t, fn) { (winListeners.get(t) || winListeners.set(t, []).get(t)).push(fn); },
    removeEventListener() {},
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  const ctx = vm.createContext(sandbox);
  let threw = null;
  try {
    new vm.Script(js + '\n;window.__peek = n => eval(n);\n', {filename: htmlPath}).runInContext(ctx);
  } catch (e) { threw = e; }

  const fire = (map, type, ev) => {
    for (const fn of (map.get(type) || []).slice()) fn(Object.assign({type, preventDefault() {}, stopPropagation() {}}, ev));
  };
  return {
    ctx, posted, threw, registry, btnEls, glCalls, rawPosted,
    // Frames and timers up to `ms` from now, each at its own time.
    tick(ms) {
      const until = clock.now + (ms || 0);
      for (;;) {
        const due = clock.queue.filter(t => !t.cancelled && !t.done && t.at <= until).sort((a, b) => a.at - b.at)[0];
        if (!due) break;
        clock.now = Math.max(clock.now, due.at);
        due.done = true;
        due.fn();
      }
      clock.now = until;
    },
    // A symbol that is not there is a finding, not a crash: this harness has
    // to be able to report on a page that predates the structure it expects.
    peek: n => { try { return ctx.__peek(n); } catch (e) { return undefined; } },
    fireWindow: (t, ev) => fire(winListeners, t, ev),
    fireDoc: (t, ev) => fire(docListeners, t, ev),
    hasWindowListener: t => (winListeners.get(t) || []).length > 0,
  };
}
// `Array_` above is a typo guard: referencing an undefined name in the sandbox
// object literal would throw here rather than inside the page.
function Array_() {}

// ---- 1. it runs, with and without WebGL ------------------------------------
const app = run({webgl: true});
if (app.threw) { fail('page throws with WebGL', app.threw.message); }
else ok('page initialises with WebGL', `${app.glCalls.length} GL calls`);

const noGl = run({webgl: false});
if (noGl.threw) {
  fail('page throws without WebGL', noGl.threw.message);
} else {
  // The whole shader block used to run unguarded at the top level, so a null
  // context killed the script before a single control was wired up.
  const filter = noGl.registry.get('filter-toggle');
  ok('page initialises without WebGL',
     filter.classList.contains('disabled') ? 'filter marked unavailable' : 'no crash');
  if (!filter.classList.contains('disabled')) fail('WebGL fallback', 'filter button was not disabled');
}

// ---- 2. the start sequence waits for the emulator --------------------------
{
  const a = run();
  a.registry.get('start-overlay').dispatch('pointerdown', {});
  if (a.posted.length) fail('start before load', `${a.posted.length} messages were sent into the void`);
  else ok('start before load', 'messages held until the emulator reports in');

  a.fireWindow('message', {origin: 'https://infinitemac.org', data: {type: 'emulator_loaded'}});
  a.tick(300);      // the press and its release are deliberately spaced
  const types = a.posted.map(p => p.msg.type);
  // The move is not decoration: a click with no move before it lands at 0,0,
  // which on a Mac is the corner of the menu bar. There is one before each of
  // the two button transitions, not one for the pair: every transition carries
  // the position it was queued at, so that a release cannot be dragged to
  // wherever the pointer has gone while it waited out the gap.
  const want = ['emulator_unpause',
                'emulator_mouse_move', 'emulator_mouse_down',
                'emulator_mouse_move', 'emulator_mouse_up'];
  if (JSON.stringify(types) === JSON.stringify(want)) ok('start after load', types.join(', '));
  else fail('start after load', 'sent ' + JSON.stringify(types));

  // A message from anywhere else must not be able to drive the page.
  const before = a.posted.length;
  a.fireWindow('message', {origin: 'https://evil.example', data: {type: 'emulator_loaded'}});
  if (a.posted.length !== before) fail('origin check', 'a foreign message was acted on');
  else ok('origin check', 'messages from other origins ignored');

  const targeted = a.posted.every(p => p.origin === 'https://infinitemac.org');
  if (targeted) ok('postMessage target origin', 'never "*"');
  else fail('postMessage target origin', 'some messages were broadcast to "*"');
}

// ---- 3. keys are pressed and, more importantly, released -------------------
{
  const a = run();
  a.registry.get('start-overlay').dispatch('pointerdown', {});
  a.fireWindow('message', {origin: 'https://infinitemac.org', data: {type: 'emulator_loaded'}});
  a.posted.length = 0;

  const btn = a.btnEls.find(b => b.getAttribute('data-code') === 'Numpad8');
  btn.dispatch('pointerdown', {});
  btn.dispatch('pointercancel', {});
  const seq = a.posted.map(p => `${p.msg.type}:${p.msg.code}`);
  if (JSON.stringify(seq) === JSON.stringify(['emulator_key_down:Numpad8', 'emulator_key_up:Numpad8']))
    ok('pointercancel releases the key', seq.join(' → '));
  else fail('pointercancel releases the key', JSON.stringify(seq));

  // Hiding the page must not leave a direction held: in Cythera that is a
  // character who walks until something stops them.
  a.posted.length = 0;
  btn.dispatch('pointerdown', {});
  a.ctx.document.hidden = true;
  a.fireDoc('visibilitychange', {});
  const released = a.posted.some(p => p.msg.type === 'emulator_key_up' && p.msg.code === 'Numpad8');
  if (released) ok('hiding the page releases held keys');
  else fail('hiding the page releases held keys', JSON.stringify(a.posted.map(p => p.msg)));
}

// ---- 4. every gesture is reachable ----------------------------------------
if (!app.peek('GESTURES') || !app.peek('recogniseGesture')) {
  fail('gesture table', 'no GESTURES/recogniseGesture — the gestures are inline conditionals that cannot be checked');
} else {
  const gestures = app.peek('GESTURES');
  const recognise = app.peek('recogniseGesture');
  // Exhaustive over every stroke of up to four turns. The recogniser never
  // emits the same direction twice in a row, so neither does this.
  const strokes = [];
  (function build(prefix) {
    if (prefix.length) strokes.push(prefix);
    if (prefix.length === 4) return;
    for (const d of 'UDLR') if (prefix[prefix.length - 1] !== d) build(prefix + d);
  })('');
  const reached = new Map();
  for (const s of strokes) {
    const g = recognise(s, false);
    if (g) (reached.get(g.name) || reached.set(g.name, []).get(g.name)).push(s);
  }
  const tap = recognise('', true);
  if (tap) reached.set(tap.name, ['(tap)']);

  const missing = gestures.filter(g => !reached.has(g.name));
  if (missing.length) {
    // This is the shape of the bug that was here: LOOK matched `includes("DR")`
    // before USE ("DRU") or CAST ("LDR") were tried, so those two could not
    // fire at all.
    fail('every gesture is reachable', 'unreachable: ' + missing.map(g => g.name).join(', '));
  } else {
    ok('every gesture is reachable', gestures.map(g => `${g.name}=${reached.get(g.name)[0]}`).join(' '));
  }
  // And the documented shapes resolve to what they claim.
  const INTENDED = [['U', 'Get'], ['D', 'Drop'], ['DRU', 'Use'], ['DLU', 'Use'],
                    ['LDR', 'Cast'], ['RDL', 'Talk'], ['DR', 'Look']];
  const wrong = INTENDED.filter(([s, name]) => (recognise(s, false) || {}).name !== name);
  if (wrong.length) fail('documented gestures resolve correctly',
    wrong.map(([s, n]) => `${s} wanted ${n}, got ${(recognise(s, false) || {name: 'nothing'}).name}`).join('; '));
  else ok('documented gestures resolve correctly', INTENDED.length + ' shapes');
}

// ---- 5. everything sent is in the API -------------------------------------
{
  const a = run();
  a.registry.get('start-overlay').dispatch('pointerdown', {});
  a.fireWindow('message', {origin: 'https://infinitemac.org', data: {type: 'emulator_loaded'}});
  // Exercise every control that talks to the emulator.
  for (const b of a.btnEls) { b.dispatch('pointerdown', {}); b.dispatch('pointerup', {}); }
  a.registry.get('pause-btn').dispatch('pointerdown', {});
  a.registry.get('pause-btn').dispatch('pointerdown', {});
  if (a.peek('moveMouse')) a.peek('moveMouse')(100, 120);
  if (a.peek('mouseButton')) { a.peek('mouseButton')(2, true); a.peek('mouseButton')(2, false); }
  if (a.peek('post')) a.peek('post')({type: 'emulator_load_disk', url: 'https://example.com/x.dsk'});
  // Then press every control in the settings drawer, whatever it happens to
  // be. A targeted list would only ever exercise the buttons already known
  // about -- and the button that sent messages the emulator has never heard of
  // was exactly the one nobody thought to press.
  for (const [id, el] of a.registry) {
    if (!/-btn$|-toggle$|-handle$/.test(id)) continue;
    try { el.dispatch('pointerdown', {}); el.dispatch('pointerup', {}); } catch (e) { /* a control that needs more DOM than this */ }
  }
  a.tick(1000);     // let everything the page queued actually go out
  // Anything the page posts directly, rather than through post().
  for (const p of a.rawPosted) a.posted.push(p);

  const badType = [], badProp = [];
  for (const {msg} of a.posted) {
    const props = CONTROL_EVENTS.get(msg.type);
    if (!props) { badType.push(msg.type); continue; }
    for (const k of Object.keys(msg)) if (!props.has(k)) badProp.push(`${msg.type}.${k}`);
  }
  if (badType.length) fail('message types', 'not in EmbedControlEvent: ' + [...new Set(badType)].join(', '));
  else ok('message types', `${new Set(a.posted.map(p => p.msg.type)).size} distinct, all in EmbedControlEvent`);
  if (badProp.length) fail('message properties', [...new Set(badProp)].join(', '));
  else ok('message properties', 'all declared');

  const codes = [...new Set(a.posted.filter(p => p.msg.code).map(p => p.msg.code))];
  const unknown = codes.filter(c => !ADB_KEYS.has(c));
  if (unknown.length) fail('key codes', 'not in the ADB map: ' + unknown.join(', '));
  else ok('key codes', `${codes.length} codes, all understood by Basilisk II`);

  // Every key the gesture and typing paths can produce, too.
  const declared = new Set([
    ...(app.peek('GESTURES') || []).map(g => g.key),
    ...Object.values(app.peek('charToCode') || {}),
  ]);
  const unknown2 = [...declared].filter(c => !ADB_KEYS.has(c));
  if (unknown2.length) fail('declared key codes', unknown2.join(', '));
  else ok('declared key codes', `${declared.size} more, all understood`);
}

// ---- 6. the iframe URL --------------------------------------------------
if (!app.peek('buildEmbedUrl')) {
  // Fall back to whatever URL is written into the markup.
  const m = /https:\/\/infinitemac\.org\/embed\?[^"'`\s]+/.exec(html);
  if (!m) fail('iframe URL', 'no buildEmbedUrl and no embed URL in the markup');
  else {
    const url = new URL(m[0].replace(/&amp;/g, '&'));
    const params = [...url.searchParams.keys()];
    const unknown = params.filter(p => !QUERY_PARAMS.has(p));
    if (unknown.length) fail('query parameters', 'run-def.ts does not read: ' + unknown.join(', '));
    else ok('query parameters', params.join(', '));
    if (url.searchParams.get('paused') !== 'true') fail('paused parameter', 'the emulator will not start paused');
  }
} else {
  const url = new URL(app.peek('buildEmbedUrl')(2));
  const params = [...url.searchParams.keys()];
  const unknown = params.filter(p => !QUERY_PARAMS.has(p));
  if (unknown.length) fail('query parameters', 'run-def.ts does not read: ' + unknown.join(', '));
  else ok('query parameters', params.join(', '));
  if (url.searchParams.get('paused') !== 'true') fail('paused parameter', 'the emulator will not start paused');
  else ok('paused parameter', 'spelled the way run-def.ts reads it');
  if (params.includes('pause')) fail('pause parameter', 'the misspelling is back');

  const settings = JSON.parse(url.searchParams.get('settings'));
  const settingsSrc = read('src/emulator/ui/settings.ts');
  const declaredSettings = new Set([...settingsSrc.matchAll(/^\s{4}(\w+)\??:/gm)].map(m => m[1]));
  const badSettings = Object.keys(settings).filter(k => !declaredSettings.has(k));
  if (badSettings.length) fail('emulator settings', 'not in EmulatorSettings: ' + badSettings.join(', '));
  else ok('emulator settings', Object.keys(settings).join(', '));
}

// ---- 7. speeds ------------------------------------------------------------
if (!app.peek('SPEEDS')) {
  fail('speed table', 'no SPEEDS table to compare against the emulator\'s own');
} else {
  const speeds = app.peek('SPEEDS');
  const wrong = speeds.filter(([v, label]) => MACEMU_SPEEDS.get(v) !== label);
  if (wrong.length) fail('speed labels', wrong.map(([v, l]) => `${v} shown as "${l}", emulator calls it "${MACEMU_SPEEDS.get(v)}"`).join('; '));
  else ok('speed labels', speeds.map(([, l]) => l).join(', '));
  const missing = [...MACEMU_SPEEDS.keys()].filter(v => !speeds.some(([s]) => s === v));
  if (missing.length) fail('speed coverage', 'not offered: ' + missing.join(', '));
  else ok('speed coverage', 'every speed Basilisk II supports');
}

// ---- 8. the notification side ---------------------------------------------
{
  if (!app.hasWindowListener('message')) fail('notifications', 'nothing listens for emulator messages');
  else ok('notifications', `listens for ${[...NOTIFICATION_EVENTS.keys()].join(', ')}`);
}

console.log(failures ? `\nFAIL — ${failures} problem(s)` : '\nmobile API check: clean');
process.exit(failures ? 1 : 0);
