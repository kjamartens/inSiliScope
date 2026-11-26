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

  // ---- the menu's line and the window ----
  // The window (2026-11-26, the user's design): a switch; on, the settings and the command(s) to run open under it.
  const css = document.createElement('style');
  css.textContent = `
  #setDlg .box.rm{width:min(600px,calc(100vw - 32px));padding:var(--space-4) var(--space-5)}
  #setDlg .rmHead{display:flex;align-items:center;gap:var(--space-2);margin-bottom:var(--space-3)}
  #setDlg .rmHead h3{margin:0;flex:1}
  #setDlg .rmX{padding:2px 7px;line-height:1}
  #setDlg .rmTop{display:flex;align-items:center;gap:var(--space-3);flex-wrap:wrap}
  #setDlg .rmTop .switch{font-weight:var(--font-weight-semibold)}
  #setDlg .rmTop .stateDot{margin-left:auto}
  #setDlg .rmLead{margin:var(--space-2) 0 0;color:var(--color-text-muted);font-size:var(--font-size-caption)}
  #setDlg .rmLead a{color:var(--color-link)}
  #setDlg .rmBody{margin-top:var(--space-4);padding-top:var(--space-4);border-top:1px solid var(--color-border)}
  #setDlg .rmGrid{display:grid;grid-template-columns:9.5em minmax(0,1fr);gap:var(--space-2) var(--space-3);align-items:center}
  #setDlg .rmGrid label{color:var(--color-text-muted)}
  #setDlg .rmGrid input[type=text],#setDlg .rmGrid select{width:100%;box-sizing:border-box}
  #setDlg .rmGrid input[type=number]{width:7em}
  #setDlg .rmMore{margin-top:var(--space-2)}
  #setDlg .rmMore summary{cursor:pointer;color:var(--color-text-muted);font-size:var(--font-size-caption);margin-bottom:var(--space-2)}
  #setDlg .rmStep{margin-top:var(--space-4)}
  #setDlg .rmStepHead{display:flex;align-items:center;gap:var(--space-2);margin-bottom:var(--space-1)}
  #setDlg .rmStepHead .n{flex:none;width:18px;height:18px;border-radius:50%;background:var(--color-accent);color:var(--color-accent-on);display:inline-flex;align-items:center;justify-content:center;font-size:var(--font-size-caption);font-weight:var(--font-weight-semibold)}
  #setDlg .rmCode{position:relative;background:var(--color-input-bg);border:1px solid var(--color-input-border);border-radius:var(--radius-md);padding:var(--space-2) 64px var(--space-2) var(--space-3)}
  #setDlg .rmCode code{display:block;font:var(--font-size-caption)/1.45 var(--font-family-mono);white-space:pre-wrap;word-break:break-all;max-height:7.5em;overflow:auto;user-select:all}
  #setDlg .rmCopy{position:absolute;top:var(--space-1);right:var(--space-1);padding:2px 8px;font-size:var(--font-size-caption)}
  #setDlg .rmStatus{margin-top:var(--space-1);font-size:var(--font-size-caption);color:var(--color-text-muted)}
  #setDlg .rmStatus:empty{display:none}
  #setDlg .rmStatus.warn{color:var(--color-status-warn)}
  #setDlg .rmLoad{margin-bottom:var(--space-1)}
  #setDlg .rmOld{font-size:var(--font-size-caption);color:var(--color-status-warn)}
  #setDlg .rmOld[hidden],#setDlg .rmLoad .rmGrid[hidden]{display:none}
  #setDlg .rmSetup{margin-top:var(--space-4)}
  #setDlg .rmSetup>summary{display:none;cursor:pointer;color:var(--color-text-muted);font-size:var(--font-size-caption);margin-bottom:var(--space-3)}
  #setDlg .rmSetup.fold>summary{display:list-item}
  #setDlg .rmLoad[hidden]{display:none}
  #setDlg .rmLoad .rmGrid{align-items:start}
  #setDlg .rmLoad .lab{color:var(--color-text-muted)}
  #setDlg .rmTick{display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--color-accent);margin-left:var(--space-2);vertical-align:middle;opacity:0}
  #setDlg .rmTick.on{animation:rmTick 900ms ease-out}
  @keyframes rmTick{from{opacity:1}to{opacity:0}}
  #setDlg .rmMeter .bar{display:flex;height:8px;border-radius:var(--radius-sm);background:var(--color-input-border);overflow:hidden;margin-top:5px}
  #setDlg .rmMeter .bar i{display:block;height:100%;transition:width var(--duration-fast)}
  #setDlg .rmMeter .bar .isc{background:var(--color-accent)} #setDlg .rmMeter .bar .oth{background:var(--color-text-muted)}
  #setDlg .rmMeter .v{display:flex;justify-content:space-between;gap:var(--space-3);margin-top:var(--space-1);font-size:var(--font-size-caption);color:var(--color-text-muted)}
  #setDlg .rmMeter .v b{color:var(--color-text);font-weight:var(--font-weight-medium)}
  #setDlg .rmKey::before{content:'';display:inline-block;width:8px;height:8px;border-radius:2px;background:var(--color-accent);margin-right:var(--space-1)}`;
  document.head.append(css);

  // the state in a few words (the switch's line), and what to know about it (under the command)
  function stateText() {
    if (S.state === 'on' && S.info) return `Connected to ${S.info.host}`;
    if (S.state === 'refused') return 'Another version';
    if (S.state === 'waiting') return 'Waiting for the server';
    return 'Off';
  }
  function statusText() {
    const i = S.info;
    if (S.state === 'on' && i)
      return `${i.threads} threads per engine · inSiliScope ${i.version}`;
    if (S.state === 'refused') return `Not used: ${S.reason}.`;
    if (S.state === 'waiting' && S.reason) return `Connection ${S.reason}.`;
    return '';
  }
  function show() {
    const note = $('remoteNote');
    if (note) {
      note.textContent = S.state === 'on' && S.info ? `Computing on ${S.info.host}` : S.state === 'waiting' ? 'Waiting for the remote server…' : S.state === 'refused' ? 'Remote server: another version' : 'Computing in this browser';
      $('remoteState').dataset.s = S.state;
      $('remoteState').title = S.state === 'refused' ? S.reason : note.textContent;
    }
    const go = $('rmGo');
    if (go) go.checked = cfg.enabled;
    const body = $('rmBody');
    if (body) body.hidden = !cfg.enabled;
    const st = $('rmState');
    if (st) { st.textContent = stateText(); st.dataset.s = S.state; }
    const ld = $('rmLoad');
    if (ld) { if (S.state === 'on') watchLoad(); else { unwatchLoad(); ld.hidden = true; } }
    const su = $('rmSetup');
    if (su && su.dataset.s !== S.state) {   // (on a change of state only: a fold opened by hand stays open)
      su.dataset.s = S.state;
      su.classList.toggle('fold', S.state === 'on');
      su.open = S.state !== 'on';
    }
    const ss = $('rmStatus');
    if (ss) { ss.textContent = statusText(); ss.className = 'rmStatus' + (S.state === 'refused' || S.reason ? ' warn' : ''); }
    const cmd = $('rmCmd');
    if (cmd) cmd.textContent = command();
    const bc = $('rmBuildCmd');
    if (bc) bc.textContent = buildCommand();
    if (self.liveState && self.liveState.showInfo) self.liveState.showInfo();
  }
  const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
  // The server machine's load while the window shows a connected server (a 'status' job every second on a connection
  // of its own, cli/remote_server.cpp LoadSampler): its CPU and memory in use, this server's share in the accent colour.
  // The CPU is the mean of the last 5 s (the user's choice, 2026-11-26: not a jumpy snapshot); the memory as it is now.
  // A dot by "CPU" flashes at each reading, so a quiet bar (one thread of 192 CPUs is 0.5 %) is seen to be read.
  const LD = { ws: null, timer: 0, gen: 0, cpu: [] }, LOAD_EVERY_MS = 1000, LOAD_MEAN_MS = 5000;
  const fmtN = v => (v < 10 ? v.toFixed(1) : String(Math.round(v)));
  const fmtGB = v => v < 1 ? Math.round(v * 1024) + ' MB' : (v >= 100 ? Math.round(v) : v.toFixed(1)) + ' GB';
  function meter(el, iscFrac, allFrac, left, right, tip) {
    if (!el) return;
    const w = f => Math.max(0, Math.min(100, 100 * f)).toFixed(1) + '%';
    el.innerHTML = `<div class="bar"><i class="isc" style="width:${w(iscFrac)}"></i><i class="oth" style="width:${w(Math.max(0, allFrac - iscFrac))}"></i></div>` +
      `<div class="v"><span>${left}</span><span class="rmKey">${right}</span></div>`;
    el.title = tip;
  }
  function showLoad(d) {
    const ld = $('rmLoad');
    if (!ld) return;
    ld.hidden = false;
    // a server older than this viewer has no 'status' job (an error), or one off Linux has no numbers
    const none = !!d.error || d.cpuPct === undefined;
    $('rmMeters').hidden = none;
    $('rmOld').hidden = !none;
    if (none) {
      $('rmOld').textContent = d.error
        ? (version() === 'dev'
          ? 'This server is older than this viewer and does not report its load. Build it again (step 1 under Settings and commands), then run step 2 again.'
          : 'This server is older than this viewer and does not report its load. Run the command under Settings and commands again: it starts this version.')
        : 'This server does not report its load (it is not a Linux machine).';
      return;
    }
    const now = performance.now();
    LD.cpu.push({ t: now, all: d.cpuPct, isc: Math.min(d.iscCpuPct, d.cpuPct) });   // (read a moment apart: the share at most the whole)
    while (LD.cpu.length > 1 && now - LD.cpu[0].t > LOAD_MEAN_MS) LD.cpu.shift();
    const mean = k => LD.cpu.reduce((a, c) => a + c[k], 0) / LD.cpu.length;
    const all = mean('all'), isc = mean('isc'), iscCpus = isc / 100 * d.cpus;
    meter($('rmCpu'), isc / 100, all / 100, `<b>${fmtN(all)} %</b> of ${d.cpus} CPUs`, `inSiliScope ${fmtN(iscCpus)} CPU${fmtN(iscCpus) === '1.0' ? '' : 's'}`,
      `The server's CPUs in use, the mean of the last ${LOAD_MEAN_MS / 1000} s, read every second; inSiliScope's share in colour ` +
      `(${fmtN(isc)} %). Load average (1 min): ${(+d.load1).toFixed(1)}. ${d.engines} engine${d.engines === 1 ? '' : 's'} of this viewer connected.`);
    const tick = $('rmTick');
    if (tick) { tick.classList.remove('on'); void tick.offsetWidth; tick.classList.add('on'); }
    meter($('rmMem'), d.iscMemGB / d.memTotalGB, d.memUsedGB / d.memTotalGB, `<b>${fmtGB(d.memUsedGB)}</b> of ${fmtGB(d.memTotalGB)}`, `inSiliScope ${fmtGB(d.iscMemGB)}`,
      "The server's memory in use (its total less what is available); inSiliScope's share in colour.");
  }
  function watchLoad() {
    if (LD.ws || !self.IscWire) return;
    const gen = ++LD.gen;
    LD.cpu = [];
    let ws;
    try { ws = new WebSocket(url()); } catch (e) { return; }
    LD.ws = ws;
    ws.binaryType = 'arraybuffer';
    const ask = () => { if (LD.gen === gen && ws.readyState === 1) ws.send(self.IscWire.encode({ type: 'status', id: 1 })); };
    ws.onopen = ask;
    ws.onmessage = e => {
      if (LD.gen !== gen) return;
      let d;
      try { d = self.IscWire.decode(e.data); } catch (err) { return; }
      if (d.type !== 'status') return;
      showLoad(d);
      if (d.error) return;   // (a server of before 2026-11-26)
      LD.timer = setTimeout(ask, LOAD_EVERY_MS);
    };
    ws.onclose = () => { if (LD.gen === gen) { LD.ws = null; clearTimeout(LD.timer); } };
  }
  function unwatchLoad() {
    LD.gen++;
    clearTimeout(LD.timer);
    if (LD.ws) { try { LD.ws.close(); } catch (e) { /* closed */ } LD.ws = null; }
  }
  const step = (n, title, tip, id) => '<div class="rmStep"><div class="rmStepHead" title="' + esc(tip) + '">' +
    (n ? `<span class="n">${n}</span>` : '') + `<span>${title}</span></div>` +
    `<div class="rmCode"><code id="${id}"></code><button class="rmCopy" data-for="${id}">Copy</button></div></div>`;
  function open() {
    if (!self.iscSettings) return;
    const dev = version() === 'dev';
    const box = self.iscSettings.dialog('<div class="rmHead"><h3>Remote server</h3>' +
      '<button class="rmX" id="rmClose" aria-label="Close" title="Close (Esc)">✕</button></div>' +
      '<div class="rmTop"><label class="switch"><input type="checkbox" role="switch" id="rmGo"><span class="track"></span>' +
      '<span>Compute on a remote server</span></label><span class="stateDot" id="rmState"></span></div>' +
      '<p class="rmLead">A Linux machine you can log in to with <code>ssh</code> computes live frames, Records and ground truth. ' +
      '<a href="https://kjamartens.github.io/inSiliScope/docs/remote/" target="_blank" rel="noopener">Set-up guide ↗</a></p>' +
      '<div class="rmBody" id="rmBody">' +
      '<div class="rmLoad" id="rmLoad" hidden><div class="rmGrid" id="rmMeters">' +
      '<span class="lab">CPU<i class="rmTick" id="rmTick" aria-hidden="true"></i></span><div class="rmMeter" id="rmCpu"></div>' +
      '<span class="lab">Memory</span><div class="rmMeter" id="rmMem"></div>' +
      '</div><div class="rmOld" id="rmOld" hidden></div></div>' +
      '<div class="rmStatus" id="rmStatus"></div>' +
      '<details class="rmSetup" id="rmSetup" open><summary>Settings and commands</summary>' +
      '<div class="rmGrid">' +
      '<label for="rmServer">Server</label><input type="text" id="rmServer" placeholder="isc-server or you@host" spellcheck="false" ' +
      'title="The name you log in with: a Host from your SSH configuration, or user@host">' +
      '<label for="rmScope">Run there</label><select id="rmScope">' +
      '<option value="all">Everything, live included</option><option value="records">Records, movies and ground truth</option></select>' +
      '<label for="rmThreads">Threads per engine</label><input type="number" id="rmThreads" min="1" max="1024" step="1" placeholder="half" ' +
      'title="CPUs each engine (live, movies, ground truth) may use. Empty: half of the server\'s.">' +
      '</div>' +
      '<details class="rmMore"><summary>More</summary><div class="rmGrid">' +
      '<label for="rmRun">Run through</label><input type="text" id="rmRun" placeholder="srun --cpus-per-task=32 --mem=64G --time=02:00:00" spellcheck="false" ' +
      'title="On a cluster: the command that starts a job (Slurm\'s srun). The server runs in that job; the login node only relays.">' +
      '<label for="rmPort">Port on this computer</label><input type="number" id="rmPort" min="1024" max="65535" step="1" ' +
      'title="This computer\'s end of the SSH tunnel (127.0.0.1 only)">' +
      '</div></details>' +
      (dev ? step(1, 'Build the server', 'Once, and again after each change to this viewer. Needs a C++ compiler on the server.', 'rmBuildCmd') : '') +
      step(dev ? 2 : 0, 'Run in a terminal and leave it open', 'PowerShell on Windows, a terminal on macOS or Linux. Closing it stops the server.', 'rmCmd') +
      '</details></div>', () => { unwatchLoad(); box.classList.remove('rm'); });
    box.classList.add('rm');
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
    for (const b of document.querySelectorAll('#setDlg .rmCopy')) b.onclick = () => {
      const t = $(b.dataset.for).textContent;
      (navigator.clipboard ? navigator.clipboard.writeText(t) : Promise.reject()).catch(() => {
        const r = document.createRange(); r.selectNodeContents($(b.dataset.for));
        const s = getSelection(); s.removeAllRanges(); s.addRange(r); document.execCommand('copy');
      });
      b.textContent = 'Copied';
      setTimeout(() => { b.textContent = 'Copy'; }, 1500);
    };
    $('rmGo').onchange = () => {
      cfg.enabled = $('rmGo').checked;
      save();
      if (cfg.enabled) { S.state = 'waiting'; S.reason = ''; show(); probe(); if (!cfg.server) $('rmServer').focus(); }
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
