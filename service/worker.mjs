// Only encrypted connection descriptions transit here. No chat, audio or TURN.
const MAX_CLIENTS = 32, MAX_ROOMS = 16, MAX_PACKET = 6097;
const reply = (text, status) => new Response(text, { status, headers: { 'Cache-Control': 'no-store' } });
export function subscriptions(text) {
  if (!text.startsWith('JOIN|') || text.length > 1100) return null;
  const rows = text.slice(5).split('|').filter(Boolean);
  if (!rows.length || rows.length > MAX_ROOMS) return null;
  const rooms = [];
  for (const row of rows) {
    if (!/^[a-f0-9]{64}[01]$/.test(row) || rooms.some(r => r.id === row.slice(0, 64))) return null;
    rooms.push({ id: row.slice(0, 64), role: Number(row[64]) });
  }
  return rooms;
}
function route(bytes) { return Array.from(bytes.slice(0,32), b => b.toString(16).padStart(2,'0')).join(''); }
function presence(id, kind) {
  const bytes = new Uint8Array(33);
  for (let i=0;i<32;i++) bytes[i] = parseInt(id.slice(i*2,i*2+2),16);
  bytes[32] = kind; return bytes;
}
function authorized(request, secret) {
  if (!secret || !/^[a-f0-9]{64}$/.test(secret)) return false;
  const supplied=request.headers.get('Authorization') || '', expected=`Bearer ${secret}`;
  if (supplied.length !== expected.length) return false;
  let mismatch=0;
  for (let i=0;i<expected.length;i++) mismatch |= supplied.charCodeAt(i)^expected.charCodeAt(i);
  return mismatch===0;
}
export default {
  async fetch(request, env) {
    const url=new URL(request.url);
    if (url.pathname==='/health' && request.method==='GET') return reply('Direct rendezvous v1; TURN disabled',200);
    if (url.pathname!=='/v1/connect') return reply('Not found',404);
    if (!authorized(request,env.GROUP_ACCESS)) return reply('Unauthorized',401);
    if (request.method!=='GET' || request.headers.get('Upgrade')?.toLowerCase()!=='websocket') return reply('WebSocket required',426);
    return env.DIRECTORY.get(env.DIRECTORY.idFromName('private-group-v1')).fetch(request);
  }
};
export class Directory {
  constructor(ctx) { this.ctx=ctx; }
  fetch() {
    if (this.ctx.getWebSockets().length>=MAX_CLIENTS) return reply('Group full',503);
    const pair=new WebSocketPair(), client=pair[0], server=pair[1];
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({rooms:[],tokens:16,at:Date.now()});
    return new Response(null,{status:101,webSocket:client});
  }
  peers(socket,room) {
    return this.ctx.getWebSockets().filter(s=>s!==socket && s.readyState===1 && s.deserializeAttachment()?.rooms.some(r=>r.id===room.id && r.role!==room.role));
  }
  offline(socket,rooms) {
    for (const room of rooms) {
      const replaced=this.ctx.getWebSockets().some(s=>s!==socket && s.readyState===1 && s.deserializeAttachment()?.rooms.some(r=>r.id===room.id && r.role===room.role));
      if (!replaced) for (const peer of this.peers(socket,room)) peer.send(presence(room.id,2));
    }
  }
  webSocketMessage(socket,message) {
    const state=socket.deserializeAttachment();
    if (!state) { socket.close(1008,'Missing state'); return; }
    const now=Date.now(); state.tokens=Math.min(16,state.tokens+(now-state.at)/1000*2); state.at=now;
    if (state.tokens<1) { socket.close(1008,'Rate limit'); return; }
    state.tokens-=1;
    if (typeof message==='string') {
      const rooms=subscriptions(message);
      if (!rooms) { socket.close(1008,'Invalid subscriptions'); return; }
      this.offline(socket,state.rooms);
      state.rooms=rooms; socket.serializeAttachment(state);
      for (const room of rooms) {
        for (const other of this.ctx.getWebSockets()) {
          if (other!==socket && other.deserializeAttachment()?.rooms.some(r=>r.id===room.id && r.role===room.role)) other.close(1008,'Duplicate client');
        }
        for (const peer of this.peers(socket,room)) { socket.send(presence(room.id,0)); peer.send(presence(room.id,0)); }
      }
      return;
    }
    const bytes=new Uint8Array(message);
    if (bytes.length<=98 || bytes.length>MAX_PACKET || bytes[32]!==1) { socket.close(1008,'Invalid frame'); return; }
    const room=state.rooms.find(r=>r.id===route(bytes));
    if (!room) { socket.close(1008,'Unknown room'); return; }
    socket.serializeAttachment(state);
    for (const peer of this.peers(socket,room)) peer.send(bytes);
  }
  webSocketClose(socket) { this.offline(socket,socket.deserializeAttachment()?.rooms || []); socket.close(); }
  webSocketError(socket) { this.offline(socket,socket.deserializeAttachment()?.rooms || []); socket.close(1011,'Connection error'); }
}
