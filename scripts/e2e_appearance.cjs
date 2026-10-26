// Checks themes, accent, text size, user.css and the "who is speaking" display in the real app.
//   1. start the app with: WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9223 Murmur.exe
//   2. run:                NODE_PATH=service/node_modules node scripts/e2e_appearance.cjs
// A user.css is written to the real data folder for a moment (an existing one is put back afterwards).
const WebSocket = require('ws');
const fs = require('node:fs');
const path = require('node:path');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (name, condition, detail = '') => { console.log(`${condition ? 'ok  ' : 'FAIL'} ${name}${detail ? ' — ' + detail : ''}`); if (!condition) failed++; };

async function connectPage() {
  let page;
  for (let attempt = 0; attempt < 30 && !page; attempt++) {
    try { page = (await (await fetch('http://127.0.0.1:9223/json')).json()).find((t) => t.type === 'page' && t.url.includes('murmur.local')); } catch { /* not listening yet */ }
    if (!page) await sleep(500);
  }
  if (!page) throw new Error('page not found on port 9223');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((r) => ws.on('open', r));
  let id = 0; const pending = new Map();
  ws.on('message', (data) => { const m = JSON.parse(data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } });
  return {
    evaluate: (expression) => new Promise((resolve, reject) => {
      const mine = ++id; pending.set(mine, (m) => (m.result?.exceptionDetails ? reject(new Error(m.result.exceptionDetails.text + ' ' + (m.result.exceptionDetails.exception?.description || ''))) : resolve(m.result?.result?.value)));
      ws.send(JSON.stringify({ id: mine, method: 'Runtime.evaluate', params: { expression, returnByValue: true, awaitPromise: true } }));
    }),
    close: () => ws.close(),
  };
}
const folder = path.join(process.env.LOCALAPPDATA, 'Murmur'), userCss = path.join(folder, 'user.css');

(async () => {
  const page = await connectPage(), ev = page.evaluate;
  const saved = fs.existsSync(userCss) ? fs.readFileSync(userCss) : null;
  try {
    await ev("localStorage.removeItem('murmur.appearance.v1'); murmurTheme.apply(murmurTheme.DEFAULTS, false); 1");
    const bg = () => ev("getComputedStyle(document.querySelector('.content')).backgroundColor");
    const accent = () => ev("getComputedStyle(document.querySelector('.primary-button')).backgroundColor");
    check('theme.js and the appearance card load in the app', (await ev("document.querySelectorAll('.theme-option').length")) === 8 && (await ev("document.querySelectorAll('#accent-row .accent-swatch').length")) === 8);
    const dark = await bg();
    check('default accent is the Murmur coral', (await accent()) === 'rgb(255, 91, 61)', await accent());

    // pick a theme and an accent through the real controls
    await ev("document.querySelector('.theme-option[data-theme=\"light\"]').click(); 1");
    await sleep(300);
    check('choosing Jour changes the surfaces', (await bg()) !== dark && (await ev("document.documentElement.dataset.theme")) === 'light', await bg());
    await ev("document.querySelectorAll('#accent-row .accent-swatch')[2].click(); 1");
    await sleep(400);                                            // buttons fade between colours
    check('choosing a swatch changes the accent colour of buttons', (await accent()) === 'rgb(116, 87, 255)', await accent());
    check('the text on the accent stays readable', (await ev("getComputedStyle(document.querySelector('.primary-button')).color")) === 'rgb(255, 255, 255)');
    await ev("document.querySelectorAll('#scale-row button')[2].click(); 1");
    check('text size scales the interface', parseFloat(await ev("document.documentElement.style.zoom")) === 1.25);
    const stored = JSON.parse(await ev("localStorage.getItem('murmur.appearance.v1')"));
    check('the choice is remembered', stored.theme === 'light' && stored.accent === '#7457ff' && stored.scale === 1.25, JSON.stringify(stored));
    await ev("location.reload(); 1");
    await sleep(2500);
    const again = await connectPage();
    check('the saved look is applied again after a reload, before anything else',
      (await again.evaluate("document.documentElement.dataset.theme + '|' + document.documentElement.style.getPropertyValue('--accent')")) === 'light|#7457ff');
    await again.evaluate("document.getElementById('appearance-reset').click(); 1");
    check('Rétablir returns to the defaults', (await again.evaluate("document.documentElement.dataset.theme + '|' + document.documentElement.style.getPropertyValue('--accent')")) === 'dark|#ff5b3d');
    again.close();
  } finally { /* the page object below is re-created because of the reload */ }

  const p2 = await connectPage(), e2 = p2.evaluate;
  // user.css: served from the data folder, picked up by "Recharger"
  fs.mkdirSync(folder, { recursive: true });
  fs.writeFileSync(userCss, ':root{--probe:42px} .brand-mark{letter-spacing:7px}');
  await e2("document.getElementById('theme-reload').click(); 1");
  await sleep(800);
  check('user.css is served from the data folder and applied after Recharger', (await e2("getComputedStyle(document.documentElement).getPropertyValue('--probe').trim()")) === '42px');
  fs.writeFileSync(userCss, '');
  await e2("document.getElementById('theme-reload').click(); 1");
  await sleep(800);
  check('an empty user.css changes nothing', (await e2("getComputedStyle(document.documentElement).getPropertyValue('--probe').trim()")) === '');
  fs.writeFileSync(userCss, 'a'.repeat(300000));
  await e2("document.getElementById('theme-reload').click(); 1");
  await sleep(800);
  check('an oversize user.css is ignored', (await e2("getComputedStyle(document.documentElement).getPropertyValue('--probe').trim()")) === '');
  if (saved) fs.writeFileSync(userCss, saved); else fs.rmSync(userCss, { force: true });

  // who is speaking
  const ring = () => e2("[document.getElementById('self-participant').classList.contains('speaking'), document.getElementById('peer-participant').classList.contains('speaking'), document.getElementById('voice-speaking').textContent, document.querySelector('.call-indicator').classList.contains('speaking')].join('|')");
  await e2("handle({type:'levels', self:0, peer:0}); 1");
  check('nobody speaks by default', (await ring()) === 'false|false||false', await ring());
  await e2("handle({type:'levels', self:0, peer:55}); 1");
  check('the other person speaking lights their ring and the call indicator', (await ring()).startsWith('false|true|') && (await ring()).endsWith('|true'), await ring());
  await e2("handle({type:'levels', self:40, peer:0}); 1");
  check('speaking yourself lights your own ring', (await ring()).startsWith('true|false|Vous parlez'), await ring());
  await e2("handle({type:'levels', self:8, peer:11}); 1");
  check('background noise below the threshold does not count', (await ring()) === 'false|false||false', await ring());

  p2.close();
  console.log(failed ? `\n${failed} check(s) failed` : '\nAll appearance checks passed');
  process.exit(failed ? 1 : 0);
})().catch((error) => { console.error('ERROR', error.message); try { if (fs.existsSync(userCss) && fs.statSync(userCss).size > 100000) fs.rmSync(userCss); } catch { /* ignore */ } process.exit(2); });
