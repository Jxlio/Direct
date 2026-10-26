// The relay credential call, tested against a stand-in for Cloudflare's API (the real one needs an account).
import test from 'node:test';
import assert from 'node:assert/strict';
import { mintTurn } from '../src/index.mjs';

const env = { TURN_KEY_ID: 'key id', TURN_KEY_API_TOKEN: 'secret-token' };
const respond = (body, status = 200) => async (url, init) => { respond.last = { url, init }; return { ok: status < 300, status, json: async () => body }; };

test('nothing is configured: no relay, and no network call', async () => {
  let called = false;
  assert.equal(await mintTurn({}, async () => { called = true; }), null);
  assert.equal(await mintTurn({ TURN_KEY_ID: 'x' }, async () => { called = true; }), null);
  assert.equal(called, false);
});

test('the key is sent as a bearer token to the key-specific address, and the answer is trimmed to what a browser needs', async () => {
  const servers = await mintTurn(env, respond({ iceServers: [
    { urls: ['stun:stun.example:3478'] },
    { urls: ['turn:turn.example:3478?transport=udp', 'turns:turn.example:5349'], username: 'user', credential: 'pass', extra: 'ignored' },
  ] }));
  assert.deepEqual(servers, [{ urls: ['stun:stun.example:3478'] }, { urls: ['turn:turn.example:3478?transport=udp', 'turns:turn.example:5349'], username: 'user', credential: 'pass' }]);
  assert.equal(respond.last.url, 'https://rtc.live.cloudflare.com/v1/turn/keys/key%20id/credentials/generate-ice-servers');
  assert.equal(respond.last.init.headers.Authorization, 'Bearer secret-token');
  assert.ok(JSON.parse(respond.last.init.body).ttl <= 24 * 3600);
});

test('a single server object is accepted, anything else is refused', async () => {
  assert.deepEqual(await mintTurn(env, respond({ iceServers: { urls: 'turn:t.example', username: 'u', credential: 'c' } })), [{ urls: 'turn:t.example', username: 'u', credential: 'c' }]);
  assert.equal(await mintTurn(env, respond({ iceServers: [] })), null);
  assert.equal(await mintTurn(env, respond({})), null);
  assert.equal(await mintTurn(env, respond({ iceServers: [{ nothing: 1 }, null] })), null);
  assert.equal(await mintTurn(env, respond({ error: 'denied' }, 403)), null);
});
