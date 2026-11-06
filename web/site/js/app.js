// CUI live components: the page around the live canvases.
import { Preview, settings, allLive } from './preview.js';
import { loadModule } from './runtime.js';

const CAT = window.CUI_CATALOG;
const $ = (s, el = document) => el.querySelector(s);
const h = (tag, attrs = {}, ...kids) => {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'html') el.innerHTML = v;
    else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of kids.flat()) if (kid !== null && kid !== undefined && kid !== false) el.append(kid.nodeType ? kid : document.createTextNode(kid));
  return el;
};
const svg = (paths, extra = '') => `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" ${extra}>${paths}</svg>`;
const I = {
  sun: svg('<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>'),
  moon: svg('<path d="M20 14.5A8 8 0 0 1 9.5 4 8 8 0 1 0 20 14.5z"/>'),
  eye: svg('<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>'),
  code: svg('<path d="m16 18 6-6-6-6M8 6l-6 6 6 6"/>'),
  reset: svg('<path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5"/>'),
  camera: svg('<path d="M14.5 4h-5L7.5 7H4a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3.5z"/><circle cx="12" cy="13" r="3.5"/>'),
  expand: svg('<path d="M15 3h6v6M9 21H3v-6M21 3l-7 7M3 21l7-7"/>'),
  copy: svg('<rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/>'),
  check: svg('<path d="M20 6 9 17l-5-5"/>'),
  split: svg('<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M12 4v16"/>'),
  frame: svg('<path d="M4 8V5a1 1 0 0 1 1-1h3M16 4h3a1 1 0 0 1 1 1v3M20 16v3a1 1 0 0 1-1 1h-3M8 20H5a1 1 0 0 1-1-1v-3"/>'),
  grip: svg('<path d="M21 15l-6 6M21 9 9 21"/>'),
  arrow: svg('<path d="M5 12h14M13 6l6 6-6 6"/>'),
  left: svg('<path d="M19 12H5M11 18l-6-6 6-6"/>'),
  note: svg('<path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/>'),
  search: svg('<circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/>'),
  circleCheck: svg('<circle cx="12" cy="12" r="9"/><path d="m8.5 12 2.5 2.5 4.5-5"/>'),
  monitor: svg('<rect x="2" y="4" width="20" height="13" rx="2"/><path d="M8 21h8M12 17v4"/>'),
};
const ACCENTS = [
  ['Neutral', '#353535', '#bebebe'], ['Violet', '#7747ff', '#a78bff'], ['Blue', '#0562ef', '#6ba3ff'], ['Green', '#00864b', '#53dca6'],
  ['Amber', '#f3ad20', '#ffcb62'], ['Orange', '#c25300', '#ffad5e'], ['Coral', '#d1403a', '#ff9386'], ['Rose', '#d23286', '#ff8bc5'],
];
const BG = { light: '#ffffff', dark: '#121212' };   // CUI_TOKEN_BACKGROUND

// ---------------------------------------------------------------- storage
const store = {
  get(k, d) { try { const v = localStorage.getItem(k); return v ? JSON.parse(v) : d; } catch (e) { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) {} },
};
const saved = store.get('cui-web-settings', {});
const prefs = { theme: saved.theme || 'system', accent: saved.accent ?? 0, accentTouched: !!saved.accentTouched,
                renderer: saved.renderer || 'gpu', speed: saved.speed || 1, reducedMotion: !!saved.reducedMotion,
                pixelRatio: saved.pixelRatio || 'auto', pair: !!saved.pair };
const savePrefs = () => store.set('cui-web-settings', prefs);
const reviewOn = () => !!prefs.review;
function applyReview() { document.body.classList.toggle('review-mode', reviewOn()); }
let notes = store.get('cui-web-notes', {});     // id -> { text, updated, context }
let reviewed = store.get('cui-web-reviewed', {});   // id -> timestamp
const saveNotes = () => { store.set('cui-web-notes', notes); updateCounts(); };
const saveReviewed = () => { store.set('cui-web-reviewed', reviewed); updateCounts(); };

const media = matchMedia('(prefers-color-scheme: dark)');
const isDark = () => prefs.theme === 'dark' || (prefs.theme === 'system' && media.matches);
function syncSettings() {
  Object.assign(settings, { renderer: prefs.renderer, speed: prefs.speed, reducedMotion: prefs.reducedMotion,
                            pixelRatio: prefs.pixelRatio, dark: isDark(), accent: prefs.accent, accentTouched: prefs.accentTouched });
}
syncSettings();

function applyTheme() {
  const dark = isDark();
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
  settings.dark = dark;
  $('#theme-toggle').innerHTML = dark ? I.moon : I.sun;
  $('#theme-toggle').title = `Theme: ${prefs.theme} (click to switch)`;
  for (const p of allLive()) p.setTheme(dark);
  thumbQueue.length = 0;
  document.querySelectorAll('img[data-thumb]').forEach((img) => { thumbIO?.unobserve(img); if (!staticThumb(img)) loadThumb(img); });
  document.querySelectorAll('.stage:not(.pair)').forEach((s) => { if (!s.dataset.fixedBg) s.style.setProperty('--stage-bg', dark ? BG.dark : BG.light); });
  renderAccents();
}
media.addEventListener('change', () => { if (prefs.theme === 'system') applyTheme(); });

function renderAccents() {
  const host = $('#accents');
  if (!host) return;
  host.replaceChildren(...ACCENTS.map(([name, l, d], i) => h('button', {
    class: 'swatch', role: 'radio', 'aria-checked': prefs.accentTouched && prefs.accent === i ? 'true' : 'false',
    'aria-label': `${name} accent`, title: `${name} accent`, style: `--c:${isDark() ? d : l}`,
    onclick: () => {
      const reset = prefs.accentTouched && prefs.accent === i;
      prefs.accent = i; prefs.accentTouched = !reset; savePrefs(); syncSettings(); renderAccents();
      for (const p of allLive()) p.setAccent(reset ? (p.content?.accent >= 0 ? p.content.accent : 0) : i);
      thumbQueue.length = 0;
      document.querySelectorAll('img[data-thumb]').forEach((img) => { thumbIO?.unobserve(img); if (!staticThumb(img)) loadThumb(img); });
      toast(reset ? 'Accent: each scene’s own' : `Accent: ${name}`);
    },
  })));
}

// ---------------------------------------------------------------- helpers
function toast(text) {
  const t = h('div', { class: 'toast' }, text);
  $('#toasts').append(t);
  setTimeout(() => t.remove(), 2200);
}
async function copy(text, what = 'Copied') {
  try { await navigator.clipboard.writeText(text); toast(what); }
  catch (e) { toast('Copy failed: clipboard not available'); }
}
const esc = (s) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// C syntax highlighting: comments, strings, numbers, keywords, CUI_ constants, cui_ calls, .fields
const KW = new Set('static const int float double bool char void if else for while do return switch case break continue default struct typedef sizeof unsigned signed enum true false NULL long short uint8_t uint32_t size_t'.split(' '));
function highlight(src) {
  const re = /(\/\*[\s\S]*?\*\/|\/\/[^\n]*)|("(?:[^"\\\n]|\\.)*")|('(?:[^'\\\n]|\\.)*')|(\b\d+(?:\.\d+)?f?\b|\b0x[0-9a-fA-F]+\b)|(\.[a-z_][a-z0-9_]*(?=\s*=))|(\b[A-Za-z_][A-Za-z0-9_]*\b)/g;
  let out = '', last = 0, m;
  while ((m = re.exec(src))) {
    out += esc(src.slice(last, m.index));
    const t = esc(m[0]);
    if (m[1]) out += `<span class="tok-com">${t}</span>`;
    else if (m[2] || m[3]) out += `<span class="tok-str">${t}</span>`;
    else if (m[4]) out += `<span class="tok-num">${t}</span>`;
    else if (m[5]) out += `<span class="tok-field">${t}</span>`;
    else if (KW.has(m[6])) out += `<span class="tok-kw">${t}</span>`;
    else if (/^CUI_[A-Z0-9_]+$/.test(m[6])) out += `<span class="tok-const">${t}</span>`;
    else if (/^cui_\w+$/.test(m[6]) && src[re.lastIndex] === '(') out += `<span class="tok-fn">${t}</span>`;
    else out += t;
    last = re.lastIndex;
  }
  return out + esc(src.slice(last));
}

// ---------------------------------------------------------------- thumbnails
// Rendered live from the scenes module (software renderer, one shared
// instance), so they always match the current code.
let thumbMod = null;
const thumbCache = new Map();
const thumbQueue = [];
let thumbBusy = false;
async function renderThumb(module, name, dark, accent) {
  const key = `${module}:${name}:${dark}:${accent}`;
  if (thumbCache.has(key)) return thumbCache.get(key);
  const p = (async () => {
    const mod = module === 'scenes' ? (thumbMod ||= await loadModule('wasm/scenes.wasm')) : await loadModule(`wasm/${module}.wasm`);
    const x = mod.exports;
    const c = name ? mod.find(name) : mod.contents[0];
    if (!c) return '';
    const scale = module === 'scenes' ? Math.min(1.5, Math.max(0.5, 560 / c.width)) : 0.5;
    const a = accent >= 0 ? accent : (c.accent >= 0 ? c.accent : 0);
    const inst = x.inst_new(c.index, c.width, c.height, scale, c.theme === 1 ? 1 : dark, a, 0);
    if (module !== 'scenes') x.inst_frame(inst, 1 / 60, 0);
    x.inst_paint(inst);
    const px = mod.pixels(inst);
    const cv = document.createElement('canvas');
    cv.width = px.w; cv.height = px.h;
    cv.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(px.data), px.w, px.h), 0, 0);
    x.inst_free(inst);
    const blob = await new Promise((r) => cv.toBlob(r, 'image/png'));
    return URL.createObjectURL(blob);
  })();
  thumbCache.set(key, p);
  return p;
}
function pumpThumbs() {
  if (thumbBusy || !thumbQueue.length) return;
  thumbBusy = true;
  const img = thumbQueue.shift();
  const run = async () => {
    try {
      const dark = isDark() ? 1 : 0, accent = prefs.accentTouched ? prefs.accent : -1;
      const want = `${dark}:${accent}`;
      if (img.isConnected && img.dataset.state !== want) {
        img.src = await renderThumb(img.dataset.module || 'scenes', img.dataset.thumb, dark, accent);
        img.dataset.state = want;
      }
    } catch (e) { console.warn('thumb', e); }
    thumbBusy = false;
    (window.requestIdleCallback || setTimeout)(pumpThumbs);
  };
  run();
}
const thumbIO = 'IntersectionObserver' in window ? new IntersectionObserver((es) => {
  for (const e of es) if (e.isIntersecting) { thumbIO.unobserve(e.target); thumbQueue.push(e.target); }
  pumpThumbs();
}, { rootMargin: '300px' }) : null;
function loadThumb(img) {
  if (thumbIO) thumbIO.observe(img); else { thumbQueue.push(img); pumpThumbs(); }
}
// Thumbnails use the rendered doc images when one exists (instant, and the
// same pixels as the live scene); anything else is rendered live.
const staticThumb = (img) => {
  const name = img.dataset.thumb, dark = isDark();
  if (!name || img.dataset.module !== 'scenes' || img.dataset.live || (prefs.accentTouched && prefs.accent)) return false;
  const dir = CAT.showcase.some((x) => x.id === name) ? 'showcase' : 'components';
  img.onerror = () => { img.onerror = null; img.dataset.live = '1'; loadThumb(img); };
  img.src = `images/${dir}/${name}${dark ? '-dark' : ''}.png`;
  img.dataset.state = `${dark ? 1 : 0}:-1`;
  return true;
};
const thumb = (name, module = 'scenes') => {
  const img = h('img', { alt: '', 'data-thumb': name || '', 'data-module': module, loading: 'lazy', decoding: 'async' });
  if (!staticThumb(img)) loadThumb(img);
  return img;
};

// ---------------------------------------------------------------- preview blocks
let blocks = [];
function disposeBlocks() { blocks.forEach((b) => b.dispose()); blocks = []; }

// A preview with a toolbar (Preview / Code) and tools.
// opts: { module, content, title, fill, codeSource, codeFile, codeLine, docRef, compact, pair }
function previewBlock(opts) {
  const sc = opts.module === 'scenes' ? CAT.scenes[opts.content] : null;
  const block = { previews: [], mounted: false, dispose() { this.unmount(); this.io?.disconnect(); } };
  const root = h('section', { class: 'preview fullscreen-host' });
  block.root = root;
  const stage = h('div', { class: 'stage' + (opts.fill ? ' flush' : '') + (opts.tight ? ' tight' : '') });
  const status = h('div', { class: 'pv-status' });
  const codeHost = h('div', { class: 'hidden' });
  let mode = 'preview';
  let pair = opts.bare ? false : (opts.pair ?? (prefs.pair && !opts.fill && !sc?.dark_only));
  let showEdge = false;

  const tabs = h('div', { class: 'tabs', role: 'tablist' },
    h('button', { role: 'tab', 'aria-selected': 'true', html: I.eye + 'Preview', onclick: () => setMode('preview') }),
    h('button', { role: 'tab', 'aria-selected': 'false', html: I.code + 'Code', onclick: () => setMode('code') }));
  const tool = (icon, label, fn, pressed, cls) => h('button', { class: 'icon-btn' + (cls ? ' ' + cls : ''), title: label, 'aria-label': label, html: icon, onclick: fn,
                                                          'aria-pressed': pressed === undefined ? null : String(pressed) });
  const pairBtn = tool(I.split, 'Light and dark side by side', () => { pair = !pair; pairBtn.setAttribute('aria-pressed', String(pair)); remount(); }, pair);
  const edgeBtn = tool(I.frame, 'Show the canvas edge', () => { showEdge = !showEdge; stage.classList.toggle('show-edge', showEdge); edgeBtn.setAttribute('aria-pressed', String(showEdge)); }, false, 'review-only');
  const codeBtn = opts.leading ? tool(I.code, 'Show the code', () => setMode(mode === 'code' ? 'preview' : 'code'), false) : null;
  const tools = h('div', { class: 'pv-tools' },
    codeBtn,
    opts.fill || sc?.dark_only ? null : pairBtn,
    opts.fill ? null : edgeBtn,
    opts.fill && sc?.dark_only ? null : h('span', { class: 'sep' }),
    tool(I.reset, 'Reset: restart this preview from its first frame', () => { remount(); toast('Preview reset'); }),
    tool(I.camera, 'Save a PNG of this preview', async () => {
      const p = block.previews[0];
      if (!p) return;
      const blob = await p.snapshot();
      const a = h('a', { href: URL.createObjectURL(blob), download: `${opts.content || opts.module}${p.dark ? '-dark' : ''}.png` });
      a.click();
    }),
    tool(I.copy, 'Copy a reference to this preview', () => copy(reference(opts, block.previews[0]), 'Reference copied'), undefined, 'review-only'),
    tool(I.expand, 'Full screen', () => { if (document.fullscreenElement) document.exitFullscreen(); else root.requestFullscreen?.(); }));
  const bar = h('div', { class: 'pv-bar' }, opts.leading || tabs, opts.title ? h('span', { class: 'pv-name review-only' }, opts.title) : null, tools);
  if (opts.bare) root.append(stage); else root.append(bar, stage, codeHost);

  function setMode(m) {
    mode = m;
    tabs.children[0].setAttribute('aria-selected', String(m === 'preview'));
    tabs.children[1].setAttribute('aria-selected', String(m === 'code'));
    codeBtn?.setAttribute('aria-pressed', String(m === 'code'));
    stage.classList.toggle('hidden', m !== 'preview');
    status.classList.toggle('hidden', m !== 'preview');
    codeHost.classList.toggle('hidden', m !== 'code');
    if (m === 'code' && !codeHost.firstChild) {
      if (typeof opts.codeSource === 'function') {
        codeHost.append(h('div', { class: 'loading' }, h('i', { class: 'spinner' }), 'Loading source'));
        opts.codeSource().then((src) => codeHost.replaceChildren(codeView(src || '/* source not found */', opts.codeFile, opts.codeLine)));
      } else codeHost.append(codeView(opts.codeSource, opts.codeFile, opts.codeLine));
    }
  }

  function updateStatus(p) {
    if (!p || !p.inst) return;
    const s = p.stats;
    status.replaceChildren(...[
      h('span', { class: 'live' + (p.animating ? '' : ' idle') }, h('i'), p.animating ? 'Animating' : 'Live'),
      h('span', {}, `${p.w} × ${p.h}`),
      h('span', {}, `build ${s.build.toFixed(2)} ms`),
      h('span', {}, `paint ${s.paint.toFixed(2)} ms`),
      h('span', {}, `${s.tris.toLocaleString()} triangles`),
      h('span', {}, p.rendererName() + (p.fit < 1 ? `, shown at ${Math.round(p.fit * 100)}%` : '')),
      settings.speed !== 1 ? h('span', {}, `${settings.speed}× speed`) : null,
      document.activeElement === p.canvas
        ? h('span', { class: 'hint kbd-on' }, 'Keyboard goes to the preview. Click outside to leave.')
        : h('span', { class: 'hint' }, opts.fill ? 'Click inside to use the keyboard' : 'Click to focus, then Tab through it. Drag the corner to resize.')].filter(Boolean));
  }

  function mountOne(host, dark, label) {
    const p = new Preview({ module: opts.module, content: opts.content, fill: opts.fill, dark, ownAccent: opts.fixedAccent,
                            container: host, padding: opts.fill || opts.tight ? 0 : opts.bare ? 24 : (pair ? 48 : 56), onStats: updateStatus,
                            width: opts.width, height: opts.height });
    host.append(p.el);
    if (label) host.append(h('span', { class: 'half-label' }, label));
    block.previews.push(p);
    p.start().then(() => {
      host.querySelector('.loading')?.remove();
      updateStatus(p);
      if (opts.fill) fitFill(p, host);
      else if (!pair && !opts.bare) addResize(p);
    }).catch((e) => {
      console.error(e);
      host.querySelector('.loading')?.remove();
      host.append(h('div', { class: 'error' }, String(e && e.stack || e)));
    });
    return p;
  }
  function addResize(p) {
    const grip = h('div', { class: 'resize', title: 'Drag to resize the viewport. Double-click for the original size.', html: I.grip });
    p.el.append(grip);
    grip.addEventListener('pointerdown', (e) => {
      e.preventDefault(); e.stopPropagation();
      grip.setPointerCapture(e.pointerId);
      const sx = e.clientX, sy = e.clientY, w0 = p.w, h0 = p.h, f = p.fit;
      const move = (ev) => p.resize(w0 + (ev.clientX - sx) / f, h0 + (ev.clientY - sy) / f);
      const up = () => { grip.removeEventListener('pointermove', move); grip.removeEventListener('pointerup', up); };
      grip.addEventListener('pointermove', move);
      grip.addEventListener('pointerup', up);
    });
    grip.addEventListener('dblclick', () => p.resize(p.natural.w, p.natural.h));
  }
  function fitFill(p, host) {
    const ro = new ResizeObserver(() => {
      const r = host.getBoundingClientRect();
      const w = opts.fixedWidth ? opts.fixedWidth() : r.width;
      if (w > 0 && r.height > 0) p.resize(w, r.height);
    });
    ro.observe(host);
    block.ro = ro;
  }

  function mount() {
    if (block.mounted) return;
    block.mounted = true;
    stage.replaceChildren();
    stage.className = 'stage' + (opts.fill ? ' flush' : '') + (pair ? ' pair' : '') + (showEdge ? ' show-edge' : '');
    if (pair) {
      const wide = sc && sc.w > 520;
      stage.classList.toggle('stack', wide);
      const a = h('div', { class: 'half', style: `background:${BG.light}` }, h('div', { class: 'loading' }, h('i', { class: 'spinner' }), 'Starting'));
      const b = h('div', { class: 'half', style: `background:${BG.dark}` });
      stage.append(a, b);
      mountOne(a, false, 'Light');
      mountOne(b, true, 'Dark');
    } else {
      stage.style.setProperty('--stage-bg', sc?.dark_only || (opts.forceDark) ? BG.dark : (isDark() ? BG.dark : BG.light));
      if (sc?.dark_only) stage.dataset.fixedBg = '1';
      stage.append(h('div', { class: 'loading' }, h('i', { class: 'spinner' }), 'Starting'));
      if (!opts.fill && sc) stage.style.minHeight = `${Math.min(sc.h, 900) * Math.min(1, (stage.clientWidth || 900) / (sc.w + 56)) + 58}px`;
      mountOne(stage, undefined);
    }
  }
  block.unmount = function () {
    block.ro?.disconnect();
    block.previews.forEach((p) => p.dispose());
    block.previews = [];
    block.mounted = false;
  };
  function remount() { block.unmount(); mount(); }
  block.remount = remount;

  // mount when near the viewport; free GPU contexts when far away
  block.io = new IntersectionObserver((es) => {
    for (const e of es) {
      if (e.isIntersecting) mount();
      else if (block.mounted && !opts.fill) {
        const r = e.boundingClientRect;
        if (r.bottom < -1600 || r.top > innerHeight + 1600) block.unmount();
      }
    }
  }, { rootMargin: '400px 0px', threshold: 0 });
  block.io.observe(root);
  block.farIO = null;
  block.el = root;
  block.refit = () => block.previews.forEach((p) => p.refit());
  blocks.push(block);
  if (opts.mode === 'code') setMode('code');
  return root;
}

function reference(opts, p) {
  const sc = opts.module === 'scenes' ? CAT.scenes[opts.content] : null;
  const parts = [];
  if (sc) parts.push(`scene "${opts.content}" (${sc.file}:${sc.line})`);
  else parts.push(`${opts.title || opts.module} app (${opts.codeFile})`);
  if (opts.docRef) parts.push(`docs ${opts.docRef}`);
  if (p && p.inst) parts.push(`${p.dark ? 'dark' : 'light'}, ${ACCENTS[p.mod.exports.inst_get_accent(p.inst)]?.[0].toLowerCase()} accent, ${p.w}×${p.h}`);
  return `CUI ${opts.item ? opts.item + ': ' : ''}${parts.join('; ')}`;
}

function addCopy(root) {
  root.querySelectorAll('pre.code').forEach((pre) => {
    if (pre.querySelector('.code-copy')) return;
    pre.classList.add('copyable');
    pre.append(h('button', { class: 'icon-btn code-copy', title: 'Copy', 'aria-label': 'Copy code', html: I.copy, onclick: () => copy(pre.querySelector('code').textContent, 'Code copied') }));
  });
}
function codeView(source, file, line) {
  const lines = (source || '').split('\n');
  const nums = lines.map((_, i) => line + i).join('\n');
  return h('div', { class: 'code-view' },
    h('div', { class: 'code-head' }, h('code', {}, `${file}:${line}`), h('span', { class: 'spacer' }),
      h('button', { class: 'btn ghost sm', html: I.copy + 'Copy code', onclick: () => copy(source, 'Code copied') }),
      h('button', { class: 'btn ghost sm', html: I.copy + 'Copy path', onclick: () => copy(`${file}:${line}`, 'Path copied') })),
    h('pre', {}, h('span', { class: 'ln' }, nums), h('code', { class: 'src', html: highlight(source || '') })));
}

// ---------------------------------------------------------------- sidebar
const order = CAT.categories.flatMap((c) => c.items);
function sidebarMode() {
  const k = (location.hash.match(/^#\/?([^/]*)/) || [])[1] || '';
  return k === 'guide' ? 'guide' : (k === 'c' || k === 'components') ? 'components' : null;
}
function renderGuideNav(q) {
  const nav = $('#nav');
  const [, gid, sec] = location.hash.match(/^#\/guide\/([^/]+)\/?(.*)$/) || [];
  const groups = [['Guide', CAT.guides.filter((g) => g.id !== 'design')], ['Principles', CAT.guides.filter((g) => g.id === 'design')]];
  const out = [];
  for (const [title, list] of groups) {
    const items = list.filter((g) => !q || g.title.toLowerCase().includes(q) || g.sections.some((x) => x.title.toLowerCase().includes(q)));
    if (!items.length) continue;
    out.push(h('div', { class: 'nav-section' }, h('div', { class: 'nav-title' }, title),
      items.flatMap((g) => [
        h('a', { href: `#/guide/${g.id}`, 'aria-current': g.id === gid && !sec ? 'page' : null }, h('span', { class: 'label' }, g.title)),
        g.id === gid || q ? h('div', { class: 'nav-sub' }, g.sections.filter((x) => !q || x.title.toLowerCase().includes(q) || g.title.toLowerCase().includes(q))
          .map((x) => h('a', { href: `#/guide/${g.id}/${x.id}`, 'aria-current': g.id === gid && x.id === sec ? 'page' : null }, h('span', { class: 'label' }, x.title)))) : null])));
  }
  out.push(h('div', { class: 'nav-section' }, h('div', { class: 'nav-title' }, 'Reference'),
    h('a', { href: '#/components' }, h('span', { class: 'label' }, 'All components'))));
  nav.replaceChildren(...(out.length ? out : [h('div', { class: 'empty' }, `Nothing matches “${q}”.`)]));
}
function renderNav(filter = '') {
  const q = filter.trim().toLowerCase();
  const nav = $('#nav');
  const mode = sidebarMode();
  document.body.classList.toggle('no-sidebar', !mode);
  if (!mode) return;
  $('#filter').placeholder = mode === 'guide' ? 'Filter the guide' : 'Filter components';
  if (mode === 'guide') return renderGuideNav(q);
  const sections = [];
  const route = location.hash;
  const match = (it) => !q || it.title.toLowerCase().includes(q) || it.summary.toLowerCase().includes(q) || it.apis.some((a) => a.includes(q));
  for (const c of CAT.categories) {
    const items = c.items.map((id) => CAT.items[id]).filter(match);
    if (!items.length) continue;
    sections.push(h('div', { class: 'nav-section' },
      h('div', { class: 'nav-title' }, c.title, (() => {
        const done = reviewOn() ? items.filter((i) => reviewed[i.id]).length : 0;
        return reviewOn() ? h('span', { class: 'tabular' + (done && done === items.length ? ' all' : '') }, `${done}/${items.length}`) : null;
      })()),
      items.map((it) => h('a', { href: `#/c/${it.id}`, 'aria-current': route === `#/c/${it.id}` ? 'page' : null },
        h('span', { class: 'label' }, it.title),
        reviewOn() && notes[it.id]?.text ? h('i', { class: 'note-dot', title: 'Has a note' }) : null,
        reviewOn() && reviewed[it.id] ? h('span', { class: 'mark', html: I.check, title: 'Reviewed' }) : null))));
  }
  nav.replaceChildren(...(sections.length ? sections : [h('div', { class: 'empty' }, `Nothing matches “${filter}”.`)]));
  const cur = nav.querySelector('[aria-current="page"]');
  if (cur && !filter) cur.scrollIntoView({ block: 'nearest' });
}
function updateCounts() {
  const n = Object.values(notes).filter((x) => x.text?.trim()).length;
  $('#notes-count').textContent = n ? ` ${n}` : '';
  const total = order.length, done = order.filter((id) => reviewed[id]).length;
  $('#review-progress').replaceChildren(
    h('div', {}, `Reviewed ${done} of ${total}`),
    h('div', { class: 'bar' }, h('i', { style: `width:${(done / total * 100).toFixed(1)}%` })));
  renderNav($('#filter').value);
}
$('#filter').addEventListener('input', (e) => renderNav(e.target.value));
$('#filter').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') { const a = $('#nav a:not([aria-current])') || $('#nav a'); if (a) location.hash = a.getAttribute('href'); }
  if (e.key === 'Escape') { e.target.value = ''; renderNav(); }
});

// ---------------------------------------------------------------- views
const main = $('#main');
function setNav(which) {
  document.querySelectorAll('#topnav a').forEach((a) => a.setAttribute('aria-current', a.dataset.nav === which ? 'page' : 'false'));
}

const FIRST_WINDOW = `#include <cui/cui.h>
#include <cui/cui_app.h>

int main(void) {
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "My app" });
    if (!app) return 1;
    bool notify = true;
    int clicks = 0;
    while (cui_app_frame(app)) {
        cui_ctx *ui = cui_app_ui(app);
        cui_column(ui, .pad = 24, .gap = 12) {
            cui_heading(ui, "Welcome", 2);
            cui_switch(ui, "Email me when a build fails", &notify);
            if (cui_button(ui, "Count", .variant = CUI_BUTTON_PRIMARY)) clicks++;
            cui_textf(ui, "Clicked %d times", clicks);
        }
    }
    cui_app_destroy(app);
    return 0;
}`;

const INSTALL = [
  ['CMake', `include(FetchContent)
FetchContent_Declare(cui
    GIT_REPOSITORY https://github.com/yappologistic/CUI.git
    GIT_TAG master)
FetchContent_MakeAvailable(cui)
target_link_libraries(my_app PRIVATE cui::cui)`],
  ['Git', 'git clone https://github.com/yappologistic/CUI.git'],
];
const DEMOS = [['segmented', 'segmented-control'], ['slider', 'slider'], ['rolling-numbers', 'rolling-numbers'],
               ['chips', 'chips-and-rating'], ['select', 'select-and-combobox'], ['menu', 'dropdown-and-context-menu']];

function installBlock() {
  let cur = 0;
  const code = h('code', {});
  const show = () => { code.textContent = INSTALL[cur][1]; tabs.querySelectorAll('button').forEach((b, i) => b.setAttribute('aria-selected', String(i === cur))); };
  const tabs = h('div', { class: 'install-tabs', role: 'tablist' }, INSTALL.map(([label], i) => h('button', { role: 'tab', onclick: () => { cur = i; show(); } }, label)));
  const box = h('div', { class: 'install' }, tabs,
    h('div', { class: 'install-code' }, h('pre', {}, code),
      h('button', { class: 'icon-btn', title: 'Copy', 'aria-label': 'Copy', html: I.copy, onclick: () => copy(INSTALL[cur][1], 'Copied') })));
  show();
  return box;
}

function viewHome() {
  setNav('home');
  const guide = (id) => CAT.guides.find((g) => g.id === id);
  const next = [['getting-started', 'Build CUI, open a window and learn the frame loop.'], ['layout', 'Rows, columns, sizes, grids, scrolling and split views.'],
                ['theming', 'Light and dark, eight accents, tokens, fonts and motion.'], ['custom-components', 'Draw your own components with the same tools.'],
                ['integration', 'Bring your own window, input and renderer.'], ['design', 'The rules every component follows.']];
  const demo = ([scene, item]) => {
    const it = CAT.items[item], sc = CAT.scenes[scene];
    if (!sc) return null;
    return h('div', { class: 'demo' },
      h('div', { class: 'demo-head' }, h('span', {}, it ? it.title : scene),
        it ? h('a', { class: 'demo-link', href: `#/c/${item}`, 'aria-label': `Open ${it.title}`, html: 'Docs' + I.arrow }) : null),
      previewBlock({ module: 'scenes', content: scene, bare: true, item: it?.title, codeSource: sc.source, codeFile: sc.file, codeLine: sc.line }));
  };
  const page = h('div', { class: 'page home' },
    h('div', { class: 'hero' },
      h('h1', {}, 'Native UI components for C'),
      h('p', { class: 'lede' }, 'About 150 components with spring motion and full keyboard support, for Windows, macOS and Linux from one C99 codebase. No dependencies, about 240 KB, MIT licensed.'),
      installBlock(),
      h('div', { class: 'hero-actions' },
        h('a', { class: 'btn primary', href: '#/guide/getting-started', html: 'Get started' + I.arrow }),
        h('a', { class: 'btn', href: '#/components' }, 'Browse components'))),
    h('div', { class: 'previews home-stage' }, previewBlock({ module: 'scenes', content: 'stage-mosaic', title: 'stage-mosaic', item: 'Overview', tight: true,
      codeSource: CAT.scenes['stage-mosaic'].source, codeFile: CAT.scenes['stage-mosaic'].file, codeLine: CAT.scenes['stage-mosaic'].line })),
    h('section', { class: 'home-demos' },
      h('div', { class: 'section-head' },
        h('h2', {}, 'Every component runs here'),
        h('p', {}, 'These are the library itself, compiled to WebAssembly. Click one and use it with the mouse or the keyboard.')),
      h('div', { class: 'demo-grid' }, DEMOS.map(demo))),
    h('section', { class: 'home-split' },
      h('div', {},
        h('h2', {}, 'Your interface is plain C'),
        h('p', {}, 'Components are functions that draw themselves and return what happened. Your variables are the state, and the whole screen is described again every frame, so there is nothing to keep in sync.'),
        h('p', {}, h('a', { class: 'text-link', href: '#/guide/getting-started/how-immediate-mode-works' }, 'How immediate mode works'))),
      (() => { const w = h('div', {}, h('pre', { class: 'code', html: '<code>' + highlight(FIRST_WINDOW) + '</code>' })); addCopy(w); return w.firstChild; })()),
    h('section', { class: 'home-next' },
      h('h2', {}, 'Read next'),
      h('ul', { class: 'link-list' }, next.filter(([id]) => guide(id)).map(([id, text]) =>
        h('li', {}, h('a', { href: `#/guide/${id}` }, h('span', { class: 'link-title' }, guide(id).title), h('span', { class: 'link-text' }, text), h('span', { class: 'link-arrow', html: I.arrow })))))));
  return page;
}

function siteFooter() {
  return h('footer', { class: 'site-footer' },
    h('span', {}, 'CUI is MIT licensed. Every preview on this site is drawn by the library itself.'),
    h('nav', {}, h('a', { href: '#/guide/getting-started' }, 'Guide'), h('a', { href: '#/components' }, 'Components'),
      h('a', { href: 'https://github.com/yappologistic/CUI', target: '_blank', rel: 'noreferrer' }, 'Source')));
}

function viewComponents() {
  setNav('components');
  const page = h('div', { class: 'page' },
    h('h1', {}, 'Components'),
    h('p', { class: 'lede' }, 'Each page shows the component running, the C that draws it, its options and its keyboard support.'));
  for (const c of CAT.categories) {
    page.append(h('div', { class: 'section-title' }, h('h2', {}, c.title), h('span', { class: 'count' }, `${c.items.length}`)),
      h('div', { class: 'grid' }, c.items.map((id) => {
        const it = CAT.items[id];
        return h('a', { class: 'card', href: `#/c/${id}` },
          h('div', { class: 'thumb' }, thumb(it.scenes[0])),
          h('div', { class: 'body' }, h('h3', {}, it.title, reviewOn() && reviewed[id] ? h('span', { class: 'mark', html: I.circleCheck, title: 'Reviewed' }) : null),
            h('p', {}, it.summary)));
      })));
  }
  return page;
}

function viewGuide(id, sec) {
  const g = CAT.guides.find((x) => x.id === id);
  if (!g) return viewMissing();
  setNav('guide');
  const idx = CAT.guides.indexOf(g);
  const prev = CAT.guides[idx - 1], next = CAT.guides[idx + 1];
  const body = h('article', { class: 'doc guide' },
    h('div', { html: g.intro }),
    g.sections.map((x) => h('section', { id: `s-${x.id}` }, h('h2', {}, h('a', { class: 'anchor', href: `#/guide/${g.id}/${x.id}` }, x.title)), h('div', { html: x.html }))));
  body.querySelectorAll('pre.code code').forEach((c) => { c.innerHTML = highlight(c.textContent); });
  addCopy(body);
  body.querySelectorAll('img[data-src]').forEach((img) => {
    img.src = `${img.dataset.src}${isDark() ? '-dark' : ''}.png`;
    img.onerror = () => { if (!img.dataset.retry) { img.dataset.retry = 1; img.src = `${img.dataset.src}.png`; } };
  });
  const toc = g.sections.length > 2 ? h('nav', { class: 'toc', 'aria-label': 'On this page' }, h('div', { class: 'toc-title' }, 'On this page'),
    g.sections.map((x) => h('a', { href: `#/guide/${g.id}/${x.id}`, 'data-sec': x.id }, x.title))) : null;
  const pager = (it2, dir) => it2 ? h('a', { class: 'pager ' + dir, href: `#/guide/${it2.id}` },
    h('span', { class: 'pager-dir' }, dir === 'prev' ? 'Previous' : 'Next'), h('span', { class: 'pager-title' }, it2.title)) : h('span');
  const page = h('div', { class: 'page guide-page' },
    h('div', { class: 'guide-main' },
      h('div', { class: 'crumbs' }, h('a', { href: '#/guide/getting-started' }, 'Guide'), '/', h('span', {}, g.title)),
      h('h1', {}, g.title),
      body,
      h('p', { class: 'source-note' }, 'From ', h('code', {}, g.file), ' in the repository.'),
      h('nav', { class: 'pagers', 'aria-label': 'Previous and next page' }, pager(prev, 'prev'), pager(next, 'next'))),
    toc);
  if (toc) {
    const io = new IntersectionObserver((es) => {
      for (const e of es) if (e.isIntersecting) {
        const sid = e.target.id.slice(2);
        toc.querySelectorAll('a').forEach((a) => a.toggleAttribute('data-active', a.dataset.sec === sid));
      }
    }, { rootMargin: '-80px 0px -70% 0px' });
    setTimeout(() => body.querySelectorAll('section[id]').forEach((el) => io.observe(el)));
  }
  if (sec) setTimeout(() => { const el = document.getElementById(`s-${sec}`); if (el) window.scrollTo(0, el.getBoundingClientRect().top + scrollY - 72); });
  return page;
}

function viewItem(id) {
  const it = CAT.items[id];
  if (!it) return viewMissing();
  setNav('components');
  const cat = CAT.categories.find((c) => c.id === it.category);
  const idx = order.indexOf(id);
  const prev = CAT.items[order[idx - 1]], next = CAT.items[order[idx + 1]];
  const docRef = `${it.doc.file}:${it.doc.line}`;
  const page = h('div', { class: 'page' },
    h('div', { class: 'crumbs' }, h('a', { href: '#/components' }, 'Components'), '/', h('span', {}, cat?.title || '')),
    h('div', { class: 'title-row' },
      h('div', {}, h('h1', {}, it.title), it.summary ? h('p', { class: 'lede' }, it.summary) : null),
      h('div', { class: 'title-actions' },
        reviewToggle(id),
        h('button', { class: 'btn sm', html: I.copy + 'Copy page', title: 'Copy this page as text', onclick: () => copy(pageText(it), 'Page copied') }),
        prev ? h('a', { class: 'icon-btn framed', href: `#/c/${prev.id}`, title: `Previous: ${prev.title}`, 'aria-label': `Previous: ${prev.title}`, html: I.left }) : h('span', { class: 'icon-btn framed disabled', html: I.left }),
        next ? h('a', { class: 'icon-btn framed', href: `#/c/${next.id}`, title: `Next: ${next.title}`, 'aria-label': `Next: ${next.title}`, html: I.arrow }) : h('span', { class: 'icon-btn framed disabled', html: I.arrow }))),
    it.apis.length ? h('div', { class: 'api-line' },
      h('span', { class: 'api-label' }, it.apis.length > 1 ? 'Functions' : 'Function'),
      it.apis.slice(0, 8).map((a) => h('button', { class: 'api', title: `Copy ${a}`, onclick: () => copy(a, `${a} copied`) }, a))) : null);
  // several scenes become variants of one preview, like tabs over a single stage
  const pv = h('div', { class: 'previews' });
  const vt = h('div', { class: 'variant-tabs', role: 'tablist', 'aria-label': 'Examples' });
  const showScene = (s) => {
    const sc = CAT.scenes[s];
    const old = pv.querySelector('.preview');
    if (old) { vt.remove(); const b = blocks.find((x) => x.root === old); if (b) { b.dispose(); blocks.splice(blocks.indexOf(b), 1); } old.remove(); }
    pv.append(previewBlock({ module: 'scenes', content: s, title: s, item: it.title, docRef, codeSource: sc.source, codeFile: sc.file, codeLine: sc.line, leading: vt }));
  };
  if (it.scenes.length > 1) {
    it.scenes.forEach((s, i) => vt.append(h('button', { role: 'tab', 'aria-selected': String(i === 0), onclick: (e) => {
      vt.querySelectorAll('button').forEach((b) => b.setAttribute('aria-selected', String(b === e.currentTarget))); showScene(s);
    } }, sceneLabel(s, it))));
  }
  if (it.scenes.length) showScene(it.scenes[0]);
  page.append(pv);
  page.append(reviewBox(id, it, docRef));
  const docs = h('div', { class: 'docs' },
    h('div', { class: 'doc', html: it.html }));
  for (const im of it.images) {
    docs.append(h('figure', { class: 'figure' }, h('img', { src: `${im.src}${isDark() ? '-dark' : ''}.png`, alt: im.alt,
      onerror: (e) => { if (!e.target.dataset.retry) { e.target.dataset.retry = 1; e.target.src = `${im.src}.png`; } } }),
      h('figcaption', { title: `docs/${im.src}.png` }, im.alt, h('span', { class: 'fig-tag' }, 'Screenshot'))));
  }
  docs.querySelectorAll('pre.code code').forEach((c) => { c.innerHTML = highlight(c.textContent); });
  addCopy(docs);
  docs.append(h('p', { class: 'source-note' }, 'From ', h('code', {}, docRef), ' in the repository.'));
  page.append(docs);
  const pager = (it2, dir) => it2 ? h('a', { class: 'pager ' + dir, href: `#/c/${it2.id}` },
    h('span', { class: 'pager-dir' }, dir === 'prev' ? 'Previous' : 'Next', h('kbd', {}, dir === 'prev' ? '[' : ']')),
    h('span', { class: 'pager-title' }, it2.title)) : h('span');
  page.append(h('nav', { class: 'pagers', 'aria-label': 'Previous and next component' }, pager(prev, 'prev'), pager(next, 'next')));
  return page;
}

// a readable name for a scene variant: "toast-demo" under Toast reads "Demo"
function sceneLabel(s, it) {
  const words = s.split('-');
  const t = it.title.toLowerCase();
  const rest = words.filter((w) => !t.includes(w));
  const label = (rest.length ? rest : words).join(' ');
  return label.charAt(0).toUpperCase() + label.slice(1);
}
function pageText(it) {
  const tmp = h('div', { html: it.html });
  tmp.querySelectorAll('pre').forEach((p) => { p.textContent = '\n```c\n' + p.textContent.trim() + '\n```\n'; });
  return `# ${it.title}\n\n${it.summary || ''}\n\n${tmp.innerText.trim()}\n`;
}

function reviewToggle(id) {
  const b = h('button', { class: 'btn sm review-toggle review-only', 'aria-pressed': String(!!reviewed[id]), title: 'Mark this component as reviewed' });
  const paint = () => { const on = !!reviewed[id]; b.setAttribute('aria-pressed', String(on)); b.innerHTML = (on ? I.circleCheck : I.check) + (on ? 'Reviewed' : 'Mark reviewed'); };
  b.onclick = () => { if (reviewed[id]) delete reviewed[id]; else reviewed[id] = Date.now(); saveReviewed(); paint(); };
  paint();
  return b;
}

function reviewBox(id, it, docRef) {
  const n = notes[id] || {};
  const ta = h('textarea', { rows: 3, placeholder: 'What looks wrong or should change? Collect notes here, then copy them all from the Notes page.' });
  ta.value = n.text || '';
  const when = h('span', {}, n.updated ? `Saved ${new Date(n.updated).toLocaleString()}` : 'Not saved yet');
  let timer = 0;
  ta.addEventListener('input', () => {
    clearTimeout(timer);
    timer = setTimeout(() => {
      const p = blocks[0]?.previews[0];
      const ctx = p && p.inst ? { theme: p.dark ? 'dark' : 'light', accent: ACCENTS[p.mod.exports.inst_get_accent(p.inst)]?.[0], size: `${p.w}×${p.h}` } : null;
      if (ta.value.trim()) notes[id] = { text: ta.value, updated: Date.now(), context: ctx };
      else delete notes[id];
      saveNotes();
      when.textContent = notes[id] ? `Saved ${new Date().toLocaleTimeString()}` : 'Cleared';
    }, 400);
  });
  const box = h('div', { class: 'review-box review-only' + (n.text ? ' open' : '') },
    h('header', {},
      h('button', { class: 'review-open', 'aria-expanded': String(!!n.text), html: I.note + '<span>' + (n.text ? 'Note' : 'Add a note') + '</span>',
        onclick: (e) => { const open = !box.classList.contains('open'); box.classList.toggle('open', open); e.currentTarget.setAttribute('aria-expanded', String(open)); if (open) ta.focus(); } }),
      h('span', { class: 'spacer' }),
      h('button', { class: 'btn ghost sm review-copy', html: I.copy + 'Copy as Markdown', onclick: () => copy(noteMarkdown(id), 'Note copied') })),
    h('div', { class: 'review-body' }, ta,
      h('footer', {}, when, h('span', { class: 'spacer' }), h('span', {}, 'Stays in this browser, with the preview’s theme, accent and size'))));
  return box;
}

function noteMarkdown(id) {
  const it = CAT.items[id], n = notes[id];
  if (!it) return '';
  const where = [`docs: ${it.doc.file}:${it.doc.line}`, ...it.scenes.map((s) => `scene "${s}": ${CAT.scenes[s].file}:${CAT.scenes[s].line}`)];
  let md = `## ${it.title}\n\n- Where: ${where.join('; ')}\n`;
  if (n?.context) md += `- Seen in: ${n.context.theme} theme, ${n.context.accent?.toLowerCase()} accent, ${n.context.size}\n`;
  md += `\n${(n?.text || '').trim()}\n`;
  return md;
}

function viewNotes() {
  setNav('notes');
  const ids = order.filter((id) => notes[id]?.text?.trim());
  const all = () => `# CUI review notes\n\nCollected in the live component preview.\n\n${ids.map(noteMarkdown).join('\n')}`;
  const page = h('div', { class: 'page' },
    h('div', { class: 'title-row' },
      h('div', {}, h('h1', {}, 'Notes'),
        h('p', { class: 'lede' }, 'Everything you wrote while reviewing, with the files and lines each component lives in. Copy it all and paste it into a request for changes.')),
      ids.length ? h('button', { class: 'btn primary', html: I.copy + 'Copy all as Markdown', onclick: () => copy(all(), `Copied ${ids.length} notes`) }) : null),
    h('div', { class: 'meta' },
      h('span', { class: 'chip' }, `${ids.length} with notes`),
      h('span', { class: 'chip' }, `${order.filter((id) => reviewed[id]).length} of ${order.length} reviewed`),
      Object.keys(reviewed).length ? h('button', { class: 'chip', onclick: () => { if (confirm('Clear every reviewed mark? Notes stay.')) { reviewed = {}; saveReviewed(); route(); } } }, 'Clear reviewed marks') : null));
  if (!ids.length) {
    page.append(h('div', { class: 'empty-state' }, h('h3', {}, 'No notes yet'),
      h('p', {}, 'Open a component, try it, and write what should change in the box under its preview.'),
      h('a', { class: 'btn', href: `#/c/${order[0]}` }, 'Start with the first component')));
    return page;
  }
  page.append(h('div', { class: 'notes-list' }, ids.map((id) => {
    const it = CAT.items[id], n = notes[id];
    return h('article', { class: 'note-item' },
      h('header', {}, h('a', { href: `#/c/${id}` }, it.title), h('span', { class: 'where' }, `${it.doc.file}:${it.doc.line}`),
        h('span', { class: 'spacer' }), h('span', { class: 'muted', style: 'font-size:12px' }, new Date(n.updated).toLocaleString())),
      h('div', { class: 'text' }, n.text));
  })));
  return page;
}

function viewShowcase() {
  setNav('showcase');
  const page = h('div', { class: 'page' },
    h('h1', {}, 'Showcase'),
    h('p', { class: 'lede' }, 'Larger compositions that put many components together. Each one runs live; open Code to see the C that draws it.'),
    h('div', { class: 'hero-actions' }, h('a', { class: 'btn', href: '#/accents' }, 'See all eight accents')));
  const pv = h('div', { class: 'previews' });
  for (const s of CAT.showcase) {
    const sc = CAT.scenes[s.id];
    pv.append(h('div', {}, h('h2', {}, s.title), h('p', { class: 'lede', style: 'margin:4px 0 12px;font-size:14px' }, s.summary),
      previewBlock({ module: 'scenes', content: s.id, title: s.id, item: s.title, codeSource: sc.source, codeFile: sc.file, codeLine: sc.line })));
  }
  page.append(pv);
  return page;
}

function viewAccents() {
  setNav('showcase');
  const page = h('div', { class: 'page' },
    h('div', { class: 'crumbs' }, h('a', { href: '#/showcase' }, 'Showcase'), '/', h('span', {}, 'Accents')),
    h('h1', {}, 'Accents'),
    h('p', { class: 'lede' }, 'The same controls in each of the eight accents. These keep their own accent when you pick one in the top bar.'));
  const grid = h('div', { class: 'accent-grid' });
  for (const name of CAT.accents) {
    const sc = CAT.scenes[name];
    grid.append(previewBlock({ module: 'scenes', content: name, title: name.replace('accent-', ''), item: 'Accents',
                               codeSource: sc.source, codeFile: sc.file, codeLine: sc.line, pair: false, fixedAccent: true }));
  }
  page.append(grid);
  return page;
}

function viewApps() {
  setNav('apps');
  return h('div', { class: 'page' },
    h('h1', {}, 'Apps'),
    h('p', { class: 'lede' }, 'Complete apps from the examples folder, running live. Each one resizes with the window, and the width presets show its smaller layouts.'),
    h('div', { class: 'grid app-grid', style: 'margin-top:28px' }, CAT.apps.map((a) => h('a', { class: 'card', href: `#/app/${a.id}` },
      h('div', { class: 'thumb', style: 'padding:0' }, thumb('', a.id)),
      h('div', { class: 'body' }, h('h3', {}, a.title), h('p', {}, a.summary))))));
}

const WIDTHS = [['Fill', 0], ['1440', 1440], ['1280', 1280], ['1024', 1024], ['768', 768], ['390', 390]];
function viewApp(id) {
  const a = CAT.apps.find((x) => x.id === id);
  if (!a) return viewMissing();
  setNav('apps');
  let width = 0;
  const seg = h('div', { class: 'seg', role: 'radiogroup', 'aria-label': 'Viewport width' });
  const page = h('div', { class: 'page wide' });
  const holder = h('div', { class: 'app-view' });
  const build = () => {
    disposeBlocks();
    holder.replaceChildren(previewBlock({ module: a.id, title: a.source, item: a.title, fill: true, codeFile: a.source, codeLine: 1,
      codeSource: () => fetchSource(a.source), fixedWidth: width ? () => width : null }));
    const stage = holder.querySelector('.stage');
    if (width) { stage.style.justifyContent = 'flex-start'; stage.style.overflowX = 'auto'; }
  };
  seg.append(...WIDTHS.map(([label, w]) => h('button', { role: 'radio', 'aria-checked': String(w === width), onclick: (e) => {
    width = w; seg.querySelectorAll('button').forEach((b) => b.setAttribute('aria-checked', String(b === e.target))); build();
  } }, label)));
  page.append(h('div', { class: 'app-head' },
    h('a', { class: 'btn ghost sm', href: '#/apps', html: I.left + 'Apps' }),
    h('h1', {}, a.title), h('p', { class: 'lede' }, a.summary), h('span', { class: 'spacer' }), h('span', { class: 'muted', style: 'font-size:12px' }, 'Width'), seg), holder);
  setTimeout(build);
  return page;
}
const sourceCache = new Map();
async function fetchSource(path) {
  if (!sourceCache.has(path)) sourceCache.set(path, fetch(`src/${path}`).then((r) => (r.ok ? r.text() : '')).catch(() => ''));
  return sourceCache.get(path);
}
function viewMissing() {
  return h('div', { class: 'page' }, h('div', { class: 'empty-state' }, h('h3', {}, 'Nothing here'),
    h('p', {}, 'That page does not exist.'), h('a', { class: 'btn', href: '#/' }, 'Go to the overview')));
}

// ---------------------------------------------------------------- router
function route() {
  disposeBlocks();
  const hash = location.hash || '#/';
  lastRoute = hash;
  const [, kind, id] = hash.match(/^#\/?([^/]*)\/?(.*)$/) || [];
  let view;
  if (!kind) view = viewHome();
  else if (kind === 'c') view = viewItem(decodeURIComponent(id));
  else if (kind === 'components') view = viewComponents();
  else if (kind === 'guide') { const [gid, sec] = (id || 'getting-started').split('/'); view = viewGuide(decodeURIComponent(gid || 'getting-started'), sec); }
  else if (kind === 'showcase') view = viewShowcase();
  else if (kind === 'accents') view = viewAccents();
  else if (kind === 'apps') view = viewApps();
  else if (kind === 'app') view = viewApp(decodeURIComponent(id));
  else if (kind === 'notes' && reviewOn()) view = viewNotes();
  else view = viewMissing();
  document.body.classList.toggle('app-mode', kind === 'app');
  document.body.classList.toggle('home-mode', !kind || kind === 'home');
  main.replaceChildren(view, ...(kind === 'app' ? [] : [siteFooter()]));
  window.scrollTo(0, 0);
  renderNav($('#filter').value);
  const it = kind === 'c' && CAT.items[id];
  const gd = kind === 'guide' && CAT.guides.find((g) => g.id === (id || 'getting-started').split('/')[0]);
  document.title = it ? `${it.title} · CUI` : gd ? `${gd.title} · CUI` : kind === 'components' ? 'Components · CUI' : 'CUI';
}
let lastRoute = '';
window.addEventListener('hashchange', () => {
  const m = location.hash.match(/^#\/guide\/([^/]+)\/(.+)$/), p = lastRoute.match(/^#\/guide\/([^/]+)/);
  if (m && p && m[1] === p[1]) {
    const el = document.getElementById(`s-${m[2]}`);
    if (el) { window.scrollTo({ top: el.getBoundingClientRect().top + scrollY - 72, behavior: prefs.reducedMotion ? 'auto' : 'smooth' }); lastRoute = location.hash; renderNav($('#filter').value); return; }
  }
  route();
});
let resizeTimer = 0;
window.addEventListener('resize', () => { clearTimeout(resizeTimer); resizeTimer = setTimeout(() => blocks.forEach((b) => b.refit()), 120); });

// ---------------------------------------------------------------- settings popover
function openSettings(anchor) {
  closePopover();
  const seg = (label, options, value, set) => {
    const s = h('div', { class: 'seg', role: 'radiogroup', 'aria-label': label });
    s.append(...options.map(([text, v]) => h('button', { role: 'radio', 'aria-checked': String(v === value), onclick: () => {
      set(v); s.querySelectorAll('button').forEach((b, i) => b.setAttribute('aria-checked', String(options[i][1] === v)));
    } }, text)));
    return s;
  };
  const sw = (checked, set) => {
    const input = h('input', { type: 'checkbox', checked: checked ? true : null, onchange: (e) => set(e.target.checked) });
    return h('label', { class: 'toggle' }, input, h('i'));
  };
  const rebuild = () => { savePrefs(); syncSettings(); route(); };
  const pop = h('div', { class: 'popover', role: 'dialog', 'aria-label': 'Settings' },
    h('div', { class: 'row' }, h('span', {}, 'Accent', h('small', {}, 'Click the active one again for each scene’s own')), h('div', { class: 'swatches', id: 'accents', role: 'radiogroup', 'aria-label': 'Accent' })),
    h('div', { class: 'row' }, h('span', {}, 'Theme'), seg('Theme', [['Light', 'light'], ['Dark', 'dark'], ['System', 'system']], prefs.theme, (v) => { prefs.theme = v; savePrefs(); applyTheme(); })),
    h('div', { class: 'row' }, h('span', {}, 'Renderer'), seg('Renderer', [['WebGL2', 'gpu'], ['Software', 'software']], prefs.renderer, (v) => { prefs.renderer = v; rebuild(); })),
    h('div', { class: 'row' }, h('span', {}, 'Motion speed'), seg('Motion speed', [['1×', 1], ['½×', 0.5], ['¼×', 0.25], ['⅒×', 0.1]], prefs.speed, (v) => { prefs.speed = v; savePrefs(); syncSettings(); })),
    h('div', { class: 'row' }, h('span', {}, 'Pixel ratio'), seg('Pixel ratio', [['Auto', 'auto'], ['1×', 1], ['2×', 2]], prefs.pixelRatio, (v) => { prefs.pixelRatio = v; rebuild(); })),
    h('div', { class: 'row' }, h('span', {}, 'Reduced motion'), sw(prefs.reducedMotion, (v) => { prefs.reducedMotion = v; savePrefs(); syncSettings(); allLive().forEach((p) => p.setReducedMotion(v)); })),
    h('div', { class: 'row' }, h('span', {}, 'Light and dark side by side'), sw(prefs.pair, (v) => { prefs.pair = v; rebuild(); })),
    h('div', { class: 'row' }, h('span', {}, 'Review tools', h('small', {}, 'Notes, reviewed marks and canvas tools')), sw(prefs.review, (v) => { prefs.review = v; savePrefs(); applyReview(); updateCounts(); route(); })));
  document.body.append(pop);
  renderAccents();
  const r = anchor.getBoundingClientRect();
  pop.style.top = `${r.bottom + 8}px`;
  pop.style.right = `${Math.max(8, innerWidth - r.right)}px`;
  setTimeout(() => document.addEventListener('pointerdown', outside));
  function outside(e) { if (!pop.contains(e.target) && e.target !== anchor) closePopover(); }
  openSettings.close = () => { pop.remove(); document.removeEventListener('pointerdown', outside); };
}
function closePopover() { openSettings.close?.(); openSettings.close = null; }
$('#settings-open').addEventListener('click', (e) => (openSettings.close ? closePopover() : openSettings(e.currentTarget)));
$('#theme-toggle').addEventListener('click', () => { prefs.theme = isDark() ? 'light' : 'dark'; savePrefs(); applyTheme(); });

// ---------------------------------------------------------------- search palette
function openSearch() {
  if ($('.scrim')) return;
  const entries = [
    ...CAT.guides.flatMap((g) => [{ href: `#/guide/${g.id}`, title: g.title, sub: g.lede, group: 'Guide', hay: `${g.title} ${g.lede}`.toLowerCase() },
      ...g.sections.map((x) => ({ href: `#/guide/${g.id}/${x.id}`, title: x.title, sub: g.title, group: 'Guide', hay: `${x.title} ${g.title}`.toLowerCase(), minor: true }))]),
    ...order.map((id) => { const it = CAT.items[id]; return { href: `#/c/${id}`, title: it.title, sub: it.summary, group: CAT.categories.find((c) => c.id === it.category).title, hay: `${it.title} ${it.summary} ${it.apis.join(' ')} ${it.scenes.join(' ')}`.toLowerCase() }; }),
    ...CAT.showcase.map((s) => ({ href: '#/showcase', title: s.title, sub: s.summary, group: 'Showcase', hay: `${s.title} ${s.summary} ${s.id}`.toLowerCase() })),
    ...CAT.apps.map((a) => ({ href: `#/app/${a.id}`, title: `${a.title} app`, sub: a.summary, group: 'Apps', hay: `${a.title} ${a.summary} app`.toLowerCase() })),
  ];
  const input = h('input', { placeholder: 'Search the guide, components and functions', 'aria-label': 'Search' });
  const list = h('div', { class: 'palette-list', role: 'listbox' });
  let results = [], sel = 0;
  const render = () => {
    const q = input.value.trim().toLowerCase().split(/\s+/).filter(Boolean);
    results = entries.filter((e) => (q.length || !e.minor) && q.every((t) => e.hay.includes(t)));
    if (q.length) results.sort((a, b) => (b.title.toLowerCase().startsWith(q[0]) - a.title.toLowerCase().startsWith(q[0])));
    results = results.slice(0, 60);
    sel = Math.min(sel, Math.max(0, results.length - 1));
    list.replaceChildren();
    let group = null;
    results.forEach((r, i) => {
      if (r.group !== group && !q.length) { group = r.group; list.append(h('div', { class: 'palette-group' }, group)); }
      list.append(h('div', { class: 'palette-item', role: 'option', 'aria-selected': String(i === sel), onclick: () => go(i), onpointermove: () => { if (sel !== i) { sel = i; mark(); } } },
        h('div', { class: 't' }, h('div', {}, r.title), r.sub ? h('div', {}, r.sub) : null), q.length ? h('span', { class: 'c' }, r.group) : null));
    });
    if (!results.length) list.append(h('div', { class: 'empty-state', style: 'padding:32px' }, 'No matches'));
  };
  const mark = () => list.querySelectorAll('.palette-item').forEach((el, i) => { el.setAttribute('aria-selected', String(i === sel)); if (i === sel) el.scrollIntoView({ block: 'nearest' }); });
  const close = () => { scrim.remove(); };
  const go = (i) => { const r = results[i]; close(); if (r) location.hash = r.href; };
  input.addEventListener('input', () => { sel = 0; render(); });
  input.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown') { sel = Math.min(sel + 1, results.length - 1); mark(); e.preventDefault(); }
    else if (e.key === 'ArrowUp') { sel = Math.max(sel - 1, 0); mark(); e.preventDefault(); }
    else if (e.key === 'Enter') go(sel);
    else if (e.key === 'Escape') close();
  });
  const scrim = h('div', { class: 'scrim', onpointerdown: (e) => { if (e.target === scrim) close(); } },
    h('div', { class: 'palette', role: 'dialog', 'aria-label': 'Search' },
      h('div', { class: 'palette-input', html: I.search }, input), list,
      h('div', { class: 'palette-foot' }, h('span', {}, h('kbd', {}, '↑↓'), ' move'), h('span', {}, h('kbd', {}, 'Enter'), ' open'), h('span', {}, h('kbd', {}, 'Esc'), ' close'))));
  scrim.querySelector('.palette-input svg').style.cssText = '';
  document.body.append(scrim);
  render();
  input.focus();
}
$('#search-open').addEventListener('click', openSearch);
document.addEventListener('keydown', (e) => {
  const t = e.target;
  if (t instanceof HTMLCanvasElement) return;   // the preview has the keyboard
  const typing = t instanceof HTMLInputElement || t instanceof HTMLTextAreaElement;
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') { e.preventDefault(); openSearch(); return; }
  if (typing) return;
  if (e.key === '/') { e.preventDefault(); openSearch(); }
  const m = location.hash.match(/^#\/c\/(.+)$/);
  if (m && (e.key === '[' || e.key === ']')) {
    const i = order.indexOf(decodeURIComponent(m[1])) + (e.key === ']' ? 1 : -1);
    if (order[i]) location.hash = `#/c/${order[i]}`;
  }
  const gm = location.hash.match(/^#\/guide\/([^/]+)/);
  if (gm && (e.key === '[' || e.key === ']')) {
    const i = CAT.guides.findIndex((g) => g.id === gm[1]) + (e.key === ']' ? 1 : -1);
    if (CAT.guides[i]) location.hash = `#/guide/${CAT.guides[i].id}`;
  }
  if (e.key === 'Escape') closePopover();
});

// ---------------------------------------------------------------- start
window.cuiDebug = { allLive, blocks: () => blocks, settings };
applyTheme();
applyReview();
updateCounts();
route();
