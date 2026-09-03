#!/usr/bin/env node
// Collects the JavaScript a page actually runs, in document order.
//
// The three pages here used to be one HTML file each with one inline <script>,
// and every harness found that script with the same regular expression. Now
// that the code shared between them lives in js/*.js, "the script" is several
// files, and a harness that only reads the inline block silently tests a page
// with its decoders missing -- which fails in a confusing way rather than an
// obvious one.
//
//   import {collectPageScripts, pageSource} from './page_scripts.mjs';
//   const code = pageSource('explorer.html');   // ready to eval
//
// It also enforces the constraint that makes the split safe in the first
// place: these must be CLASSIC scripts. A `type="module"` script is fetched
// with CORS, and a page opened from file:// has an opaque origin, so a module
// would not load at all when the page is double-clicked instead of served.
// Finding one here is an error, not a warning.

import {readFileSync} from 'node:fs';
import {dirname, resolve} from 'node:path';

const SCRIPT_TAG = /<script\b([^>]*)>([\s\S]*?)<\/script>/gi;
const ATTR = /(\w[\w-]*)\s*=\s*"([^"]*)"/g;

export function collectPageScripts(htmlPath) {
  const html = readFileSync(htmlPath, 'utf8');
  const base = dirname(htmlPath);
  const sources = [];
  for (const m of html.matchAll(SCRIPT_TAG)) {
    const attrs = {};
    for (const a of m[1].matchAll(ATTR)) attrs[a[1].toLowerCase()] = a[2];
    if ((attrs.type || '').toLowerCase() === 'module') {
      throw new Error(
        `${htmlPath} has a <script type="module">. These pages have to keep working from ` +
        `file://, where module scripts are blocked by CORS. Use a classic <script src> instead.`);
    }
    if (attrs.src) {
      if (/^[a-z]+:\/\//i.test(attrs.src)) { sources.push({name: attrs.src, code: '', external: true}); continue; }
      const p = resolve(base, attrs.src);
      sources.push({name: attrs.src, code: readFileSync(p, 'utf8'), path: p});
    } else if (m[2].trim()) {
      sources.push({name: htmlPath + ' (inline)', code: m[2], inline: true});
    }
  }
  return {html, sources};
}

// One string, in order, with each file separated so a syntax error in one
// cannot swallow the next.
export function pageSource(htmlPath) {
  return collectPageScripts(htmlPath).sources.map(s => s.code).join('\n;\n');
}

// A one-line description for a harness to print, so it is obvious when a page
// is being tested with a file missing.
export function describeScripts(htmlPath) {
  const {sources} = collectPageScripts(htmlPath);
  return sources
    .map(s => `${s.name.replace(/^.*\//, '')} ${Math.round(s.code.length / 1024)}k`)
    .join(', ');
}
