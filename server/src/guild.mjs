// Pure protocol logic for one guild (server). It knows nothing about sockets or the
// hosting platform: a store with get/put/list (the Durable Object storage shape) and a
// clock are injected, so the same code runs in Cloudflare and in the contract tests.
import {
  LIMITS, PROTOCOL, authMessage, encMessage, envelopeMessage, fingerprint, hexToBytes, isBase64, isChannelId, isDisplayName, isId, isName,
  nameMessage, ownerMessage, randomHex, roleMessage, verify, voiceMessage,
} from './protocol.mjs';

const seqKey = (seq) => String(seq).padStart(12, '0');
const keyKey = (fp, epoch) => `k:${fp}:${String(epoch).padStart(10, '0')}`;
const messageKey = (channel, seq) => `m:${channel}:${seqKey(seq)}`;
const blobKey = (id, index) => `b:${id}:${String(index).padStart(3, '0')}`;
const voiceKey = (channel, fp) => `v:${channel}:${fp}`;
const isFp = (value) => typeof value === 'string' && /^[0-9a-f]{64}$/.test(value);
const staff = (user) => user && (user.role === 'owner' || user.role === 'moderator');
const fail = (code, ref, extra = {}) => ({ replies: [{ t: 'error', code, ref, ...extra }], broadcasts: [] });
const ok = (replies = [], broadcasts = [], more = {}) => ({ replies, broadcasts, ...more });
// What a request costs against the connection's allowance: typing is cheap noise, file chunks are
// many and small, everything else counts as one.
const COST = Object.assign(Object.create(null), { typing: 0.5, 'blob.put': 0.25, 'blob.get': 0.25, 'voice.signal': 0.5 });

export class Guild {
  // options.turn: async () => iceServers | null, supplied by the host (it holds the TURN secrets).
  constructor(id, store, now = () => Date.now(), options = {}) {
    this.id = id;
    this.store = store;
    this.now = now;
    this.turn = options.turn;
  }
  newConnection() {
    return { nonce: randomHex(32), fp: null, pub: null, authed: false, member: false, voice: null, tokens: LIMITS.burst, at: this.now() };
  }
  hello(conn) {
    return { t: 'hello', protocol: PROTOCOL, nonce: conn.nonce, limits: LIMITS };
  }
  async channels() {
    const list = await this.store.list({ prefix: 'c:' });
    return [...list.values()].sort((a, b) => a.created - b.created);
  }
  async summary(conn) {
    const guild = await this.store.get('g');
    if (!guild) return { t: 'ready', exists: false, member: false, you: conn.fp };
    const user = await this.store.get(`u:${conn.fp}`);
    conn.member = !!user;
    if (!user) return { t: 'ready', exists: true, member: false, you: conn.fp };
    const channels = await this.channels(), presence = {};
    for (const channel of channels) if (channel.type === 'voice') presence[channel.id] = await this.voiceMembers(channel.id);
    return { t: 'ready', exists: true, member: true, you: conn.fp, role: user.role, name: guild.name, owner: guild.owner,
      channels, last: (await this.store.get('n')) || 0, epoch: (await this.store.get('ep')) || 1, presence };
  }
  throttled(conn, cost = 1) {
    const now = this.now();
    if (!Number.isFinite(conn.tokens)) conn.tokens = 0;
    conn.tokens = Math.min(LIMITS.burst, conn.tokens + ((now - conn.at) / 1000) * LIMITS.refillPerSecond);
    conn.at = now;
    if (conn.tokens < cost) return true;
    conn.tokens -= cost;
    return false;
  }
  // Handles one client frame. Returns {replies, broadcasts, direct?, kick?, close?}; never throws on bad input.
  async handle(conn, raw) {
    if (typeof raw !== 'string' || raw.length > LIMITS.blobFrameBytes) return { ...fail('too_large'), close: true };
    let msg;
    try { msg = JSON.parse(raw); } catch { return raw.length > LIMITS.frameBytes ? { ...fail('too_large'), close: true } : fail('bad_request'); }
    // Only file chunks may use the large frames; everything else keeps the small limit.
    if (raw.length > LIMITS.frameBytes && msg?.t !== 'blob.put') return { ...fail('too_large', typeof msg?.ref === 'string' ? msg.ref.slice(0, 40) : undefined), close: true };
    if (!msg || typeof msg !== 'object' || typeof msg.t !== 'string') return fail('bad_request');
    const ref = typeof msg.ref === 'string' ? msg.ref.slice(0, 40) : undefined;
    if (this.throttled(conn, Object.hasOwn(COST, msg.t) ? COST[msg.t] : 1)) return fail('rate_limited', ref, { retry_after: 1 });
    const result = await this.dispatch(conn, msg, ref);
    if (ref) for (const item of result.replies) if (item.ref === undefined) item.ref = ref;
    return result;
  }
  async dispatch(conn, msg, ref) {
    if (msg.t === 'ping') return ok([{ t: 'pong' }]);
    if (msg.t === 'auth') return this.auth(conn, msg, ref);
    if (!conn.authed) return fail('unauthorized', ref);
    switch (msg.t) {
      case 'guild.create': return this.create(conn, msg, ref);
      case 'guild.join': return this.join(conn, msg, ref);
      case 'invite.create': return this.invite(conn, msg, ref);
      case 'channel.create': return this.channelCreate(conn, msg, ref);
      case 'members.list': return this.members(conn, ref);
      case 'keys.put': return this.keyPut(conn, msg, ref);
      case 'keys.get': return this.keyGet(conn, ref);
      case 'profile.set': return this.profileSet(conn, msg, ref);
      case 'member.remove': return this.remove(conn, msg, ref);
      case 'member.unban': return this.unban(conn, msg, ref);
      case 'role.set': return this.roleSet(conn, msg, ref);
      case 'owner.transfer': return this.transfer(conn, msg, ref);
      case 'envelope.send': return this.send(conn, msg, ref);
      case 'history.fetch': return this.history(conn, msg, ref);
      case 'typing': return this.typing(conn, msg, ref);
      case 'blob.put': return this.blobPut(conn, msg, ref);
      case 'blob.get': return this.blobGet(conn, msg, ref);
      case 'voice.join': return this.voiceJoin(conn, msg, ref);
      case 'voice.leave': return this.voiceLeave(conn, ref);
      case 'voice.signal': return this.voiceSignal(conn, msg, ref);
      case 'turn.get': return this.turnGet(conn, ref);
      default: return fail('bad_request', ref);
    }
  }
  async auth(conn, msg, ref) {
    if (conn.authed) return fail('conflict', ref);
    const pub = hexToBytes(msg.pub, 65);
    const sig = hexToBytes(msg.sig, 64);
    if (!pub || !sig) return fail('bad_request', ref);
    if (!(await verify(pub, authMessage(this.id, conn.nonce), sig))) return { ...fail('unauthorized', ref), close: true };
    conn.pub = msg.pub;
    conn.fp = await fingerprint(pub);
    conn.authed = true;
    return ok([await this.summary(conn)]);
  }
  // The encryption key and the display name are published signed by the identity, so nobody can
  // swap them in transit and the service cannot rename a member.
  async displayName(conn, msg) {
    const sig = hexToBytes(msg.namesig, 64);
    if (!isDisplayName(msg.name) || !sig) return null;
    return (await verify(hexToBytes(conn.pub, 65), nameMessage(this.id, msg.name), sig)) ? { name: msg.name, namesig: msg.namesig } : null;
  }
  async encryptionKey(conn, msg) {
    const enc = hexToBytes(msg.enc, 65), sig = hexToBytes(msg.encsig, 64);
    if (!enc || enc[0] !== 4 || !sig) return null;
    const valid = await verify(hexToBytes(conn.pub, 65), encMessage(this.id, msg.enc), sig);
    const named = valid && await this.displayName(conn, msg);
    return named ? { enc: msg.enc, encsig: msg.encsig, ...named } : null;
  }
  async create(conn, msg, ref) {
    // `name` is the founder's display name (signed with the key); the server's own name travels as `title`.
    const title = msg.title ?? msg.name;
    if (!isName(title)) return fail('bad_request', ref);
    if (await this.store.get('g')) return fail('conflict', ref);
    const keys = await this.encryptionKey(conn, msg);
    if (!keys) return fail('bad_request', ref, { reason: 'encryption_key' });
    const now = this.now();
    await this.store.put('g', { name: title.trim(), owner: conn.fp, created: now });
    await this.store.put(`u:${conn.fp}`, { pub: conn.pub, ...keys, role: 'owner', joined: now });
    await this.store.put('uc', 1);
    await this.store.put('cc', 1);
    await this.store.put('ep', 1);
    await this.store.put('c:general', { id: 'general', name: 'general', type: 'text', created: now });
    return ok([await this.summary(conn)]);
  }
  async join(conn, msg, ref) {
    if (!(await this.store.get('g'))) return fail('not_found', ref);
    if (await this.store.get(`u:${conn.fp}`)) return fail('conflict', ref);
    if (await this.store.get(`x:${conn.fp}`)) return fail('forbidden', ref, { reason: 'removed' });
    if (typeof msg.invite !== 'string' || !/^[0-9a-f]{32}$/.test(msg.invite)) return fail('bad_request', ref);
    const keys = await this.encryptionKey(conn, msg);
    if (!keys) return fail('bad_request', ref, { reason: 'encryption_key' });
    const invite = await this.store.get(`i:${msg.invite}`);
    const now = this.now();
    if (!invite || invite.exp < now || invite.uses >= invite.max) return fail('forbidden', ref);
    const count = (await this.store.get('uc')) || 0;
    if (count >= LIMITS.members) return fail('forbidden', ref, { reason: 'full' });
    invite.uses += 1;
    await this.store.put(`i:${msg.invite}`, invite);
    await this.store.put(`u:${conn.fp}`, { pub: conn.pub, ...keys, role: 'member', joined: now });
    await this.store.put('uc', count + 1);
    const summary = await this.summary(conn);
    return ok([summary], [{ t: 'member', fp: conn.fp, pub: conn.pub, ...keys, role: 'member', joined: now }]);
  }
  async user(conn) {
    const user = await this.store.get(`u:${conn.fp}`);
    conn.member = !!user;
    return user;
  }
  async invite(conn, msg, ref) {
    const user = await this.user(conn);
    if (!staff(user)) return fail('forbidden', ref);
    const max = Number.isInteger(msg.max_uses) ? msg.max_uses : 1;
    const ttl = Number.isInteger(msg.ttl_s) ? msg.ttl_s : 24 * 3600;
    if (max < 1 || max > LIMITS.inviteUses || ttl < 60 || ttl > LIMITS.inviteTtlSeconds) return fail('bad_request', ref);
    const code = randomHex(16);
    await this.store.put(`i:${code}`, { uses: 0, max, exp: this.now() + ttl * 1000, by: conn.fp });
    return ok([{ t: 'invite', ref, code, expires: this.now() + ttl * 1000 }]);
  }
  async members(conn, ref) {
    if (!(await this.user(conn))) return fail('forbidden', ref);
    const users = await this.store.list({ prefix: 'u:', limit: LIMITS.members });
    const epoch = (await this.store.get('ep')) || 1;
    const items = [];
    for (const [key, user] of users) {
      const fp = key.slice(2);
      items.push({ fp, pub: user.pub, enc: user.enc, encsig: user.encsig, name: user.name, namesig: user.namesig,
        role: user.role, rolesig: user.rolesig, roleserial: user.roleserial, joined: user.joined, keyed: !!(await this.store.get(keyKey(fp, epoch))) });
    }
    const former = [...(await this.store.list({ prefix: 'x:', limit: LIMITS.members })).entries()]
      .map(([key, gone]) => ({ fp: key.slice(2), pub: gone.pub, name: gone.name, namesig: gone.namesig, at: gone.at }));
    const chain = [...(await this.store.list({ prefix: 'oc:', limit: 64 })).values()];
    return ok([{ t: 'members', ref, epoch, items, former, chain }]);
  }
  // Staff hand each group key to each member, wrapped for that member's encryption key. The
  // service stores the blobs without being able to open them. Staff may also stage the next
  // epoch's key for themselves, so a crash between removing a member and handing out the new
  // key loses nothing.
  async keyPut(conn, msg, ref) {
    const user = await this.user(conn);
    if (!staff(user)) return fail('forbidden', ref);
    if (!isFp(msg.to) || !(await this.store.get(`u:${msg.to}`))) return fail('not_found', ref);
    const current = (await this.store.get('ep')) || 1;
    if (!Number.isInteger(msg.epoch) || msg.epoch < 1 || msg.epoch > current + 1 || !isBase64(msg.blob, LIMITS.keyBlobBase64)) return fail('bad_request', ref);
    if (msg.epoch === current + 1 && msg.to !== conn.fp) return fail('bad_request', ref, { reason: 'epoch_not_open' });
    await this.store.put(keyKey(msg.to, msg.epoch), { epoch: msg.epoch, blob: msg.blob, from: conn.fp });
    return ok([{ t: 'ack', ref }], msg.epoch <= current ? [{ t: 'keys', to: msg.to, epoch: msg.epoch }] : []);
  }
  async keyGet(conn, ref) {
    if (!(await this.user(conn))) return fail('forbidden', ref);
    const list = await this.store.list({ prefix: `k:${conn.fp}:`, reverse: true, limit: LIMITS.epochs });
    return ok([{ t: 'key', ref, items: [...list.values()].reverse() }]);
  }
  // Changes the display name. The name is signed, so the service cannot rename anyone.
  async profileSet(conn, msg, ref) {
    const user = await this.user(conn);
    if (!user) return fail('forbidden', ref);
    const named = await this.displayName(conn, msg);
    if (!named) return fail('bad_request', ref);
    await this.store.put(`u:${conn.fp}`, { ...user, ...named });
    return ok([{ t: 'ack', ref }], [{ t: 'profile', fp: conn.fp, ...named }]);
  }
  // Removes a member and opens the next key epoch in one step. The removed identity cannot read
  // the new keys, cannot rejoin, and its connections are closed. A moderator may remove plain
  // members; only the owner may remove a moderator.
  async remove(conn, msg, ref) {
    const user = await this.user(conn);
    if (!staff(user)) return fail('forbidden', ref);
    if (!isFp(msg.fp) || msg.fp === conn.fp) return fail('bad_request', ref);
    const target = await this.store.get(`u:${msg.fp}`);
    if (!target) return fail('not_found', ref);
    if (target.role === 'owner' || (user.role === 'moderator' && target.role !== 'member')) return fail('forbidden', ref);
    const current = (await this.store.get('ep')) || 1;
    if (msg.epoch !== current + 1) return fail('conflict', ref, { epoch: current });
    await this.store.delete(`u:${msg.fp}`);
    for (const key of (await this.store.list({ prefix: `k:${msg.fp}:` })).keys()) await this.store.delete(key);
    // Kept so that their earlier messages stay verifiable: the identity key and signed name remain public.
    await this.store.put(`x:${msg.fp}`, { by: conn.fp, at: this.now(), pub: target.pub, name: target.name, namesig: target.namesig });
    await this.store.put('uc', Math.max(0, ((await this.store.get('uc')) || 1) - 1));
    await this.store.put('ep', msg.epoch);
    const broadcasts = [{ t: 'removed', fp: msg.fp, epoch: msg.epoch }, ...(await this.dropVoice(msg.fp))];
    return ok([{ t: 'ack', ref, epoch: msg.epoch }], broadcasts, { kick: [msg.fp] });
  }
  // Lets a removed identity join again with a new invitation. Owner only.
  async unban(conn, msg, ref) {
    const user = await this.user(conn);
    if (!user || user.role !== 'owner') return fail('forbidden', ref);
    if (!isFp(msg.fp) || !(await this.store.get(`x:${msg.fp}`))) return fail('not_found', ref);
    await this.store.delete(`x:${msg.fp}`);
    return ok([{ t: 'ack', ref }]);
  }
  // Moderators are named by the owner with a signature, so the service cannot invent one.
  async roleSet(conn, msg, ref) {
    const user = await this.user(conn);
    if (!user || user.role !== 'owner') return fail('forbidden', ref);
    if (!isFp(msg.fp) || msg.fp === conn.fp || (msg.role !== 'moderator' && msg.role !== 'member') || !Number.isInteger(msg.serial)) return fail('bad_request', ref);
    const target = await this.store.get(`u:${msg.fp}`);
    if (!target) return fail('not_found', ref);
    if (msg.serial <= ((await this.store.get('rs')) || 0)) return fail('conflict', ref);
    const sig = hexToBytes(msg.sig, 64);
    if (!sig || !(await verify(hexToBytes(conn.pub, 65), roleMessage(this.id, msg.fp, msg.role, msg.serial), sig))) return fail('unauthorized', ref);
    await this.store.put(`u:${msg.fp}`, { ...target, role: msg.role, rolesig: msg.sig, roleserial: msg.serial });
    await this.store.put('rs', msg.serial);
    return ok([{ t: 'ack', ref }], [{ t: 'role', fp: msg.fp, role: msg.role, serial: msg.serial, sig: msg.sig }]);
  }
  // Hands the server to another member. The link is signed by the current owner, so every member
  // can follow the chain of owners from the one named in their invitation. Moderators are reset:
  // their signatures were made by the previous owner.
  async transfer(conn, msg, ref) {
    const user = await this.user(conn);
    if (!user || user.role !== 'owner') return fail('forbidden', ref);
    if (!isFp(msg.to) || msg.to === conn.fp || !Number.isInteger(msg.serial)) return fail('bad_request', ref);
    const target = await this.store.get(`u:${msg.to}`);
    if (!target) return fail('not_found', ref);
    const epoch = (await this.store.get('ep')) || 1;
    if (!(await this.store.get(keyKey(msg.to, epoch)))) return fail('conflict', ref, { reason: 'not_keyed' });   // the new owner must already hold the key
    if (msg.serial <= ((await this.store.get('rs')) || 0)) return fail('conflict', ref);
    const sig = hexToBytes(msg.sig, 64);
    if (!sig || !(await verify(hexToBytes(conn.pub, 65), ownerMessage(this.id, msg.to, msg.serial), sig))) return fail('unauthorized', ref);
    for (const [key, member] of await this.store.list({ prefix: 'u:', limit: LIMITS.members })) {
      if (member.role === 'moderator') await this.store.put(key, { ...member, role: 'member', rolesig: undefined, roleserial: undefined });
    }
    await this.store.put(`u:${conn.fp}`, { ...user, role: 'member' });
    await this.store.put(`u:${msg.to}`, { ...target, role: 'owner' });
    await this.store.put('g', { ...(await this.store.get('g')), owner: msg.to });
    await this.store.put(`oc:${seqKey(msg.serial)}`, { from: conn.fp, to: msg.to, serial: msg.serial, sig: msg.sig });
    await this.store.put('rs', msg.serial);
    return ok([{ t: 'ack', ref }], [{ t: 'owner', from: conn.fp, to: msg.to, serial: msg.serial, sig: msg.sig }]);
  }
  async channelCreate(conn, msg, ref) {
    const user = await this.user(conn);
    if (!staff(user)) return fail('forbidden', ref);
    if (!isName(msg.name) || (msg.type !== 'text' && msg.type !== 'voice')) return fail('bad_request', ref);
    const count = (await this.store.get('cc')) || 0;
    if (count >= LIMITS.channels) return fail('forbidden', ref, { reason: 'too_many_channels' });
    const channel = { id: randomHex(8), name: msg.name.trim(), type: msg.type, created: this.now() };
    await this.store.put(`c:${channel.id}`, channel);
    await this.store.put('cc', count + 1);
    return ok([{ t: 'channel', ref, channel }], [{ t: 'channel', channel }]);
  }
  async send(conn, msg, ref) {
    const user = await this.user(conn);
    if (!user) return fail('forbidden', ref);
    if (!isChannelId(msg.channel) || !isId(msg.id) || !isBase64(msg.body)) {
      return fail(typeof msg.body === 'string' && msg.body.length > Math.ceil(LIMITS.envelopeBytes / 3) * 4 ? 'too_large' : 'bad_request', ref);
    }
    const sig = hexToBytes(msg.sig, 64);
    if (!sig || !(await verify(hexToBytes(user.pub, 65), envelopeMessage(this.id, msg.channel, msg.id, msg.body), sig))) return fail('unauthorized', ref);
    const channel = await this.store.get(`c:${msg.channel}`);
    if (!channel || channel.type !== 'text') return fail('not_found', ref);
    const duplicate = `d:${conn.fp}:${msg.id}`;
    if (await this.store.get(duplicate)) return fail('conflict', ref);
    const seq = ((await this.store.get('n')) || 0) + 1;
    const envelope = { t: 'envelope', channel: msg.channel, seq, id: msg.id, from: conn.fp, ts: this.now(), body: msg.body, sig: msg.sig };
    await this.store.put('n', seq);
    await this.store.put(duplicate, seq);
    await this.store.put(messageKey(msg.channel, seq), envelope);
    return ok([{ t: 'ack', ref, seq, id: msg.id }], [envelope]);
  }
  async history(conn, msg, ref) {
    if (!(await this.user(conn))) return fail('forbidden', ref);
    if (!isChannelId(msg.channel) || !(await this.store.get(`c:${msg.channel}`))) return fail('not_found', ref);
    const limit = Number.isInteger(msg.limit) ? Math.min(Math.max(msg.limit, 1), LIMITS.historyPage) : LIMITS.historyPage;
    const prefix = `m:${msg.channel}:`;
    let items;
    if (Number.isInteger(msg.after)) {
      const list = await this.store.list({ prefix, start: messageKey(msg.channel, Math.max(msg.after, 0) + 1), limit });
      items = [...list.values()];
    } else {
      const options = { prefix, reverse: true, limit };
      if (Number.isInteger(msg.before)) options.end = messageKey(msg.channel, Math.max(msg.before, 0));
      items = [...(await this.store.list(options)).values()].reverse();
    }
    return ok([{ t: 'history', ref, channel: msg.channel, items, more: items.length === limit }]);
  }

  // ----- typing: not stored, not acknowledged -----
  async typing(conn, msg) {
    if (!(await this.user(conn)) || !isChannelId(msg.channel)) return ok();
    return ok([], [{ t: 'typing', channel: msg.channel, fp: conn.fp }]);
  }

  // ----- attachments: encrypted by the sender, stored as opaque chunks -----
  async blobPut(conn, msg, ref) {
    if (!(await this.user(conn))) return fail('forbidden', ref);
    if (typeof msg.id !== 'string' || !/^[0-9a-f]{32}$/.test(msg.id) || !Number.isInteger(msg.total) || msg.total < 1 || msg.total > LIMITS.blobChunks ||
        !Number.isInteger(msg.index) || msg.index < 0 || msg.index >= msg.total) return fail('bad_request', ref);
    if (!isBase64(msg.data, LIMITS.blobChunkBase64)) return fail(typeof msg.data === 'string' && msg.data.length > LIMITS.blobChunkBase64 ? 'too_large' : 'bad_request', ref);
    let meta = await this.store.get(`bm:${msg.id}`);
    if (meta && (meta.from !== conn.fp || meta.total !== msg.total)) return fail('conflict', ref);
    meta = meta || { from: conn.fp, total: msg.total, at: this.now(), sizes: {} };
    const previous = meta.sizes[msg.index] || 0;
    const used = ((await this.store.get('bq')) || 0) - previous + msg.data.length;
    if (used > LIMITS.blobBytes) return fail('forbidden', ref, { reason: 'quota' });
    meta.sizes[msg.index] = msg.data.length;
    await this.store.put(blobKey(msg.id, msg.index), msg.data);
    await this.store.put(`bm:${msg.id}`, meta);
    await this.store.put('bq', used);
    return ok([{ t: 'ack', ref, id: msg.id, index: msg.index }]);
  }
  async blobGet(conn, msg, ref) {
    if (!(await this.user(conn))) return fail('forbidden', ref);
    if (typeof msg.id !== 'string' || !/^[0-9a-f]{32}$/.test(msg.id) || !Number.isInteger(msg.index) || msg.index < 0) return fail('bad_request', ref);
    const meta = await this.store.get(`bm:${msg.id}`);
    const data = meta && msg.index < meta.total ? await this.store.get(blobKey(msg.id, msg.index)) : undefined;
    if (data === undefined) return fail('not_found', ref);
    return ok([{ t: 'blob', ref, id: msg.id, index: msg.index, total: meta.total, data }]);
  }

  // ----- voice channels: who is in, and the signalling between them (media never touches the service) -----
  async voiceMembers(channel) {
    const cutoff = this.now() - 6 * 3600 * 1000;      // a crashed host must not leave ghosts forever
    const list = await this.store.list({ prefix: `v:${channel}:` });
    const alive = [];
    for (const [key, entry] of list) { if (entry.at >= cutoff) alive.push(key.slice(`v:${channel}:`.length)); else await this.store.delete(key); }
    return alive;
  }
  async voiceJoin(conn, msg, ref) {
    if (!(await this.user(conn))) return fail('forbidden', ref);
    const channel = isChannelId(msg.channel) ? await this.store.get(`c:${msg.channel}`) : null;
    if (!channel || channel.type !== 'voice') return fail('not_found', ref);
    const broadcasts = conn.voice && conn.voice !== msg.channel ? await this.leaveVoice(conn) : [];
    const present = await this.voiceMembers(msg.channel);
    if (!present.includes(conn.fp) && present.length >= LIMITS.voiceParticipants) return fail('forbidden', ref, { reason: 'full' });
    await this.store.put(voiceKey(msg.channel, conn.fp), { at: this.now() });
    conn.voice = msg.channel;
    return ok([{ t: 'voice.state', ref, channel: msg.channel, participants: present.filter((fp) => fp !== conn.fp), limit: LIMITS.voiceParticipants }],
      [...broadcasts, { t: 'voice', channel: msg.channel, fp: conn.fp, joined: true }]);
  }
  async leaveVoice(conn) {
    const channel = conn.voice;
    conn.voice = null;
    if (!channel) return [];
    await this.store.delete(voiceKey(channel, conn.fp));
    return [{ t: 'voice', channel, fp: conn.fp, joined: false }];
  }
  async voiceLeave(conn, ref) {
    if (!(await this.user(conn))) return fail('forbidden', ref);
    return ok([{ t: 'ack', ref }], await this.leaveVoice(conn));
  }
  // Called by the host when a socket closes, so presence never outlives the connection.
  async disconnect(conn) {
    return ok([], conn.authed ? await this.leaveVoice(conn) : []);
  }
  async dropVoice(fp) {
    const out = [];
    for (const [key] of await this.store.list({ prefix: 'v:' })) {
      const [, channel, who] = key.split(':');
      if (who === fp) { await this.store.delete(key); out.push({ t: 'voice', channel, fp, joined: false }); }
    }
    return out;
  }
  async voiceSignal(conn, msg, ref) {
    const user = await this.user(conn);
    if (!user) return fail('forbidden', ref);
    if (!conn.voice || conn.voice !== msg.channel) return fail('forbidden', ref);
    if (!isFp(msg.to) || !isId(msg.id) || !isBase64(msg.body, LIMITS.voiceSignalBase64)) return fail('bad_request', ref);
    if (!(await this.voiceMembers(msg.channel)).includes(msg.to)) return fail('not_found', ref);
    const sig = hexToBytes(msg.sig, 64);
    if (!sig || !(await verify(hexToBytes(user.pub, 65), voiceMessage(this.id, msg.channel, msg.to, msg.id, msg.body), sig))) return fail('unauthorized', ref);
    return ok([{ t: 'ack', ref }], [], { direct: [{ fp: msg.to, channel: msg.channel, message: { t: 'voice.signal', channel: msg.channel, from: conn.fp, id: msg.id, body: msg.body, sig: msg.sig } }] });
  }
  // Short-lived relay credentials, minted by the host from its own TURN key; null when none is configured.
  async turnGet(conn, ref) {
    if (!(await this.user(conn))) return fail('forbidden', ref);
    let servers = null;
    if (this.turn) { try { servers = await this.turn(); } catch { servers = null; } }
    return ok([{ t: 'turn', ref, iceServers: servers }]);
  }
}

// Minimal in-memory store with the subset of the Durable Object storage API used above.
export class MemoryStore {
  constructor() { this.map = new Map(); }
  async get(key) { const v = this.map.get(key); return v === undefined ? undefined : structuredClone(v); }
  async put(key, value) { this.map.set(key, structuredClone(value)); }
  async delete(key) { return this.map.delete(key); }
  async list({ prefix = '', start, end, reverse = false, limit } = {}) {
    let keys = [...this.map.keys()].filter((k) => k.startsWith(prefix) && (start === undefined || k >= start) && (end === undefined || k < end)).sort();
    if (reverse) keys.reverse();
    if (limit !== undefined) keys = keys.slice(0, limit);
    return new Map(keys.map((k) => [k, structuredClone(this.map.get(k))]));
  }
}
