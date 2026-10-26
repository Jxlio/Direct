// Test harness. LocalHub runs the protocol core in-process, mirroring src/index.mjs;
// RemoteHub talks to a deployed or `wrangler dev` service over WebSocket (MURMUR_SERVICE_URL).
import { Guild, MemoryStore } from '../src/guild.mjs';
import { authMessage, bytesToHex, encMessage, envelopeMessage, nameMessage, randomHex } from '../src/protocol.mjs';

export class Identity {
  static async create() {
    const pair = await crypto.subtle.generateKey({ name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign', 'verify']);
    const identity = new Identity();
    identity.key = pair.privateKey;
    identity.pub = bytesToHex(new Uint8Array(await crypto.subtle.exportKey('raw', pair.publicKey)));
    // Separate encryption key pair (ECDH), like the app derives from the identity.
    const encPair = await crypto.subtle.generateKey({ name: 'ECDH', namedCurve: 'P-256' }, true, ['deriveBits']);
    identity.encKey = encPair.privateKey;
    identity.name = `Membre-${randomHex(2)}`;
    identity.enc = bytesToHex(new Uint8Array(await crypto.subtle.exportKey('raw', encPair.publicKey)));
    return identity;
  }
  // Fields that guild.create and guild.join must carry: the encryption key, signed by the identity.
  async encFields(guild, name = this.name) {
    return { enc: this.enc, encsig: await this.sign(encMessage(guild, this.enc)), name, namesig: await this.sign(nameMessage(guild, name)) };
  }
  async nameFields(guild, name) { return { name, namesig: await this.sign(nameMessage(guild, name)) }; }
  async sign(message) {
    return bytesToHex(new Uint8Array(await crypto.subtle.sign({ name: 'ECDSA', hash: 'SHA-256' }, this.key, message)));
  }
}

export class Client {
  constructor(transport) { this.transport = transport; this.queue = []; this.waiters = []; this.closed = false; }
  push(message) {
    const index = this.waiters.findIndex((w) => w.match(message));
    if (index >= 0) { const [w] = this.waiters.splice(index, 1); w.resolve(message); } else this.queue.push(message);
  }
  send(object) { return this.transport.send(typeof object === 'string' ? object : JSON.stringify(object)); }
  close() { return this.transport.close?.(); }
  wait(type, timeout = 2000) {
    const match = (m) => m.t === type;
    const index = this.queue.findIndex(match);
    if (index >= 0) return Promise.resolve(this.queue.splice(index, 1)[0]);
    return new Promise((resolve, reject) => {
      const waiter = { match, resolve };
      this.waiters.push(waiter);
      setTimeout(() => { const i = this.waiters.indexOf(waiter); if (i >= 0) { this.waiters.splice(i, 1); reject(new Error(`timeout waiting for ${type}`)); } }, timeout);
    });
  }
  // Sends a request and returns the first reply, whatever its type.
  async ask(object) {
    const ref = randomHex(4);
    const reply = new Promise((resolve) => this.waiters.push({ match: (m) => m.ref === ref, resolve }));
    await this.send({ ...object, ref });
    return Promise.race([reply, new Promise((_, reject) => setTimeout(() => reject(new Error(`no reply to ${object.t}`)), 2000))]);
  }
  async quiet(ms = 100) { await new Promise((r) => setTimeout(r, ms)); return this.queue.splice(0); }
  async login(identity, guild) {
    const hello = await this.wait('hello');
    this.hello = hello;
    await this.send({ t: 'auth', pub: identity.pub, sig: await identity.sign(authMessage(guild, hello.nonce)) });
    return this.wait('ready');
  }
}

export async function envelope(identity, guild, channel, id, body) {
  return { t: 'envelope.send', channel, id, body, sig: await identity.sign(envelopeMessage(guild, channel, id, body)) };
}

export class LocalHub {
  constructor() { this.clock = 1_700_000_000_000; this.stores = new Map(); this.connections = new Map(); this.local = true; }
  now = () => this.clock;
  advance(ms) { this.clock += ms; }
  core(guild) {
    if (!this.stores.has(guild)) this.stores.set(guild, new MemoryStore());
    return new Guild(guild, this.stores.get(guild), this.now, { turn: this.turn });
  }
  connect(guild) {
    const core = this.core(guild);
    const conn = core.newConnection();
    const list = this.connections.get(guild) ?? this.connections.set(guild, []).get(guild);
    const hub = this;
    const client = new Client({
      async send(text) {
        const result = await core.handle(conn, text);
        hub.deliver(list, client, result);
      },
      close: () => hub.drop(client),
    });
    list.push({ conn, client });
    client.core = core; client.conn = conn; client.guild = guild;
    client.push(JSON.parse(JSON.stringify(core.hello(conn))));
    return client;
  }
  // Mirrors GuildObject.deliver in src/index.mjs.
  deliver(list, client, result) {
    const copy = (item) => JSON.parse(JSON.stringify(item));
    for (const item of result.replies) client.push(copy(item));
    for (const item of result.broadcasts || []) for (const peer of list) if (peer.conn.member) peer.client.push(copy(item));
    for (const { fp, channel, message } of result.direct || []) for (const peer of list) if (peer.conn.fp === fp && peer.conn.voice === channel) peer.client.push(copy(message));
    if (result.close) client.closed = true;
    for (const fp of result.kick || []) for (const peer of list) if (peer.conn.fp === fp) { peer.conn.member = false; peer.client.closed = true; }
  }
  // A socket going away: presence in a voice channel ends.
  async drop(client) {
    const list = this.connections.get(client.guild);
    const result = await client.core.disconnect(client.conn);
    this.deliver(list.filter((peer) => peer.client !== client), client, { ...result, replies: [] });
    client.closed = true;
    list.splice(list.findIndex((peer) => peer.client === client), 1);
  }
  close() {}
}

export class RemoteHub {
  constructor(base) { this.base = base.replace(/\/$/, '').replace(/^http/, 'ws'); this.sockets = []; this.local = false; }
  async connect(guild) {
    const { default: WebSocket } = await import('ws');
    const socket = new WebSocket(`${this.base}/v2/guild/${guild}/connect?protocol=2`);
    this.sockets.push(socket);
    const client = new Client({ send: (text) => socket.send(text), close: () => socket.close() });
    socket.on('message', (data) => client.push(JSON.parse(data.toString())));
    socket.on('close', () => { client.closed = true; });
    await new Promise((resolve, reject) => { socket.once('open', resolve); socket.once('error', reject); });
    return client;
  }
  close() { for (const s of this.sockets) s.terminate(); }
}
export const newGuildId = () => randomHex(16);
