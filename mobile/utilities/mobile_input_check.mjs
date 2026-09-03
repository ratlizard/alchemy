#!/usr/bin/env node
// Drives mobile.html's pointer, wheel and keyboard handling in a stub DOM.
//
//   node utilities/mobile_input_check.mjs mobile.html
//
// mobile_api_check.mjs asks whether what the page sends is in the emulator's
// API. This asks the other half of the question: whether anything is sent at
// all, for each of the ways a person can touch the thing.
//
// It was written after the page spent a long time being unusable with a mouse.
// #touch-overlay is stretched across the emulator's iframe so that it can
// catch touches, so it also catches every click -- and it listened for
// `touchstart`, `touchmove` and `touchend`, which a mouse does not produce.
// The emulator was on screen, visibly running, and could not be clicked.
// Nothing noticed, because every check ran the page rather than used it.
//
// So the stub DOM here is a little more honest than a pile of no-ops: an
// element's bounding rectangle is computed from the CSS transforms the page
// itself set, which is what makes "a click at the middle of the window lands
// at the middle of the emulated screen" a real assertion about the coordinate
// arithmetic rather than about a hard-coded rectangle.
//
// The last section is not about input at all, and is here because this is the
// harness that already builds a whole page and always runs. mobile.html
// registers a service worker before it will start the emulator, to get the two
// headers that make the embed cross-origin isolated -- without which the
// emulator loses SharedArrayBuffer and runs at about a thirtieth of its speed
// (measured; see coi-serviceworker.js). Getting that wrong means a page that
// reloads itself forever, so it is checked here rather than trusted.

import {readFileSync, existsSync} from 'node:fs';
import vm from 'node:vm';
import {pageSource} from './page_scripts.mjs';

const [htmlPath = 'mobile.html'] = process.argv.slice(2);
if (!existsSync(htmlPath)) { console.error('missing: ' + htmlPath); process.exit(2); }

const html = readFileSync(htmlPath, 'utf8');
const js = pageSource(htmlPath);

let failures = 0;
const fail = (what, detail) => { failures++; console.log('  FAIL ' + what + (detail ? ' — ' + detail : '')); };
const ok = (what, detail) => console.log('  ok   ' + what + (detail ? ' — ' + detail : ''));
const check = (cond, what, detail) => cond ? ok(what, detail) : fail(what, detail);

const VIEW_W = 390, VIEW_H = 844;

// ---- the stub DOM ----------------------------------------------------------
function parseTransform(t) {
  const out = {x: 0, y: 0, s: 1};
  if (!t) return out;
  const tr = /translate\(\s*(-?[\d.]+)px\s*,\s*(-?[\d.]+)px\s*\)/.exec(t);
  if (tr) { out.x = Number(tr[1]); out.y = Number(tr[2]); }
  const sc = /scale\(\s*(-?[\d.]+)\s*\)/.exec(t);
  if (sc) out.s = Number(sc[1]);
  return out;
}

function makeEl(tag, id, doc) {
  const listeners = new Map();
  const captured = new Set();
  const el = {
    tagName: String(tag).toUpperCase(), id: id || '', tabIndex: -1, title: '', hidden: false,
    style: {}, innerHTML: '', textContent: '', value: '', src: '', width: 0, height: 0,
    className: '', children: [], dataset: {}, isContentEditable: false,
    classList: {
      _s: new Set(),
      add(...c) { c.forEach(x => this._s.add(x)); },
      remove(...c) { c.forEach(x => this._s.delete(x)); },
      contains(c) { return this._s.has(c); },
      toggle(c, on) { const v = on === undefined ? !this._s.has(c) : on; if (v) this._s.add(c); else this._s.delete(c); return v; },
    },
    appendChild(c) { el.children.push(c); return c; },
    setAttribute(k, v) { if (k === 'class') el.className = String(v); el.dataset['attr_' + k] = String(v); },
    getAttribute(k) { return k === 'class' ? el.className : (el.dataset['attr_' + k] ?? null); },
    addEventListener(t, fn) { (listeners.get(t) || listeners.set(t, []).get(t)).push(fn); },
    removeEventListener(t, fn) { const l = listeners.get(t) || []; const i = l.indexOf(fn); if (i >= 0) l.splice(i, 1); },
    listenerCount(t) { return (listeners.get(t) || []).length; },
    dispatch(t, ev) {
      const event = Object.assign(
        {type: t, target: el, preventDefault() { event.defaultPrevented = true; }, stopPropagation() {}},
        ev);
      for (const fn of (listeners.get(t) || []).slice()) fn(event);
      return event;
    },
    setPointerCapture(pid) { captured.add(pid); },
    releasePointerCapture(pid) { captured.delete(pid); },
    hasPointerCapture(pid) { return captured.has(pid); },
    focus() { doc.activeElement = el; }, blur() {}, click() { el.dispatch('click', {}); }, remove() {},
    // Derived from what the page put in style, so the geometry under test is
    // the page's own rather than this file's idea of it.
    getBoundingClientRect() {
      const own = parseTransform(el.style.transform);
      const w = parseFloat(el.style.width) || el.offsetWidth || 0;
      const h = parseFloat(el.style.height) || el.offsetHeight || 0;
      const parent = el.id === 'scale-target' ? doc.getElementById('pan-zoom-container') : null;
      const pz = parent ? parseTransform(parent.style.transform) : {x: 0, y: 0, s: 1};
      const left = pz.x + own.x * pz.s, top = pz.y + own.y * pz.s;
      const width = w * own.s * pz.s, height = h * own.s * pz.s;
      return {left, top, width, height, right: left + width, bottom: top + height};
    },
    getContext: () => null, offsetWidth: 160, offsetHeight: 180,
    querySelector: sel => el._q(sel)[0] || null,
    querySelectorAll: sel => el._q(sel),
    _q: sel => el.children.filter(c => c.className.split(/\s+/).includes(String(sel).replace(/^\./, ''))),
  };
  return el;
}

function buildPage({webgl = true, serviceWorker = null, search = '', isolated = false,
                    session = new Map()} = {}) {
  const registry = new Map();
  const byClass = new Map();
  // A clock that starts at zero is not a browser's clock, and the page reads
  // Date.now() to space its button transitions: with now === 0 the very first
  // press finds "no time has passed since the last one" and waits out the gap,
  // which a real page never does. Start where a browser would.
  const clock = {now: Date.parse('2026-01-01T00:00:00Z'), queue: []};
  const doc = {};
  const mk = (tag, id) => makeEl(tag, id, doc);

  for (const m of html.matchAll(/\bid="([^"]+)"/g)) registry.set(m[1], mk('div', m[1]));
  // Elements the markup hides to begin with. Whether the help panel is open
  // decides where the keyboard goes, so getting this wrong would test nothing.
  for (const m of html.matchAll(/\bid="([^"]+)"[^>]*\shidden(?=[\s>])/g)) registry.get(m[1]).hidden = true;
  registry.get('modal-input').tagName = 'INPUT';
  registry.get('hidden-keyboard').tagName = 'INPUT';

  for (const winId of ['hud-dpad', 'hud-actions']) {
    const win = registry.get(winId);
    for (const cls of ['drag-handle', 'resize-handle', 'mode-toggle', 'btn-grid', 'gesture-area']) {
      const child = mk('div');
      child.className = cls;
      win.appendChild(child);
    }
  }
  const codes = [...new Set([...html.matchAll(/data-code="([^"]+)"/g)].map(m => m[1]))];
  const btnEls = codes.map(code => {
    const b = mk('div');
    b.className = 'btn';
    b.setAttribute('data-code', code);
    return b;
  });
  byClass.set('.btn[data-code]', btnEls);
  byClass.set('.hud-window', ['hud-dpad', 'hud-actions'].map(id => registry.get(id)));
  byClass.set('.action-btn', []);
  byClass.set('.dir-btn', []);

  const posted = [];
  registry.get('mac-iframe').contentWindow = {postMessage: msg => posted.push(Object.assign({at: clock.now}, msg))};

  const glCalls = [];
  registry.get('gl-canvas').getContext = kind => {
    if (kind !== 'webgl' || !webgl) return null;
    return new Proxy({
      VERTEX_SHADER: 1, FRAGMENT_SHADER: 2, COMPILE_STATUS: 3, LINK_STATUS: 4,
      getShaderParameter: () => true, getProgramParameter: () => true,
      getShaderInfoLog: () => '', getProgramInfoLog: () => '',
      getAttribLocation: () => 0, getUniformLocation: () => ({}),
    }, {get(t, k) { return k in t ? t[k] : (...a) => { glCalls.push(String(k)); return {}; }; }});
  };
  registry.get('gesture-canvas').getContext = () => new Proxy({}, {get: () => () => ({})});

  const winListeners = new Map(), docListeners = new Map(), store = new Map();
  const reloads = [];
  Object.assign(doc, {
    getElementById: id => registry.get(id) || registry.set(id, mk('div', id)).get(id),
    createElement: t => mk(t),
    querySelector: sel => (byClass.get(sel) || [])[0] || null,
    querySelectorAll: sel => byClass.get(sel) || [],
    addEventListener(t, fn) { (docListeners.get(t) || docListeners.set(t, []).get(t)).push(fn); },
    removeEventListener() {},
    documentElement: mk('html'), body: mk('body'),
    hidden: false, fullscreenElement: null, activeElement: null,
  });

  const sandbox = {
    console: {log() {}, warn() {}, error() {}},
    Math, JSON, Object, Array, String, Number, Boolean, Set, Map, Error, TypeError,
    Date: Object.assign(function (...a) { return new Date(...a); }, {now: () => clock.now}),
    RegExp, Promise, isNaN, isFinite, parseInt, parseFloat, Infinity, NaN, undefined,
    URL, URLSearchParams, Uint8Array, Float32Array,
    setTimeout: (fn, ms) => { clock.queue.push({fn, at: clock.now + (ms || 0), id: clock.queue.length + 1}); return clock.queue.length; },
    clearTimeout: id => { const t = clock.queue.find(t => t.id === id); if (t) t.cancelled = true; },
    setInterval: () => 0, clearInterval() {},
    // The page coalesces pointer moves to one a frame and spaces its button
    // transitions in milliseconds, so both have to be on the same clock as the
    // timers or none of it is reproducible.
    requestAnimationFrame: fn => { clock.queue.push({fn, at: clock.now + 16, id: clock.queue.length + 1}); return clock.queue.length; },
    cancelAnimationFrame: id => { const t = clock.queue.find(t => t.id === id); if (t) t.cancelled = true; },
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
    // With no serviceWorker the page's cross-origin-isolation dance resolves
    // at once and goes straight on to booting the emulator, which is what
    // every section but the last one wants. The last one passes a stub.
    navigator: serviceWorker ? {userAgent: 'node', serviceWorker} : {userAgent: 'node'},
    // matches:false is "no hover-capable pointer", i.e. a touch device, which
    // is what this page is for and the branch worth exercising.
    matchMedia: () => ({matches: false, addEventListener() {}, addListener() {}}),
    crossOriginIsolated: isolated,
    location: {search, href: 'file:///' + htmlPath, hash: '', reload() { reloads.push(clock.now); }},
    innerWidth: VIEW_W, innerHeight: VIEW_H,
    document: doc,
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
    const event = Object.assign({type, preventDefault() { event.defaultPrevented = true; }, stopPropagation() {}}, ev);
    for (const fn of (map.get(type) || []).slice()) fn(event);
    return event;
  };
  const api = {
    ctx, posted, threw, registry, btnEls, glCalls, clock,
    el: id => registry.get(id),
    peek: n => { try { return ctx.__peek(n); } catch (e) { return undefined; } },
    fireWindow: (t, ev) => fire(winListeners, t, ev),
    fireDoc: (t, ev) => fire(docListeners, t, ev),
    // Everything the page has queued -- frames and timers alike -- up to the
    // moment `ms` from now, in order.
    settle() { api.tick(300); },
    reloads, session,
    tick(ms) {
      // A real event loop, not one jump: the clock stops at each callback's
      // own time, so a callback that schedules another 40ms out gets its turn
      // inside the same tick and reads a Date.now() that makes sense.
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
    start() {
      registry.get('start-overlay').dispatch('pointerdown', {});
      api.fireWindow('message', {origin: 'https://infinitemac.org', data: {type: 'emulator_loaded'}});
      api.settle();
      posted.length = 0;
    },
    // Tell the page what the emulator actually came up with, the way the
    // emulator does.
    screen(width, height) {
      api.fireWindow('message', {origin: 'https://infinitemac.org', data: {type: 'emulator_screen', width, height}});
      api.settle();
      posted.length = 0;
    },
    types: () => posted.map(m => m.type),
    moves: () => posted.filter(m => m.type === 'emulator_mouse_move'),
    lastMove: () => api.moves()[api.moves().length - 1],
    buttons: () => posted.filter(m => m.type === 'emulator_mouse_down' || m.type === 'emulator_mouse_up')
                         .map(m => `${m.type === 'emulator_mouse_down' ? 'down' : 'up'}${m.button}`),
    keys: () => posted.filter(m => m.type === 'emulator_key_down' || m.type === 'emulator_key_up')
                      .map(m => `${m.type === 'emulator_key_down' ? '+' : '-'}${m.code}`),
    clear() { posted.length = 0; },
  };
  return api;
}

// A gesture, spelled the way the browser delivers one.
function pointer(el, id, type) {
  const send = (kind, x, y, extra) =>
    el.dispatch(kind, Object.assign({pointerId: id, pointerType: type, clientX: x, clientY: y, button: 0, buttons: 1}, extra));
  return {
    down: (x, y, extra) => send('pointerdown', x, y, extra),
    move: (x, y, extra) => send('pointermove', x, y, extra),
    up: (x, y, extra) => send('pointerup', x, y, extra),
    cancel: (x, y, extra) => send('pointercancel', x, y, extra),
  };
}

const near = (a, b, tol = 1.5) => Math.abs(a - b) <= tol;

// ---- 1. it runs -------------------------------------------------------------
const boot = buildPage();
if (boot.threw) {
  fail('page initialises', boot.threw.message);
  console.log('\nFAIL — the page did not run, so nothing below could be checked');
  process.exit(1);
}
ok('page initialises', `${boot.glCalls.length} GL calls`);

// ---- 2. a mouse can click the emulator -------------------------------------
{
  const a = buildPage();
  a.start();
  a.screen(640, 480);
  const mouse = pointer(a.el('touch-overlay'), 1, 'mouse');
  mouse.down(VIEW_W / 2, VIEW_H / 2);
  mouse.up(VIEW_W / 2, VIEW_H / 2);
  a.settle();
  const move = a.lastMove();
  check(a.buttons().join(' ') === 'down0 up0', 'a mouse click reaches the emulator', a.buttons().join(' ') || 'nothing was sent');
  check(!!move && near(move.x, 320) && near(move.y, 240),
        'the click lands where the mouse is', move ? `(${move.x}, ${move.y}) of 640x480` : 'no move was sent');
}

// ---- 3. hovering, and the right button -------------------------------------
{
  const a = buildPage();
  a.start(); a.screen(640, 480);
  const mouse = pointer(a.el('touch-overlay'), 1, 'mouse');
  mouse.move(VIEW_W / 2, VIEW_H / 2);
  a.settle();
  const hover = a.lastMove();
  check(!!hover && near(hover.x, 320) && near(hover.y, 240),
        'a mouse with no button down still moves the pointer', hover ? `(${hover.x}, ${hover.y})` : 'nothing');
  a.clear();
  mouse.down(100, 400, {button: 2});
  mouse.up(100, 400, {button: 2});
  a.settle();
  check(a.buttons().join(' ') === 'down2 up2', 'the right button goes through as the right button', a.buttons().join(' '));
  const menu = a.el('touch-overlay').dispatch('contextmenu', {});
  check(menu.defaultPrevented, 'the browser context menu is suppressed');
}

// ---- 4. nothing is left held -----------------------------------------------
{
  const a = buildPage();
  a.start(); a.screen(640, 480);
  const finger = pointer(a.el('touch-overlay'), 1, 'touch');
  finger.down(100, 400);
  a.clear();
  finger.cancel(100, 400);
  a.settle();
  check(a.buttons().join(' ') === 'up0', 'a cancelled gesture releases the button', a.buttons().join(' ') || 'the button stayed down');

  const b = buildPage();
  b.start(); b.screen(640, 480);
  pointer(b.el('touch-overlay'), 1, 'touch').down(100, 400);
  b.clear();
  b.ctx.document.hidden = true;
  b.fireDoc('visibilitychange', {});
  b.settle();
  check(b.buttons().join(' ') === 'up0', 'hiding the page releases the button', b.buttons().join(' ') || 'the button stayed down');
}

// ---- 5. the touch modes ----------------------------------------------------
{
  // Direct: the finger presses as it lands, so a drag is a drag.
  const a = buildPage();
  a.start(); a.screen(640, 480);
  const finger = pointer(a.el('touch-overlay'), 1, 'touch');
  finger.down(VIEW_W / 2, VIEW_H / 2);
  a.settle();
  const down = a.lastMove();
  finger.move(VIEW_W / 2 + 30, VIEW_H / 2);
  finger.up(VIEW_W / 2 + 30, VIEW_H / 2);
  a.settle();
  check(a.buttons().join(' ') === 'down0 up0', 'direct: a finger drag holds the button down', a.buttons().join(' '));
  check(!!down && near(down.x, 320) && near(down.y, 240 - 12, 1.5),
        'direct: aim sits a little above the fingertip', down ? `(${down.x}, ${down.y})` : 'nothing');

  // Trackpad: a tap clicks where the pointer already is, and the pointer moves
  // relatively rather than jumping to the finger.
  const b = buildPage();
  b.start(); b.screen(640, 480);
  b.peek('setTouchMode')(1);
  b.clear();
  const t = pointer(b.el('touch-overlay'), 1, 'touch');
  t.down(100, 400);
  b.tick(50);
  const noJump = b.moves().length === 0;
  t.up(100, 400);
  b.settle();
  check(noJump, 'trackpad: the pointer does not jump to the finger');
  check(b.buttons().join(' ') === 'down0 up0', 'trackpad: a tap is a click', b.buttons().join(' '));

  // Trackpad, held still: the drag lock after 0.4s.
  const c = buildPage();
  c.start(); c.screen(640, 480);
  c.peek('setTouchMode')(1);
  c.clear();
  const held = pointer(c.el('touch-overlay'), 1, 'touch');
  held.down(100, 400);
  c.tick(500);
  const locked = c.buttons().join(' ') === 'down0';
  held.up(100, 400);
  c.settle();
  check(locked && c.buttons().join(' ') === 'down0 up0', 'trackpad: holding still locks a drag', c.buttons().join(' '));

  // Pan View: dragging moves the view, and the pointer stays in the middle.
  const d = buildPage();
  d.start(); d.screen(640, 480);
  d.peek('setTouchMode')(2);
  d.clear();
  const pan = pointer(d.el('touch-overlay'), 1, 'touch');
  pan.down(200, 400);
  pan.move(160, 380);
  pan.up(160, 380);
  d.settle();
  check(d.buttons().length === 0, 'pan view: a drag does not click', d.buttons().join(' '));
  check(d.moves().length > 0, 'pan view: the pointer is kept on the crosshair');
}

// ---- 6. two fingers --------------------------------------------------------
{
  const a = buildPage();
  a.start(); a.screen(640, 480);
  const overlay = a.el('touch-overlay');
  const one = pointer(overlay, 1, 'touch'), two = pointer(overlay, 2, 'touch');
  one.down(150, 400);
  two.down(250, 400);
  one.up(150, 400);
  two.up(250, 400);
  a.settle();
  check(a.buttons().slice(-2).join(' ') === 'down2 up2', 'a two-finger tap is a right click', a.buttons().join(' ') || 'nothing');

  const b = buildPage();
  b.start(); b.screen(640, 480);
  const o = b.el('touch-overlay');
  const p1 = pointer(o, 1, 'touch'), p2 = pointer(o, 2, 'touch');
  p1.down(150, 400);
  b.settle();
  const landed = b.buttons().join(' ');       // direct mode presses as it lands
  p2.down(250, 400);
  b.settle();
  const handedBack = b.buttons().join(' ');
  b.clear();
  p2.move(350, 400);
  const zoomed = parseTransform(b.el('pan-zoom-container').style.transform).s;
  p1.up(150, 400); p2.up(350, 400);
  b.settle();
  check(zoomed > 1.2, 'pinching zooms the view', 'scale ' + zoomed.toFixed(2));
  // Direct mode presses the button the moment a finger lands, which is what
  // makes dragging work; a second finger has to take that press back, or the
  // Mac is left holding the mouse down for the length of the pinch.
  check(landed === 'down0' && handedBack === 'down0 up0',
        'a second finger takes back the press the first one made', handedBack);
  check(b.buttons().length === 0, 'and the pinch itself is not a click', b.buttons().join(' '));
}

// ---- 7. the wheel ----------------------------------------------------------
{
  const a = buildPage();
  a.start(); a.screen(640, 480);
  const ev = a.el('touch-overlay').dispatch('wheel', {clientX: 195, clientY: 422, deltaY: -240, deltaMode: 0});
  const scale = parseTransform(a.el('pan-zoom-container').style.transform).s;
  check(scale > 1.2, 'the wheel zooms', 'scale ' + scale.toFixed(2));
  check(ev.defaultPrevented, 'the wheel does not also scroll the page');
}

// ---- 8. a real keyboard ----------------------------------------------------
{
  const a = buildPage();
  a.start();
  a.fireDoc('keydown', {code: 'Numpad8', key: '8', target: {tagName: 'BODY'}});
  a.fireDoc('keyup', {code: 'Numpad8', key: '8', target: {tagName: 'BODY'}});
  check(a.keys().join(' ') === '+Numpad8 -Numpad8', 'a keyboard drives the emulated Mac', a.keys().join(' ') || 'nothing was sent');

  a.clear();
  a.fireDoc('keydown', {code: 'KeyG', key: 'g', target: a.el('modal-input')});
  check(a.keys().length === 0, 'typing in a field of this page stays in the field', a.keys().join(' '));

  a.clear();
  a.fireDoc('keydown', {code: 'KeyR', key: 'r', ctrlKey: true, target: {tagName: 'BODY'}});
  check(a.keys().length === 0, 'the browser keeps its own shortcuts', a.keys().join(' '));

  // macOS swallows the keyup for a key pressed while Command is held, so the
  // Mac is left holding it. Infinite Mac works around this for its own
  // keyboard; anything sent through the embed API arrives past that point.
  a.clear();
  const meta = {ctrlKey: false, metaKey: true, target: {tagName: 'BODY'}};
  a.fireDoc('keydown', Object.assign({code: 'MetaLeft', key: 'Meta'}, meta));
  a.fireDoc('keydown', Object.assign({code: 'KeyS', key: 's'}, meta));
  a.fireDoc('keyup', Object.assign({code: 'MetaLeft', key: 'Meta'}, meta));   // and no keyup for S
  const stuck = a.keys().filter(k => k === '+KeyS').length - a.keys().filter(k => k === '-KeyS').length;
  check(stuck === 0, 'a key held under Command is not left down', a.keys().join(' '));

  // Before the emulator has been started there is nothing to type at.
  const b = buildPage();
  b.fireDoc('keydown', {code: 'KeyG', key: 'g', target: {tagName: 'BODY'}});
  check(b.posted.length === 0, 'keys before the machine starts are not queued up', JSON.stringify(b.types()));
}

// ---- 9. the floating controls ----------------------------------------------
{
  const a = buildPage();
  a.start();
  const joy = pointer(a.el('move-gesture-zone'), 1, 'mouse');
  joy.down(80, 80);
  joy.move(80, 20);              // straight up
  const pressed = a.keys().join(' ');
  joy.up(80, 20);
  check(pressed.includes('+Numpad8'), 'the joystick works under a mouse', pressed || 'nothing');
  check(a.keys().join(' ').includes('-Numpad8'), 'and lets go when the drag ends', a.keys().join(' '));

  a.clear();
  const stroke = pointer(a.el('action-gesture-zone'), 2, 'mouse');
  stroke.down(20, 20);
  for (let y = 20; y <= 120; y += 20) stroke.move(20, y);     // down
  for (let x = 20; x <= 120; x += 20) stroke.move(x, 120);    // then right: an L
  stroke.up(120, 120);
  a.settle();
  check(a.keys().join(' ') === '+KeyL -KeyL', 'a gesture can be drawn with a mouse', a.keys().join(' ') || 'nothing');
}

/* ---- 10. what the emulated Mac actually ends up seeing ---------------------
   The checks above ask what the page sends. This one asks what survives.

   From updateInputBufferWithEvents() in the emulator's own
   src/emulator/common/common.ts: everything queued between two of its input
   syncs is collapsed into a single state. The FIRST mousemove of the batch is
   kept and every later one is dropped; each mousedown/mouseup overwrites the
   one before it, because what reaches the Mac is a button LEVEL, not an edge.

   So a press and its release inside one window cancel out and the click never
   happens, and a release and the next press inside one window lose the
   release -- the level stays down and the Mac reads the next tap somewhere
   else as a DRAG from the last one. That is what "Direct mode drags in random
   directions when I tap" was.

   The sync period is not fixed: the emulator consumes input when the guest
   next polls, so a busy or slow machine has a longer window. Hence the sweep. */
function asTheMacSeesIt(posted, syncEveryMs) {
  const states = [];
  // Anchored to the first message rather than to absolute zero: the clock this
  // runs on starts where a browser's does, so counting sync windows up from
  // zero would mean stepping through fifty-odd years of them.
  let batch = [], nextSync = (posted.length ? posted[0].at : 0) + syncEveryMs;
  const flush = () => {
    if (!batch.length) return;
    let pos = null, button = null;
    for (const m of batch) {
      if (m.type === 'emulator_mouse_move') { if (pos === null) pos = {x: m.x, y: m.y}; }
      else if (m.type === 'emulator_mouse_down') button = 'down';
      else if (m.type === 'emulator_mouse_up') button = 'up';
    }
    if (pos || button) states.push({pos, button});
    batch = [];
  };
  for (const m of posted) {
    while (m.at >= nextSync) { flush(); nextSync += syncEveryMs; }
    batch.push(m);
  }
  flush();
  return states;
}

// The level the Mac is holding, and where the pointer was each time it changed.
function buttonStory(states) {
  const story = [];
  let level = 'up', at = null;
  for (const s of states) {
    if (s.pos) at = s.pos;
    if (s.button && s.button !== level) { level = s.button; story.push(`${level}@${at ? at.x + ',' + at.y : '?'}`); }
  }
  return story;
}

{
  /* Up to a 50ms window, i.e. a guest polling at 20Hz. Past that the emulator
     would swallow a physical mouse click as well, so it is not this page's
     problem to solve.

     GAPS is the other axis, and the one this check used to be blind to. It
     compared the SHAPE of the story -- down up down up -- and threw the
     positions away, so a release that reached the Mac at the next tap's
     position still read as two clicks. That is precisely what Direct mode did
     with a gap shorter than BUTTON_GAP_MS: the first tap's release sat in the
     outbox waiting out the gap, and the position flushed ahead of it was
     whichever one was pending by then, so the Mac saw a drag from the first
     tap to the second. Recorded off the real emulator, before the fix:

         +  0ms  mouse_move (164,726) / mouse_down
         + 68ms  mouse_move (542,907) / mouse_up      <- released at tap two
         +140ms  mouse_down                            <- tap two presses

     So the assertion is now that each release lands where its own press did,
     at every combination of sync window and gap. */
  const PERIODS = [8, 16, 33, 50];
  const GAPS = [0, 25, 40, 69, 120];
  const A = [120, 300], B = [300, 500];
  let clean = 0; const broken = [];
  for (const period of PERIODS) for (const gap of GAPS) {
    const a = buildPage();
    a.start(); a.screen(640, 480);
    const finger = pointer(a.el('touch-overlay'), 1, 'touch');
    finger.down(A[0], A[1]); finger.up(A[0], A[1]);
    a.tick(gap);
    finger.down(B[0], B[1]); finger.up(B[0], B[1]);
    a.settle();
    const story = buttonStory(asTheMacSeesIt(a.posted, period));
    const shape = story.map(s => s.split('@')[0]).join(' ');
    // Where the four transitions happened, in emulated-screen coordinates.
    // FINGER_OFFSET_Y moves the press up from the fingertip, so the pair are
    // compared with each other rather than with the touch.
    const where = story.map(s => s.split('@')[1]);
    const paired = shape === 'down up down up' &&
                   where[0] === where[1] && where[2] === where[3] &&
                   where[0] !== where[2];
    if (paired) clean++;
    else broken.push(`${period}ms window, ${gap}ms apart: ${story.join(' ') || 'nothing'}`);
  }
  check(broken.length === 0, 'two taps are two clicks, each released where it pressed',
        broken.length ? broken.slice(0, 3).join(' | ') : clean + ' window/gap combinations');
}

// ---- 11. the screen size comes from the emulator ---------------------------
{
  const a = buildPage();
  a.start();
  a.screen(800, 600);
  pointer(a.el('touch-overlay'), 1, 'mouse').move(VIEW_W / 2, VIEW_H / 2);
  a.settle();
  const m = a.lastMove();
  check(!!m && near(m.x, 400) && near(m.y, 300),
        'coordinates follow the resolution the emulator reports', m ? `(${m.x}, ${m.y}) of 800x600` : 'nothing');
  const inBounds = a.moves().every(p => p.x >= 0 && p.x < 800 && p.y >= 0 && p.y < 600);
  check(inBounds, 'the pointer is never sent off the emulated screen');
}

// ---- 12. cross-origin isolation, and the reload it costs --------------------
/* The page will not ask for the emulator until it knows whether it is going to
   reload, because a reload halfway through downloading a machine and a disk
   image is the one expensive thing on a phone. What must hold:

     - with no service worker at all (a file:// copy, a browser without them),
       the page gives up immediately and boots;
     - with one, it registers, and reloads exactly once when the worker takes
       control -- and records that it tried, so a browser that refuses the
       headers gets a slow page rather than a loop;
     - a second load in the same tab does not reload again;
     - already isolated means nothing to do at all;
     - ?coi=0 unregisters and stays on the slow path. */
function swStub() {
  const listeners = new Map();
  const stub = {
    registered: [], unregistered: 0, controller: null,
    addEventListener(t, fn) { (listeners.get(t) || listeners.set(t, []).get(t)).push(fn); },
    register(url) { stub.registered.push(url); return Promise.resolve({active: null}); },
    getRegistrations() { return Promise.resolve([{unregister() { stub.unregistered++; return Promise.resolve(true); }}]); },
    // What the browser does when the worker calls clients.claim().
    takeControl() { stub.controller = {}; for (const fn of listeners.get('controllerchange') || []) fn(); },
  };
  return stub;
}
{
  // The page settles its isolation question through a promise, so the harness
  // has to let the microtask queue run before looking: the fake clock only
  // moves timers, and a resolved promise is not a timer.
  const turn = () => new Promise(r => setImmediate(r));

  const plain = buildPage();
  await turn(); plain.settle();
  check(!!plain.el('mac-iframe').src && plain.reloads.length === 0,
        'no service worker: the emulator is asked for at once, with no reload',
        plain.el('mac-iframe').src ? 'iframe.src set' : 'iframe never loaded');

  const sw = swStub();
  const a = buildPage({serviceWorker: sw});
  check(!a.el('mac-iframe').src, 'the emulator is held back until isolation settles');
  await turn(); a.settle();
  check(sw.registered.length === 1 && /coi-serviceworker\.js$/.test(sw.registered[0]),
        'a service worker is registered', sw.registered.join(', ') || 'none');
  sw.takeControl();
  await turn(); a.settle();
  check(a.reloads.length === 1, 'the page reloads once when the worker takes control',
        a.reloads.length + ' reload(s)');
  check(a.session.get('cythera-mobile-coi-reload') === '1',
        'and records that it tried, so a browser that will not isolate cannot loop');

  // The reload lands in the same tab, so sessionStorage still says "tried".
  // Whatever the outcome, it must not go round again.
  const again = swStub();
  const b = buildPage({serviceWorker: again, session: a.session});
  await turn(); b.settle();
  again.takeControl();
  await turn(); b.settle();
  check(b.reloads.length === 0, 'a second load in the same tab does not reload again',
        b.reloads.length + ' reload(s)');
  check(!!b.el('mac-iframe').src, 'and it boots either way');

  const already = swStub();
  const c = buildPage({serviceWorker: already, isolated: true});
  await turn(); c.settle();
  check(c.reloads.length === 0 && already.registered.length === 0,
        'an already-isolated page does nothing', 'no register, no reload');
  check(!!c.el('mac-iframe').src, 'and boots at once');

  const offSw = swStub();
  const off = buildPage({serviceWorker: offSw, search: '?coi=0'});
  await turn(); off.settle();
  check(offSw.unregistered === 1 && offSw.registered.length === 0 && off.reloads.length === 0,
        '?coi=0 unregisters and stays on the slow path',
        `${offSw.unregistered} unregistered, ${offSw.registered.length} registered`);
  check(!!off.el('mac-iframe').src, 'and still boots the emulator');
}

// ---- 13. what the service worker itself does -------------------------------
/* Driven directly, with a stub of the worker's own global scope: it is a
   separate file with no DOM, and the danger in it is not what it adds but what
   it touches. explorer.html and canvas.html share its scope -- /cythera/ is as
   narrow as a worker registered from mobile.html can be -- and require-corp on
   either of those would break a ?src=<url> fetch that has no CORP header. So
   the assertion that matters is that it declines to respond to anything but a
   navigation to mobile.html. */
{
  const swPath = 'coi-serviceworker.js';
  if (!existsSync(swPath)) {
    fail('coi-serviceworker.js', 'not found beside mobile.html');
  } else {
    const listeners = new Map();
    const scope = {
      addEventListener(t, fn) { (listeners.get(t) || listeners.set(t, []).get(t)).push(fn); },
      skipWaiting: () => { scope.skipped = true; },
      clients: {claim: () => Promise.resolve()},
      Headers: class { constructor(h) { this.m = new Map(Object.entries(h || {})); }
                       set(k, v) { this.m.set(k.toLowerCase(), v); }
                       get(k) { return this.m.get(k.toLowerCase()); } },
      Response: class { constructor(body, init) { Object.assign(this, init); this.body = body; } },
      fetch: req => Promise.resolve({status: 200, statusText: 'OK', type: 'basic',
                                     body: 'BODY:' + req.url, headers: {'content-type': 'text/html'}}),
      console: {log() {}, warn() {}},
      URL, Promise, Object, Error,
    };
    scope.self = scope;
    vm.createContext(scope);
    let threw = null;
    try { new vm.Script(readFileSync(swPath, 'utf8'), {filename: swPath}).runInContext(scope); }
    catch (e) { threw = e; }
    check(!threw, 'the service worker parses and installs', threw ? threw.message : 'clean');

    for (const fn of listeners.get('install') || []) fn({});
    check(scope.skipped === true, 'it takes over immediately (skipWaiting)');

    const fetchOf = (url, mode) => {
      let answered = null;
      const event = {request: {url, mode: mode || 'navigate'},
                     respondWith(p) { answered = p; }};
      for (const fn of listeners.get('fetch') || []) fn(event);
      return answered;
    };
    const untouched = [
      ['https://e-z-g.github.io/cythera/explorer.html', 'navigate'],
      ['https://e-z-g.github.io/cythera/canvas.html', 'navigate'],
      ['https://e-z-g.github.io/cythera/js/mac-hfs.js', 'no-cors'],
      ['https://infinitemac.org/embed?disk=Mac%20OS%207.6', 'navigate'],
    ];
    const meddled = untouched.filter(([u, m]) => fetchOf(u, m) !== null).map(([u]) => u);
    check(meddled.length === 0, 'it leaves every other request alone', meddled.join(', ') || '4 checked');

    const answered = fetchOf('https://e-z-g.github.io/cythera/mobile.html', 'navigate');
    check(answered !== null, 'it answers the navigation to mobile.html');
    if (answered) {
      const res = await answered;
      const coop = res.headers && res.headers.get('cross-origin-opener-policy');
      const coep = res.headers && res.headers.get('cross-origin-embedder-policy');
      check(coop === 'same-origin' && coep === 'require-corp',
            'with the two headers cross-origin isolation needs',
            `COOP ${coop}, COEP ${coep}`);
      check(res.body === 'BODY:https://e-z-g.github.io/cythera/mobile.html',
            'and the page itself, unchanged');
    }
  }
}

// The iframe has to be allowed to inherit the isolation, or the headers above
// buy nothing: an embedded document is only isolated if its embedder both is
// isolated and says so on the frame.
check(/<iframe[^>]*allow="[^"]*cross-origin-isolated/.test(html),
      'the emulator iframe is allowed to inherit the isolation');

console.log(failures ? `\nFAIL — ${failures} problem(s)` : '\ninput check: clean');
process.exit(failures ? 1 : 0);
