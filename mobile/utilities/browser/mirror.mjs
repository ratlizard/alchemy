// A caching mirror of infinitemac.org, for a browser that cannot reach the
// internet. Not needed where it can -- leave $MIRROR unset and the pages talk
// to the real thing.
//
// It exists because of one specific sandbox: every host reset inside Chromium
// while `curl` fetched the same URL without complaint. So this fetches with
// curl on the browser's behalf and caches what comes back, which also makes the
// second run of a driver much faster than the first -- infinite-mac ships tens
// of megabytes of wasm and disk chunks.
//
// Two things here were paid for the hard way and must not be simplified away:
//
//  * **The method is forwarded.** infinite-mac PUTs `/CD-ROM/<base64>` to tell
//    its own worker which image to serve; a mirror that turned that into a GET
//    got `Malformed CD-ROM src chunk` back from the far end, which reads like a
//    bug in the disk image rather than in the proxy.
//  * **The method is part of the cache key**, for the same reason -- a cached
//    GET must not answer a PUT.
//
// The isolation headers are rewritten rather than passed through: the emulator
// needs a SharedArrayBuffer, which needs the embedding document to be
// cross-origin isolated, which needs everything it loads to opt in with CORP.
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFile } from 'node:child_process';

const UPSTREAM = process.env.UPSTREAM || 'https://infinitemac.org';
const PORT = Number(process.env.PORT || 9000);
const CACHE = process.env.MIRROR_CACHE || path.join(os.tmpdir(), 'infinite-mac-mirror');
fs.mkdirSync(CACHE, { recursive: true });

// Hop-by-hop headers, and the two isolation headers this sets for itself.
const DROP = new Set(['content-encoding', 'content-length', 'transfer-encoding',
                      'connection', 'cross-origin-opener-policy',
                      'cross-origin-embedder-policy']);

function fetchUpstream(url, range, method) {
    return new Promise((resolve, reject) => {
        const key = crypto.createHash('sha1')
            .update(`${method || 'GET'}|${url}|${range || ''}`).digest('hex');
        const body = path.join(CACHE, key + '.body');
        const head = path.join(CACHE, key + '.head');
        if (fs.existsSync(body) && fs.existsSync(head))
            return resolve({ head: fs.readFileSync(head, 'utf8'), body: fs.readFileSync(body) });
        const args = ['-sS', '-L', '-D', head, '-o', body, '-X', method || 'GET', url];
        if (range) args.push('-H', 'Range: ' + range);
        execFile('curl', args, { maxBuffer: 1 << 28 }, err => {
            if (err) return reject(err);
            resolve({ head: fs.readFileSync(head, 'utf8'), body: fs.readFileSync(body) });
        });
    });
}

http.createServer(async (req, res) => {
    try {
        const { head, body } = await fetchUpstream(UPSTREAM + req.url, req.headers.range, req.method);
        // -L means the dump can hold several header blocks; the last is the one
        // that produced the body.
        const blocks = head.split(/\r?\n\r?\n/).filter(b => b.trim());
        const lines = blocks[blocks.length - 1].split(/\r?\n/);
        const status = parseInt(lines[0].split(' ')[1], 10) || 200;
        const out = {};
        for (const line of lines.slice(1)) {
            const i = line.indexOf(':');
            if (i < 0) continue;
            const k = line.slice(0, i).trim().toLowerCase();
            if (DROP.has(k)) continue;
            out[k] = line.slice(i + 1).trim();
        }
        out['cross-origin-opener-policy'] = 'same-origin';
        out['cross-origin-embedder-policy'] = 'credentialless';
        out['cross-origin-resource-policy'] = 'cross-origin';
        out['access-control-allow-origin'] = '*';
        res.writeHead(status, out);
        res.end(body);
    } catch (e) {
        res.writeHead(502);
        res.end(String(e).slice(0, 400));
    }
}).listen(PORT, () => console.log(`mirroring ${UPSTREAM} on http://localhost:${PORT}`));
