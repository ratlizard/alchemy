// The minimal DOM that the non-UI harnesses evaluate a page's script body in.
//
// Three harnesses -- decoder_snapshot.mjs, loader_test.mjs and export_test.mjs
// -- each carried their own copy of this stub, near-identical but drifted in
// small ways (one memoised getElementById, two did not; one stubbed
// URL.createObjectURL, two took the real URL). All three shared one flaw:
// getContext() returned null.
//
// That was harmless for years, because none of these three checks look at
// pixels. Then explorer.html gained the animated colour-cycling
// footer scene, which asks for a 2D context while the script body is still
// being evaluated -- so all three began failing at load with
//
//   FATAL: script body threw while loading:
//   Cannot read properties of null (reading 'createImageData')
//
// while viewer_smoke.mjs kept passing, because its fuller stub has a real
// context. Three copies meant one fix had to be made three times, so instead
// there is now one canvas here and the copies are gone.
//
// The context is deliberately thin. It remembers the last putImageData and
// hands it back from getImageData, which is the whole of what the decoders
// need: they write pixels in and read them out again. Everything that would
// actually rasterise -- drawImage, fillText, the gradients -- is a no-op,
// because nothing in these harnesses inspects what a browser would have drawn.
// If a check ever needs that, it wants viewer_smoke.mjs's stub, not this one.

const noop = () => {};

// A page that loads successfully starts its animation loops, and a repeating
// timer keeps Node's event loop alive forever -- so a harness that had finished
// all its assertions and printed its result would still never exit. (This only
// began to bite once the stub gained a canvas: before that the viewer's footer
// scene threw during load and no timer was ever registered.) Unref'ing the
// handle lets the process end when the harness is done, while the timer still
// fires normally for as long as there is other work. setTimeout is deliberately
// left alone: a one-shot is often the thing being waited for -- export_test's
// toBlob resolves through one -- and unref'ing those would let the process exit
// before they fired.
function keepaliveFreeInterval(fn, ms, ...rest) {
  const h = setInterval(fn, ms, ...rest);
  if (h && typeof h.unref === 'function') h.unref();
  return h;
}

// A 2D context good enough to decode into. viewer_smoke.mjs imports this too,
// so there is one canvas implementation rather than two that can disagree.
export function makeCanvasContext(cv) {
  const im = (w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(Math.max(0, w * h * 4)) });
  return {
    canvas: cv, fillStyle: '#000', strokeStyle: '#000', lineWidth: 1, font: '', textAlign: 'left',
    globalAlpha: 1, globalCompositeOperation: 'source-over', imageSmoothingEnabled: true,
    createImageData: (w, h) => im(w, h),
    // putImageData adopts the image's size when the canvas has not been given
    // one, and fillRect and drawImage actually write pixels. That is not
    // gold-plating: the classic-Mac colour-table decoders (clut, actb, dctb,
    // mctb, pltt) draw their swatch grids with fillRect and nothing else, so
    // against a no-op they all hashed to the same empty buffer and two
    // different palettes looked identical. utilities/rsrc_sandbox.mjs used to
    // carry a second canvas that did this, and the two disagreeing was the
    // whole reason there is one implementation here.
    putImageData(d) { cv._px = d; cv.width = cv.width || d.width; cv.height = cv.height || d.height; },
    getImageData(x, y, w, h) { return cv._px || im(w, h); },
    drawImage(src) { if (src && src._px) cv._px = src._px; },
    fillRect(x, y, w, h) {
      if (!cv.width || !cv.height) return;
      if (!cv._px) cv._px = im(cv.width, cv.height);
      const m = /rgb\((\d+),\s*(\d+),\s*(\d+)\)/.exec(this.fillStyle) || [];
      const [r, g, b] = [+m[1] || 0, +m[2] || 0, +m[3] || 0];
      for (let yy = Math.max(0, y); yy < y + h && yy < cv.height; yy++)
        for (let xx = Math.max(0, x); xx < x + w && xx < cv.width; xx++) {
          const o = (yy * cv.width + xx) * 4;
          cv._px.data[o] = r; cv._px.data[o + 1] = g; cv._px.data[o + 2] = b; cv._px.data[o + 3] = 255;
        }
    },
    clearRect() {}, strokeRect() {},
    beginPath() {}, closePath() {}, moveTo() {}, lineTo() {}, arc() {}, stroke() {}, fill() {},
    save() {}, restore() {}, translate() {}, scale() {}, setTransform() {}, setLineDash() {},
    fillText() {}, strokeText() {}, measureText: t => ({ width: String(t).length * 6 }),
    createRadialGradient: () => ({ addColorStop() {} }),
    createLinearGradient: () => ({ addColorStop() {} }),
    createPattern: () => null, clip() {}, rect() {}, quadraticCurveTo() {}, ellipse() {},
  };
}

// One element type stands in for every tag. It answers whatever the page asks
// of it without recording anything, except the 2D context, which is made once
// and kept so that a decode-then-read round trip on the same element works.
export function makeElement() {
  const el = {
    style: {}, classList: { add: noop, remove: noop, toggle: noop, contains: () => false },
    children: [], options: [], dataset: {}, disabled: false,
    innerHTML: '', textContent: '', value: '', width: 0, height: 0,
    // A text node appended to an element lands in its textContent, which is
    // how the viewer writes its status line (createTextNode + appendChild)
    // and how loader_test reads it back.
    appendChild(c) { if (c && c.nodeType === 3) this.textContent += c.textContent; return c; },
    removeChild: noop, addEventListener: noop,
    removeEventListener: noop, setAttribute: noop, getAttribute: () => null,
    querySelector: () => null, querySelectorAll: () => [], focus: noop, click: noop,
    remove: noop, insertBefore: noop, cloneNode: () => makeElement(),
    // <audio>. stopAllViewActivity() pauses whatever a view left playing, so
    // every harness that changes view reaches these even when it never made a
    // sound: without them the first render of a view that has audio in it
    // throws "audio.pause is not a function" before drawing anything.
    paused: true, currentTime: 0, src: '',
    play() { this.paused = false; return Promise.resolve(); },
    pause() { this.paused = true; },
    getContext(kind) {
      if (kind !== '2d') return null;
      return this._ctx || (this._ctx = makeCanvasContext(this));
    },
    toDataURL: () => 'data:image/png;base64,',
    getBoundingClientRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
  };
  return el;
}

// memoiseIds: return the same element for a repeated getElementById. loader_test
//   needs this -- it sets a value on an element and expects the page to read it
//   back -- and it is closer to a real document, so it is the default.
// objectUrls: stub URL.createObjectURL rather than passing Node's real URL.
//   export_test needs it, since it catches saves instead of performing them.
export function makeSandbox({ memoiseIds = true, objectUrls = false } = {}) {
  const els = new Map();
  const document = {
    getElementById: memoiseIds
      ? id => (els.has(id) ? els.get(id) : els.set(id, makeElement()).get(id))
      : () => makeElement(),
    createElement: () => makeElement(),
    createTextNode: t => ({ nodeType: 3, textContent: String(t), data: String(t) }),
    createElementNS: () => makeElement(), querySelector: () => null, querySelectorAll: () => [],
    addEventListener: noop, removeEventListener: noop,
    body: makeElement(), head: makeElement(), documentElement: makeElement(),
  };
  const sandbox = {
    document, console, TextDecoder, TextEncoder, Uint8Array, Int16Array, Uint32Array,
    Float32Array, Uint8ClampedArray, ArrayBuffer, DataView, Math, JSON, Map, Set, Date,
    Object, Array, String, Number, Boolean, Error, RegExp, Promise, isNaN, parseInt,
    parseFloat, Infinity, NaN, undefined, URLSearchParams,
    setTimeout, clearTimeout, setInterval: keepaliveFreeInterval, clearInterval,
    requestAnimationFrame: noop, cancelAnimationFrame: noop,
    fetch: () => Promise.reject(new Error('offline')),
    Blob: globalThis.Blob,
    URL: objectUrls ? { createObjectURL: () => 'blob:stub', revokeObjectURL: noop } : globalThis.URL,
    CompressionStream: globalThis.CompressionStream, Response: globalThis.Response,
    matchMedia: () => ({ matches: false, addEventListener: noop }),
    localStorage: { getItem: () => null, setItem: noop, removeItem: noop },
    location: { hash: '', href: 'file:///x', search: '' },
    history: { replaceState: noop, pushState: noop },
    navigator: { userAgent: 'node' },
    performance: { now: () => 0 },
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  sandbox.self = sandbox;
  return { sandbox, els, document };
}
