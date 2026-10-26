// Cloudflare hosting for the Murmur service: one Durable Object per guild.
// Only the protocol core in guild.mjs decides anything; this file moves frames.
import { Guild } from './guild.mjs';

const MAX_SOCKETS = 200;
const reply = (text, status) => new Response(text, { status, headers: { 'Cache-Control': 'no-store' } });

// Short-lived relay credentials from Cloudflare's TURN service, when a key is configured:
//   wrangler secret put TURN_KEY_ID ; wrangler secret put TURN_KEY_API_TOKEN
// Without them the service answers "no relay" and clients use direct connections only.
export async function mintTurn(env, fetcher = fetch) {
  if (!env?.TURN_KEY_ID || !env?.TURN_KEY_API_TOKEN) return null;
  const response = await fetcher(`https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(env.TURN_KEY_ID)}/credentials/generate-ice-servers`, {
    method: 'POST',
    headers: { Authorization: `Bearer ${env.TURN_KEY_API_TOKEN}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({ ttl: 6 * 3600 }),
  });
  if (!response.ok) return null;
  const data = await response.json();
  const list = Array.isArray(data.iceServers) ? data.iceServers : data.iceServers ? [data.iceServers] : [];
  const servers = list.filter((s) => s && (typeof s.urls === 'string' || Array.isArray(s.urls)))
    .map((s) => ({ urls: s.urls, ...(s.username ? { username: s.username, credential: s.credential } : {}) }));
  return servers.length ? servers : null;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/health' && request.method === 'GET') return reply('Murmur service v2', 200);
    const match = /^\/v2\/guild\/([0-9a-f]{32})\/connect$/.exec(url.pathname);
    if (!match) return reply('Not found', 404);
    if (request.method !== 'GET' || request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') return reply('WebSocket required', 426);
    if (url.searchParams.get('protocol') !== '2') return reply('Unsupported protocol', 400);
    return env.GUILDS.get(env.GUILDS.idFromName(match[1])).fetch(request);
  },
};

export class GuildObject {
  constructor(ctx, env) { this.ctx = ctx; this.env = env; }
  core(id) { return new Guild(id, this.ctx.storage, () => Date.now(), { turn: () => mintTurn(this.env) }); }
  fetch(request) {
    if (this.ctx.getWebSockets().length >= MAX_SOCKETS) return reply('Guild full', 503);
    const guild = /\/v2\/guild\/([0-9a-f]{32})\//.exec(new URL(request.url).pathname)[1];
    const pair = new WebSocketPair();
    const [client, server] = [pair[0], pair[1]];
    this.ctx.acceptWebSocket(server);
    const core = this.core(guild), conn = core.newConnection();
    conn.guild = guild;
    server.serializeAttachment(conn);
    server.send(JSON.stringify(core.hello(conn)));
    return new Response(null, { status: 101, webSocket: client });
  }
  deliver(result) {
    for (const item of result.broadcasts || []) {
      const text = JSON.stringify(item);
      for (const peer of this.ctx.getWebSockets()) if (peer.readyState === 1 && peer.deserializeAttachment()?.member) peer.send(text);
    }
    // Private deliveries (call signalling) go only to the addressee's connection in that voice channel.
    for (const { fp, channel, message } of result.direct || []) {
      const text = JSON.stringify(message);
      for (const peer of this.ctx.getWebSockets()) {
        const state = peer.readyState === 1 && peer.deserializeAttachment();
        if (state && state.fp === fp && state.voice === channel) peer.send(text);
      }
    }
    // Removed members lose their connections once they have been told.
    for (const fp of result.kick || []) for (const peer of this.ctx.getWebSockets()) if (peer.deserializeAttachment()?.fp === fp) peer.close(1008, 'Removed');
  }
  async webSocketMessage(socket, message) {
    const conn = socket.deserializeAttachment();
    if (!conn) { socket.close(1008, 'Missing state'); return; }
    const result = await this.core(conn.guild).handle(conn, message);
    socket.serializeAttachment(conn);
    for (const item of result.replies) socket.send(JSON.stringify(item));
    this.deliver(result);
    if (result.close) socket.close(1008, 'Protocol violation');
  }
  // Presence in a voice channel must not outlive the connection.
  async goodbye(socket) {
    const conn = socket.deserializeAttachment();
    if (!conn?.voice) return;
    try { this.deliver(await this.core(conn.guild).disconnect(conn)); } catch { /* the entry expires by itself */ }
  }
  async webSocketClose(socket) { await this.goodbye(socket); socket.close(); }
  async webSocketError(socket) { await this.goodbye(socket); socket.close(1011, 'Connection error'); }
}
