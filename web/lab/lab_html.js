// web/lab.html only (added by web/lab/serve.mjs after the viewer's own script): a badge saying the page runs on
// the JS reference, and a reload on every save (web/prototype, web/lab, web/index.html) that keeps the view and
// every control you changed. Only controls that differ from their HTML default are restored, so a default you
// edit in web/index.html shows up. Click the badge to forget the saved state. The About section gets a line with the
// checkout the page is served from: branch, commit, uncommitted files (/__lab/info).
(() => {
  const KEY = 'isc-lab-state';
  const controls = () => document.querySelectorAll('input[id], select[id]');
  const same = (a, b) => a === b || (a.trim() !== '' && b.trim() !== '' && +a === +b);  // '1' = '1.00'
  const isDefault = el => {
    if (el.type === 'checkbox') return el.checked === el.defaultChecked;
    if (el.tagName === 'SELECT') return el.selectedIndex === Math.max(0, [...el.options].findIndex(o => o.defaultSelected));
    return same(el.value, el.defaultValue);
  };

  function save() {
    const changed = {};
    for (const el of controls()) if (el.type !== 'file' && !isDefault(el)) changed[el.id] = el.type === 'checkbox' ? el.checked : el.value;
    try { sessionStorage.setItem(KEY, JSON.stringify({ changed, view })); } catch {}
  }
  function restore() {
    let s = null;
    try { s = JSON.parse(sessionStorage.getItem(KEY) || 'null'); } catch {}
    if (!s) return;
    for (const [id, v] of Object.entries(s.changed || {})) {
      const el = document.getElementById(id);
      if (!el) continue;
      if (el.type === 'checkbox') el.checked = v; else el.value = v;
      el.dispatchEvent(new Event('input'));
      el.dispatchEvent(new Event('change'));
    }
    if (s.view) Object.assign(view, s.view);
    requestDraw();
  }

  restore();
  addEventListener('pagehide', save); // F5 keeps the state too
  new EventSource('/__lab/events').onmessage = () => { save(); location.reload(); };

  document.title = 'insiliscope lab (JS)';
  const b = document.createElement('div');
  b.textContent = 'lab: JS backend';
  b.title = 'web/lab.html: the viewer on the JS reference (web/lab/engine.js -> web/prototype). Reloads on save, ' +
    'keeping the view and changed controls. Click to reset them.';
  b.style.cssText = 'position:fixed;top:6px;right:8px;z-index:1000;padding:2px 8px;border-radius:4px;cursor:pointer;' +
    'font:12px system-ui,sans-serif;background:#d97706;color:#fff;opacity:0.9';
  b.onclick = () => { try { sessionStorage.removeItem(KEY); } catch {} removeEventListener('pagehide', save); location.reload(); };
  document.body.appendChild(b);

  // About: the checkout this page is served from (read on every load, so a branch switch + reload shows).
  fetch('/__lab/info', { cache: 'no-store' }).then(r => r.json()).then(i => {
    const about = document.querySelector('details[data-group="about"]');
    if (!about || !i.sha) return;
    const tag = document.createElement('code');
    tag.textContent = `${i.head} @ ${i.sha}` + (i.changedFiles ? ` + ${i.changedFiles} uncommitted file${i.changedFiles > 1 ? 's' : ''}` : '');
    tag.style.cssText = 'padding:1px 6px;border-radius:4px;background:#d97706;color:#fff';
    const line = document.createElement('p');
    line.className = 'sub';
    line.append('Lab build: ', tag, ` ${i.subject}`);
    about.appendChild(line);
  }).catch(() => {});
})();
