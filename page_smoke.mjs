// Execute the page's script top to bottom under a stub document, so a
// declaration used before its line (a TDZ ReferenceError, which killed the
// touch pads on a phone) fails here and not there. Parsing alone did not
// catch it. Usage: node page_smoke.mjs
import { readFileSync } from 'node:fs';
const html = readFileSync(new URL('./index.html', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];
const ids = [...html.matchAll(/ id="([^"]+)"/g)].map(m => m[1]);
const el = id => ({ id, style: {}, dataset: {}, classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
  addEventListener() {}, setPointerCapture() {}, getBoundingClientRect() { return { left: 0, top: 0, width: 640, height: 480 }; },
  getContext() { return { putImageData() {}, clearRect() {} }; }, querySelector() { return el('q'); }, querySelectorAll() { return []; },
  appendChild() {}, focus() {}, blur() {}, click() {}, set textContent(v) {}, set innerHTML(v) {}, hidden: true, disabled: false, value: '', checked: false, files: [] });
const document = { getElementById: id => { if (!ids.includes(id)) throw new Error(`no element #${id}`); return el(id); }, querySelectorAll: () => [], addEventListener() {}, createElement: () => el('new'), body: el('body'), activeElement: null, hidden: false };
const window = { addEventListener() {}, innerWidth: 393, innerHeight: 700, AudioContext: function () {} };
const stubs = { document, window, matchMedia: () => ({ matches: true }), localStorage: { getItem: () => null, setItem() {} }, navigator: { storage: {} },
  location: { search: '' }, requestAnimationFrame() {}, setInterval() {}, setTimeout: () => 0, clearTimeout() {}, performance: { now: () => 0 },
  WebAssembly: { instantiateStreaming: () => new Promise(() => {}), instantiate: () => new Promise(() => {}) }, fetch: () => new Promise(() => {}),
  indexedDB: { open: () => ({}) }, TextDecoder, TextEncoder, URLSearchParams, console, Math, JSON, Object, Number, String, Array, Promise, Error, Map, Set, Uint8Array, Uint8ClampedArray, DataView, Blob: function () {}, URL, confirm: () => false, alert() {} };
const fn = new Function(...Object.keys(stubs), script.replace(/^\s*'use strict';/, ''));
fn(...Object.values(stubs));
console.log('page script ran to the end');
