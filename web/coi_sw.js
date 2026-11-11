// Cross-origin isolation for the viewer on a host that sets no headers (GitHub Pages), 2026-11-11.
//
// The viewer's threaded module (insiliscope_module_mt.js: Emscripten pthreads) needs shared memory, which a browser
// gives only to a cross-origin isolated page: one served with
//   Cross-Origin-Opener-Policy: same-origin
//   Cross-Origin-Embedder-Policy: require-corp
// This service worker adds those two headers to the viewer page itself (its directory's index) and to nothing else:
// the docs and every other page and file pass through as they are. Under them the page may load cross-origin files
// only if they allow it (the usage counts' script does not: it is left out there; docs/privacy.md). The page registers
// it (web/index.html, `iscCoi`) and reloads once so it is served through it; without it (a browser or window with no
// service workers) the viewer runs its single-threaded module and says so.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));

function isViewerPage(url) {
  const scope = new URL(self.registration.scope);
  return url.origin === scope.origin && (url.pathname === scope.pathname || url.pathname === scope.pathname + 'index.html');
}

self.addEventListener('fetch', e => {
  const r = e.request;
  if (r.mode !== 'navigate' || !isViewerPage(new URL(r.url))) return;   // (everything else: the network, untouched)
  e.respondWith(fetch(r).then(res => {
    if (res.status === 0 || res.type === 'opaqueredirect') return res;
    const h = new Headers(res.headers);
    h.set('Cross-Origin-Opener-Policy', 'same-origin');
    h.set('Cross-Origin-Embedder-Policy', 'require-corp');
    return new Response(res.body, { status: res.status, statusText: res.statusText, headers: h });
  }));
});
