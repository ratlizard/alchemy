// Drop a .dsk exported by explorer.html onto mobile.html's emulator, press the
// page's own Install button, and then take Cythera as far as the opening
// narration -- which is where an edit made in the viewer becomes visible in the
// game's own voice. Every click goes through mobile.html's own pointer path, so
// anything that works here works for a finger.
//
// Read utilities/browser/README.md before changing the waits or the
// coordinates; both are the residue of an afternoon.
//
//   DSK=edited.dsk node utilities/browser/install_and_play.cjs
//
// Environment: PAGE (default http://localhost:8123/mobile.html), DSK, OUT (a
// directory for the screenshots), and BOOT_MS / INSTALL_MS if the machine this
// runs on is slower than the one the defaults were measured on.
const { chromium } = require(process.env.PLAYWRIGHT
    || '/opt/node22/lib/node_modules/playwright/index.js');
const fs = require('fs');
const path = require('path');

const PAGE = process.env.PAGE || 'http://localhost:8123/mobile.html';
const DSK = process.env.DSK || 'edited.dsk';
const OUT = process.env.OUT || '.';
const BOOT_MS = Number(process.env.BOOT_MS || 45000);
const INSTALL_MS = Number(process.env.INSTALL_MS || 70000);

// Where the game's own buttons are, in the emulated screen's pixels. Cythera
// runs at 640x480; these are converted to browser coordinates at run time, so
// the window may be any size. See the table in README.md.
const NEW_GAME = [244, 195];
const ARCHETYPE_OK = [414, 414];

(async () => {
    const browser = await chromium.launch({
        executablePath: process.env.CHROME || '/opt/pw-browsers/chromium-1194/chrome-linux/chrome',
        args: ['--no-sandbox', '--enable-unsafe-swiftshader'],
    });
    // 800x600 is not arbitrary. mobile.html asks for a tall screen and
    // infinite-mac gives back one that fits the window, so the emulated screen
    // ends up 640 x (viewportHeight * 640 / viewportWidth) -- at 800x600 that
    // is exactly 640x480, which is the size Cythera's own layout was measured
    // at. Change the viewport and the emulated screen changes with it: the
    // start screen re-centres on the taller desktop and every coordinate below
    // moves. The check after boot says so out loud rather than missing quietly.
    const ctx = await browser.newContext({
        viewport: { width: 800, height: 600 }, deviceScaleFactor: 1, hasTouch: true });
    const page = await ctx.newPage();

    let shots = 0;
    const shot = async label => {
        const name = path.join(OUT, String(shots++).padStart(2, '0') + '-' + label + '.png');
        // The HUD floats over the emulated screen; hide it for the photograph
        // only, so the input path under test is never the one being changed.
        await page.evaluate(() => { document.getElementById('hud-layer').style.visibility = 'hidden'; });
        await page.waitForTimeout(250);
        await page.screenshot({ path: name });
        await page.evaluate(() => { document.getElementById('hud-layer').style.visibility = ''; });
        console.log('  shot', name);
        return name;
    };

    // #scale-target *is* the emulated screen: its style width/height are what
    // the emulator reported, and its rect carries the fit-to-window scale. That
    // is enough to go the other way, from an emulated pixel to a browser one.
    const toBrowser = ([ex, ey]) => page.evaluate(([x, y]) => {
        const el = document.getElementById('scale-target');
        const r = el.getBoundingClientRect();
        // One scale for both axes -- the page fits the screen uniformly -- so
        // the width alone gives it, and a wrong height cannot skew a click.
        const k = r.width / (parseFloat(el.style.width) || r.width);
        return { x: r.left + x * k, y: r.top + y * k };
    }, [ex, ey]);

    const emulatedSize = () => page.evaluate(() => {
        const el = document.getElementById('scale-target');
        return [parseFloat(el.style.width), parseFloat(el.style.height)];
    });

    // A press and a release with nothing between is not a click as far as the
    // Toolbox is concerned -- Cythera's own buttons take it about half the
    // time, and the Standard File dialog's TrackControl almost never. Hold it.
    async function click(emu, hold = 350) {
        const p = await toBrowser(emu);
        await page.mouse.move(p.x, p.y);
        await page.waitForTimeout(200);
        await page.mouse.down();
        await page.waitForTimeout(hold);
        await page.mouse.up();
    }

    // How much of the middle of the frame is near-white. A whole-canvas
    // fingerprint cannot work here: the start screen animates a torch, so it
    // changes every frame whether or not anything was clicked. This separates
    // the dark red start screen from the big light Standard File dialog.
    const lightness = () => frame().evaluate(() => {
        const c = document.querySelector('canvas');
        const g = c.getContext('2d');
        const d = g.getImageData(Math.round(c.width * 0.2), Math.round(c.height * 0.08),
                                 Math.round(c.width * 0.6), Math.round(c.height * 0.35)).data;
        let light = 0;
        for (let i = 0; i < d.length; i += 4)
            if (d[i] > 170 && d[i + 1] > 170 && d[i + 2] > 170) light++;
        return light / (d.length / 4);
    });
    const frame = () => page.frames().find(f => f !== page.mainFrame() && /\/\/[^/]+\//.test(f.url()));

    async function clickUntil(emu, want, label, tries = 4, settle = 15000) {
        for (let i = 1; i <= tries; i++) {
            await click(emu);
            await page.waitForTimeout(settle);
            const l = await lightness();
            console.log(`  ${label}: click ${i} -> lightness ${l.toFixed(3)}`);
            if (want(l)) return true;
            // The first click of a session can be spent activating Cythera:
            // after the installer runs, AppleScript's own application is
            // frontmost. So this retries rather than assuming one is enough.
        }
        return false;
    }

    await page.goto(PAGE, { waitUntil: 'load', timeout: 120000 });
    await page.waitForFunction(
        () => document.getElementById('start-status')?.textContent?.includes('Ready'),
        null, { timeout: 240000 });
    await page.locator('#start-overlay').dispatchEvent('pointerdown');
    await page.waitForTimeout(BOOT_MS);
    const [ew, eh] = await emulatedSize();
    console.log(`  booted; the emulated screen is ${ew}x${eh}`);
    if (ew !== 640 || eh !== 480)
        console.log('  ! expected 640x480 -- the coordinates below were measured there,\n'
                  + '    and Cythera re-centres its start screen on a screen of another shape');

    // Dropping onto infinite-mac's own document is the supported way in; the
    // embed API's load_disk takes a URL its worker fetches server-side, which
    // no local file can satisfy. See CLAUDE.md.
    await frame().evaluate(([name, bytes]) => {
        const dt = new DataTransfer();
        dt.items.add(new File([new Uint8Array(bytes)], name));
        const target = document.querySelector('.ScreenFrame');
        for (const type of ['dragenter', 'dragover', 'drop'])
            target.dispatchEvent(new DragEvent(type, { bubbles: true, cancelable: true, dataTransfer: dt }));
    }, [path.basename(DSK), Array.from(fs.readFileSync(DSK))]);
    console.log('  dropped', DSK, '(' + fs.statSync(DSK).size + ' bytes)');
    await page.waitForTimeout(16000);

    await page.locator('#install-btn').dispatchEvent('pointerdown');
    await page.waitForTimeout(600);
    await page.locator('#modal-ok').dispatchEvent('pointerdown');
    console.log('  Install pressed -- keystrokes only, no pointer');
    // There is no completion signal to wait for, only the clock: the script
    // copies 5.6MB between disks and then launches the game.
    await page.waitForTimeout(INSTALL_MS);
    await shot('start-screen');

    if (!await clickUntil(NEW_GAME, l => l > 0.20, 'New Game')) {
        await shot('no-create-player');
        console.log('  the Create Player dialog never appeared; see the shots');
        await browser.close();
        process.exit(1);
    }
    await shot('create-player');

    // Save is the dialog's default button, and Return is what a person presses.
    await page.evaluate(() => { pressKey('Enter'); });
    await page.waitForTimeout(120);
    await page.evaluate(() => { releaseKey('Enter'); });
    await page.waitForTimeout(16000);
    await shot('archetype');

    await click(ARCHETYPE_OK);          // take the Explorer archetype as offered
    await page.waitForTimeout(15000);
    const last = await shot('narration');
    console.log('\n  the opening dream is in ' + last + ' -- read the text off it');
    await browser.close();
})();
