// Takes screenshots of the real app (welcome, circles, chat, settings, themes) for a visual check.
//   NODE_PATH=service/node_modules node scripts/screenshots.cjs http://127.0.0.1:8788 <output folder>
const WebSocket = require('ws');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn, execFileSync } = require('node:child_process');
const origin = process.argv[2] || 'http://127.0.0.1:8788';
const out = path.resolve(process.argv[3] || 'out/screens');
const exe = path.resolve(__dirname, '../out/build-latest/Murmur.exe');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
fs.mkdirSync(out, { recursive: true });

(async () => {
  const root = path.join(os.tmpdir(), 'murmur-shots'); fs.rmSync(root, { recursive: true, force: true }); fs.mkdirSync(root, { recursive: true });
  const child = spawn(exe, [], { stdio: 'ignore', env: { ...process.env, MURMUR_DATA_ROOT: root, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: '--remote-debugging-port=9225' } });
  const stop = () => { try { execFileSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], { stdio: 'ignore' }); } catch { /* gone */ } fs.rmSync(root, { recursive: true, force: true }); };
  process.on('exit', stop);
  let page;
  for (let i = 0; i < 40 && !page; i++) { try { page = (await (await fetch('http://127.0.0.1:9225/json')).json()).find((t) => t.type === 'page' && t.url.includes('murmur.local')); } catch { /* starting */ } if (!page) await sleep(500); }
  const ws = new WebSocket(page.webSocketDebuggerUrl); await new Promise((r) => ws.on('open', r));
  let id = 0; const pending = new Map();
  ws.on('message', (d) => { const m = JSON.parse(d); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } });
  const call = (method, params = {}) => new Promise((resolve) => { const n = ++id; pending.set(n, resolve); ws.send(JSON.stringify({ id: n, method, params })); });
  const ev = async (expression) => (await call('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true })).result?.result?.value;
  const shot = async (name) => { await sleep(600); const r = await call('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(path.join(out, name + '.png'), Buffer.from(r.result.data, 'base64')); console.log('saved', name); };
  const until = async (fn, label) => { for (let i = 0; i < 60; i++) { if (await ev(fn)) return; await sleep(250); } throw new Error('timeout ' + label); };
  const click = (selector) => ev(`document.querySelector(${JSON.stringify(selector)}).click(); 1`);
  await until("!!document.getElementById('profile-name') && !!window.murmurServers && !!window.murmurOnboarding", 'ready');
  // The welcome shows by itself on a fresh profile.
  await shot('01-welcome');
  await click('#ob-start'); await ev("document.getElementById('ob-name').value='Alice'; 1"); await shot('02-you');
  await click('[data-ob-theme=light]'); await shot('03-you-day'); await click('[data-ob-theme=dark]');
  await click('#ob-next'); await shot('04-first-circle');
  await ev(`document.getElementById('ob-service').value=${JSON.stringify(origin)}; document.getElementById('ob-circle').value='Les copains'; 1`);
  await click('#ob-finish');
  await until("murmurServers.debug.servers[0]?.keyState === 'ready'", 'server');
  await ev("document.getElementById('channel-name').value='vocal'; document.getElementById('channel-type').value='voice'; document.getElementById('channel-create').click(); 1");
  await ev("document.getElementById('server-input').value='On se retrouve ce soir sur le vocal ?'; document.getElementById('server-composer').requestSubmit(); 1");
  await shot('05-chat-night');
  await click('#server-menu'); await shot('06-circle-settings'); await click('#ss-close');
  // a few more circles (names only, not connected) to see several covers
  await ev("['Rando','Studio','Jeux','Bureau','Maison'].forEach(n => murmurServers.debug.makeServer && murmurServers.debug.servers.push(murmurServers.debug.makeServer('http://127.0.0.1:8788', Array.from({length:32},()=>Math.floor(Math.random()*16).toString(16)).join(''), n))); murmurServers.debug.render(); 1");
  await click('[data-page=servers]'); await shot('07-circles');
  await click('.theme-option[data-theme=light]'); await shot('08-circles-day');
  await click('#side-servers [data-server]'); await shot('09-chat-day');
  await click('[data-page=settings]'); await ev("document.querySelector('#appearance-card').scrollIntoView(); 1"); await shot('10-appearance-day');
  await click('.theme-option[data-theme=dark]'); await ev("document.querySelectorAll('#accent-row .accent-swatch')[2].click(); document.querySelectorAll('#accent2-row .accent-swatch')[0].click(); document.querySelectorAll('#logo-row button')[3].click(); 1");
  await shot('11-appearance-violet-coral-dots');
  await click('[data-page=servers]'); await shot('12-circles-violet');
  ws.close(); stop(); process.exit(0);
})().catch((e) => { console.error('ERROR', e.message); process.exit(2); });
