// Loads the JS prototype's generator (web/prototype/index.html above `// ---- viewer ---`, plus the body of
// microtubules.js) as one DOM-free unit. Isomorphic: Node and the browser (web/lab).
//
//   const P = loadPrototype(htmlText, mtText);       // fresh, isolated instance (own caches)
//   const p = P.paramsFrom({ ...P.defaults, mtWobbleTurn: 1.2 });  // the page's own params() normalisation
//   P.gen.buildMicrotubulesForCell(seed, cx, cy, cell, p);
//
// `defaults`/`inputs` come from the page's <input>s, so a new slider in the prototype appears here without
// touching this file. paramsFrom() runs the page's own params() against those values.

const GEN_START = '<script id="mainScript">';
const GEN_END = '// ---- viewer ---';
const MT_OPEN = 'window.__MT_SRC = function () {';

// Generator exports (functions/values declared at the generator's top level).
export const GEN_EXPORTS = ['pcg4d', 'hashUnit', 'hashStream', 'rawCandidate', 'buildCandidateMap', 'packMap',
  'interactionChunks', 'getCytoGeometry', 'sampleCytoMeshHeight', 'cellOutlineLocal', 'ensureCytoCacheFresh',
  'cytoCache', 'getMtCellGeometry', 'buildMicrotubulesForCell', 'buildMicrotubuleLabelPoints'];

function sliceOrThrow(text, a, b, what) {
  if (a < 0 || b < 0 || b <= a) throw new Error(what + ' markers not found');
  return text.slice(a, b);
}

export function generatorSource(html) {
  const s = html.indexOf(GEN_START);
  return sliceOrThrow(html, s + GEN_START.length, html.indexOf(GEN_END), 'generator (mainScript / viewer)');
}

export function mtBodySource(mtText) {
  const o = mtText.indexOf(MT_OPEN);
  if (o < 0) throw new Error('microtubules.js wrapper not found');
  return mtText.slice(mtText.indexOf('{', o) + 1, mtText.lastIndexOf('}'));
}

// Text of `function params() {...}` from the viewer half (it reads els.<id>.value / .checked).
export function paramsSource(html) {
  const s = html.indexOf('\nfunction params() {');
  if (s < 0) throw new Error('params() not found');
  const e = html.indexOf('\n}\n', s);
  return html.slice(s + 1, e + 2);
}

const decode = s => s.replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"')
  .replace(/&#39;/g, "'").replace(/<[^>]*>/g, '').replace(/\s+/g, ' ').trim();

// Every <input> of the side panel, in page order: {id, type, value|checked, min, max, step, label, group}.
export function parseInputs(html) {
  const side = html.slice(0, html.indexOf('<div id="canvasHost">'));
  const out = [];
  let group = '';
  const re = /<legend>([\s\S]*?)<\/legend>|<(label|div)\b[^>]*class="(row|pair)"[^>]*>([\s\S]*?)<\/\2>/g;
  for (const m of side.matchAll(re)) {
    if (m[1] != null) { group = decode(m[1]); continue; }
    const inner = m[4];
    const label = decode(inner.replace(/<input\b[^>]*>|<span id="\w+Val"><\/span>|<button[\s\S]*?<\/button>/g, ''));
    for (const t of inner.matchAll(/<input\b[^>]*>/g)) {
      const tag = t[0];
      const attr = k => new RegExp(`\\b${k}="([^"]*)"`).exec(tag)?.[1];
      const id = attr('id'), type = attr('type') || 'text';
      if (!id) continue;
      const o = { id, type, label, group };
      if (type === 'checkbox') o.checked = /\bchecked\b/.test(tag);
      else o.value = attr('value') ?? '';
      for (const k of ['min', 'max', 'step']) if (attr(k) != null) o[k] = +attr(k);
      out.push(o);
    }
  }
  return out;
}

export function inputDefaults(inputs) {
  const d = {};
  for (const i of inputs) d[i.id] = i.type === 'checkbox' ? i.checked : (i.type === 'number' || i.type === 'range' ? +i.value : i.value);
  return d;
}

// Script text that defines the generator and returns its exports + paramsFrom(values).
export function prototypeScript(html, mtText) {
  return [
    "'use strict';",
    // Viewer-only hooks the generator half references (cache clears release GPU buffers there).
    'function freeMeshGl() {}',
    mtBodySource(mtText),
    generatorSource(html),
    'function paramsFrom(vals) {',
    '  const els = new Proxy({}, { get: (_, k) => ({ value: String(vals[k]), checked: !!vals[k] }) });',
    paramsSource(html),
    '  return params();',
    '}',
    `return { paramsFrom, gen: { ${GEN_EXPORTS.join(', ')} } };`,
  ].join('\n');
}

// Runs as a function body (new Function), so every top-level name stays private to this instance. Not a
// Node vm context: code in a separate vm context runs ~9x slower (cross-context builtins), same results.
export function loadPrototype(html, mtText) {
  const inputs = parseInputs(html);
  const defaults = inputDefaults(inputs);
  const { paramsFrom, gen } = new Function(prototypeScript(html, mtText))();
  return { inputs, defaults, paramsFrom, gen };
}
