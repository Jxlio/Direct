// End-to-end check of messaging features, roles and attachments in the real Windows app.
//   1. start the service:   cd server && npm run dev
//   2. start the app with:  WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9223 Murmur.exe
//   3. run:                 NODE_PATH=service/node_modules node scripts/e2e_features.cjs http://127.0.0.1:8788
// Other members are played from here with the same crypto the page uses.
const WebSocket = require('ws');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const C = require(path.resolve(__dirname, '../src/windows/ui/e2ee.js'));
const origin = process.argv[2] || 'http://127.0.0.1:8788';
const HARNESS = 'file:///C:/Tools/Murmur/server/test/harness.mjs';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (name, condition, detail = '') => { console.log(`${condition ? 'ok  ' : 'FAIL'} ${name}${detail ? ' — ' + detail : ''}`); if (!condition) failed++; };
const PNG = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64');

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
  const call = (method, params) => new Promise((resolve, reject) => { const mine = ++id; pending.set(mine, (m) => (m.error ? reject(new Error(m.error.message)) : resolve(m.result))); ws.send(JSON.stringify({ id: mine, method, params })); });
  return {
    call,
    evaluate: async (expression) => {
      const r = await call('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
      if (r.exceptionDetails) throw new Error(r.exceptionDetails.text + ' ' + (r.exceptionDetails.exception?.description || ''));
      return r.result?.value;
    },
    close: () => ws.close(),
  };
}
async function until(fn, label, timeout = 10000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) { const v = await fn(); if (v) return v; await sleep(150); }
  throw new Error('timeout: ' + label);
}
const sha = (hex) => C.sha256Hex(C.fromHex(hex));

async function player(hub, guild, name) {
  const { Identity } = await import(HARNESS);
  const identity = await Identity.create(), client = await hub.connect(guild);
  if (name) identity.name = name;
  const agree = async (peerEnc) => new Uint8Array(await crypto.subtle.deriveBits({ name: 'ECDH', public: await crypto.subtle.importKey('raw', C.fromHex(peerEnc), { name: 'ECDH', namedCurve: 'P-256' }, false, []) }, identity.encKey, 256));
  await client.login(identity, guild);
  return { identity, client, agree, fp: await sha(identity.pub), keys: new Map(), epoch: 1 };
}
async function fetchKeys(p, guild, senderEnc) {
  const { items } = await p.client.ask({ t: 'keys.get' });
  for (const item of items) p.keys.set(item.epoch, await C.unwrapKey(await p.agree(senderEnc), guild, item.epoch, senderEnc, p.identity.enc, item.blob));
  p.epoch = Math.max(p.epoch, ...items.map((i) => i.epoch));
}
async function post(p, guild, channel, payload) {
  const { envelope } = await import(HARNESS);
  const id = 'p' + Math.random().toString(36).slice(2, 12).padEnd(10, '0');
  const body = await C.encryptPayload(p.keys.get(p.epoch), p.epoch, guild, channel, id, payload);
  return p.client.ask(await envelope(p.identity, guild, channel, id, body));
}
const rows = (ev) => ev("JSON.stringify([...document.querySelectorAll('#server-messages .message')].map(r => ({ uid: r.dataset.uid, text: r.querySelector('.bubble')?.textContent, meta: r.querySelector('.message-meta')?.textContent, mentioned: r.classList.contains('mentioned'), deleted: r.classList.contains('deleted'), reply: r.querySelector('.reply-quote')?.textContent, reactions: [...r.querySelectorAll('.reaction')].map(x => x.textContent), img: !!r.querySelector('.file-card img'), card: r.querySelector('.file-card button')?.textContent })))").then(JSON.parse);
const rowOf = async (ev, text) => (await rows(ev)).find((r) => r.text?.includes(text));
const click = (ev, selector) => ev(`document.querySelector(${JSON.stringify(selector)}).click(); 1`);
const showWindow = (command) => execFileSync('powershell', ['-NoProfile', '-Command', `Add-Type -Namespace W -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);'; $p = Get-Process Murmur | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1; [W.U]::ShowWindow($p.MainWindowHandle, ${command}) | Out-Null`]);
const windowTitle = () => execFileSync('powershell', ['-NoProfile', '-Command', "(Get-Process Murmur -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle } | Select-Object -First 1).MainWindowTitle"]).toString().trim();

(async () => {
  const page = await connectPage(), ev = page.evaluate;
  const { RemoteHub } = await import(HARNESS);
  const hub = new RemoteHub(origin);

  // ---- a fresh server with the app as owner ----
  await ev("murmurServers.debug.forgetName(); 1");
  while ((await ev('murmurServers.debug.servers.length')) > 0) {
    await ev("document.querySelector('#side-servers [data-server=\"0\"]').click(); document.getElementById('server-remove').click(); document.getElementById('server-remove').click(); 1");
    await sleep(200);
  }
  await ev("document.getElementById('profile-name').value='Alice'; document.getElementById('profile-save').click(); 1");
  await until(() => ev("document.getElementById('profile-saved').textContent.includes('Enregistré')"), 'name saved');
  await ev(`document.getElementById('srv-origin').value=${JSON.stringify(origin)}; document.getElementById('srv-name').value='Fonctions'; document.getElementById('srv-create').click(); 1`);
  await until(() => ev("murmurServers.debug.servers[0]?.keyState === 'ready'"), 'server ready');
  const invite = await until(() => ev("document.getElementById('invite-code').value"), 'invitation');
  const [, , guild, code, ownerFp] = invite.split('|');
  const bob = await player(hub, guild, 'Bob');
  await bob.client.ask({ t: 'guild.join', invite: code, ...(await bob.identity.encFields(guild)) });
  const owner = (await bob.client.ask({ t: 'members.list' })).items.find((m) => m.role === 'owner');
  await until(async () => (await bob.client.ask({ t: 'members.list' })).items.find((m) => m.fp === bob.fp).keyed, 'bob keyed');
  await fetchKeys(bob, guild, owner.enc);
  // The service allows 5 requests a second per connection; a long conversation must keep to that.
  const send = async (payload) => { const answer = await post(bob, guild, 'general', payload); if (answer.t !== 'ack') throw new Error('send refused: ' + answer.code); await sleep(230); return answer.seq; };

  // ---- replies and mentions ----
  const first = await send({ text: 'Premier message de Bob' });
  await until(() => rowOf(ev, 'Premier message'), 'first message');
  showWindow(6); await sleep(1500);                                             // minimised: the page must keep listening
  const second = await send({ text: '@Alice tu as vu ça ?', reply: first, mentions: [ownerFp] });
  await until(() => rowOf(ev, 'tu as vu'), 'mention message while minimised');
  check('messages still arrive while the window is minimised and a server is connected', true);
  const mention = await rowOf(ev, 'tu as vu');
  check('a reply shows the message it answers', /Bob.*Premier message/.test(mention.reply || ''), mention.reply);
  check('a message that mentions me is highlighted', mention.mentioned === true);
  check('the unread count reaches the window title', await until(async () => /\(\d+\)/.test(windowTitle()) && windowTitle(), 'title', 6000).then(Boolean).catch(() => false), windowTitle());
  showWindow(9); await sleep(1200);                                              // back to normal for the rest

  // ---- typing ----
  await bob.client.send({ t: 'typing', channel: 'general' });
  await until(() => ev("document.getElementById('typing-note').textContent.includes('Bob')"), 'typing note');
  check('someone typing is shown, then goes away when their message arrives', true);
  await send({ text: 'fini' });
  await until(() => ev("document.getElementById('typing-note').textContent === ''"), 'typing cleared');

  // ---- edit, forged edit, delete, forged delete ----
  const draft = await send({ text: 'brouillon' });
  await until(() => rowOf(ev, 'brouillon'), 'draft');
  const twin = await player(hub, guild, 'Eve');
  const { code: code2 } = await (async () => { await click(ev, '#invite-make'); await sleep(900); return { code: (await ev("document.getElementById('invite-code').value")).split('|')[3] }; })();
  await twin.client.ask({ t: 'guild.join', invite: code2, ...(await twin.identity.encFields(guild)) });
  await until(async () => (await twin.client.ask({ t: 'members.list' })).items.find((m) => m.fp === twin.fp).keyed, 'twin keyed');
  await fetchKeys(twin, guild, owner.enc);
  await post(twin, guild, 'general', { t: 'edit', target: draft, text: 'forgé par Eve' });
  await post(bob, guild, 'general', { t: 'edit', target: draft, text: 'version finale' });
  await until(() => rowOf(ev, 'version finale'), 'edit applied');
  const edited = await rowOf(ev, 'version finale');
  check('the author’s edit replaces the text and is marked', /modifié/.test(edited.meta), edited.meta);
  check('someone else’s edit is ignored', !(await rowOf(ev, 'forgé par Eve')));
  const doomed = await send({ text: 'à supprimer' });
  await until(() => rowOf(ev, 'à supprimer'), 'doomed');
  await post(twin, guild, 'general', { t: 'del', target: doomed });
  await sleep(500);
  check('someone else’s deletion is ignored', !!(await rowOf(ev, 'à supprimer')) && !(await rowOf(ev, 'à supprimer')).deleted);
  await post(bob, guild, 'general', { t: 'del', target: doomed });
  await until(async () => (await rowOf(ev, 'Message supprimé'))?.deleted, 'deletion shown');
  check('the author’s deletion hides the message', true);

  // ---- reactions, both directions ----
  await post(bob, guild, 'general', { t: 'react', target: first, emoji: '👍', on: true });
  await until(async () => (await rowOf(ev, 'Premier message'))?.reactions.length, 'reaction shown');
  check('a reaction appears with its count', (await rowOf(ev, 'Premier message')).reactions[0].includes('👍 1'), JSON.stringify((await rowOf(ev, 'Premier message')).reactions));
  await ev(`document.querySelector('#server-messages [data-uid="${first}"] .reaction').click(); 1`);
  await until(async () => (await rowOf(ev, 'Premier message')).reactions[0]?.includes('2'), 'my reaction added');
  check('clicking a reaction adds mine', true);
  const mine = await bob.client.wait('envelope', 8000).then(() => true).catch(() => false);
  check('my reaction reaches the other members as an envelope', mine);

  // ---- the app replies, edits, reacts and deletes through the real buttons ----
  await ev(`document.querySelector('#server-messages [data-uid="${second}"] [data-action="reply"]').click(); 1`);
  check('the reply bar names who is being answered', (await ev("document.getElementById('compose-text').textContent")).includes('Bob'));
  await ev("document.getElementById('server-input').value='Oui, je vois !'; document.getElementById('server-composer').requestSubmit(); 1");
  let seen, payload;                                                              // skip the app's earlier reaction envelope
  do {
    seen = await bob.client.wait('envelope', 8000);
    payload = seen.from === ownerFp ? await C.decryptPayload(bob.keys.get(C.parseBody(seen.body).epoch), guild, seen.channel, seen.id, seen.body) : {};
  } while (payload.reply === undefined);
  check('the app’s reply carries the number of the message it answers', payload.reply === second && payload.text === 'Oui, je vois !', JSON.stringify(payload));
  await until(() => rowOf(ev, 'Oui, je vois'), 'my reply shown');
  await ev(`document.querySelector('#server-messages [data-uid="${seen.seq}"] [data-action="edit"]').click(); 1`);
  check('editing loads the text into the composer', (await ev("document.getElementById('server-input').value")) === 'Oui, je vois !');
  await ev("document.getElementById('server-input').value='Oui, je vois bien !'; document.getElementById('server-composer').requestSubmit(); 1");
  await until(() => rowOf(ev, 'je vois bien'), 'my edit shown');
  check('the app’s own edit is applied', /modifié/.test((await rowOf(ev, 'je vois bien')).meta));
  check('the owner, who moderates, gets a delete button on other people’s messages', (await ev(`!!document.querySelector('#server-messages [data-uid="${second}"] [data-action="delete"]')`)) === true);

  // ---- attachments: from the guest, then from the app ----
  const fileId = C.toHex(crypto.getRandomValues(new Uint8Array(16)));
  const sealed = await C.encryptFile(new Uint8Array(PNG), fileId);
  for (let i = 0; i < sealed.chunks.length; i++) await bob.client.ask({ t: 'blob.put', id: fileId, index: i, total: sealed.chunks.length, data: sealed.chunks[i] });
  await send({ text: 'Une image', files: [{ blob: fileId, name: 'point.png', size: PNG.length, mime: 'image/png', key: sealed.key, chunks: sealed.chunks.length }] });
  await until(async () => (await rowOf(ev, 'Une image'))?.card, 'file card');
  check('a picture shows as a card first, not downloaded', /point\.png/.test((await rowOf(ev, 'Une image')).card));
  await ev(`document.querySelector('#server-messages .file-card button').click(); 1`);
  await until(async () => (await rowOf(ev, 'Une image')).img, 'picture loaded', 8000);
  check('clicking loads, decrypts and shows the picture', true);
  const tmp = path.join(os.tmpdir(), 'murmur-e2e.png'); fs.writeFileSync(tmp, PNG);
  const doc = await page.call('DOM.getDocument', {});
  const node = await page.call('DOM.querySelector', { nodeId: doc.root.nodeId, selector: '#server-file' });
  await page.call('DOM.setFileInputFiles', { nodeId: node.nodeId, files: [tmp] });
  let upload;
  do { upload = await bob.client.wait('envelope', 15000); } while (upload.from !== ownerFp || !(await C.decryptPayload(bob.keys.get(C.parseBody(upload.body).epoch), guild, upload.channel, upload.id, upload.body)).files);
  const sent = await C.decryptPayload(bob.keys.get(C.parseBody(upload.body).epoch), guild, upload.channel, upload.id, upload.body);
  const parts = [];
  for (let i = 0; i < sent.files[0].chunks; i++) parts.push((await bob.client.ask({ t: 'blob.get', id: sent.files[0].blob, index: i })).data);
  check('the app’s picture reaches the others encrypted and decrypts to the same bytes', Buffer.compare(Buffer.from(await C.decryptFile(sent.files[0].key, sent.files[0].blob, parts)), PNG) === 0);
  check('the service never saw the picture’s name', !JSON.stringify(parts).includes('murmur-e2e'));
  fs.rmSync(tmp, { force: true });
  const refused = await ev(`(async () => { const f = new File([new Uint8Array(10)], 'x.svg', { type: 'image/svg+xml' }); const dt = new DataTransfer(); dt.items.add(f); const input = document.getElementById('server-file'); input.files = dt.files; input.dispatchEvent(new Event('change')); await new Promise(r => setTimeout(r, 600)); return document.getElementById('server-notice').textContent; })()`);
  check('a file that is not a JPEG, PNG, GIF or WebP picture is refused', /JPEG|images/.test(refused), refused);

  // ---- search ----
  await ev("document.getElementById('search-input').value='VERSION finale'; document.getElementById('search-form').requestSubmit(); 1");
  await until(() => ev("document.querySelectorAll('#search-results .search-hit').length > 0"), 'search hit');
  check('search finds an edited message, ignoring case and showing the current text', (await ev("document.querySelector('#search-results .search-hit span').textContent")).includes('version finale'));
  await ev("document.getElementById('search-input').value='introuvable-xyz'; document.getElementById('search-form').requestSubmit(); 1");
  await until(() => ev("document.querySelector('#search-results .search-note')?.textContent.includes('Aucun')"), 'no result');
  check('a search with no match says so', true);
  await ev("document.getElementById('search-input').value='deuxième page'; 1");

  // ---- history: older pages are fetched and searchable ----
  for (let i = 0; i < 60; i++) await send({ text: `remplissage ${i}` });
  await send({ text: 'aiguille dans la botte de foin' });
  for (let i = 0; i < 60; i++) await send({ text: `suite ${i}` });
  await until(() => rowOf(ev, 'suite 59'), 'long history', 40000);
  await ev("document.getElementById('search-input').value='aiguille'; document.getElementById('search-form').requestSubmit(); 1");
  await until(() => ev("document.querySelectorAll('#search-results .search-hit').length > 0"), 'search hit in older history', 15000);
  check('search reaches back into older history', true);
  await ev("document.querySelector('#search-results .search-hit').click(); 1");
  await until(() => rowOf(ev, 'aiguille'), 'jumped', 8000);
  check('a result opens the message in context', !!(await rowOf(ev, 'aiguille')));

  // ---- moderators ----
  const promoteTwin = async () => ev(`(() => { const row = [...document.querySelectorAll('#member-list .member')].find(r => r.querySelector('span').textContent.startsWith('Eve')); const b = [...row.querySelectorAll('button')].find(x => x.textContent === 'Modérateur'); b.click(); b.click(); })()`);
  check('before being promoted a member cannot invite', (await twin.client.ask({ t: 'invite.create' })).code === 'forbidden');
  await promoteTwin();
  await until(async () => (await twin.client.ask({ t: 'invite.create' })).t === 'invite', 'twin became moderator', 8000);
  check('the owner can make a member a moderator, signed so the service cannot invent one', (await ev("[...document.querySelectorAll('#member-list .member.is-moderator .member-name')].some(s => s.textContent.startsWith('Eve'))")) === true);
  // The moderator removes a member and rotates the key; the owner (the app) must accept the new key from a moderator.
  const third = await player(hub, guild, 'Carl');
  const inv3 = await twin.client.ask({ t: 'invite.create', max_uses: 2, ttl_s: 3600 });
  await third.client.ask({ t: 'guild.join', invite: inv3.code, ...(await third.identity.encFields(guild)) });
  await until(() => ev("[...document.querySelectorAll('#member-list .member span')].some(s => s.textContent.includes('Carl'))"), 'carl listed');
  const members = (await twin.client.ask({ t: 'members.list' })).items, epoch = twin.epoch + 1, rotated = C.newGroupKey();
  const selfEntry = members.find((m) => m.fp === twin.fp);
  const wrapFor = async (m) => C.wrapKey(await twin.agree(m.enc), guild, epoch, selfEntry.enc, m.enc, rotated);
  await twin.client.ask({ t: 'keys.put', to: twin.fp, epoch, blob: await wrapFor(selfEntry) });
  check('a moderator removes a plain member', (await twin.client.ask({ t: 'member.remove', fp: third.fp, epoch })).t === 'ack');
  for (const m of members.filter((x) => x.fp !== third.fp)) await twin.client.ask({ t: 'keys.put', to: m.fp, epoch, blob: await wrapFor(m) });
  twin.keys.set(epoch, rotated); twin.epoch = epoch;
  await post(twin, guild, 'general', { text: 'Message du modérateur après rotation' });
  await until(() => rowOf(ev, 'Message du modérateur'), 'message under the moderator’s key', 10000);
  check('the app accepts and uses a group key handed out by a moderator', !!(await rowOf(ev, 'Message du modérateur')) && (await ev('murmurServers.debug.servers[0].epoch')) === epoch);

  // ---- reinstating, then handing the server over ----
  await until(() => ev("[...document.querySelectorAll('#member-list .former span')].some(s => s.textContent.includes('Carl'))"), 'carl listed as removed');
  await ev(`(() => { const row = [...document.querySelectorAll('#member-list .former')].find(r => r.textContent.includes('Carl')); const b = row.querySelector('button'); b.click(); b.click(); })()`);
  await until(async () => (await twin.client.ask({ t: 'members.list' })).former.every((m) => m.fp !== third.fp), 'carl reinstated');
  check('the owner can reinstate a removed identity', true);
  await ev(`(() => { const row = [...document.querySelectorAll('#member-list .member')].find(r => r.querySelector('.member-name').textContent.startsWith('Eve')); const b = [...row.querySelectorAll('button')].find(x => x.textContent === 'Donner la propriété'); b.click(); b.click(); })()`);
  await until(async () => (await twin.client.ask({ t: 'invite.create' })).t === 'invite' && (await twin.client.ask({ t: 'members.list' })).items.find((m) => m.fp === twin.fp).role === 'owner', 'ownership passed');
  await until(() => ev("murmurServers.debug.servers[0].role === 'member' && murmurServers.debug.servers[0].owner === " + JSON.stringify(twin.fp)), 'app follows the chain', 10000);
  check('the app follows the signed chain of owners and demotes itself', true);
  check('the persisted owner moved to the new one, so a restart still verifies', JSON.parse(await ev("localStorage.getItem('murmur.servers.v1')"))[0].owner === twin.fp);
  check('the old owner is now an ordinary member without staff buttons', (await ev("document.getElementById('server-owner-tools').hidden")) === true);

  console.log('GUILD=' + guild);
  hub.close(); page.close();
  console.log(failed ? `\n${failed} check(s) failed` : '\nAll messaging checks passed');
  process.exit(failed ? 1 : 0);
})().catch(async (error) => {
  console.error('ERROR', error.message);
  try {
    const page = await connectPage();
    console.error('APP STATE', await page.evaluate("JSON.stringify(murmurServers.debug.servers.map(s=>({status:s.status,key:s.keyState,epoch:s.epoch,role:s.role,notice:s.notice,members:s.members.size,caches:[...s.cache].map(([k,c])=>[k,c.items.length,c.newest])})))"));
    page.close();
  } catch { /* no page to inspect */ }
  process.exit(2);
});
