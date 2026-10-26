// Appearance: theme, accent colour and text size. Loaded in <head> without defer so the saved look
// is in place before the first paint; style.css defines what each theme and variable means.
(function (root) {
  'use strict';
  const KEY = 'murmur.appearance.v1';
  const THEMES = [
    ['auto', 'Auto'], ['dark', 'Nuit'], ['light', 'Jour'], ['midnight', 'Minuit'], ['amoled', 'Noir pur'], ['dusk', 'Crépuscule'], ['contrast', 'Contraste élevé'], ['marathon', 'Marathon', '#c8f31d'],
  ];
  const ACCENTS = [
    ['#ff5b3d', 'Corail'], ['#e6ff00', 'Lime'], ['#7457ff', 'Violet'], ['#6b7178', 'Graphite'],
    ['#23a559', 'Vert'], ['#00a8fc', 'Bleu clair'], ['#eb459f', 'Rose'], ['#d9951a', 'Ambre'],
  ];
  const LOGOS = [['petals', 'Pétales'], ['ripple', 'Ondes'], ['rings', 'Anneaux'], ['dots', 'Cercles']];
  const SCALES = [[1, 'Normal'], [1.12, 'Grand'], [1.25, 'Très grand']];
  const DEFAULTS = Object.freeze({ theme: 'dark', accent: '#ff5b3d', accent2: '#e6ff00', logo: 'petals', scale: 1 });

  const channel = value => { const v = value / 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; };
  function luminance(hex) {
    const n = parseInt(hex.slice(1), 16);
    return 0.2126 * channel((n >> 16) & 255) + 0.7152 * channel((n >> 8) & 255) + 0.0722 * channel(n & 255);
  }
  const ratio = (a, b) => { const [x, y] = [luminance(a), luminance(b)].sort((p, q) => q - p); return (x + 0.05) / (y + 0.05); };
  const validColor = value => typeof value === 'string' && /^#[0-9a-f]{6}$/i.test(value);
  // Text on the accent is white or near-black, whichever reads better on that colour.
  const readableOn = hex => (ratio('#ffffff', hex) >= ratio('#101214', hex) ? '#ffffff' : '#101214');

  function sanitize(value) {
    const out = { ...DEFAULTS };
    if (value && THEMES.some(([id]) => id === value.theme)) out.theme = value.theme;
    if (value && validColor(value.accent)) out.accent = value.accent.toLowerCase();
    if (value && validColor(value.accent2)) out.accent2 = value.accent2.toLowerCase();
    if (value && LOGOS.some(([id]) => id === value.logo)) out.logo = value.logo;
    if (value && SCALES.some(([scale]) => scale === value.scale)) out.scale = value.scale;
    return out;
  }
  function load() {
    try { return sanitize(JSON.parse(root.localStorage.getItem(KEY))); } catch { return { ...DEFAULTS }; }
  }
  function apply(settings, persist = true) {
    const look = sanitize(settings), html = root.document.documentElement;
    // "Auto" follows the system's day/night setting; the stylesheet only knows the real themes.
    html.dataset.theme = look.theme === 'auto' ? (root.matchMedia && root.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark') : look.theme;
    html.style.setProperty('--accent', look.accent);
    html.style.setProperty('--on-accent', readableOn(look.accent));
    html.style.setProperty('--accent-2', look.accent2);
    html.style.setProperty('--on-accent-2', readableOn(look.accent2));
    html.dataset.logo = look.logo;
    html.style.zoom = look.scale === 1 ? '' : String(look.scale);
    if (persist) { try { root.localStorage.setItem(KEY, JSON.stringify(look)); } catch { /* kept for this session */ } }
    return look;
  }

  const api = { THEMES, ACCENTS, LOGOS, SCALES, DEFAULTS, luminance, ratio, readableOn, sanitize, load, apply };
  if (typeof module !== 'undefined') module.exports = api;
  if (root.document) {
    root.murmurTheme = api; apply(load(), false);
    try { root.matchMedia('(prefers-color-scheme: light)').addEventListener('change', () => { if (load().theme === 'auto') apply(load(), false); }); } catch { /* no system setting to follow */ }
  }
})(typeof window !== 'undefined' ? window : globalThis);
