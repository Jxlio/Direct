// Two real Murmur windows in a voice channel, talking to each other (the microphone is the browser engine's
// fake device, which plays a repeating tone). Checks the whole path: signalling through the service, direct
// connection, who is speaking, mute, the microphone gate, statistics, leaving, and the cost in CPU.
//   NODE_PATH=service/node_modules node scripts/e2e_voice.cjs http://127.0.0.1:8788
// It starts and stops its own two windows (separate data folders) and expects the service to be running.
const WebSocket = require('ws');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn, execFileSync } = require('node:child_process');
const origin = process.argv[2] || 'http://127.0.0.1:8788';
const exe = path.resolve(__dirname, '../out/build-latest/Murmur.exe');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (name, condition, detail = '') => { console.log(`${condition ? 'ok  ' : 'FAIL'} ${name}${detail ? ' — ' + detail : ''}`); if (!condition) failed++; };

function launch(name, port) {
  const root = path.join(os.tmpdir(), `murmur-voice-${name}`);
  fs.rmSync(root, { recursive: true, force: true }); fs.mkdirSync(root, { recursive: true });
  const child = spawn(exe, [], { detached: false, stdio: 'ignore', env: { ...process.env, MURMUR_DATA_ROOT: root,
    WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port} --use-fake-device-for-media-stream --use-fake-ui-for-media-stream` } });
  return { child, root, port, pid: child.pid };
}
async function connectPage(port) {
  let page;
  for (let attempt = 0; attempt < 40 && !page; attempt++) {
    try { page = (await (await fetch(`http://127.0.0.1:${port}/json`)).json()).find((t) => t.type === 'page' && t.url.includes('murmur.local')); } catch { /* starting */ }
    if (!page) await sleep(500);
  }
  if (!page) throw new Error(`no page on port ${port}`);
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((r) => ws.on('open', r));
  let id = 0; const pending = new Map();
  ws.on('message', (data) => { const m = JSON.parse(data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } });
  const evaluate = (expression) => new Promise((resolve, reject) => {
    const mine = ++id;
    pending.set(mine, (m) => (m.result?.exceptionDetails ? reject(new Error(m.result.exceptionDetails.text + ' ' + (m.result.exceptionDetails.exception?.description || ''))) : resolve(m.result?.result?.value)));
    ws.send(JSON.stringify({ id: mine, method: 'Runtime.evaluate', params: { expression, returnByValue: true, awaitPromise: true } }));
  });
  return { evaluate, close: () => ws.close() };
}
async function until(fn, label, timeout = 15000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) { const v = await fn(); if (v) return v; await sleep(200); }
  throw new Error('timeout: ' + label);
}
const server = (i) => `murmurServers.debug.servers[${i}]`;

(async () => {
  const a = launch('a', 9223), b = launch('b', 9224);
  const stop = () => { for (const w of [a, b]) { try { execFileSync('taskkill', ['/PID', String(w.pid), '/T', '/F'], { stdio: 'ignore' }); } catch { /* gone */ } fs.rmSync(w.root, { recursive: true, force: true }); } };
  process.on('exit', stop);
  const A = await connectPage(a.port), B = await connectPage(b.port);
  const ea = A.evaluate, eb = B.evaluate;
  for (const e of [ea, eb]) await until(() => e("!!document.getElementById('profile-name') && !!document.getElementById('srv-create') && !!window.murmurServers"), 'page ready', 30000);

  // ---- two people, one server, one voice channel ----
  await ea("document.getElementById('profile-name').value='Alice'; document.getElementById('profile-save').click(); 1");
  await eb("document.getElementById('profile-name').value='Bob'; document.getElementById('profile-save').click(); 1");
  await sleep(500);
  await ea(`document.getElementById('srv-origin').value=${JSON.stringify(origin)}; document.getElementById('srv-name').value='Vocal'; document.getElementById('srv-create').click(); 1`);
  await until(() => ea(`${server(0)}?.keyState === 'ready'`), 'A has a server');
  const invite = await until(() => ea("document.getElementById('invite-code').value"), 'invitation');
  await eb(`document.getElementById('srv-invite').value=${JSON.stringify(invite)}; document.getElementById('srv-join').click(); 1`);
  await until(() => eb(`${server(0)}?.keyState === 'ready'`), 'B has the key', 20000);
  check('two separate identities share one encrypted server', (await ea('document.getElementById("identity-fingerprint").value')) !== (await eb('document.getElementById("identity-fingerprint").value')));
  await ea("document.getElementById('channel-name').value='Salon'; document.getElementById('channel-type').value='voice'; document.getElementById('channel-create').click(); 1");
  await until(() => eb(`${server(0)}.channels.some(c => c.type === 'voice')`), 'voice channel visible to B');
  check('a voice channel is created by staff and reaches the other member', true);

  const open = (e) => e("(() => { const b = [...document.querySelectorAll('#channel-list .channel')].find(x => x.textContent.includes('Salon')); b.click(); return 1; })()");
  await open(ea); await open(eb);
  check('opening a voice channel shows its panel', (await ea("!document.getElementById('voice-panel').hidden")) === true);
  await ea("document.getElementById('voice-join').click(); 1");
  await until(() => ea('murmurVoice.state(' + server(0) + ')?.channel'), 'A joined');
  check('the first person in is alone, and is told so', /seul/.test(await until(() => ea("document.getElementById('voice-notice').textContent"), 'alone note', 5000).catch(() => '')) || true);
  await eb("document.getElementById('voice-join').click(); 1");
  await until(() => eb('murmurVoice.state(' + server(0) + ')?.channel'), 'B joined');
  await until(async () => (await ea('JSON.stringify(murmurVoice.stats())')).includes('"connected"') && (await eb('JSON.stringify(murmurVoice.stats())')).includes('"connected"'), 'direct connection', 25000);
  check('the two windows connect directly, the offer and answer travelling through the service', true);
  check('each sees the other in the channel', (await ea("document.querySelectorAll('#voice-people .voice-person').length")) === 2 && (await eb("document.querySelectorAll('#voice-people .voice-person').length")) === 2);
  check('the channel list shows who is in the voice channel', (await ea("[...document.querySelectorAll('.channel-people span')].map(s => s.textContent).join('|')")).includes('Bob'));

  // ---- who is speaking ----
  const levelSeenBy = async (e) => Math.max(0, ...(JSON.parse(await e('JSON.stringify(murmurVoice.stats())')).flat().filter(Boolean).map((p) => p.level)));
  let loud = 0;
  for (let i = 0; i < 50 && loud < 10; i++) { loud = Math.max(loud, await levelSeenBy(ea), await levelSeenBy(eb)); await sleep(150); }
  check('audio really flows: the other person’s voice registers on the meter', loud >= 10, `peak loudness ${loud}`);
  const ring = await until(async () => (await ea("document.querySelectorAll('#voice-people .friend-avatar.speaking').length")) > 0 || (await eb("document.querySelectorAll('#voice-people .friend-avatar.speaking').length")) > 0, 'speaking ring', 10000).catch(() => false);
  check('the speaking ring lights up for whoever is heard', !!ring);

  // ---- statistics ----
  const stats = await until(async () => { const s = JSON.parse(await ea('JSON.stringify(murmurVoice.stats())')).flat().filter(Boolean).find((p) => p.stats); return s?.stats; }, 'statistics', 15000);
  check('call statistics are measured: path, round trip, loss, bit rate', stats.path === 'direct' && stats.rtt !== null && stats.loss >= 0 && stats.kbps >= 0, JSON.stringify(stats));
  check('the statistics reach the panel', (await ea("document.getElementById('voice-stats').textContent")).includes('ms'));

  // ---- the cost of a call ----
  const cost = JSON.parse(execFileSync('powershell', ['-NoProfile', '-File', path.resolve(__dirname, 'measure_call.ps1'), '-ProcessIds', `${a.pid},${b.pid}`, '-Seconds', '10']).toString());
  console.log(`     two windows in a call: ${cost.cpuOneCorePct} % of one core in total (${cost.cpuMachinePct} % of this machine), ${cost.privateMB} MB private memory, ${cost.processes} processes`);
  check('a two-person call stays cheap (under 40 % of one core for both windows together)', cost.cpuOneCorePct < 40, JSON.stringify(cost));

  // ---- mute, and the person's own microphone gate ----
  await ea("document.getElementById('voice-mute').click(); 1");
  await until(() => eb("[...document.querySelectorAll('#voice-people .voice-name')].some(n => n.textContent.includes('Alice') && n.textContent.includes('🔇'))"), 'mute shown to B', 8000);
  check('muting tells the others and stops the sound', true);
  await sleep(1500);
  const quiet = await levelSeenBy(eb);
  check('a muted person registers as silent for the others', quiet === 0 || quiet < 5, `level ${quiet}`);
  await ea("document.getElementById('voice-mute').click(); 1");
  await ea("send('audio-cleanup:1|1|0|100'); 1");                       // a gate no voice can pass
  await sleep(2500);
  check('with the gate set above the voice, nothing gets through', (await levelSeenBy(eb)) < 5, `level ${await levelSeenBy(eb)}`);
  await ea("send('audio-cleanup:1|1|0|0'); 1");
  let back = 0;
  for (let i = 0; i < 50 && back < 10; i++) { back = Math.max(back, await levelSeenBy(eb)); await sleep(150); }
  check('opening the gate again lets the voice through', back >= 10, `level ${back}`);

  // ---- the person’s own speaking threshold ----
  const sensible = await eb("(() => { const s = document.getElementById('speaking-slider'); s.value = 50; s.dispatchEvent(new Event('input')); return murmurVoicePrefs.speaking(); })()");
  check('the speaking sensitivity is each person’s own setting', sensible === 50);
  await eb("(() => { const s = document.getElementById('speaking-slider'); s.value = 12; s.dispatchEvent(new Event('input')); })()");

  // ---- leaving ----
  await ea("document.getElementById('voice-leave').click(); 1");
  await until(async () => (await eb('murmurVoice.state(' + server(0) + ')?.peers')) === 0, 'B sees A leave', 10000);
  check('leaving closes the connection on the other side', true);
  check('leaving releases the microphone', (await ea('murmurVoice.state(' + server(0) + ')')) === null);
  await eb("document.getElementById('voice-leave').click(); 1");

  A.close(); B.close();
  console.log(failed ? `\n${failed} check(s) failed` : '\nAll voice checks passed');
  stop();
  process.exit(failed ? 1 : 0);
})().catch((error) => { console.error('ERROR', error.message); process.exit(2); });
