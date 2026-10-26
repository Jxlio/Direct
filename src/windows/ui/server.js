// Servers: the page speaks protocol v2 (docs/protocole.md) over a relay socket owned by the app.
// The app holds the identity and does the only two things that need it: sign, and agree on a
// shared secret. Messages are encrypted here with e2ee.js, so the service stores ciphertext only.
// events.js decides what a channel looks like (who may edit or delete what); voice.js adds calls.
(() => {
  'use strict';
  const C = window.murmurCrypto, E = window.murmurEvents;
  const STORE_KEY = 'murmur.servers.v1', PROFILE_KEY = 'murmur.profile.v1', NOTIFY_KEY = 'murmur.notify.v1';
  const MAX_SERVERS = 4, NAME_MAX = 32, MAX_FILE = 1100000;
  const REQUEST_TIMEOUT = 8000, TYPING_FOR = 5000, TYPING_EVERY = 3000, SEARCH_PAGES = 20;
  const servers = [];            // runtime objects, slot = index
  let identityPublic = '', identityEnc = '';
  let myName = '';
  let notifyMode = 'mentions';   // 'all' | 'mentions' | 'none'
  let selected = -1, channelId = '';
  let view = null;               // thread of the channel on screen
  let viewKey = '';
  let inviteShown = '';
  let reply = null, editing = null;                  // what the composer is answering or rewriting (a message number)
  let lastTyping = 0, announcedUnread = 0;
  const waiting = new Map();     // signature / agreement id -> resolve
  const randomHex = length => C.toHex(crypto.getRandomValues(new Uint8Array(length)));
  const alphabet = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
  function newId() {
    let time = Date.now(), head = '';
    for (let i = 0; i < 10; i++) { head = alphabet[time % 32] + head; time = Math.floor(time / 32); }
    return head + Array.from(crypto.getRandomValues(new Uint8Array(16)), b => alphabet[b % 32]).join('');
  }
  const short = fp => fp.slice(0, 8);
  const groups = fp => fp.slice(0, 16).replace(/(.{4})(?=.)/g, '$1 ');
  // A name is what a person chose; it is shown only if its signature checks out. Control and
  // direction-changing characters are refused so a name cannot disguise itself.
  const FORBIDDEN = [[0x00, 0x1f], [0x7f, 0x7f], [0x200b, 0x200f], [0x2028, 0x202e], [0x2066, 0x2069]];
  const validName = value => typeof value === 'string' && value === value.trim() && value.length > 0 && [...value].length <= NAME_MAX
    && ![...value].some(ch => FORBIDDEN.some(([low, high]) => ch.codePointAt(0) >= low && ch.codePointAt(0) <= high));

  function load() {
    try {
      const saved = JSON.parse(localStorage.getItem(STORE_KEY) || '[]');
      for (const item of saved.slice(0, MAX_SERVERS))
        if (typeof item?.origin === 'string' && /^[0-9a-f]{32}$/.test(item.guild || '') && /^[0-9a-f]{64}$/.test(item.owner || ''))
          servers.push(makeServer(item.origin, item.guild, String(item.name || 'Cercle').slice(0, 64), item.owner));
      const profile = JSON.parse(localStorage.getItem(PROFILE_KEY) || 'null');
      if (validName(profile?.name)) myName = profile.name;
      const notify = JSON.parse(localStorage.getItem(NOTIFY_KEY) || 'null');
      if (['all', 'mentions', 'none'].includes(notify?.mode)) notifyMode = notify.mode;
    } catch { /* storage may be unavailable; the list then lasts for this session only */ }
  }
  function save() {
    try { localStorage.setItem(STORE_KEY, JSON.stringify(servers.filter(s => s.owner).map(s => ({ origin: s.origin, guild: s.guild, name: s.name, owner: s.owner })))); } catch { /* see load() */ }
  }
  function makeServer(origin, guild, name, owner = '') {
    return { slot: -1, origin, guild, name, owner, status: 'closed', notice: '', you: '', member: false, role: '', channels: [], cache: new Map(),
      unread: new Map(), refs: new Map(), intent: null, invite: '', retry: 0, timer: 0, removed: false,
      members: new Map(), former: new Map(), keys: new Map(), epoch: 1, keyState: 'none', queue: Promise.resolve(), session: 0,
      files: new Map(), typing: new Map(), mentions: new Map(), voice: null };
  }
  const indexOf = server => servers.indexOf(server);
  const currentKey = server => server.keys.get(server.epoch);
  const staffRole = role => role === 'owner' || role === 'moderator';
  const isStaff = (server, fp) => staffRole(server.members.get(fp)?.role);
  const iAmStaff = server => staffRole(server.role);

  // ----- transport -----
  function connect(server) {
    if (server.removed || !identityPublic) return;
    clearTimeout(server.timer);
    server.slot = indexOf(server); server.session++;
    server.status = 'connecting'; server.notice = '';
    send(`service-open:${server.slot}|${server.origin}|${server.guild}`);
    render();
  }
  function scheduleReconnect(server) {
    if (server.removed) return;
    clearTimeout(server.timer);
    const delay = Math.min(30000, 1500 * 2 ** server.retry++);
    server.timer = setTimeout(() => connect(server), delay);
  }
  function frame(server, object) { send(`service-send:${indexOf(server)}|${JSON.stringify(object)}`); }
  function request(server, object, timeout = REQUEST_TIMEOUT) {
    const ref = randomHex(4);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        server.refs.delete(ref);
        reject(new Error('Le cercle ne répond pas.'));
        // A silent socket is treated as lost; the next attempt rebuilds it.
        send(`service-close:${indexOf(server)}`);
      }, timeout);
      server.refs.set(ref, answer => { clearTimeout(timer); answer.t === 'error' ? reject(Object.assign(new Error(errorText(answer)), { reply: answer })) : resolve(answer); });
      frame(server, { ...object, ref });
    });
  }
  function errorText(answer) {
    return ({ forbidden: 'Action refusée.', unauthorized: 'Signature refusée.', not_found: 'Introuvable.', conflict: 'Déjà fait, ou état dépassé : réessayez.',
      too_large: 'Trop volumineux.', bad_request: 'Demande invalide.', rate_limited: 'Trop rapide, réessayez dans un instant.',
      closed: 'Connexion perdue.' })[answer.code] || `Erreur ${answer.code}.`;
  }
  // Requests to the app (signature, key agreement) are answered by id.
  function ask(command, argument, label) {
    const id = randomHex(6);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { waiting.delete(id); reject(new Error(`${label} indisponible.`)); }, 5000);
      waiting.set(id, value => { clearTimeout(timer); value ? resolve(value) : reject(new Error(`${label} refusée.`)); });
      send(`${command}:${id}|${argument}`);
    });
  }
  const sign = text => ask('identity-sign', text, 'Signature');
  const agree = peerEnc => ask('identity-ecdh', peerEnc, 'Clé partagée').then(C.fromHex);
  const nameFields = async (guild, name) => ({ name, namesig: await sign(`murmur/v2/name|${guild}|${name}`) });
  const identityFields = async guild => ({ enc: identityEnc, encsig: await sign(`murmur/v2/enc|${guild}|${identityEnc}`), ...(await nameFields(guild, myName)) });
  const serial = () => Date.now();

  // ----- members, roles and owners -----
  const pubOf = (server, fp) => (server.members.get(fp) || server.former.get(fp))?.pub;
  async function loadMembers(server) {
    const answer = await request(server, { t: 'members.list' });
    server.members.clear(); server.former.clear();
    for (const item of answer.items) await acceptMember(server, item);
    for (const item of answer.former || []) await acceptFormer(server, item);
    server.epoch = Math.max(server.epoch, answer.epoch || 1);
    await followOwners(server, answer.chain || []);
    // Roles are what the owner signed, not what the service says.
    for (const item of answer.items) {
      const member = server.members.get(item.fp);
      if (!member) continue;
      member.role = item.fp === server.owner ? 'owner' : await moderatorVerified(server, item) ? 'moderator' : 'member';
    }
    if (server.members.get(server.owner)?.role !== 'owner') throw Object.assign(new Error('Le propriétaire annoncé par le service ne correspond pas à l’invitation. Connexion refusée.'), { fatal: true });
    server.role = server.members.get(server.you)?.role || server.role;
  }
  // From the owner we trust (the invitation, or the last one we followed), follow each signed hand-over.
  async function followOwners(server, chain) {
    let current = server.owner;
    for (const link of [...chain].sort((a, b) => a.serial - b.serial)) {
      if (link.from !== current) continue;
      const pub = pubOf(server, current);
      if (pub && await C.verifyEcdsa(pub, C.textBytes(`murmur/v2/owner|${server.guild}|${link.to}|${link.serial}`), link.sig)) current = link.to;
    }
    if (current !== server.owner) { server.owner = current; save(); }
  }
  const moderatorVerified = async (server, item) => item.role === 'moderator' && Number.isInteger(item.roleserial) && !!item.rolesig
    && await C.verifyEcdsa(pubOf(server, server.owner) || '', C.textBytes(`murmur/v2/role|${server.guild}|${item.fp}|moderator|${item.roleserial}`), item.rolesig);
  async function acceptMember(server, item) {
    try {
      if (item.fp !== await C.sha256Hex(C.fromHex(item.pub))) return;                      // identity must hash to its fingerprint
      if (!(await C.verifyEcdsa(item.pub, C.textBytes(`murmur/v2/enc|${server.guild}|${item.enc}`), item.encsig))) return;
      const named = validName(item.name) && await C.verifyEcdsa(item.pub, C.textBytes(`murmur/v2/name|${server.guild}|${item.name}`), item.namesig);
      server.members.set(item.fp, { pub: item.pub, enc: item.enc, role: item.fp === server.owner ? 'owner' : 'member', keyed: !!item.keyed, name: named ? item.name : '' });
    } catch { /* an invalid entry is simply not trusted */ }
  }
  // A former member's key is accepted when it hashes to the fingerprint on their old messages, so the
  // service cannot hand us a different key; it keeps those messages verifiable after an exclusion.
  async function acceptFormer(server, item) {
    try {
      if (item.fp !== await C.sha256Hex(C.fromHex(item.pub))) return;
      const named = validName(item.name) && await C.verifyEcdsa(item.pub, C.textBytes(`murmur/v2/name|${server.guild}|${item.name}`), item.namesig);
      server.former.set(item.fp, { pub: item.pub, name: named ? item.name : '' });
    } catch { /* ignored */ }
  }
  // Two people may pick the same name: then each shows a short suffix of its fingerprint.
  function nameOf(server, fp) {
    if (fp === server.you) return 'Vous';
    const member = server.members.get(fp) || server.former.get(fp);
    if (!member?.name) return short(fp);
    const same = [...server.members].filter(([other, m]) => other !== fp && m.name.toLowerCase() === member.name.toLowerCase());
    return same.length || member.name.toLowerCase() === 'vous' ? `${member.name} #${fp.slice(0, 4)}` : member.name;
  }
  async function updateProfile(server, item) {
    const member = server.members.get(item.fp);
    if (!member) return;
    const valid = validName(item.name) && await C.verifyEcdsa(member.pub, C.textBytes(`murmur/v2/name|${server.guild}|${item.name}`), item.namesig);
    if (valid) { member.name = item.name; relabel(server); }
  }
  // Rows keep their text; only the author label changes when a name or a role does.
  function relabel(server) {
    for (const [channel, cache] of server.cache) {
      refreshRows(server, cache);
      if (indexOf(server) === selected && channel === channelId && view) view.reset(cache.items.slice());
    }
    render();
  }
  async function roleChanged(server, message) {
    const member = server.members.get(message.fp);
    if (!member) return;
    const owner = pubOf(server, server.owner);
    const valid = owner && await C.verifyEcdsa(owner, C.textBytes(`murmur/v2/role|${server.guild}|${message.fp}|${message.role}|${message.serial}`), message.sig);
    if (!valid) return;                                   // a role change the owner did not sign is ignored
    member.role = message.role === 'moderator' ? 'moderator' : 'member';
    if (message.fp === server.you) server.role = member.role;
    relabel(server);
  }
  async function ownerChanged(server) {
    try { await loadMembers(server); await fetchKeys(server); relabel(server); } catch (error) { if (error.fatal) { fail(server, error.message); send(`service-close:${indexOf(server)}`); } }
  }

  // ----- keys -----
  // Every epoch's key addressed to us, accepted from the owner or a verified moderator only.
  async function fetchKeys(server) {
    const answer = await request(server, { t: 'keys.get' });
    const mine = server.members.get(server.you);
    for (const item of answer.items) {
      if (server.keys.has(item.epoch)) continue;
      const sender = server.members.get(item.from);
      if (!sender || !staffRole(sender.role)) continue;                                         // a key from an ordinary member is ignored
      try { server.keys.set(item.epoch, await C.unwrapKey(await agree(sender.enc), server.guild, item.epoch, sender.enc, mine.enc, item.blob)); }
      catch { /* a blob that does not open is not ours */ }
    }
    return refreshKeyState(server);
  }
  function refreshKeyState(server) {
    server.keyState = currentKey(server) ? 'ready' : 'waiting';
    return server.keyState === 'ready';
  }
  async function ensureKey(server) {
    if (!currentKey(server)) await fetchKeys(server);
    if (!currentKey(server) && server.intent === 'create' && server.role === 'owner') {
      server.keys.set(server.epoch, C.newGroupKey());
      await deliver(server, server.you, [server.epoch]);
    }
    return refreshKeyState(server);
  }
  async function deliver(server, fp, epochs = [...server.keys.keys()].sort((a, b) => a - b), stage = false) {
    const member = server.members.get(fp), mine = server.members.get(server.you);
    if (!member || !mine) return;
    for (const epoch of epochs) {
      const key = server.keys.get(epoch);
      if (!key) continue;
      const blob = await C.wrapKey(await agree(member.enc), server.guild, epoch, mine.enc, member.enc, key);
      await request(server, { t: 'keys.put', to: fp, epoch, blob });
    }
    if (!stage) member.keyed = true;
  }
  async function deliverPending(server) {
    if (!iAmStaff(server) || !currentKey(server)) return;
    for (const [fp, member] of server.members) if (!member.keyed && fp !== server.you) {
      try { await deliver(server, fp); } catch { /* retried at the next connection */ }
    }
  }
  // Exclusion: stage the next key for ourselves, remove the member and open the next epoch in one
  // step, then hand the new key to everybody who remains. The excluded identity never gets it.
  async function excludeMember(server, fp) {
    if (!iAmStaff(server) || fp === server.you || !server.members.has(fp)) return;
    const epoch = server.epoch + 1, key = C.newGroupKey();
    try {
      server.keys.set(epoch, key);
      await deliver(server, server.you, [epoch], true);
      await request(server, { t: 'member.remove', fp, epoch });
      server.epoch = epoch; server.members.delete(fp);
      await loadMembers(server);
      for (const other of server.members.keys()) if (other !== server.you) { try { await deliver(server, other, [epoch]); } catch { /* that member's key is unusable; the others still get theirs */ } }
      refreshKeyState(server);
      setNotice('');
    } catch (error) {
      if (server.epoch !== epoch) server.keys.delete(epoch);
      setNotice(error.message);
    }
    render();
  }
  async function setRole(server, fp, role) {
    if (server.role !== 'owner' || fp === server.you) return;
    const number = serial();
    try {
      const sig = await sign(`murmur/v2/role|${server.guild}|${fp}|${role}|${number}`);
      await request(server, { t: 'role.set', fp, role, serial: number, sig });
      await loadMembers(server);
      setNotice('');
    } catch (error) { setNotice(error.message); }
    render();
  }
  async function transferOwnership(server, fp) {
    if (server.role !== 'owner' || fp === server.you) return;
    const number = serial();
    try {
      if (!server.members.get(fp)?.keyed) throw new Error('Ce membre n’a pas encore reçu la clé du groupe : attendez qu’il se soit connecté.');
      const sig = await sign(`murmur/v2/owner|${server.guild}|${fp}|${number}`);
      await request(server, { t: 'owner.transfer', to: fp, serial: number, sig });
      await loadMembers(server);
      setNotice('');
    } catch (error) { setNotice(error.message); }
    render();
  }
  async function reinstate(server, fp) {
    if (server.role !== 'owner') return;
    try { await request(server, { t: 'member.unban', fp }); await loadMembers(server); setNotice('Réintégré : envoyez-lui une nouvelle invitation.'); } catch (error) { setNotice(error.message); }
    render();
  }

  // ----- protocol -----
  async function onFrame(server, message) {
    const session = server.session;
    switch (message.t) {
      case 'hello': {
        server.status = 'authenticating';
        try {
          const sig = await sign(`murmur/v2/auth|${server.guild}|${message.nonce}`);
          frame(server, { t: 'auth', pub: identityPublic, sig });
        } catch (error) { fail(server, error.message); }
        break;
      }
      case 'ready': await onReady(server, message, session); break;
      case 'envelope': server.queue = server.queue.then(() => addEnvelope(server, message, true)).catch(() => {}); break;
      case 'channel': addChannel(server, message.channel); break;
      case 'member': await acceptMember(server, message); relabel(server); deliverPending(server).catch(() => {}); break;
      case 'profile': await updateProfile(server, message); break;
      case 'role': await roleChanged(server, message); break;
      case 'owner': await ownerChanged(server); break;
      case 'removed': await onRemoved(server, message); break;
      case 'typing': onTyping(server, message); break;
      case 'keys':
        if (message.to === server.you) afterKey(server, fetchKeys(server));
        break;
      case 'voice': case 'voice.signal': window.murmurVoice?.onFrame(server, message); break;
      case 'error':
        if (!message.ref) fail(server, errorText(message));
        break;
      default: break;
    }
    const done = message.ref && server.refs.get(message.ref);
    if (done) { server.refs.delete(message.ref); done(message); }
  }
  async function onRemoved(server, message) {
    if (message.fp === server.you) {
      // Excluded: forget everything readable, the key included.
      server.status = 'absent'; server.member = false; server.notice = 'Vous avez été exclu de ce cercle.';
      server.keys.clear(); server.keyState = 'none'; server.cache.clear(); server.members.clear(); server.refs.clear(); server.files.clear();
      window.murmurVoice?.leave(server);
      if (indexOf(server) === selected) { view?.reset([]); viewKey = ''; }
      render();
      return;
    }
    server.members.delete(message.fp);
    loadMembers(server).then(() => relabel(server), () => {});            // learn the former member's key and name
    if (message.epoch > server.epoch) { server.epoch = message.epoch; refreshKeyState(server); }
    relabel(server);
  }
  function fail(server, text) { server.status = 'error'; server.notice = text; render(); }
  async function afterKey(server, promise) {
    try { if (await promise) await rebuild(server); } catch (error) { setNotice(error.message); }
    render();
  }
  async function onReady(server, message, session) {
    server.you = message.you; server.retry = 0;
    if (!message.exists) {
      if (server.intent === 'create') {
        try {
          server.owner = message.you;
          await request(server, { t: 'guild.create', ...(await identityFields(server.guild)), title: server.name });
        } catch (error) { fail(server, error.message); }
      } else { server.status = 'absent'; server.notice = 'Ce cercle n’existe pas encore sur ce service.'; render(); }
      return;
    }
    if (!message.member) {
      if (server.intent === 'join' && server.invite) {
        const code = server.invite; server.invite = '';
        try { await request(server, { t: 'guild.join', invite: code, ...(await identityFields(server.guild)) }); }
        catch (error) {
          const why = error.reply?.reason === 'removed' ? 'Vous avez été exclu de ce cercle.' : error.reply?.code === 'forbidden' ? 'Invitation invalide, expirée ou épuisée.' : error.message;
          fail(server, why);
        }
      } else if (server.notice !== 'Vous avez été exclu de ce cercle.') { server.status = 'absent'; server.notice = 'Vous n’êtes pas membre. Utilisez une invitation.'; render(); }
      else { server.status = 'absent'; render(); }
      return;
    }
    if (session !== server.session) return;
    server.member = true; server.role = message.role; server.name = message.name; server.channels = message.channels;
    server.epoch = Math.max(server.epoch, message.epoch || 1);
    server.status = 'ready'; server.notice = '';
    try {
      await loadMembers(server);
      await ensureKey(server);
      await rebuild(server);
      await deliverPending(server);
    } catch (error) {
      if (error.fatal) { fail(server, error.message); send(`service-close:${indexOf(server)}`); return; }
      server.notice = error.message;
    }
    save();
    if (server.intent) {
      const created = server.intent === 'create'; server.intent = null;
      if (created) await showInvite(server);
    }
    keepAlive();
    render();
    if (indexOf(server) === selected) await openChannel(server, channelId && server.channels.some(c => c.id === channelId) ? channelId : server.channels.find(c => c.type === 'text')?.id);
    else await resync(server);
    server.initialPresence = message.presence || {};
    window.murmurVoice?.onReady(server);
  }

  // ----- messages -----
  function cacheFor(server, channel) {
    if (!server.cache.has(channel)) server.cache.set(channel, { raw: [], store: E.createStore(), items: [], newest: 0, oldest: 0, complete: false, loaded: false });
    return server.cache.get(channel);
  }
  const context = server => ({ me: server.you, isStaff: fp => isStaff(server, fp), nameOf: fp => nameOf(server, fp) });
  // The row to draw for a message, with what only this page knows added: whether I may moderate it, and
  // the pictures already downloaded.
  function rowFor(server, cache, seq) {
    const row = E.row(cache.store, seq, context(server));
    if (!row) return null;
    row.canModerate = iAmStaff(server);
    row.files = row.files.map(file => ({ ...file, url: server.files.get(file.blob)?.url, loading: server.files.get(file.blob)?.loading, error: server.files.get(file.blob)?.error }));
    return row;
  }
  function refreshRows(server, cache) {
    cache.items = E.ordered(cache.store).map(seq => rowFor(server, cache, seq)).filter(Boolean);
  }
  // Turns a stored envelope into an event: check who signed it, then decrypt it with the key of its
  // epoch. An envelope always produces something; a problem becomes a note instead of text, never a
  // silently missing message.
  async function toEvent(server, envelope) {
    const event = { seq: envelope.seq, id: envelope.id, from: envelope.from, ts: envelope.ts, payload: { v: 1, text: '' } };
    let pub = pubOf(server, envelope.from);
    if (!pub) { try { await loadMembers(server); } catch { /* shown below */ } pub = pubOf(server, envelope.from); }
    const signed = pub && await C.verifyEcdsa(pub, C.textBytes(`murmur/v2/envelope|${server.guild}|${envelope.channel}|${envelope.id}|${envelope.body}`), envelope.sig);
    if (!signed) { event.payload.text = '⚠ Signature invalide : message ignoré.'; return event; }
    try {
      const epoch = C.parseBody(envelope.body).epoch, key = server.keys.get(epoch);
      if (!key) { event.payload.text = '🔒 Message chiffré : clé de cette période non disponible.'; return event; }
      event.payload = await C.decryptPayload(key, server.guild, envelope.channel, envelope.id, envelope.body);
    } catch { event.payload.text = '⚠ Message illisible ou modifié.'; }
    return event;
  }
  const mentionsMe = (server, row) => !!row?.mentionsMe && row.from !== server.you;
  async function addEnvelope(server, envelope, live) {
    const cache = cacheFor(server, envelope.channel);
    if (cache.newest && envelope.seq <= cache.newest) return;      // already known (history overlap)
    const event = await toEvent(server, envelope);
    if (cache.newest && envelope.seq <= cache.newest) return;
    cache.raw.push(envelope); cache.newest = envelope.seq; cache.oldest = cache.oldest || envelope.seq;
    const target = E.ingest(cache.store, event);
    const showing = indexOf(server) === selected && envelope.channel === channelId && view;
    if (target === null) return;
    const row = rowFor(server, cache, target);
    if (row && target === envelope.seq) {
      cache.items.push(row);
      if (showing) view.apply({ append: [row] });
    } else if (row) {
      const index = cache.items.findLastIndex(item => item.uid === target);
      if (index >= 0) cache.items[index] = row;
      if (showing) view.apply({ update: [row] });
    }
    forgetTyping(server, envelope.channel, envelope.from);
    if (live && envelope.from !== server.you) {
      const here = document.hasFocus() && showing;
      if (!here) {
        server.unread.set(envelope.channel, (server.unread.get(envelope.channel) || 0) + 1);
        if (row && target === envelope.seq && mentionsMe(server, row)) server.mentions.set(envelope.channel, (server.mentions.get(envelope.channel) || 0) + 1);
      }
    }
    if (live) {
      renderSide(); announceUnread();
      const sound = window.murmurSound;
      if (sound && target === envelope.seq) {
        if (envelope.from === server.you) sound.play('send');
        else if (row && mentionsMe(server, row)) sound.play('mention');
        else if (document.hasFocus() && showing) sound.play('tick');
        else sound.play('receive');
      }
    }
  }
  // After keys arrive, rows that showed "waiting" are produced again from the stored envelopes.
  async function rebuild(server) {
    for (const [channel, cache] of server.cache) {
      cache.store = E.createStore();
      for (const envelope of cache.raw) E.ingest(cache.store, await toEvent(server, envelope));
      refreshRows(server, cache);
      if (indexOf(server) === selected && channel === channelId && view) { view.reset(cache.items.slice()); viewKey = `${server.guild}/${channel}`; }
    }
  }
  function addChannel(server, channel) {
    if (server.channels.some(c => c.id === channel.id)) return;
    server.channels.push(channel);
    if (indexOf(server) === selected) renderChannels();
  }
  async function loadLatest(server, channel) {
    const cache = cacheFor(server, channel);
    const answer = await request(server, { t: 'history.fetch', channel, limit: 50 });
    cache.raw = answer.items.slice(); cache.store = E.createStore();
    for (const item of answer.items) E.ingest(cache.store, await toEvent(server, item));
    refreshRows(server, cache);
    cache.newest = answer.items.at(-1)?.seq || 0; cache.oldest = answer.items[0]?.seq || 0;
    cache.complete = !answer.more; cache.loaded = true;
  }
  async function loadOlder(server, channel) {
    const cache = cacheFor(server, channel);
    if (cache.complete || !cache.oldest || cache.busy) return 0;
    cache.busy = true;
    try {
      const answer = await request(server, { t: 'history.fetch', channel, limit: 50, before: cache.oldest });
      const known = new Set(cache.items.map(row => row.uid));
      for (const item of answer.items) E.ingest(cache.store, await toEvent(server, item));
      cache.raw.unshift(...answer.items);
      cache.oldest = answer.items[0]?.seq || cache.oldest; cache.complete = !answer.more;
      const fresh = E.ordered(cache.store).filter(seq => !known.has(seq)).map(seq => rowFor(server, cache, seq)).filter(Boolean);
      cache.items.unshift(...fresh);
      if (indexOf(server) === selected && channel === channelId && view) view.prepend(fresh);
      return answer.items.length;
    } catch { return 0; } finally { cache.busy = false; }
  }
  // After a reconnection or a wake-up, fetch what arrived while the page could not listen.
  async function resync(server) {
    for (const [channel, cache] of server.cache) {
      if (!cache.loaded) continue;
      try {
        const answer = await request(server, { t: 'history.fetch', channel, after: cache.newest, limit: 50 });
        for (const item of answer.items) await addEnvelope(server, item, false);
      } catch { return; }
    }
  }
  async function openChannel(server, channel) {
    if (!channel) { render(); return; }
    const kind = server.channels.find(c => c.id === channel)?.type;
    if (kind === 'voice') { window.murmurVoice?.open(server, channel); return; }
    channelId = channel; server.unread.delete(channel); server.mentions.delete(channel); cancelCompose();
    const cache = cacheFor(server, channel);
    renderChannels(); renderSide(); announceUnread();
    mountThread(server, channel);
    try {
      if (!cache.loaded) await loadLatest(server, channel); else await resync(server);
      if (indexOf(server) === selected && channel === channelId) mountThread(server, channel);
    } catch (error) { setNotice(error.message); }
    render();
  }
  function mountThread(server, channel) {
    const key = `${server.guild}/${channel}`;
    const cache = cacheFor(server, channel);
    if (!view) {   // created once: every thread adds a scroll listener to its container
      view = createThread($('server-messages'), $('server-scroll'), items => { $('server-empty').hidden = items.length > 0; });
      view.thread.onTop = () => { const current = servers[selected]; if (current?.status === 'ready' && channelId) loadOlder(current, channelId); };
    }
    if (viewKey !== key || view.thread.items.length !== cache.items.length) { view.reset(cache.items.slice()); viewKey = key; }
  }
  async function showInvite(server) {
    try {
      const answer = await request(server, { t: 'invite.create', max_uses: Number($('invite-uses').value) || 10, ttl_s: Number($('invite-ttl').value) || 7 * 24 * 3600 });
      inviteShown = `murmur4|${server.origin}|${server.guild}|${answer.code}|${server.owner}`;
    } catch (error) { setNotice(error.message); }
    renderInvite();
  }

  // ----- sending -----
  async function sendPayload(server, channel, payload) {
    const key = currentKey(server);
    if (!key) throw new Error('La clé du groupe n’est pas encore arrivée.');
    const id = newId();
    const body = await C.encryptPayload(key, server.epoch, server.guild, channel, id, payload);
    const sig = await sign(`murmur/v2/envelope|${server.guild}|${channel}|${id}|${body}`);
    return request(server, { t: 'envelope.send', channel, id, body, sig });
  }
  const escapeRegExp = value => value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  // People named with @ in the text: only those who are really in this server.
  function mentionsIn(server, text) {
    const found = [];
    for (const [fp, member] of server.members) {
      if (fp === server.you || !member.name) continue;
      if (new RegExp(`@${escapeRegExp(member.name)}(?![\\p{L}\\p{N}_])`, 'iu').test(text)) found.push(fp);
    }
    return found;
  }
  async function sendMessage(text, file) {
    const server = servers[selected];
    text = text.trim();
    if (!server || server.status !== 'ready' || !channelId || (!text && !file)) return;
    try {
      if (editing !== null) {
        const mine = E.row(cacheFor(server, channelId).store, editing, context(server));
        if (mine && mine.from === server.you) await sendPayload(server, channelId, { t: 'edit', target: editing, text });
        cancelCompose(); setNotice(''); return;
      }
      const payload = { text };
      if (reply !== null) payload.reply = reply;
      const mentions = mentionsIn(server, text);
      if (mentions.length) payload.mentions = mentions;
      if (file) payload.files = [await uploadFile(server, file)];
      await sendPayload(server, channelId, payload);
      cancelCompose(); setNotice('');
    } catch (error) { setNotice(error.message); window.murmurSound?.play('error'); }
  }
  async function react(server, seq, emoji) {
    const cache = cacheFor(server, channelId), row = E.row(cache.store, seq, context(server));
    if (!row) return;
    const mine = row.reactions.find(r => r.emoji === emoji)?.mine;
    try { await sendPayload(server, channelId, { t: 'react', target: seq, emoji, on: !mine }); } catch (error) { setNotice(error.message); }
  }
  async function remove(server, seq) {
    try { await sendPayload(server, channelId, { t: 'del', target: seq }); } catch (error) { setNotice(error.message); }
  }
  async function createChannel(name, type = 'text') {
    const server = servers[selected];
    if (!server || !iAmStaff(server)) return;
    try { await request(server, { t: 'channel.create', name: name.trim(), type }); setNotice(''); } catch (error) { setNotice(error.message); }
  }

  // ----- files -----
  async function uploadFile(server, file) {
    if (file.size > MAX_FILE) throw new Error('Image trop volumineuse (1 Mo au maximum).');
    const bytes = new Uint8Array(await file.arrayBuffer());
    const mime = C.sniffImage(bytes);
    if (!mime) throw new Error('Seules les images JPEG, PNG, GIF et WebP sont acceptées.');
    const id = randomHex(16), sealed = await C.encryptFile(bytes, id);
    for (let index = 0; index < sealed.chunks.length; index++) {
      setNotice(`Envoi de l’image… ${index + 1}/${sealed.chunks.length}`);
      await request(server, { t: 'blob.put', id, index, total: sealed.chunks.length, data: sealed.chunks[index] }, 20000);
    }
    server.files.set(id, { url: await toDataUrl(bytes, mime) });
    return { blob: id, name: String(file.name || 'image').slice(0, 80), size: bytes.length, mime, key: sealed.key, chunks: sealed.chunks.length };
  }
  const toDataUrl = (bytes, mime) => new Promise(resolve => { const reader = new FileReader(); reader.onload = () => resolve(reader.result); reader.readAsDataURL(new Blob([bytes], { type: mime })); });
  async function showFile(server, blob) {
    const cache = cacheFor(server, channelId);
    const file = [...cache.store.base.values()].flatMap(ev => (Array.isArray(ev.payload.files) ? ev.payload.files : [])).find(f => f?.blob === blob);
    if (!file || server.files.get(blob)?.url) return;
    const mark = state => { server.files.set(blob, state); for (const row of cache.items) if (row.files?.some(f => f.blob === blob)) { const fresh = rowFor(server, cache, row.uid); const i = cache.items.indexOf(row); cache.items[i] = fresh; if (view) view.apply({ update: [fresh] }); } };
    mark({ loading: true });
    try {
      if (!Number.isInteger(file.chunks) || file.chunks < 1 || file.chunks > 24 || typeof file.key !== 'string') throw new Error('Fichier invalide');
      const chunks = [];
      for (let index = 0; index < file.chunks; index++) chunks.push((await request(server, { t: 'blob.get', id: blob, index }, 20000)).data);
      const bytes = await C.decryptFile(file.key, blob, chunks), mime = C.sniffImage(bytes);
      if (!mime) throw new Error('Format non pris en charge');
      mark({ url: await toDataUrl(bytes, mime) });
    } catch (error) { mark({ error: error.message === 'Fichier invalide' || error.message === 'Format non pris en charge' ? error.message : 'Fichier indisponible' }); }
  }

  // ----- composing: reply, edit, emoji, mentions, typing -----
  function cancelCompose() {
    reply = null; editing = null;
    $('compose-context').hidden = true;
    if ($('server-input').dataset.editing) { $('server-input').value = ''; delete $('server-input').dataset.editing; }
  }
  function startReply(server, seq) {
    const row = E.row(cacheFor(server, channelId).store, seq, context(server));
    if (!row || row.deleted) return;
    cancelCompose(); reply = seq;
    $('compose-text').textContent = `Réponse à ${row.author} : ${E.snippet(row.text) || '(pièce jointe)'}`;
    $('compose-context').hidden = false; $('server-input').focus();
  }
  function startEdit(server, seq) {
    const row = E.row(cacheFor(server, channelId).store, seq, context(server));
    if (!row || row.deleted || row.from !== server.you) return;
    cancelCompose(); editing = seq;
    $('server-input').value = row.text; $('server-input').dataset.editing = '1';
    $('compose-text').textContent = 'Modification du message — Entrée pour enregistrer, Échap pour annuler';
    $('compose-context').hidden = false; $('server-input').focus();
  }
  const EMOJI = ['😀', '😁', '😂', '🤣', '😊', '😍', '😘', '😎', '🤔', '😅', '😢', '😭', '😡', '😱', '🥳', '😴', '🙄', '😏', '🤗', '😬',
    '👍', '👎', '👏', '🙏', '💪', '👋', '🤝', '✌️', '👌', '🙌', '🔥', '✨', '🎉', '💯', '❤️', '💔', '💙', '💚', '💜', '🧡',
    '🎮', '🎧', '🎵', '📷', '💻', '📱', '☕', '🍕', '🍔', '🍺', '🎂', '🏆', '⚽', '🚀', '🌍', '🌞', '🌙', '⭐', '🐶', '🐱'];
  const QUICK = ['👍', '❤️', '😂', '😮', '😢', '🎉'];
  function emojiPanel(anchor, pick, list = EMOJI) {
    const panel = $('emoji-panel');
    panel.replaceChildren();
    for (const emoji of list) {
      const button = document.createElement('button'); button.type = 'button'; button.textContent = emoji; button.setAttribute('aria-label', emoji);
      button.addEventListener('click', () => { panel.hidden = true; pick(emoji); });
      panel.append(button);
    }
    const box = anchor.getBoundingClientRect();
    panel.hidden = false;
    const width = panel.offsetWidth, height = panel.offsetHeight;
    panel.style.left = `${Math.max(8, Math.min(window.innerWidth - width - 8, box.left))}px`;
    panel.style.top = `${Math.max(8, box.top - height - 6)}px`;
  }
  function insertAtCaret(text) {
    const input = $('server-input'), start = input.selectionStart ?? input.value.length, end = input.selectionEnd ?? start;
    input.value = input.value.slice(0, start) + text + input.value.slice(end);
    input.selectionStart = input.selectionEnd = start + text.length; input.focus();
  }
  // @name suggestions follow the caret.
  function suggestMentions() {
    const server = servers[selected], list = $('mention-list'), input = $('server-input');
    const before = input.value.slice(0, input.selectionStart ?? 0), match = /(?:^|\s)@([^\s@]{0,32})$/.exec(before);
    if (!server || !match) { list.hidden = true; return; }
    const wanted = match[1].toLowerCase();
    const people = [...server.members].filter(([fp, m]) => fp !== server.you && m.name && m.name.toLowerCase().startsWith(wanted)).slice(0, 6);
    list.replaceChildren();
    for (const [, member] of people) {
      const button = document.createElement('button'); button.type = 'button'; button.textContent = `@${member.name}`;
      button.addEventListener('mousedown', event => {
        event.preventDefault();
        input.value = `${before.slice(0, before.length - match[1].length - 1)}@${member.name} ${input.value.slice(input.selectionStart)}`;
        const caret = before.length - match[1].length + member.name.length + 1;
        input.selectionStart = input.selectionEnd = caret; list.hidden = true; input.focus();
      });
      list.append(button);
    }
    list.hidden = !people.length;
  }
  function onTyping(server, message) {
    if (message.fp === server.you) return;
    const channel = server.typing.get(message.channel) || new Map();
    channel.set(message.fp, Date.now() + TYPING_FOR); server.typing.set(message.channel, channel);
    renderTyping(); setTimeout(renderTyping, TYPING_FOR + 100);
  }
  function forgetTyping(server, channel, fp) { server.typing.get(channel)?.delete(fp); renderTyping(); }
  function renderTyping() {
    const server = servers[selected], now = Date.now();
    const people = server ? [...(server.typing.get(channelId) || [])].filter(([, until]) => until > now).map(([fp]) => nameOf(server, fp)) : [];
    $('typing-note').textContent = !people.length ? '' : people.length === 1 ? `${people[0]} écrit…` : people.length === 2 ? `${people[0]} et ${people[1]} écrivent…` : 'Plusieurs personnes écrivent…';
  }
  function announceTyping() {
    const server = servers[selected];
    if (!server || server.status !== 'ready' || !channelId || Date.now() - lastTyping < TYPING_EVERY || !$('server-input').value.trim()) return;
    lastTyping = Date.now(); frame(server, { t: 'typing', channel: channelId });
  }

  // ----- notifications: the window flashes and its title counts; nothing readable ever leaves the page -----
  const totalUnread = () => servers.reduce((sum, s) => sum + [...s.unread.values()].reduce((a, b) => a + b, 0), 0);
  function announceUnread() {
    const count = notifyMode === 'none' ? 0 : notifyMode === 'all' ? totalUnread() : servers.reduce((sum, s) => sum + [...s.mentions.values()].reduce((a, b) => a + b, 0), 0);
    if (count !== announcedUnread) { announcedUnread = count; send(`unread:${count}`); }
  }
  // While any server is connected the window must keep receiving, even minimised.
  function keepAlive() { send(`keepalive:${servers.some(s => s.status === 'ready') ? 1 : 0}`); }

  // ----- search: done here, because the service only ever holds ciphertext -----
  async function searchServer(query) {
    const server = servers[selected], box = $('search-results');
    if (!server || !query.trim()) { box.hidden = true; return; }
    const cache = cacheFor(server, channelId);
    let hits = E.search(cache.store, query, context(server));
    for (let page = 0; page < SEARCH_PAGES && hits.length < 10 && !cache.complete; page++) {
      $('search-note').textContent = `Recherche dans l’historique… (${cache.raw.length} messages lus)`;
      if (!(await loadOlder(server, channelId))) break;
      hits = E.search(cache.store, query, context(server));
    }
    box.replaceChildren();
    const note = document.createElement('div'); note.className = 'search-note';
    note.textContent = hits.length ? `${hits.length} résultat${hits.length > 1 ? 's' : ''}${cache.complete ? '' : ' dans les messages déjà lus'}` : cache.complete ? 'Aucun résultat.' : 'Aucun résultat dans les messages lus.';
    box.append(note);
    for (const hit of hits.slice(0, 30)) {
      const button = document.createElement('button'); button.type = 'button'; button.className = 'search-hit'; button.dataset.seq = hit.seq;
      const who = document.createElement('strong'); who.textContent = `${nameOf(server, hit.from)} · ${new Date(hit.ts).toLocaleString('fr-FR', { dateStyle: 'short', timeStyle: 'short' })}`;
      const what = document.createElement('span'); what.textContent = E.snippet(hit.text);
      button.append(who, what); box.append(button);
    }
    box.hidden = false; $('search-note').textContent = '';
  }
  async function jumpTo(seq) {
    const server = servers[selected], cache = server && cacheFor(server, channelId);
    if (!cache) return;
    for (let page = 0; page < SEARCH_PAGES && !cache.store.base.has(seq) && !cache.complete; page++) if (!(await loadOlder(server, channelId))) break;
    const index = cache.items.findIndex(item => item.uid === seq);
    if (index < 0) { setNotice('Ce message est trop ancien pour être retrouvé ici.'); return; }
    view.reset(cache.items.slice(0, Math.min(cache.items.length, index + 40)));
    viewKey = '';
    const row = document.querySelector(`#server-messages [data-uid="${seq}"]`);
    row?.scrollIntoView({ block: 'center' }); row?.classList.add('flash');
    setTimeout(() => row?.classList.remove('flash'), 1600);
    $('search-results').hidden = true; $('back-to-present').hidden = index + 40 >= cache.items.length;
  }

  // ----- actions from the page -----
  function addServer(server) {
    if (servers.length >= MAX_SERVERS) return false;
    servers.push(server); save(); connect(server); select(servers.length - 1); return true;
  }
  // https to a named host, or plain http to this computer for development; never a private network address.
  function publicOrigin(origin) {
    let url;
    try { url = new URL(origin); } catch { return false; }
    if (url.origin !== origin.replace(/\/+$/, '')) return false;
    const host = url.hostname;
    if (url.protocol === 'http:') return host === '127.0.0.1' || host === 'localhost';
    if (url.protocol !== 'https:') return false;
    if (/^(localhost|.*\.local|.*\.internal)$/i.test(host) || host.includes(':')) return false;
    const ip = host.match(/^(\d+)\.(\d+)\.(\d+)\.(\d+)$/);
    if (ip) { const [a, b] = [Number(ip[1]), Number(ip[2])]; return !(a === 10 || a === 127 || a === 0 || (a === 169 && b === 254) || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168)); }
    return true;
  }
  function createServer(origin, name) {
    origin = origin.trim().replace(/\/+$/, ''); name = name.trim();
    if (!myName) return 'Choisissez d’abord votre nom, juste au-dessus.';
    if (!/^https:\/\/[A-Za-z0-9.-]+(:\d+)?$/.test(origin) && !/^http:\/\/(127\.0\.0\.1|localhost)(:\d+)?$/.test(origin)) return 'Adresse du service invalide (https://…).';
    if (!name || name.length > 64) return 'Donnez un nom au cercle (64 caractères maximum).';
    if (!identityPublic) return 'Identité indisponible.';
    const server = makeServer(origin, randomHex(16), name); server.intent = 'create';
    return addServer(server) ? '' : `Maximum ${MAX_SERVERS} cercles.`;
  }
  function joinServer(text) {
    const parts = text.trim().split('|');
    if (parts.length === 5 && !publicOrigin(parts[1])) return 'Adresse du service refusée (réseau local ou invalide).';
    if (!myName) return 'Choisissez d’abord votre nom, juste au-dessus.';
    if (parts.length !== 5 || parts[0] !== 'murmur4' || !/^[0-9a-f]{32}$/.test(parts[2]) || !/^[0-9a-f]{32}$/.test(parts[3]) || !/^[0-9a-f]{64}$/.test(parts[4])) return 'Invitation invalide.';
    if (!identityPublic) return 'Identité indisponible.';
    const existing = servers.find(s => s.guild === parts[2]);
    if (existing) { select(indexOf(existing)); return ''; }
    const server = makeServer(parts[1], parts[2], 'Cercle', parts[4]); server.intent = 'join'; server.invite = parts[3];
    return addServer(server) ? '' : `Maximum ${MAX_SERVERS} cercles.`;
  }
  // The name is the same on every server, and each server gets it signed for that server only.
  async function setMyName(value) {
    value = value.trim();
    if (!validName(value)) return `Un nom de 1 à ${NAME_MAX} caractères, sans caractères spéciaux.`;
    myName = value;
    try { localStorage.setItem(PROFILE_KEY, JSON.stringify({ name: myName })); } catch { /* kept for this session */ }
    for (const server of servers) {
      if (server.status !== 'ready') continue;
      try { await request(server, { t: 'profile.set', ...(await nameFields(server.guild, myName)) }); } catch (error) { return error.message; }
    }
    return '';
  }
  function removeServer(index) {
    const server = servers[index];
    if (!server) return;
    server.removed = true; clearTimeout(server.timer); send(`service-close:${index}`);
    window.murmurVoice?.leave(server);
    servers.splice(index, 1); save();
    servers.forEach((s, i) => { if (s.status !== 'closed') s.slot = i; });
    selected = -1; channelId = ''; viewKey = ''; inviteShown = '';
    $('server-messages').replaceChildren(); view?.reset([]);
    keepAlive(); render(); openPage('servers');
  }
  function select(index) {
    selected = index; channelId = ''; viewKey = ''; inviteShown = ''; cancelCompose();
    const server = servers[index];
    if (!server) return;
    if (server.status === 'closed' || server.status === 'error') connect(server);
    view?.reset([]);
    render();
    if (server.status === 'ready') openChannel(server, server.channels.find(c => c.type === 'text')?.id);
  }

  // ----- rendering -----
  const keyText = server => server.keyState === 'ready' ? 'Chiffré de bout en bout' : server.keyState === 'waiting' ? 'En attente de la clé du propriétaire…' : '';
  const statusText = server => server.status === 'ready' ? keyText(server) || 'Connecté'
    : ({ connecting: 'Connexion…', authenticating: 'Authentification…', closed: 'Déconnecté', error: server.notice || 'Erreur', absent: server.notice })[server.status] || '';
  function setNotice(text) { $('server-notice').textContent = text; }
  function renderSide() {
    const list = $('side-servers');
    list.replaceChildren();
    servers.forEach((server, index) => {
      const button = document.createElement('button');
      button.className = 'side-friend' + (index === selected ? ' selected' : '');
      button.dataset.page = 'server'; button.dataset.server = index;
      const mini = document.createElement('span'); mini.className = 'friend-avatar'; mini.textContent = (server.name[0] || 'S').toUpperCase();
      const name = document.createElement('span'); name.textContent = server.name;
      const unread = [...server.unread.values()].reduce((a, b) => a + b, 0);
      button.append(mini, name);
      if (unread) { const badge = document.createElement('span'); badge.className = 'side-badge'; badge.textContent = unread; button.append(badge); }
      list.append(button);
    });
    if (!servers.length) { const empty = document.createElement('p'); empty.textContent = 'Aucun cercle.'; list.append(empty); }
    renderCircles();
  }
  // The circles as cards with a cover. The cover is one of six designs picked from the name, so a circle keeps its look.
  function renderCircles() {
    const grid = $('circles-grid');
    grid.replaceChildren();
    servers.forEach((server, index) => {
      const card = document.createElement('button'); card.type = 'button'; card.className = 'circle-card'; card.dataset.page = 'server'; card.dataset.server = index;
      const cover = document.createElement('span'); cover.className = 'circle-cover'; cover.dataset.cover = String([...server.name].reduce((sum, c) => sum + c.charCodeAt(0), 0) % 15);
      const badge = document.createElement('span'); badge.className = 'circle-badge'; badge.textContent = 'E2E'; cover.append(badge);
      const body = document.createElement('span'); body.className = 'circle-body';
      const name = document.createElement('strong'); name.textContent = server.name;
      const meta = document.createElement('small'); meta.textContent = server.members.size ? `${server.members.size} membre${server.members.size > 1 ? 's' : ''}` : statusText(server);
      const dot = document.createElement('i'); dot.className = 'circle-dot' + (server.status === 'ready' ? ' on' : '');
      const unread = [...server.unread.values()].reduce((a, b) => a + b, 0);
      body.append(name, meta, dot);
      if (unread) { const count = document.createElement('b'); count.className = 'circle-unread'; count.textContent = unread; body.append(count); }
      card.append(cover, body); grid.append(card);
    });
    if (!servers.length) { const empty = document.createElement('p'); empty.className = 'circles-empty'; empty.textContent = 'Aucun cercle pour le moment : créez-en un ou rejoignez-en un avec une invitation.'; grid.append(empty); }
  }
  function renderChannels() {
    const server = servers[selected], list = $('channel-list');
    list.replaceChildren();
    if (!server) return;
    for (const channel of server.channels) {
      const button = document.createElement('button');
      const inCall = server.voice?.channel === channel.id;
      button.className = 'channel' + (channel.id === channelId ? ' selected' : '') + (inCall ? ' live' : '');
      button.textContent = (channel.type === 'voice' ? '🔊 ' : '# ') + channel.name;
      const unread = server.unread.get(channel.id);
      if (unread) { const badge = document.createElement('span'); badge.className = 'side-badge'; badge.textContent = unread; button.append(badge); }
      button.addEventListener('click', () => openChannel(server, channel.id));
      list.append(button);
      if (channel.type === 'voice') window.murmurVoice?.decorate(server, channel, list);
    }
  }
  function memberButton(label, handler, danger) {
    const button = document.createElement('button'); button.type = 'button'; button.className = `tiny-button${danger ? ' remove' : ''}`; button.textContent = label;
    button.addEventListener('click', () => {
      if (button.dataset.confirm !== 'yes') { button.dataset.confirm = 'yes'; button.textContent = 'Confirmer ?'; return; }
      handler();
    });
    return button;
  }
  const ROLE_NAME = { owner: 'Propriétaire', moderator: 'Modérateur', member: 'Membre' };
  function avatarFor(name) { const avatar = document.createElement('div'); avatar.className = 'friend-avatar member-avatar'; avatar.textContent = ((name || '?').trim()[0] || '?').toUpperCase(); return avatar; }
  function roleBadge(role) { const badge = document.createElement('span'); badge.className = `role-badge is-${role}`; badge.textContent = ROLE_NAME[role] || ''; badge.hidden = role === 'member'; return badge; }
  function renderMembers() {
    const server = servers[selected], list = $('member-list'), side = $('side-members');
    list.replaceChildren(); side.replaceChildren();
    if (!server) return;
    const rank = role => (role === 'owner' ? 0 : role === 'moderator' ? 1 : 2);
    const people = [...server.members].sort(([a, x], [b, y]) => rank(x.role) - rank(y.role) || nameOf(server, a).localeCompare(nameOf(server, b)));
    for (const [fp, member] of people) {
      const name = nameOf(server, fp);
      const mini = document.createElement('div'); mini.className = `side-member is-${member.role}`; mini.title = `${ROLE_NAME[member.role] || 'Membre'} · empreinte ${groups(fp)}`;
      const miniName = document.createElement('b'); miniName.textContent = name;
      mini.append(avatarFor(name), miniName, roleBadge(member.role)); side.append(mini);

      const row = document.createElement('div'); row.className = `member is-${member.role}`;
      const label = document.createElement('span'); label.className = 'member-name'; label.textContent = name;
      label.title = `Empreinte : ${groups(fp)}`;
      const who = document.createElement('div'); who.className = 'member-who'; who.append(label, roleBadge(member.role));
      row.append(avatarFor(name), who);
      const tools = document.createElement('span'); tools.className = 'member-tools';
      if (fp !== server.you) {
        if (server.role === 'owner' && member.role !== 'owner') {
          tools.append(memberButton(member.role === 'moderator' ? 'Retirer modérateur' : 'Modérateur', () => setRole(server, fp, member.role === 'moderator' ? 'member' : 'moderator')));
          if (member.keyed) tools.append(memberButton('Donner la propriété', () => transferOwnership(server, fp), true));
        }
        if (iAmStaff(server) && member.role !== 'owner' && (server.role === 'owner' || member.role === 'member')) tools.append(memberButton('Exclure', () => excludeMember(server, fp), true));
      }
      row.append(tools);
      list.append(row);
    }
    if (server.role === 'owner' && server.former.size) {
      const title = document.createElement('div'); title.className = 'member-title'; title.textContent = 'Exclus'; list.append(title);
      for (const [fp, gone] of server.former) {
        const row = document.createElement('div'); row.className = 'member former';
        const name = gone.name || short(fp);
        const label = document.createElement('span'); label.className = 'member-name'; label.textContent = name; label.title = `Empreinte : ${groups(fp)}`;
        const who = document.createElement('div'); who.className = 'member-who'; who.append(label);
        const tools = document.createElement('span'); tools.className = 'member-tools'; tools.append(memberButton('Réintégrer', () => reinstate(server, fp)));
        row.append(avatarFor(name), who, tools); list.append(row);
      }
    }
  }
  // Settings of the selected server: one panel per subject.
  function renderSettings(server) {
    const initial = ((server.name || '?').trim()[0] || '?').toUpperCase();
    $('server-crest').textContent = $('ss-crest').textContent = initial;
    $('ss-title').textContent = $('ss-hero-name').textContent = server.name;
    $('ss-hero-status').textContent = statusText(server);
    $('ss-count').textContent = String(server.members.size);
    $('ss-role').textContent = ROLE_NAME[server.role] || 'Membre';
    $('ss-owner').textContent = server.owner ? groups(server.owner) : '';
    $('ss-origin').textContent = server.origin;
    $('ss-invites-nav').hidden = !iAmStaff(server);
    $('ss-members-note').textContent = server.role === 'owner' ? 'Vous pouvez nommer des modérateurs, exclure, donner la propriété ou réintégrer. Chaque geste est signé.' : iAmStaff(server) ? 'Vous pouvez exclure des membres simples.' : 'Seuls le propriétaire et les modérateurs peuvent gérer les membres.';
    const list = $('ss-channels'); list.replaceChildren();
    for (const channel of server.channels) {
      const row = document.createElement('div'); row.className = 'ss-channel';
      row.textContent = (channel.type === 'voice' ? '🔊 ' : '# ') + channel.name; list.append(row);
    }
  }
  function showSettingsPanel(name) {
    for (const button of document.querySelectorAll('.ss-nav [data-ss]')) button.classList.toggle('active', button.dataset.ss === name);
    for (const panel of document.querySelectorAll('[data-ss-panel]')) panel.hidden = panel.dataset.ssPanel !== name;
  }
  function openSettings() {
    if (!servers[selected]) return;
    renderSettings(servers[selected]);
    showSettingsPanel('overview');
    $('server-settings').hidden = false;
    window.murmurSound?.play('open');
    $('ss-close').focus();
  }
  function closeSettings() { if ($('server-settings').hidden) return; window.murmurSound?.play('close'); $('server-settings').hidden = true; $('server-menu').focus(); }
  function renderInvite() {
    $('invite-box').hidden = !inviteShown;
    $('invite-code').value = inviteShown;
  }
  function render() {
    renderSide();
    $('profile-name').value = document.activeElement === $('profile-name') ? $('profile-name').value : myName;
    $('notify-mode').value = notifyMode;
    const server = servers[selected];
    $('server-empty-state').hidden = !!server;
    $('server-body').hidden = !server;
    if (!server) return;
    $('server-title').textContent = server.name;
    const channel = server.channels.find(c => c.id === channelId);
    $('server-channel-title').textContent = channel ? `# ${channel.name}` : 'Salons';
    $('server-status').textContent = statusText(server);
    $('server-status').classList.toggle('online', server.status === 'ready' && server.keyState === 'ready');
    $('server-owner-fp').textContent = server.owner ? `Propriétaire : ${groups(server.owner)}` : '';
    $('server-members').textContent = server.members.size ? `${server.members.size} membre${server.members.size > 1 ? 's' : ''}` : '';
    $('server-owner-tools').hidden = !iAmStaff(server);
    const ready = server.status === 'ready' && !!channelId && !!currentKey(server);
    $('server-input').disabled = $('server-send').disabled = !ready;
    $('server-attach').disabled = $('server-emoji').disabled = !ready;
    $('search-form').hidden = !channelId;
    renderChannels(); renderMembers(); renderInvite(); renderTyping();
    renderSettings(server);
    if (!iAmStaff(server) && !document.querySelector('[data-ss-panel="invites"]').hidden) showSettingsPanel('overview');
    window.murmurVoice?.render(server);
  }
  function renderServersPage(message) { $('servers-notice').textContent = message || ''; }

  // ----- wiring -----
  $('side-servers').addEventListener('click', event => {
    const button = event.target.closest('[data-server]');
    if (button) select(Number(button.dataset.server));
  });
  $('circles-grid').addEventListener('click', event => {
    const button = event.target.closest('[data-server]');
    if (button) select(Number(button.dataset.server));
  });
  $('profile-save').addEventListener('click', async () => {
    const problem = await setMyName($('profile-name').value);
    renderServersPage(problem);
    $('profile-saved').textContent = problem ? '' : 'Enregistré ✓';
    render();
  });
  $('notify-mode').addEventListener('change', () => {
    notifyMode = $('notify-mode').value;
    try { localStorage.setItem(NOTIFY_KEY, JSON.stringify({ mode: notifyMode })); } catch { /* session only */ }
    announcedUnread = -1; announceUnread();
  });
  $('srv-create').addEventListener('click', () => {
    const problem = createServer($('srv-origin').value, $('srv-name').value);
    renderServersPage(problem);
    if (!problem) { $('srv-name').value = ''; openPage('server'); }
  });
  $('srv-join').addEventListener('click', () => {
    const problem = joinServer($('srv-invite').value);
    renderServersPage(problem);
    if (!problem) { $('srv-invite').value = ''; openPage('server'); }
  });
  $('server-composer').addEventListener('submit', event => {
    event.preventDefault();
    const input = $('server-input'), text = input.value;
    if (!text.trim()) return;
    input.value = ''; input.focus(); $('mention-list').hidden = true;
    sendMessage(text);
  });
  $('server-input').addEventListener('keydown', event => {
    if (event.key === 'Escape') { cancelCompose(); $('mention-list').hidden = true; return; }
    if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) { event.preventDefault(); $('server-composer').requestSubmit(); }
  });
  $('server-input').addEventListener('input', () => { suggestMentions(); announceTyping(); });
  $('compose-cancel').addEventListener('click', cancelCompose);
  $('server-emoji').addEventListener('click', event => { event.stopPropagation(); emojiPanel($('server-emoji'), insertAtCaret); });
  $('server-attach').addEventListener('click', () => $('server-file').click());
  $('server-file').addEventListener('change', async () => {
    const file = $('server-file').files[0]; $('server-file').value = '';
    if (!file) return;
    const text = $('server-input').value; $('server-input').value = '';
    await sendMessage(text, file);
  });
  document.addEventListener('click', event => { if (!event.target.closest('#emoji-panel, #server-emoji, .message-tools')) $('emoji-panel').hidden = true; });
  // Actions on a message (reply, edit, delete, react, load a picture, jump to a quoted message).
  $('server-messages').addEventListener('click', event => {
    const button = event.target.closest('[data-action]'), server = servers[selected], row = event.target.closest('[data-uid]');
    if (!button || !server || !row) return;
    const seq = Number(row.dataset.uid), action = button.dataset.action;
    if (action === 'reply') startReply(server, seq);
    else if (action === 'edit') startEdit(server, seq);
    else if (action === 'delete') { if (button.dataset.confirm === 'yes') remove(server, seq); else { button.dataset.confirm = 'yes'; button.textContent = '❓'; button.title = 'Cliquer encore pour supprimer'; } }
    else if (action === 'react' && button.dataset.emoji) react(server, seq, button.dataset.emoji);
    else if (action === 'react') { event.stopPropagation(); emojiPanel(button, emoji => react(server, seq, emoji), QUICK.concat(EMOJI)); }
    else if (action === 'file') showFile(server, button.dataset.blob);
    else if (action === 'jump') jumpTo(Number(button.dataset.target));
  });
  $('search-form').addEventListener('submit', event => { event.preventDefault(); searchServer($('search-input').value); });
  $('search-results').addEventListener('click', event => { const hit = event.target.closest('[data-seq]'); if (hit) jumpTo(Number(hit.dataset.seq)); });
  $('back-to-present').addEventListener('click', () => { const server = servers[selected]; if (!server) return; view.reset(cacheFor(server, channelId).items.slice()); $('back-to-present').hidden = true; });
  $('server-menu').addEventListener('click', openSettings);
  $('ss-close').addEventListener('click', closeSettings);
  $('server-settings').addEventListener('click', event => { if (event.target === $('server-settings')) closeSettings(); });
  document.querySelector('.ss-nav').addEventListener('click', event => { const button = event.target.closest('[data-ss]'); if (button) showSettingsPanel(button.dataset.ss); });
  document.addEventListener('keydown', event => { if (event.key === 'Escape' && !$('server-settings').hidden) { event.stopPropagation(); closeSettings(); } });
  $('channel-create').addEventListener('click', () => {
    const input = $('channel-name');
    if (input.value.trim()) { createChannel(input.value, $('channel-type').value); input.value = ''; }
  });
  $('invite-make').addEventListener('click', () => { const server = servers[selected]; if (server) showInvite(server); });
  $('invite-copy').addEventListener('click', () => { navigator.clipboard?.writeText($('invite-code').value).then(() => { $('invite-copy').textContent = 'Copié ✓'; }, () => { $('invite-code').select(); }); });
  $('server-remove').addEventListener('click', () => {
    const button = $('server-remove');
    if (button.dataset.confirm !== 'yes') { button.dataset.confirm = 'yes'; button.textContent = 'Confirmer ?'; return; }
    button.dataset.confirm = ''; button.textContent = 'Retirer de la liste';
    closeSettings();
    removeServer(selected);
  });
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState !== 'visible') return;
    for (const server of servers) {
      if (server.status === 'ready') resync(server);
      else if (server.status === 'closed' || server.status === 'error') connect(server);
    }
  });
  // Looking at the window again counts as reading the channel on screen.
  window.addEventListener('focus', () => { const server = servers[selected]; if (server && channelId) { server.unread.delete(channelId); server.mentions.delete(channelId); renderSide(); announceUnread(); } });

  window.murmurServers = {
    onState(data) {
      const first = !identityPublic && data.identityPublic;
      identityPublic = data.identityPublic || ''; identityEnc = data.identityEnc || '';
      if (first) servers.forEach(connect);
    },
    handle(data) {
      if (data.type === 'signature' || data.type === 'ecdh') {
        const done = waiting.get(data.id);
        if (done) { waiting.delete(data.id); done(data.type === 'ecdh' ? data.secret : data.signature); }
        return;
      }
      const server = servers[data.slot];
      if (!server) return;
      if (data.event === 'open') { server.status = 'authenticating'; render(); }
      else if (data.event === 'message') { try { onFrame(server, JSON.parse(data.data)); } catch { /* ignore malformed frames */ } }
      else if (data.event === 'close') {
        for (const done of server.refs.values()) done({ t: 'error', code: 'closed' });
        server.refs.clear();
        window.murmurVoice?.onClose(server);
        if (server.status !== 'error' && server.status !== 'absent') server.status = 'closed';
        keepAlive(); render();
        if (server.status === 'closed') scheduleReconnect(server);
      }
    },
    // What voice.js needs from this file.
    api: { setName: name => setMyName(name), createServer: (origin, name) => { const problem = createServer(origin, name); renderServersPage(problem); return problem; }, joinServer: text => { const problem = joinServer(text); renderServersPage(problem); return problem; }, servers: () => servers, request, frame, sign, nameOf, indexOf, render, setNotice, selected: () => servers[selected], me: () => myName, randomHex },
    debug: { servers, select, makeServer, cacheFor, render, forgetName() { myName = ''; render(); } },
  };
  load(); render();
})();
