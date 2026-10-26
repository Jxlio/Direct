// Themes must stay readable: every text level on every surface, and every accent with its label.
// Run with: node --test tests/ui/
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const theme = require('../../src/windows/ui/theme.js');
const css = fs.readFileSync(path.resolve(__dirname, '../../src/windows/ui/style.css'), 'utf8');

// Reads the variables of one theme block, on top of the defaults declared in the first :root rule.
function tokens(id) {
  const read = (selector) => {
    const start = css.indexOf(selector);
    assert.ok(start >= 0, `missing ${selector}`);
    const body = css.slice(css.indexOf('{', start) + 1, css.indexOf('}', start));
    return Object.fromEntries([...body.matchAll(/--([a-z0-9-]+):(#[0-9a-fA-F]{3,8})\b/g)].map(([, name, value]) => [name, value.length === 4 ? '#' + [...value.slice(1)].map((c) => c + c).join('') : value]));
  };
  return { ...read(':root{--bg-deep'), ...(id === 'dark' ? {} : read(`:root[data-theme="${id}"]`)) };
}

test('every theme is declared in the stylesheet and in the picker', () => {
  // "auto" is not a look of its own: it picks Nuit or Jour from the system, so only the picker tile has a rule.
  for (const [id] of theme.THEMES) assert.ok(id === 'dark' || css.includes(`:root[data-theme="${id}"]`) || (id === 'auto' && css.includes('.theme-preview[data-theme="auto"]')), id);
  assert.equal(new Set(theme.THEMES.map(([id]) => id)).size, theme.THEMES.length);
});

test('text meets contrast on every surface it is drawn on', () => {
  const surfaces = ['bg-deep', 'bg-side', 'bg-main', 'bg-card', 'bg-raised', 'bg-hover'];
  for (const [id] of theme.THEMES.filter(([id]) => id !== 'auto')) {
    const t = tokens(id);
    for (const surface of surfaces) {
      assert.ok(theme.ratio(t.text, t[surface]) >= 7, `${id}: text on ${surface} = ${theme.ratio(t.text, t[surface]).toFixed(2)}`);
      assert.ok(theme.ratio(t['text-2'], t[surface]) >= 4.5, `${id}: text-2 on ${surface} = ${theme.ratio(t['text-2'], t[surface]).toFixed(2)}`);
      assert.ok(theme.ratio(t['text-3'], t[surface]) >= 4.5, `${id}: text-3 on ${surface} = ${theme.ratio(t['text-3'], t[surface]).toFixed(2)}`);
      assert.ok(theme.ratio(t['text-4'], t[surface]) >= 3.5, `${id}: text-4 on ${surface} = ${theme.ratio(t['text-4'], t[surface]).toFixed(2)}`);
    }
  }
});

test('surfaces and borders stay distinguishable', () => {
  for (const [id] of theme.THEMES.filter(([id]) => id !== 'auto')) {
    const t = tokens(id);
    assert.ok(theme.ratio(t['line-strong'], t['bg-card']) >= (id === 'contrast' ? 7 : 1.5), `${id}: border on card`);
    assert.notEqual(t['bg-side'].toLowerCase(), t['line'].toLowerCase(), `${id}: sidebar equals its border`);
  }
});

test('the label on every accent colour is readable, whatever theme or custom colour is chosen', () => {
  const colors = [...theme.ACCENTS.map(([hex]) => hex), '#000000', '#ffffff', '#808080', '#ffff00', '#0000ff', '#7f7f7f', '#23a559'];
  for (const hex of colors) assert.ok(theme.ratio(theme.readableOn(hex), hex) >= 4.5, `${hex} → ${theme.readableOn(hex)} = ${theme.ratio(theme.readableOn(hex), hex).toFixed(2)}`);
});

test('accent colours are visible against every card', () => {
  for (const [id] of theme.THEMES.filter(([id]) => id !== 'auto')) for (const [hex, name] of theme.ACCENTS) {
    const t = tokens(id);
    // Lime is made for dark surfaces; on Jour it is a fill (with a darker edge and dark label), never thin lines.
    if (id === 'light' && hex === '#e6ff00') continue;
    assert.ok(theme.ratio(hex, t['bg-card']) >= 2.2, `${id}: ${name} ${hex} on card = ${theme.ratio(hex, t['bg-card']).toFixed(2)}`);
  }
});

test('saved settings are validated, never trusted', () => {
  assert.deepEqual(theme.sanitize(null), theme.DEFAULTS);
  assert.deepEqual(theme.sanitize({ theme: 'nope', accent: 'red', scale: 9 }), theme.DEFAULTS);
  assert.deepEqual(theme.sanitize({ theme: 'light', accent: '#AABBCC', scale: 1.25 }), { theme: 'light', accent: '#aabbcc', accent2: theme.DEFAULTS.accent2, logo: theme.DEFAULTS.logo, scale: 1.25 });
  assert.equal(theme.sanitize({ accent: 'url(javascript:1)' }).accent, theme.DEFAULTS.accent);
  assert.equal(theme.sanitize({ accent: '#12345' }).accent, theme.DEFAULTS.accent);
});

test('a second accent and a logo are chosen the same careful way', () => {
  assert.equal(theme.sanitize({ accent2: '#7457FF' }).accent2, '#7457ff');
  assert.equal(theme.sanitize({ accent2: 'expression(1)' }).accent2, theme.DEFAULTS.accent2);
  assert.equal(theme.sanitize({ logo: 'rings' }).logo, 'rings');
  assert.equal(theme.sanitize({ logo: '<script>' }).logo, theme.DEFAULTS.logo);
  for (const [id] of theme.LOGOS) assert.ok(fs.readFileSync(path.resolve(__dirname, '../../src/windows/ui/index.html'), 'utf8').includes(`id="logo-${id}"`), id);
});
