// Serve mobile.html and coi-serviceworker.js over http, because a service
// worker cannot register from a file:// origin and the whole point of that
// worker is to give the page the two headers GitHub Pages will not send.
// Without it the emulator falls back to message passing and runs about thirty
// times slower, which expires every timeout in the drivers.
//
// $MIRROR rewrites EMBED_ORIGIN in the copy of the page it serves, for the
// sandbox where the browser cannot reach infinitemac.org (see mirror.mjs).
// Unset, the page is served exactly as committed.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const PORT = Number(process.env.PORT || 8123);
const MIRROR = process.env.MIRROR || '';
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' };

http.createServer((req, res) => {
    const name = new URL(req.url, 'http://x').pathname.replace(/^\/+/, '') || 'mobile.html';
    // Only the two files the page needs. Serving the whole tree would let a
    // driver test something other than what is committed.
    if (!['mobile.html', 'coi-serviceworker.js'].includes(name)) {
        res.writeHead(404); res.end('not served: ' + name); return;
    }
    fs.readFile(path.join(ROOT, name), (err, buf) => {
        if (err) { res.writeHead(404); res.end(String(err)); return; }
        let body = buf;
        if (MIRROR && name === 'mobile.html')
            body = Buffer.from(buf.toString('utf8').split('https://infinitemac.org').join(MIRROR));
        res.writeHead(200, { 'content-type': TYPES[path.extname(name)] || 'application/octet-stream' });
        res.end(body);
    });
}).listen(PORT, () => console.log(`mobile.html on http://localhost:${PORT}/mobile.html`
    + (MIRROR ? ` (embedding ${MIRROR})` : '')));
