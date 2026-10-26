// Contract tests for the features added after the first release of protocol v2: typing, attachments,
// moderators, ownership transfer, reinstatement, voice channels and relay credentials.
import test from 'node:test';
import assert from 'node:assert/strict';
import { Identity, LocalHub, RemoteHub, envelope, newGuildId } from './harness.mjs';
import { LIMITS, ownerMessage, roleMessage, voiceMessage, bytesToHex, randomHex } from '../src/protocol.mjs';

const remote = process.env.MURMUR_SERVICE_URL;
const hub = remote ? new RemoteHub(remote) : new LocalHub();
if (!remote) hub.turn = async () => [{ urls: ['turn:relay.example:3478'], username: 'u', credential: 'c' }];
test.after(() => hub.close());
const b64 = (text) => Buffer.from(text).toString('base64');
const sha = async (hex) => bytesToHex(new Uint8Array(await crypto.subtle.digest('SHA-256', Buffer.from(hex, 'hex'))));

async function guildWithOwner() {
  const guild = newGuildId(), owner = await Identity.create(), client = await hub.connect(guild);
  await client.login(owner, guild);
  assert.equal((await client.ask({ t: 'guild.create', name: 'Amis', ...(await owner.encFields(guild)) })).member, true);
  return { guild, owner, client, fp: await sha(owner.pub) };
}
async function member(guild, ownerClient) {
  const invite = await ownerClient.ask({ t: 'invite.create', max_uses: 9, ttl_s: 3600 });
  const identity = await Identity.create(), client = await hub.connect(guild);
  await client.login(identity, guild);
  assert.equal((await client.ask({ t: 'guild.join', invite: invite.code, ...(await identity.encFields(guild)) })).member, true);
  return { identity, client, fp: await sha(identity.pub) };
}
const sign = (identity, message) => identity.sign(message);
// Presence changes are told to everybody, the person concerned included: wait for the one wanted.
async function presence(client, fp, joined) {
  for (let i = 0; i < 12; i++) { const m = await client.wait('voice', 4000); if (m.fp === fp && m.joined === joined) return m; }
  throw new Error('presence not seen');
}

test('typing reaches the others, is never stored, and is cheap but not free', async () => {
  const { guild, client: ownerClient, fp: ownerFp } = await guildWithOwner();
  const { client } = await member(guild, ownerClient);
  await client.send({ t: 'typing', channel: 'general' });
  const seen = await ownerClient.wait('typing');
  assert.deepEqual([seen.channel, seen.fp.length], ['general', 64]);
  assert.notEqual(seen.fp, ownerFp);
  assert.deepEqual((await ownerClient.ask({ t: 'history.fetch', channel: 'general' })).items, []);
  // A non-member typing is ignored, and a flood is limited by the shared allowance.
  const stranger = await Identity.create(), outside = await hub.connect(guild);
  await outside.login(stranger, guild);
  await outside.send({ t: 'typing', channel: 'general' });
  assert.equal((await ownerClient.quiet(150)).filter((m) => m.t === 'typing').length, 0);
  for (let i = 0; i < 80; i++) await client.send({ t: 'typing', channel: 'general' });
  await new Promise((r) => setTimeout(r, 200));
  assert.ok((await client.quiet(10)).some((m) => m.code === 'rate_limited'));
});

test('attachments travel as numbered encrypted chunks that only members can read', async () => {
  const { guild, client: ownerClient } = await guildWithOwner();
  const { client } = await member(guild, ownerClient);
  const id = randomHex(16), chunk = (n) => Buffer.alloc(40000, n).toString('base64');
  for (let i = 0; i < 3; i++) assert.equal((await ownerClient.ask({ t: 'blob.put', id, index: i, total: 3, data: chunk(i + 1) })).t, 'ack');
  for (let i = 0; i < 3; i++) {
    const got = await client.ask({ t: 'blob.get', id, index: i });
    assert.deepEqual([got.t, got.total, got.data === chunk(i + 1)], ['blob', 3, true]);
  }
  assert.equal((await client.ask({ t: 'blob.get', id, index: 3 })).code, 'not_found');
  assert.equal((await client.ask({ t: 'blob.get', id: randomHex(16), index: 0 })).code, 'not_found');
  // Only the uploader adds chunks; the total cannot change; sizes and counts are bounded.
  assert.equal((await client.ask({ t: 'blob.put', id, index: 0, total: 3, data: chunk(9) })).code, 'conflict');
  assert.equal((await ownerClient.ask({ t: 'blob.put', id, index: 1, total: 4, data: chunk(9) })).code, 'conflict');
  assert.equal((await ownerClient.ask({ t: 'blob.put', id: randomHex(16), index: 0, total: LIMITS.blobChunks + 1, data: chunk(1) })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'blob.put', id: randomHex(16), index: 3, total: 3, data: chunk(1) })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'blob.put', id: randomHex(16), index: 0, total: 1, data: 'x'.repeat(101) })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'blob.put', id: randomHex(16), index: 0, total: 1, data: 'A'.repeat(LIMITS.blobChunkBase64 + 4) })).code, 'too_large');
  assert.equal((await ownerClient.ask({ t: 'blob.put', id: 'zz', index: 0, total: 1, data: chunk(1) })).code, 'bad_request');
  const stranger = await Identity.create(), outside = await hub.connect(guild);
  await outside.login(stranger, guild);
  assert.equal((await outside.ask({ t: 'blob.get', id, index: 0 })).code, 'forbidden');
  assert.equal((await outside.ask({ t: 'blob.put', id: randomHex(16), index: 0, total: 1, data: chunk(1) })).code, 'forbidden');
  // A large chunk frame is accepted; the same size for anything else is refused.
  const big = Buffer.alloc(48000, 5).toString('base64');
  assert.equal((await ownerClient.ask({ t: 'blob.put', id: randomHex(16), index: 0, total: 1, data: big })).t, 'ack');
  assert.equal((await ownerClient.ask({ t: 'ping', pad: 'x'.repeat(LIMITS.frameBytes) })).code, 'too_large');
});

test('the owner names moderators with a signature; moderators run the day-to-day, not the roles', async () => {
  const { guild, owner, client: ownerClient, fp: ownerFp } = await guildWithOwner();
  const mod = await member(guild, ownerClient), plain = await member(guild, ownerClient), other = await member(guild, ownerClient);
  const note = Buffer.alloc(61, 7).toString('base64');
  // Before: a member can do none of it.
  assert.equal((await mod.client.ask({ t: 'invite.create' })).code, 'forbidden');
  assert.equal((await mod.client.ask({ t: 'channel.create', name: 'x', type: 'text' })).code, 'forbidden');
  // Naming needs the owner, a fresh serial and the owner's own signature.
  const serial = 1, role = 'moderator';
  const sig = await sign(owner, roleMessage(guild, mod.fp, role, serial));
  assert.equal((await mod.client.ask({ t: 'role.set', fp: mod.fp, role, serial, sig })).code, 'forbidden');
  assert.equal((await ownerClient.ask({ t: 'role.set', fp: mod.fp, role, serial, sig: await sign(owner, roleMessage(guild, plain.fp, role, serial)) })).code, 'unauthorized');
  assert.equal((await ownerClient.ask({ t: 'role.set', fp: ownerFp, role, serial, sig })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'role.set', fp: mod.fp, role: 'owner', serial, sig })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'role.set', fp: mod.fp, role, serial, sig })).t, 'ack');
  const announced = await plain.client.wait('role');
  assert.deepEqual([announced.fp, announced.role, announced.serial, announced.sig], [mod.fp, 'moderator', 1, sig]);
  assert.equal((await ownerClient.ask({ t: 'role.set', fp: mod.fp, role, serial, sig })).code, 'conflict');               // a serial is used once
  const listed = (await ownerClient.ask({ t: 'members.list' })).items.find((m) => m.fp === mod.fp);
  assert.deepEqual([listed.role, listed.rolesig, listed.roleserial], ['moderator', sig, 1]);
  // After: invitations, channels, key hand-out and removing plain members are allowed; the rest is not.
  assert.equal((await mod.client.ask({ t: 'invite.create' })).t, 'invite');
  assert.equal((await mod.client.ask({ t: 'channel.create', name: 'mod-room', type: 'text' })).t, 'channel');
  assert.equal((await mod.client.ask({ t: 'keys.put', to: plain.fp, epoch: 1, blob: note })).t, 'ack');
  assert.equal((await mod.client.ask({ t: 'role.set', fp: plain.fp, role: 'moderator', serial: 2, sig })).code, 'forbidden');
  assert.equal((await mod.client.ask({ t: 'member.unban', fp: plain.fp })).code, 'forbidden');
  assert.equal((await mod.client.ask({ t: 'member.remove', fp: ownerFp, epoch: 2 })).code, 'forbidden');                 // never the owner
  assert.equal((await mod.client.ask({ t: 'member.remove', fp: plain.fp, epoch: 2 })).t, 'ack');
  assert.equal((await mod.client.ask({ t: 'member.remove', fp: mod.fp, epoch: 3 })).code, 'bad_request');                 // never themselves
  // A moderator cannot remove another moderator; the owner can.
  const sig2 = await sign(owner, roleMessage(guild, other.fp, role, 2));
  assert.equal((await ownerClient.ask({ t: 'role.set', fp: other.fp, role, serial: 2, sig: sig2 })).t, 'ack');
  assert.equal((await mod.client.ask({ t: 'member.remove', fp: other.fp, epoch: 3 })).code, 'forbidden');
  assert.equal((await ownerClient.ask({ t: 'member.remove', fp: other.fp, epoch: 3 })).t, 'ack');
  // Stepping down is a signed change too.
  const down = await sign(owner, roleMessage(guild, mod.fp, 'member', 3));
  assert.equal((await ownerClient.ask({ t: 'role.set', fp: mod.fp, role: 'member', serial: 3, sig: down })).t, 'ack');
  assert.equal((await mod.client.ask({ t: 'invite.create' })).code, 'forbidden');
});

test('ownership passes to a member who already holds the key, by a signed chain', async () => {
  const { guild, owner, client: ownerClient, fp: ownerFp } = await guildWithOwner();
  const heir = await member(guild, ownerClient), mod = await member(guild, ownerClient);
  const sig = await sign(owner, ownerMessage(guild, heir.fp, 5));
  const note = Buffer.alloc(61, 7).toString('base64');
  assert.equal((await heir.client.ask({ t: 'owner.transfer', to: heir.fp, serial: 5, sig })).code, 'forbidden');
  assert.equal((await ownerClient.ask({ t: 'owner.transfer', to: ownerFp, serial: 5, sig })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'owner.transfer', to: 'f'.repeat(64), serial: 5, sig })).code, 'not_found');
  assert.deepEqual([(await ownerClient.ask({ t: 'owner.transfer', to: heir.fp, serial: 5, sig })).code], ['conflict']);   // not keyed yet
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: heir.fp, epoch: 1, blob: note })).t, 'ack');
  assert.equal((await ownerClient.ask({ t: 'owner.transfer', to: heir.fp, serial: 5, sig: await sign(owner, ownerMessage(guild, mod.fp, 5)) })).code, 'unauthorized');
  // A moderator named earlier loses the title: the old owner signed it.
  assert.equal((await ownerClient.ask({ t: 'role.set', fp: mod.fp, role: 'moderator', serial: 1, sig: await sign(owner, roleMessage(guild, mod.fp, 'moderator', 1)) })).t, 'ack');
  assert.equal((await ownerClient.ask({ t: 'owner.transfer', to: heir.fp, serial: 5, sig })).t, 'ack');
  const told = await mod.client.wait('owner');
  assert.deepEqual([told.from, told.to, told.serial, told.sig], [ownerFp, heir.fp, 5, sig]);
  const list = await heir.client.ask({ t: 'members.list' });
  const roles = Object.fromEntries(list.items.map((m) => [m.fp, m.role]));
  assert.deepEqual(roles, { [ownerFp]: 'member', [heir.fp]: 'owner', [mod.fp]: 'member' });
  assert.deepEqual(list.chain.map((link) => [link.from, link.to, link.serial]), [[ownerFp, heir.fp, 5]]);
  // The old owner is an ordinary member now; the new one has the powers.
  assert.equal((await ownerClient.ask({ t: 'invite.create' })).code, 'forbidden');
  assert.equal((await heir.client.ask({ t: 'invite.create' })).t, 'invite');
  assert.equal((await heir.client.ask({ t: 'member.remove', fp: ownerFp, epoch: 2 })).t, 'ack');
  // A serial that was used cannot be reused.
  assert.equal((await heir.client.ask({ t: 'owner.transfer', to: mod.fp, serial: 5, sig })).code, 'conflict');
});

test('a removed identity can be reinstated by the owner, and comes back only with a new invitation', async () => {
  const { guild, client: ownerClient } = await guildWithOwner();
  const { identity, client, fp } = await member(guild, ownerClient);
  assert.equal((await ownerClient.ask({ t: 'member.remove', fp, epoch: 2 })).t, 'ack');
  for (let i = 0; i < 40 && !client.closed; i++) await new Promise((r) => setTimeout(r, 50));   // over a network the close arrives just after the ack
  assert.equal(client.closed, true);
  const again = await hub.connect(guild);
  await again.login(identity, guild);
  const invite = await ownerClient.ask({ t: 'invite.create' });
  const joinAgain = () => again.ask({ t: 'guild.join', invite: invite.code, ...identity.encFields(guild) && (identity.encFields(guild)) });
  void joinAgain;
  const fields = await identity.encFields(guild);
  assert.equal((await again.ask({ t: 'guild.join', invite: invite.code, ...fields })).reason, 'removed');
  assert.equal((await again.ask({ t: 'member.unban', fp })).code, 'forbidden');                    // not for the removed themselves
  assert.equal((await ownerClient.ask({ t: 'member.unban', fp: 'f'.repeat(64) })).code, 'not_found');
  assert.equal((await ownerClient.ask({ t: 'member.unban', fp })).t, 'ack');
  assert.equal((await ownerClient.ask({ t: 'member.unban', fp })).code, 'not_found');
  assert.equal((await again.ask({ t: 'guild.join', invite: invite.code, ...fields })).member, true);
  assert.equal((await ownerClient.ask({ t: 'members.list' })).former.length, 0);
});

test('voice channels: presence, a participant limit and private signalling that only the addressee receives', async () => {
  const { guild, client: ownerClient } = await guildWithOwner();
  const voice = await ownerClient.ask({ t: 'channel.create', name: 'Salon', type: 'voice' });
  const channel = voice.channel.id;
  const [a, b, c] = [await member(guild, ownerClient), await member(guild, ownerClient), await member(guild, ownerClient)];
  assert.equal((await a.client.ask({ t: 'voice.join', channel: 'general' })).code, 'not_found');         // text channels have no voice
  assert.equal((await a.client.ask({ t: 'voice.join', channel: 'nonexistent' })).code, 'not_found');
  const first = await a.client.ask({ t: 'voice.join', channel });
  assert.deepEqual([first.t, first.participants, first.limit], ['voice.state', [], LIMITS.voiceParticipants]);
  const second = await b.client.ask({ t: 'voice.join', channel });
  assert.deepEqual(second.participants, [a.fp]);
  // Someone who connects later is told who is already in each voice channel.
  const late = await hub.connect(guild);
  const lateReady = await late.login(c.identity, guild);
  assert.deepEqual([...lateReady.presence[channel]].sort(), [a.fp, b.fp].sort());
  const joined = await presence(a.client, b.fp, true);
  assert.equal(joined.channel, channel);
  // Signalling: signed, addressed, only to someone present in the same channel.
  const id = 'sig' + randomHex(6), body = b64('{"sdp":"v=0"}');
  const sign3 = (who, to, i, bd) => sign(who.identity, voiceMessage(guild, channel, to, i, bd));
  assert.equal((await c.client.ask({ t: 'voice.signal', channel, to: a.fp, id, body, sig: await sign3(c, a.fp, id, body) })).code, 'forbidden');     // not in the channel
  assert.equal((await b.client.ask({ t: 'voice.signal', channel, to: c.fp, id, body, sig: await sign3(b, c.fp, id, body) })).code, 'not_found');       // addressee absent
  assert.equal((await b.client.ask({ t: 'voice.signal', channel, to: a.fp, id, body, sig: await sign3(a, a.fp, id, body) })).code, 'unauthorized');    // someone else's signature
  assert.equal((await b.client.ask({ t: 'voice.signal', channel, to: a.fp, id, body: b64('other'), sig: await sign3(b, a.fp, id, body) })).code, 'unauthorized');
  assert.equal((await b.client.ask({ t: 'voice.signal', channel, to: a.fp, id, body: 'A'.repeat(LIMITS.voiceSignalBase64 + 4), sig: '0'.repeat(128) })).code, 'bad_request');
  assert.equal((await b.client.ask({ t: 'voice.signal', channel, to: a.fp, id, body, sig: await sign3(b, a.fp, id, body) })).t, 'ack');
  const got = await a.client.wait('voice.signal');
  assert.deepEqual([got.from, got.id, got.body, got.channel], [b.fp, id, body, channel]);
  assert.equal((await ownerClient.quiet(150)).filter((m) => m.t === 'voice.signal').length, 0);
  assert.equal((await c.client.quiet(10)).filter((m) => m.t === 'voice.signal').length, 0);
  // Leaving, closing the connection and being removed all end the presence.
  assert.equal((await b.client.ask({ t: 'voice.leave' })).t, 'ack');
  await presence(a.client, b.fp, false);
  await b.client.ask({ t: 'voice.join', channel });
  await presence(a.client, b.fp, true);
  await b.client.close();
  await presence(a.client, b.fp, false);                   // closing the connection ends the presence
  await c.client.ask({ t: 'voice.join', channel });
  await presence(a.client, c.fp, true);
  assert.equal((await ownerClient.ask({ t: 'member.remove', fp: c.fp, epoch: 2 })).t, 'ack');
  await presence(a.client, c.fp, false);                   // so does being removed
});

test('a voice channel holds a limited number of people', { skip: remote ? 'slow against a real service' : false }, async () => {
  const { guild, client: ownerClient } = await guildWithOwner();
  const channel = (await ownerClient.ask({ t: 'channel.create', name: 'Petit', type: 'voice' })).channel.id;
  const people = [];
  for (let i = 0; i < LIMITS.voiceParticipants; i++) people.push(await member(guild, ownerClient));
  for (const person of people) assert.equal((await person.client.ask({ t: 'voice.join', channel })).t, 'voice.state');
  const late = await member(guild, ownerClient);
  assert.deepEqual([(await late.client.ask({ t: 'voice.join', channel })).code], ['forbidden']);
  assert.equal((await people[0].client.ask({ t: 'voice.join', channel })).t, 'voice.state');       // rejoining your own seat is fine
});

test('relay credentials come from the host and are only for members', async () => {
  const { guild, client: ownerClient } = await guildWithOwner();
  const reply = await ownerClient.ask({ t: 'turn.get' });
  assert.equal(reply.t, 'turn');
  if (!remote) assert.deepEqual(reply.iceServers, [{ urls: ['turn:relay.example:3478'], username: 'u', credential: 'c' }]);
  else assert.ok(reply.iceServers === null || Array.isArray(reply.iceServers));
  const stranger = await Identity.create(), outside = await hub.connect(guild);
  await outside.login(stranger, guild);
  assert.equal((await outside.ask({ t: 'turn.get' })).code, 'forbidden');
});

test('without a relay configured the answer is "none", and a failing relay API never breaks the service', { skip: remote ? 'in-process only' : false }, async () => {
  const bare = new LocalHub();
  const guild = newGuildId(), owner = await Identity.create(), client = await bare.connect(guild);
  await client.login(owner, guild);
  await client.ask({ t: 'guild.create', name: 'x', ...(await owner.encFields(guild)) });
  assert.equal((await client.ask({ t: 'turn.get' })).iceServers, null);
  bare.turn = async () => { throw new Error('API down'); };
  const other = await bare.connect(guild);
  await other.login(owner, guild);
  assert.equal((await other.ask({ t: 'turn.get' })).iceServers, null);
});

test('frame types named like inherited properties do not switch the rate limit off', async () => {
  const { client } = await guildWithOwner();
  for (const t of ['constructor', 'toString', '__proto__', 'hasOwnProperty', 'valueOf']) await client.ask({ t });
  let limited = 0;
  for (let i = 0; i < 80; i++) if ((await client.ask({ t: 'ping' })).code === 'rate_limited') limited++;
  assert.ok(limited > 0, 'the connection is still throttled');
});
