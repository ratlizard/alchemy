/* Cross-origin isolation for mobile.html, and nothing else.
   =========================================================================

   WHAT THIS IS FOR

   infinite-mac runs its emulator in a Worker and talks to it through a
   SharedArrayBuffer. `SharedArrayBuffer` only exists in a document that is
   *cross-origin isolated*, and when it is missing the emulator falls back to
   a message-passing path that its own console warns about:

       SharedArrayBuffer is not available, fallback more will be used.
       Performance may be degraded.

   "Degraded" is an understatement. Measured on the real emulator (Mac OS 7.6
   on a Quadra 650, All Out, reading `currentIPMS` out of the emulator's own
   stats over a 30-second window after boot):

       SharedArrayBuffer available     56,233 instructions/ms
       fallback                         1,896 instructions/ms

   That is 3.4% of the speed -- about thirty times slower -- and it is the
   whole of the difference between this page and playing the same machine on
   infinitemac.org directly. Two other suspects were measured at the same time
   and are innocent: `screen_update_messages` costs nothing detectable (1,896
   with it on against 1,892 with it off), and neither does the tall emulated
   screen this page asks for (640x1385 against 640x480 was within noise).

   WHY A SERVICE WORKER

   Cross-origin isolation is granted by two response headers on the *document*:

       Cross-Origin-Opener-Policy:   same-origin
       Cross-Origin-Embedder-Policy: require-corp

   and it is inherited: an iframe is only isolated if its embedder is. So it is
   not enough that infinitemac.org sends them (it does, along with
   Cross-Origin-Resource-Policy: cross-origin, which is what lets a
   require-corp parent embed it). This page has to send them too, and this page
   is served by GitHub Pages, which offers no way to set a header on a file.

   A service worker is the way out, because it can re-serve its own origin's
   responses with headers added. That is the whole of this file. The technique
   is gzuidhof/coi-serviceworker's; the code is not, and the scoping below is
   deliberately narrower than that project's.

   WHY IT ONLY TOUCHES mobile.html

   A service worker registered from /cythera/mobile.html has /cythera/ for its
   scope -- there is no way to narrow that without moving the script into a
   subdirectory, and then it would not cover mobile.html at all. So it is in
   scope for explorer.html and canvas.html as well, and it must be harmless to
   them: `require-corp` means every subresource of a document needs CORP or
   CORS, and explorer.html can be pointed at an arbitrary `?src=<url>`, which
   would then start failing. A fetch handler that returns without calling
   respondWith leaves the request entirely alone, so those two pages go over
   the network exactly as they did before, and only mobile.html -- which loads
   no subresource at all except the emulator iframe -- gets the headers.

   Nothing here is required for the page to work. If service workers are
   unavailable (a `file://` copy on a USB stick, a browser with them disabled,
   a private window) the registration in mobile.html fails quietly and the page
   runs on the slow path, as it did before this file existed.

   Part of https://github.com/ratlizard/alchemy -- GPL-3.0-or-later.
*/

self.addEventListener('install', () => self.skipWaiting());

// claim() so the very first visit can reload straight into a controlled page
// instead of waiting for the tab to be closed and reopened.
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', event => {
    const request = event.request;
    // Only a top-level navigation to mobile.html. Anything else -- a
    // subresource, another page in the scope, a cross-origin request -- falls
    // through untouched, which is what returning without respondWith means.
    if (request.mode !== 'navigate') return;
    let path;
    try { path = new URL(request.url).pathname; } catch (e) { return; }
    if (!path.endsWith('/mobile.html') && path !== '/mobile.html') return;

    event.respondWith((async () => {
        const response = await fetch(request);
        // An opaque response has no readable headers or body to copy; hand it
        // back as it came rather than turning it into an error.
        if (response.status === 0 || response.type === 'opaque' ||
            response.type === 'opaqueredirect') return response;
        const headers = new Headers(response.headers);
        headers.set('Cross-Origin-Opener-Policy', 'same-origin');
        // require-corp rather than credentialless: Safari is the browser this
        // page exists for, and Safari does not implement credentialless.
        headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
        return new Response(response.body, {
            status: response.status,
            statusText: response.statusText,
            headers: headers,
        });
    })());
});
