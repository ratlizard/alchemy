// Drive explorer.html for real: load an archive, change one word of a resource
// through the page's own Edit Bytes, and press the page's own disk-image export
// button. Nothing here reaches inside the page except to read what it is
// showing -- the edit goes into the textarea and the download comes out of the
// button, so what this produces is what a visitor would produce.
//
//   RESID=240 NEEDLE=summer REPLACE=bummer \
//       SRC=http://localhost:8200/reference/CytheraData.bin \
//       node utilities/browser/edit_and_export.cjs
//
// The replacement must be the same length as the needle. That is not a
// limitation of the page -- writeDelverArchive is happy to resize a resource --
// but a same-length edit keeps this driver's arithmetic trivial and makes the
// result easy to check by eye.
//
// The resource ids worth knowing: 0x0240 is the opening dream, whose first
// sentence the game narrates before anything else happens, which makes it the
// cheapest thing in the archive to prove an edit with. See
// utilities/browser/README.md and CLAUDE.md.
const { chromium } = require(process.env.PLAYWRIGHT
    || '/opt/node22/lib/node_modules/playwright/index.js');
const fs = require('fs');
const path = require('path');

const SRC = process.env.SRC || 'http://localhost:8200/reference/CytheraData.bin';
const BASE = process.env.BASE || 'http://localhost:8200/explorer.html';
const RESID = parseInt(process.env.RESID || '240', 16);
const NEEDLE = process.env.NEEDLE || 'summer';
const REPLACE = process.env.REPLACE || 'bummer';
const OUT = process.env.OUT || 'edited.dsk';

if (NEEDLE.length !== REPLACE.length) {
    console.error('NEEDLE and REPLACE must be the same length');
    process.exit(2);
}

(async () => {
    const browser = await chromium.launch({
        executablePath: process.env.CHROME || '/opt/pw-browsers/chromium-1194/chrome-linux/chrome',
        args: ['--no-sandbox', '--enable-unsafe-swiftshader'],
    });
    const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 }, acceptDownloads: true });
    const page = await ctx.newPage();
    page.on('pageerror', e => console.log('  [page error]', e.message.slice(0, 200)));

    // ?cache=skip so a remembered copy cannot stand in for the file named here,
    // and the deep link opens the resource so Edit Bytes has something to edit.
    await page.goto(`${BASE}?cache=skip&src=${encodeURIComponent(SRC)}#${RESID.toString(16)}`,
                    { waitUntil: 'load', timeout: 120000 });
    // fileBytes is a top-level const, so it is not a property of window; ask
    // the page for it through its own scope.
    await page.evaluate(() => {
        window.__archiveSize = () => (typeof fileBytes !== 'undefined' && fileBytes) ? fileBytes.length : 0;
    });
    await page.waitForFunction(() => window.__archiveSize() > 1000000, null, { timeout: 180000 });
    console.log('  loaded', await page.evaluate(() => ({
        bytes: window.__archiveSize(),
        rsrc: window.CYTHERA_RSRC_RAW ? window.CYTHERA_RSRC_RAW.length : 0,
        finder: window.ARCHIVE_FINDER,
    })));

    await page.evaluate(r => { if (typeof currentResid === 'undefined' || !currentResid) previewResource(r); }, RESID);
    await page.waitForTimeout(1500);
    await page.locator('#editBytesBtn').click();
    await page.waitForTimeout(800);

    // The box holds the resource's PLAINTEXT as hex, whatever it is stored as
    // on disk; applyResourceEdit re-encrypts on the way back out.
    const before = await page.locator('#editBytesText').inputValue();
    const bytes = before.split(/\s+/).map(h => parseInt(h, 16));
    const text = bytes.map(b => (b >= 32 && b < 127) ? String.fromCharCode(b) : ' ').join('');
    const at = text.indexOf(NEEDLE);
    if (at < 0) {
        console.error(`  ${JSON.stringify(NEEDLE)} is not in 0x${RESID.toString(16)}; it begins`,
                      JSON.stringify(text.slice(0, 200)));
        await browser.close();
        process.exit(1);
    }
    console.log(`  replacing ${JSON.stringify(NEEDLE)} at byte ${at} with ${JSON.stringify(REPLACE)}`);
    for (let i = 0; i < REPLACE.length; i++) bytes[at + i] = REPLACE.charCodeAt(i);
    await page.locator('#editBytesText').fill(
        bytes.map((b, i) => b.toString(16).padStart(2, '0') + ((i % 16 === 15) ? '\n' : ' ')).join('').trimEnd());
    await page.locator('button:has-text("Apply")').first().click();
    // Apply rebuilds the WHOLE archive and re-enters parseArchiveBytes, so this
    // is not a quick operation and the size afterwards is expected to differ.
    await page.waitForTimeout(3000);
    console.log('  after the edit', await page.evaluate(() => ({
        bytes: window.__archiveSize(),
        dirty: window.EDITED_RESIDS ? window.EDITED_RESIDS.size : 0,
    })));

    await page.locator('#archiveMenuBtn').click();
    await page.waitForTimeout(600);
    const [download] = await Promise.all([
        page.waitForEvent('download', { timeout: 120000 }),
        page.locator('#exportEditedDskBtn').click(),
    ]);
    const out = path.resolve(OUT);
    await download.saveAs(out);
    console.log('  wrote', out, fs.statSync(out).size, 'bytes -',
                'now: DSK=' + OUT, 'node utilities/browser/install_and_play.cjs');
    await browser.close();
})();
