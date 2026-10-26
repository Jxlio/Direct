// End-to-end check of Servers (including encryption) in the real Windows app, against a running service.
//   1. start the service:   cd server && npm run dev
//   2. start the app with:  WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9223 Murmur.exe
//   3. run:                 NODE_PATH=service/node_modules node scripts/e2e_servers.cjs http://127.0.0.1:8788
//   4. after restarting the app:  ... http://127.0.0.1:8788 --after-restart <server id printed by step 3>
// The page is driven through the DevTools protocol; other members are played with the contract-test
// harness and the same e2ee.js the page uses. Exit code 0 means every check held.
const WebSocket = require('ws');
const path = require('node:path');
const C = require(path.resolve(__dirname, '../src/windows/ui/e2ee.js'));
const origin = process.argv[2] || 'http://127.0.0.1:8788';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (name, condition, detail = '') => { console.log(`${condition ? 'ok  ' : 'FAIL'} ${name}${detail ? ' — ' + detail : ''}`); if (!condition) failed++; };
const HARNESS = 'file:///C:/Tools/Murmur/server/test/harness.mjs';

async function connectPage() {
  let page;
  for (let attempt = 0; attempt < 30 && !page; attempt++) {     // the app may still be starting
    try { page = (await (await fetch('http://127.0.0.1:9223/json')).json()).find((t) => t.type === 'page' && t.url.includes('murmur.local')); } catch { /* not listening yet */ }
    if (!page) await sleep(500);
  }
  if (!page) throw new Error('page not found on port 9223');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((r) => ws.on('open', r));
  let id = 0; const pending = new Map();
  ws.on('message', (data) => { const m = JSON.parse(data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } });
  const evaluate = (expression) => new Promise((resolve, reject) => {
    const mine = ++id; pending.set(mine, (m) => (m.result?.exceptionDetails ? reject(new Error(m.result.exceptionDetails.text + ' ' + (m.result.exceptionDetails.exception?.description || ''))) : resolve(m.result?.result?.value)));
    ws.send(JSON.stringify({ id: mine, method: 'Runtime.evaluate', params: { expression, returnByValue: true, awaitPromise: true } }));
  });
  return { evaluate, close: () => ws.close() };
}
async function until(fn, label, timeout = 10000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) { const v = await fn(); if (v) return v; await sleep(150); }
  throw new Error('timeout: ' + label);
}
const sha = (hex) => C.sha256Hex(C.fromHex(hex));
const server = (guild) => `murmurServers.debug.servers.find(s=>s.guild===${JSON.stringify(guild)})`;
const selectAndRemove = (guild) => `(() => { const i = murmurServers.debug.servers.findIndex(s=>s.guild===${JSON.stringify(guild)}); document.querySelector('#side-servers [data-server="'+i+'"]').click(); document.getElementById('server-remove').click(); document.getElementById('server-remove').click(); })()`;

// A member played by this script: owns an identity and an encryption key, and can read and write ciphertext.
async function player(hub, guild) {
  const { Identity } = await import(HARNESS);
  const identity = await Identity.create(), client = await hub.connect(guild);
  const agree = async (peerEnc) => new Uint8Array(await crypto.subtle.deriveBits({ name: 'ECDH', public: await crypto.subtle.importKey('raw', C.fromHex(peerEnc), { name: 'ECDH', namedCurve: 'P-256' }, false, []) }, identity.encKey, 256));
  await client.login(identity, guild);
  return { identity, client, agree, fp: await sha(identity.pub), group: null };
}
async function receiveKey(p, guild, ownerEnc) {
  const { items } = await p.client.ask({ t: 'keys.get' });
  if (!items.length) return false;
  const latest = items.at(-1);
  p.group = { key: await C.unwrapKey(await p.agree(ownerEnc), guild, latest.epoch, ownerEnc, p.identity.enc, latest.blob), epoch: latest.epoch };
  return true;
}
async function say(p, guild, channel, id, text, group = p.group) {
  const { envelope } = await import(HARNESS);
  return p.client.ask(await envelope(p.identity, guild, channel, id, await C.encryptMessage(group.key, group.epoch, guild, channel, id, text)));
}
const bubbles = (ev) => ev("[...document.querySelectorAll('#server-messages .bubble')].map(b=>b.textContent).join('|')");
async function probe(ev, command, id, argument, type, field) {
  return ev(`new Promise(resolve => { const timer=setTimeout(()=>resolve('timeout'),3000); const old=window.murmurServers.handle; window.murmurServers.handle=(d)=>{ if(d.type===${JSON.stringify(type)}&&d.id===${JSON.stringify(id)}){ clearTimeout(timer); window.murmurServers.handle=old; resolve(d[${JSON.stringify(field)}]===null?'refused':'ACCEPTED'); } else old(d); }; send(${JSON.stringify(command + ':' + id + '|' + argument)}); })`);
}

async function afterRestart(guild) {
  const page = await connectPage(), ev = page.evaluate;
  await until(() => ev("murmurServers.debug.servers[0]?.status === 'ready'"), 'server restored and reconnected', 20000);
  check('server list survives a restart', (await ev('murmurServers.debug.servers[0].guild')) === guild);
  check('the group key is fetched again from the service after a restart', !!(await until(() => ev("murmurServers.debug.servers[0].keyState === 'ready'"), 'key restored', 10000)));
  await ev("document.querySelector('#side-servers [data-server=\"0\"]').click(); 1");
  await until(() => ev("document.querySelectorAll('#server-messages .message').length >= 3"), 'history reloaded from the service');
  const texts = await bubbles(ev);
  check('history is decrypted after a restart, including the removed member’s old messages', texts.includes('Réponse de l’app') && texts.includes('Salut depuis') && !texts.includes('🔒') && !texts.includes('Signature invalide'), texts.slice(0, 120));
  const authors = await ev("[...document.querySelectorAll('#server-messages .message-meta')].map(m=>m.textContent.split(' · ')[0]).join('|')");
  check('the removed member is still named, not shown as a bare fingerprint', authors.includes('Bob'), authors);
  page.close();
  console.log(failed ? `${failed} check(s) failed` : 'Restart check passed');
  process.exit(failed ? 1 : 0);
}
if (process.argv[3] === '--after-restart') { afterRestart(process.argv[4]).catch((e) => { console.error('ERROR', e.message); process.exit(2); }); return; }

(async () => {
  const page = await connectPage(), ev = page.evaluate;
  const { RemoteHub } = await import(HARNESS);
  const hub = new RemoteHub(origin);
  check('identity reached the page', (await ev("document.getElementById('identity-fingerprint').value.length > 10")) === true);

  // Start clean through the real UI: servers kept from earlier runs are removed one by one.
  while ((await ev('murmurServers.debug.servers.length')) > 0) {
    await ev("document.querySelector('#side-servers [data-server=\"0\"]').click(); document.getElementById('server-remove').click(); document.getElementById('server-remove').click(); 1");
    await sleep(200);
  }
  check('server list can be emptied from the page', (await ev('murmurServers.debug.servers.length')) === 0);

  // ---- the app's user chooses a name; a server cannot be created without one ----
  await ev("localStorage.removeItem('murmur.profile.v1'); murmurServers.debug.forgetName(); 1");
  await ev("document.getElementById('profile-name').value=''; document.getElementById('srv-origin').value='" + origin + "'; document.getElementById('srv-name').value='x'; document.getElementById('srv-create').click(); 1");
  check('a server cannot be created before choosing a name', /nom/i.test(await ev("document.getElementById('servers-notice').textContent")));
  await ev("document.getElementById('profile-name').value='Alice'; document.getElementById('profile-save').click(); 1");
  await until(() => ev("document.getElementById('profile-saved').textContent.includes('Enregistré')"), 'name saved');
  check('invalid names are refused', await (async () => { await ev("document.getElementById('profile-name').value='x'.repeat(40); document.getElementById('profile-save').click(); 1"); return /32/.test(await until(() => ev("document.getElementById('servers-notice').textContent"), 'notice')); })());
  await ev("document.getElementById('profile-name').value='Alice'; document.getElementById('profile-save').click(); 1");

  // ---- create a server in the app ----
  await ev(`document.getElementById('srv-origin').value=${JSON.stringify(origin)}; document.getElementById('srv-name').value='Test E2E'; document.getElementById('srv-create').click(); 1`);
  await until(() => ev("murmurServers.debug.servers[0]?.status === 'ready' && murmurServers.debug.servers[0].keyState === 'ready'"), 'server ready with key');
  const info = JSON.parse(await ev("JSON.stringify({role: murmurServers.debug.servers[0].role, channels: murmurServers.debug.servers[0].channels.map(c=>c.name), status: document.getElementById('server-status').textContent})"));
  check('creator is owner, general exists, header says end-to-end encrypted', info.role === 'owner' && info.channels.includes('general') && info.status.includes('Chiffré'), JSON.stringify(info));
  const invite = await until(() => ev("document.getElementById('invite-code').value"), 'invitation');
  const [, , guild, code, ownerFp] = invite.split('|');
  check('invitation carries the owner fingerprint', /^murmur4\|http:\/\/127\.0\.0\.1:8788\|[0-9a-f]{32}\|[0-9a-f]{32}\|[0-9a-f]{64}$/.test(invite));

  // ---- a guest joins, the app (owner) hands over the group key ----
  const guest = await player(hub, guild);
  const joined = await guest.client.ask({ t: 'guild.join', invite: code, ...(await guest.identity.encFields(guild)) });
  check('guest joined with the invitation', joined.member === true);
  const owner = (await guest.client.ask({ t: 'members.list' })).items.find((m) => m.role === 'owner');
  check('the owner announced by the service is the one in the invitation', owner.fp === ownerFp && (await sha(owner.pub)) === ownerFp);
  await until(async () => (await guest.client.ask({ t: 'members.list' })).items.find((m) => m.role === 'member').keyed, 'app delivered the key', 10000);
  check('the app delivered the group key to the new member', await receiveKey(guest, guild, owner.enc));

  // ---- messages are ciphertext on the service and readable by members ----
  const text = 'Salut depuis l’invité — é à ü 😀';
  await say(guest, guild, 'general', 'guest00001', text);
  const shown = await until(() => bubbles(ev).then((t) => t.includes('Salut') && t), 'guest message shown');
  check('the app decrypts and shows the guest’s message (UTF-8 intact)', shown.includes(text), shown);
  await ev("document.getElementById('server-input').value='Réponse de l’app'; document.getElementById('server-composer').requestSubmit(); 1");
  let received;                       // the guest also receives the echo of its own message first
  do { received = await guest.client.wait('envelope', 8000); } while (received.from === guest.fp);
  check('the service holds only ciphertext of the app’s message', !Buffer.from(received.body, 'base64').toString('latin1').includes('Réponse'));
  check('the guest decrypts the app’s message', (await C.decryptMessage(guest.group.key, guild, received.channel, received.id, received.body)) === 'Réponse de l’app');
  check('that message is attributed to the app’s identity', received.from === ownerFp);

  // ---- tampering is visible, not silent ----
  await say(guest, guild, 'general', 'forged0001', 'chiffré avec une autre clé', { key: C.newGroupKey(), epoch: 1 });
  await until(() => bubbles(ev).then((t) => t.includes('illisible')), 'unreadable message flagged');
  check('a message under another key is flagged unreadable, never shown as text', !(await bubbles(ev)).includes('autre clé'));

  // ---- names ----
  const authors = () => ev("[...document.querySelectorAll('#server-messages .message-meta')].map(m=>m.textContent.split(' · ')[0]).join('|')");
  check('the guest’s chosen name is shown next to its message', (await authors()).includes(guest.identity.name), await authors());
  check('own messages show “Vous”', (await authors()).split('|').includes('Vous'));
  await guest.client.ask({ t: 'profile.set', ...(await guest.identity.nameFields(guild, 'Bob') ) });
  await until(() => authors().then((a) => a.includes('Bob')), 'rename shown');
  check('a rename reaches the app and relabels earlier messages', !(await authors()).includes(guest.identity.name));
  const twin = await player(hub, guild);
  const invite3 = await ev("(async()=>{ document.getElementById('invite-make').click(); await new Promise(r=>setTimeout(r,800)); return document.getElementById('invite-code').value; })()");
  await twin.client.ask({ t: 'guild.join', invite: invite3.split('|')[3], ...(await twin.identity.encFields(guild, 'alice')) });
  await until(() => ev("[...document.querySelectorAll('#member-list .member span')].some(s=>/^alice #[0-9a-f]{4}$/.test(s.textContent))"), 'twin labelled');
  check('a member who picks the same name as another is told apart by a fingerprint suffix', true);
  check('the owner sees itself as “Vous” in the member list', (await ev("[...document.querySelectorAll('#member-list .member span')].some(s=>s.textContent==='Vous')")) === true);
  check('the member list names everyone', (await ev("document.querySelectorAll('#member-list .member').length")) === 3);
  try { await until(async () => (await twin.client.ask({ t: 'members.list' })).items.find((m) => m.fp === twin.fp).keyed, 'twin keyed', 10000); }
  catch (error) { console.log('DEBUG twin closed=', twin.client.closed, 'queue=', JSON.stringify(twin.client.queue.map((m) => m.t + ':' + (m.code || ''))).slice(0, 300)); throw error; }
  const twinOwner = (await twin.client.ask({ t: 'members.list' })).items.find((m) => m.role === 'owner');
  await receiveKey(twin, guild, twinOwner.enc);

  // ---- exclusion: the guest is removed, the key changes, the twin keeps reading ----
  const oldKey = guest.group.key;
  await ev("(() => { const rows=[...document.querySelectorAll('#member-list .member')]; const row=rows.find(r=>r.querySelector('span').textContent==='Bob'); const b=[...row.querySelectorAll('button')].find(x=>x.textContent==='Exclure'); b.click(); b.click(); })()");
  check('the removed member is told, then disconnected', (await guest.client.wait('removed', 8000)).fp === guest.fp);
  await until(() => ev("murmurServers.debug.servers[0].epoch === 2 && murmurServers.debug.servers[0].members.size === 2"), 'app moved to epoch 2');
  check('the app opens the next epoch and drops the member from its list', true);
  const fresh = await hub.connect(guild);
  await fresh.login(guest.identity, guild);
  check('the removed member can neither read keys nor members', (await fresh.ask({ t: 'keys.get' })).code === 'forbidden' && (await fresh.ask({ t: 'members.list' })).code === 'forbidden');
  const inviteAgain = await ev("(async()=>{ document.getElementById('invite-make').click(); await new Promise(r=>setTimeout(r,800)); return document.getElementById('invite-code').value; })()");
  const rejoin = await fresh.ask({ t: 'guild.join', invite: inviteAgain.split('|')[3], ...(await guest.identity.encFields(guild, 'Bob')) });
  check('the removed identity cannot rejoin with a new invitation', rejoin.code === 'forbidden' && rejoin.reason === 'removed');
  await until(async () => (await twin.client.ask({ t: 'keys.get' })).items.length === 2, 'twin received the new key', 10000);
  const ring = (await twin.client.ask({ t: 'keys.get' })).items;
  twin.rings = new Map();
  for (const item of ring) twin.rings.set(item.epoch, await C.unwrapKey(await twin.agree(twinOwner.enc), guild, item.epoch, twinOwner.enc, twin.identity.enc, item.blob));
  check('remaining members get the new epoch’s key alongside the old one', [...twin.rings.keys()].join() === '1,2');
  await ev("document.getElementById('server-input').value='Message après exclusion'; document.getElementById('server-composer').requestSubmit(); 1");
  let later;
  do { later = await twin.client.wait('envelope', 8000); } while (later.from === twin.fp);
  check('the app encrypts new messages under the new epoch', C.parseBody(later.body).epoch === 2);
  check('a remaining member reads the new message', (await C.decryptMessage(twin.rings.get(2), guild, later.channel, later.id, later.body)) === 'Message après exclusion');
  check('the removed member’s old key cannot read it', await C.decryptMessage(oldKey, guild, later.channel, later.id, later.body).then(() => false, () => true));
  check('the removed member’s old key still reads old messages in the thread', (await bubbles(ev)).includes('Salut depuis'));
  await say(Object.assign({}, twin, { group: { key: twin.rings.get(2), epoch: 2 } }), guild, 'general', 'twin000001', 'Salut de la part de l’autre membre');
  await until(() => bubbles(ev).then((t) => t.includes('l’autre membre')), 'message in epoch 2 shown');
  check('the app reads a member’s message under the new epoch', true);
  guest.group = null;

  // ---- channels ----
  await ev("document.getElementById('channel-name').value='jeux'; document.getElementById('channel-create').click(); 1");
  check('owner creates a channel, members are told', (await twin.client.wait('channel', 8000)).channel.name === 'jeux');

  // ---- reconnection ----
  await ev("send('service-close:0'); 1");
  await until(() => ev("murmurServers.debug.servers[0].status !== 'ready'"), 'drop noticed', 5000);
  await until(() => ev("murmurServers.debug.servers[0].status === 'ready'"), 'reconnected', 15000);
  check('app reconnects by itself and keeps the group key', (await ev('murmurServers.debug.servers[0].keyState')) === 'ready');
  await say(twin, guild, 'general', 'twin000002', 'après coupure', { key: twin.rings.get(2), epoch: 2 });
  const after = await until(() => bubbles(ev).then((t) => t.includes('après coupure') && t), 'message after reconnect');
  check('messages after the reconnection are decrypted, earlier ones kept', after.includes('Réponse de l’app') && after.includes('Salut depuis') && (await ev('murmurServers.debug.servers[0].epoch')) === 2);

  // ---- joining a server whose owner is not the one the invitation names ----
  const guild2 = require('node:crypto').randomBytes(16).toString('hex');
  const owner2 = await player(hub, guild2);
  await owner2.client.ask({ t: 'guild.create', name: 'Chez Alice', ...(await owner2.identity.encFields(guild2)) });
  const first = await owner2.client.ask({ t: 'invite.create', max_uses: 5, ttl_s: 3600 });
  await ev(`document.getElementById('srv-invite').value=${JSON.stringify(`murmur4|${origin}|${guild2}|${first.code}|${'ab'.repeat(32)}`)}; document.getElementById('srv-join').click(); 1`);
  await until(() => ev(`${server(guild2)}?.status === 'error'`), 'refusal of a wrong owner', 10000);
  const refusal = await ev(`${server(guild2)}.notice`);
  check('an owner that does not match the invitation is refused', /propri/i.test(refusal), refusal);
  await ev(selectAndRemove(guild2));

  // ---- joining properly while the owner is away: the app waits, then unlocks when the key arrives ----
  const second = await owner2.client.ask({ t: 'invite.create', max_uses: 5, ttl_s: 3600 });
  await ev(`document.getElementById('srv-invite').value=${JSON.stringify(`murmur4|${origin}|${guild2}|${second.code}|${owner2.fp}`)}; document.getElementById('srv-join').click(); 1`);
  await until(() => ev(`${server(guild2)}?.status === 'ready' && ${server(guild2)}.keyState === 'waiting'`), 'joined second server, waiting for the key', 15000);
  check('without the owner’s key the app waits and refuses to send', (await ev(`${server(guild2)}.keyState`)) === 'waiting' && (await ev("document.getElementById('server-input').disabled")) === true);
  const list = (await owner2.client.ask({ t: 'members.list' })).items;
  const appMember = list.find((m) => m.role === 'member'), ownerSelf = list.find((m) => m.role === 'owner');
  owner2.group = { key: C.newGroupKey(), epoch: 1 };
  await owner2.client.ask({ t: 'keys.put', to: appMember.fp, epoch: 1, blob: await C.wrapKey(await owner2.agree(appMember.enc), guild2, 1, ownerSelf.enc, appMember.enc, owner2.group.key) });
  await until(() => ev(`${server(guild2)}.keyState === 'ready'`), 'key arrives after joining', 10000);
  check('when the owner delivers the key, the app unlocks without restarting', (await ev("document.getElementById('server-input').disabled")) === false);
  await say(owner2, guild2, 'general', 'alice00001', 'Bienvenue chez Alice');
  await until(() => bubbles(ev).then((t) => t.includes('Bienvenue chez Alice')), 'second server message');
  check('messages of the second server are decrypted with its own key', true);
  await ev(selectAndRemove(guild2));

  // ---- the page cannot make the identity sign or agree on arbitrary data ----
  check('arbitrary text is not signed', (await probe(ev, 'identity-sign', 'probe1', 'arbitrary text', 'signature', 'signature')) === 'refused');
  check('a point that is not on the curve is refused for key agreement', (await probe(ev, 'identity-ecdh', 'probe2', '04' + '11'.repeat(64), 'ecdh', 'secret')) === 'refused');

  console.log('GUILD=' + guild);
  hub.close(); page.close();
  console.log(failed ? `\n${failed} check(s) failed` : '\nAll end-to-end checks passed');
  process.exit(failed ? 1 : 0);
})().catch(async (error) => {
  console.error('ERROR', error.message);
  try {
    const page = await connectPage();
    console.error('APP STATE', await page.evaluate("JSON.stringify(murmurServers.debug.servers.map(s=>({status:s.status,key:s.keyState,epoch:s.epoch,notice:s.notice,members:s.members.size,caches:[...s.cache].map(([k,c])=>[k,c.items.length,c.newest])})))"));
    page.close();
  } catch { /* no page to inspect */ }
  process.exit(2);
});
