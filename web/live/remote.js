// SPDX-License-Identifier: BSD-3-Clause
// The remote server (docs/remote.md): ☰ › Remote server… shows the one command that starts it over SSH (with a tunnel
// from this computer's port to the server's socket); once it answers, the live, movie and ground-truth engines send their
// heavy jobs there (web/live/remote_proxy.js in each worker, cli/remote_server.cpp on the server). The page probes the
// port every few seconds while "use a remote server" is on; it stays local until the server answers, and goes back to
// local when the connection is lost (the live sample's light replayed on the new engine: L.rebind).
(function () {
  const $ = id => document.getElementById(id);
  const KEY = 'isc.remote', REPO = 'https://raw.githubusercontent.com/kjamartens/inSiliScope';
  const PROTOCOL = 1, ABI = 21;
  const load = () => { try { return JSON.parse(localStorage.getItem(KEY) || '{}') || {}; } catch (e) { return {}; } };
  const save = () => { try { localStorage.setItem(KEY, JSON.stringify(cfg)); } catch (e) { /* private window */ } };
  const hex = n => Array.from(crypto.getRandomValues(new Uint8Array(n)), b => b.toString(16).padStart(2, '0')).join('');
  // enabled: use a remote server; server: the ssh name; port: this computer's end of the tunnel; threads: per engine
  // (empty: half the server's CPUs, the launcher's default); scope: 'all' (live included) | 'records'; runThrough: a
  // scheduler's command (srun ...); token, sock: this browser's own (the socket /tmp/isc-<sock>/s, the token in the URL).
  const cfg = Object.assign({ enabled: false, server: '', port: 18765, threads: '', scope: 'all', runThrough: '' }, load());
  if (!/^[0-9a-f]{32}$/.test(cfg.token || '')) cfg.token = hex(16);
  if (!/^[0-9a-f]{8}$/.test(cfg.sock || '')) cfg.sock = hex(4);
  save();
  // state: off | waiting (enabled, no answer yet) | on | refused (another version) ; info: the server's hello
  const S = { on: false, state: cfg.enabled ? 'waiting' : 'off', info: null, reason: '', timer: 0, probing: false };

  const version = () => (self.ISC_BUILD && self.ISC_BUILD.release) || 'dev';
  const expect = () => ({ protocol: PROTOCOL, abi: ABI, world: self.ISC_WORLD_VERSION || '', version: version() });
  const url = () => `ws://127.0.0.1:${+cfg.port || 18765}/?t=${cfg.token}`;
  const sockPath = () => `/tmp/isc-${cfg.sock}/s`;
  // The command that starts the server: ssh with the tunnel, the launcher fetched (or its copy used) and run with
  // this page's version, socket and token. It runs the same in PowerShell, cmd, bash and zsh (no $, no backslashes).
  // The launcher comes from the repository at this page's release tag (a version's launcher with its version), else
  // at the commit the site was built from, else main (a local checkout); localStorage isc.remote.ref sets another
  // (a branch under test), isc.remote.script a whole URL or a path on the server.
  function refOf() {
    const b = self.ISC_BUILD || {};
    let ref = b.release || b.commit || 'main';
    try { ref = localStorage.getItem('isc.remote.ref') || ref; } catch (e) { /* default */ }
    return ref;
  }
  function scriptUrl() {
    let script = '';
    try { script = localStorage.getItem('isc.remote.script') || ''; } catch (e) { /* default */ }
    return script || `${REPO}/${refOf()}/tools/remote.sh`;
  }
  const hostOf = () => (cfg.server || '').trim().replace(/["'\s]/g, '') || 'isc-server';
  function launcher() {
    const script = scriptUrl(), f = '~/.isc-remote.sh';
    return /^https?:/.test(script) ? `(curl -fsSL ${script} -o ${f} || wget -qO ${f} ${script}) && sh ${f}` : `sh ${script}`;
  }
  // A page that is not a release (a branch, a checkout) has no server to download: the launcher builds one from the
  // repository's source at the page's ref (once per change), as version dev.
  const buildCommand = () => `ssh ${hostOf()} "${launcher()} build --ref ${refOf()}"`;
  function command() {
    const args = ['serve', '--version', version(), '--socket', sockPath(), '--token', cfg.token];
    if (+cfg.threads > 0) args.push('--threads', String(Math.round(+cfg.threads)));
    const rt = (cfg.runThrough || '').trim().replace(/['"]/g, '');
    if (rt) args.push('--run-through', `'${rt}'`);
    return `ssh -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -L ${+cfg.port || 18765}:${sockPath()} ${hostOf()} "${launcher()} ${args.join(' ')}"`;
  }

  // ---- the connection ----
  function probe() {
    clearTimeout(S.timer);
    if (!cfg.enabled || S.on || S.probing || !self.IscWire) return;
    S.probing = true;
    let ws, answered = false;
    const again = () => { S.probing = false; if (cfg.enabled && !S.on) S.timer = setTimeout(probe, 3000); };
    try { ws = new WebSocket(url()); } catch (e) { again(); return; }
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => ws.send(self.IscWire.encode({ type: 'hello', id: 0, protocol: PROTOCOL, version: version() }));
    ws.onmessage = e => {
      let d;
      try { d = self.IscWire.decode(e.data); } catch (err) { return; }
      if (d.type !== 'hello') return;
      answered = true;
      ws.close();
      const x = expect();
      const why = d.protocol !== x.protocol ? `it speaks protocol ${d.protocol}, this page ${x.protocol}`
        : d.abi !== x.abi ? `its core is ABI ${d.abi}, this page's ${x.abi}`
        : x.world && d.world !== x.world ? `its world version is ${d.world}, this page's ${x.world}` : '';
      S.probing = false;
      if (why) { S.state = 'refused'; S.reason = `the server is another version (${d.version}): ${why}`; S.info = d; show(); S.timer = setTimeout(probe, 10000); return; }
      S.on = true; S.state = 'on'; S.info = d; S.reason = '';
      show();
      rebind();
    };
    ws.onclose = () => { if (!answered) again(); };
  }
  function rebind() {
    if (self.iscRebindEngines) self.iscRebindEngines();
    if (self.liveState && self.liveState.rebind) self.liveState.rebind();
  }
  function stop(state, reason) {
    const was = S.on;
    S.on = false; S.state = state; S.reason = reason || '';
    show();
    if (was) rebind();
    probe();
  }
  // the workers' word (remote_proxy.js): a lost connection sends every engine back here
  if (typeof BroadcastChannel !== 'undefined') {
    new BroadcastChannel('isc-remote').onmessage = e => {
      const m = e.data || {};
      if ((m.state === 'lost' || m.state === 'failed') && S.on) stop(cfg.enabled ? 'waiting' : 'off', m.reason ? 'lost: ' + m.reason : '');
    };
  }

  // ---- the menu's line and the dialog ----
  function statusText() {
    const i = S.info;
    if (S.state === 'on' && i)
      return `Connected to ${i.host}: ${i.cpus} CPUs, ${Math.round(i.memGB)} GB, ${i.threads} threads per engine (inSiliScope ${i.version}). ` +
        (cfg.scope === 'records' ? 'Records, movies and ground truth run there; live runs here.' : 'Live, Records, movies and ground truth run there.');
    if (S.state === 'refused') return 'Not used: ' + S.reason + '.';
    if (S.state === 'waiting') return 'Waiting for the server' + (S.reason ? ` (${S.reason})` : '') + ': run the command below in a terminal and leave it open. This page connects by itself; until then it computes here.';
    return 'Everything is computed in this browser.';
  }
  function show() {
    const note = $('remoteNote');
    if (note) note.textContent = S.state === 'on' && S.info ? `Computing on ${S.info.host}` : S.state === 'waiting' ? 'Waiting for the remote server…' : S.state === 'refused' ? 'Remote server: another version' : 'Computing in this browser';
    const st = $('rmStatus');
    if (st) { st.textContent = statusText(); st.className = S.state === 'refused' ? 'warn' : 'msg'; }
    const go = $('rmGo');
    if (go) go.textContent = cfg.enabled ? 'Stop using it' : 'Use it';
    const cmd = $('rmCmd');
    if (cmd) cmd.value = command();
    const bc = $('rmBuildCmd');
    if (bc) bc.value = buildCommand();
    if (self.liveState && self.liveState.showInfo) self.liveState.showInfo();
  }
  const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
  function open() {
    if (!self.iscSettings) return;
    self.iscSettings.dialog('<h3>Remote server</h3>' +
      '<div class="msg">A Linux machine (a workstation, a server or a cluster) you can log in to with <code>ssh</code> computes for this ' +
      'page: you need no admin rights and nothing installed; the server fetches this version of inSiliScope by itself. ' +
      '<a href="https://kjamartens.github.io/inSiliScope/docs/remote/" target="_blank" rel="noopener">How to set it up ↗</a></div>' +
      '<div class="row"><label for="rmServer">Server</label><input type="text" id="rmServer" placeholder="isc-server or you@host" ' +
      'title="The name you log in with: a Host from your SSH configuration, or user@host"></div>' +
      '<div class="row"><label for="rmScope">Send to the server</label><select id="rmScope">' +
      '<option value="all">Everything, live included</option><option value="records">Records, movies and ground truth</option></select></div>' +
      '<div class="row"><label for="rmThreads">Threads per engine</label><input type="number" id="rmThreads" min="1" max="1024" step="1" placeholder="half the CPUs" ' +
      'title="How many CPUs each engine (live, movies, ground truth) may use. Empty: half of the server\'s. On a shared machine, leave some for others."></div>' +
      '<details><summary>More</summary>' +
      '<div class="row"><label for="rmRun">Run through</label><input type="text" id="rmRun" placeholder="srun --cpus-per-task=32 --mem=64G --time=02:00:00" ' +
      'title="On a cluster with a job scheduler: the command that starts a job (Slurm\'s srun). The server then runs in that job; the login node only relays."></div>' +
      '<div class="row"><label for="rmPort">Port on this computer</label><input type="number" id="rmPort" min="1024" max="65535" step="1" ' +
      'title="This computer\'s end of the SSH tunnel (127.0.0.1 only)"></div></details>' +
      (version() === 'dev'
        ? '<div class="msg" id="rmDev">This viewer is not a release, so there is no server to download: build one on the server ' +
          'first (a few minutes; again after each change to the viewer), then run the command below.</div>' +
          '<div class="row"><label for="rmBuildCmd">Build first</label><textarea id="rmBuildCmd" readonly rows="3" spellcheck="false" ' +
          'title="Run it once in PowerShell (Windows) or a terminal: it builds the server from the source of this viewer\'s version in your home directory on the server (needs a C++ compiler there)"></textarea></div>'
        : '') +
      '<div class="row"><label for="rmCmd">Command</label><textarea id="rmCmd" readonly rows="4" spellcheck="false" ' +
      'title="Run it in PowerShell (Windows) or a terminal (macOS, Linux) and leave the window open: closing it stops the server"></textarea></div>' +
      '<div class="row"><span class="sp"></span><button id="rmCopy">Copy the command</button></div>' +
      '<div id="rmStatus" class="msg"></div>' +
      '<div class="row"><span class="sp"></span><button id="rmGo"></button><button id="rmClose">Close</button></div>');
    $('rmServer').value = cfg.server; $('rmScope').value = cfg.scope; $('rmThreads').value = cfg.threads;
    $('rmRun').value = cfg.runThrough; $('rmPort').value = cfg.port;
    const edit = () => {
      const scope = $('rmScope').value, port = Math.round(+$('rmPort').value) || 18765;
      const rescope = scope !== cfg.scope, report = port !== cfg.port;
      Object.assign(cfg, { server: $('rmServer').value.trim(), scope, threads: $('rmThreads').value, runThrough: $('rmRun').value, port });
      save();
      show();
      if (S.on && (rescope || report)) { if (report) stop('waiting'); else rebind(); }
    };
    for (const id of ['rmServer', 'rmScope', 'rmThreads', 'rmRun', 'rmPort']) $(id).addEventListener(id === 'rmScope' ? 'change' : 'input', edit);
    $('rmCopy').onclick = () => {
      const t = $('rmCmd');
      t.select();
      (navigator.clipboard ? navigator.clipboard.writeText(t.value) : Promise.reject()).catch(() => document.execCommand('copy'));
      $('rmCopy').textContent = 'Copied';
      setTimeout(() => { if ($('rmCopy')) $('rmCopy').textContent = 'Copy the command'; }, 1500);
    };
    $('rmGo').onclick = () => {
      cfg.enabled = !cfg.enabled;
      save();
      if (cfg.enabled) { S.state = 'waiting'; show(); probe(); }
      else stop('off');
    };
    $('rmClose').onclick = () => self.iscSettings.closeDialog();
    show();
  }

  // the menu's Compute section (index.html makes it)
  const btn = $('remoteOpen');
  if (btn) btn.addEventListener('click', ev => {
    ev.stopPropagation();
    const m = $('menu'), b = $('menuBtn');
    if (m) m.classList.remove('open');
    if (b) b.setAttribute('aria-expanded', 'false');
    open();
  });

  self.iscRemote = {
    // an engine's link (spawnGenWorker): null while no server is connected
    config: name => (S.on ? { url: url(), scope: cfg.scope, name, expect: expect() } : null),
    on: () => S.on,
    scope: () => cfg.scope,
    host: () => (S.on && S.info ? S.info.host : ''),
    state: () => ({ ...S, timer: undefined }),
    command,
    open,
    esc,
  };
  show();
  if (cfg.enabled) probe();
})();
