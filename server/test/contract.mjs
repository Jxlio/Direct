// Contract tests for Murmur service protocol v2 (docs/protocole.md).
// Default: run against the in-process core. Set MURMUR_SERVICE_URL to test a real service.
import test from 'node:test';
import assert from 'node:assert/strict';
import { Identity, LocalHub, RemoteHub, envelope, newGuildId } from './harness.mjs';
import { LIMITS, authMessage } from '../src/protocol.mjs';

const remote = process.env.MURMUR_SERVICE_URL;
const hub = remote ? new RemoteHub(remote) : new LocalHub();
test.after(() => hub.close());
const b64 = (text) => Buffer.from(text).toString('base64');

async function guildWithOwner() {
  const guild = newGuildId(), owner = await Identity.create();
  const client = await hub.connect(guild);
  const ready = await client.login(owner, guild);
  assert.equal(ready.exists, false);
  const created = await client.ask({ t: 'guild.create', ...(await owner.encFields(guild)), title: 'Amis' });   // `name` in the fields is the founder, not the server
  assert.equal(created.member, true);
  assert.equal(created.name, 'Amis', 'the server keeps its own name, not the founder display name');
  return { guild, owner, client, created };
}
async function member(guild, ownerClient) {
  const invite = await ownerClient.ask({ t: 'invite.create', max_uses: 5, ttl_s: 3600 });
  const identity = await Identity.create(), client = await hub.connect(guild);
  await client.login(identity, guild);
  const joined = await client.ask({ t: 'guild.join', invite: invite.code, ...(await identity.encFields(guild)) });
  assert.equal(joined.member, true);
  return { identity, client };
}

test('hello announces protocol 2, a fresh nonce and the limits', async () => {
  const a = await hub.connect(newGuildId()), b = await hub.connect(newGuildId());
  const [x, y] = [await a.wait('hello'), await b.wait('hello')];
  assert.equal(x.protocol, 2);
  assert.match(x.nonce, /^[0-9a-f]{64}$/);
  assert.notEqual(x.nonce, y.nonce);
  assert.equal(x.limits.historyPage, LIMITS.historyPage);
});

test('requests before authentication are refused', async () => {
  const client = await hub.connect(newGuildId());
  await client.wait('hello');
  const reply = await client.ask({ t: 'guild.create', name: 'x' });
  assert.equal(reply.code, 'unauthorized');
});

test('a signature over another guild is rejected', async () => {
  const guild = newGuildId(), identity = await Identity.create(), client = await hub.connect(guild);
  const hello = await client.wait('hello');
  await client.send({ t: 'auth', pub: identity.pub, sig: await identity.sign(authMessage(newGuildId(), hello.nonce)) });
  assert.equal((await client.wait('error')).code, 'unauthorized');
});

test('the first member creates a guild and owns a default text channel', async () => {
  const { created, client } = await guildWithOwner();
  assert.equal(created.role, 'owner');
  assert.deepEqual(created.channels.map((c) => c.name), ['general']);
  assert.equal((await client.ask({ t: 'guild.create', name: 'Autre' })).code, 'conflict');
  assert.equal(created.you.length, 64);
});

test('joining needs a valid invite; invites are limited in uses', async () => {
  const { guild, client: ownerClient } = await guildWithOwner();
  const stranger = await Identity.create(), client = await hub.connect(guild);
  const ready = await client.login(stranger, guild);
  assert.equal(ready.member, false);
  assert.equal((await client.ask({ t: 'guild.join', invite: 'a'.repeat(32), ...(await stranger.encFields(guild)) })).code, 'forbidden');
  assert.equal((await client.ask({ t: 'guild.join', invite: 'zz', ...(await stranger.encFields(guild)) })).code, 'bad_request');
  const invite = await ownerClient.ask({ t: 'invite.create', max_uses: 1, ttl_s: 3600 });
  assert.equal((await client.ask({ t: 'guild.join', invite: invite.code, ...(await stranger.encFields(guild)) })).member, true);
  const late = await Identity.create(), second = await hub.connect(guild);
  await second.login(late, guild);
  assert.equal((await second.ask({ t: 'guild.join', invite: invite.code, ...(await late.encFields(guild)) })).code, 'forbidden');
});

test('expired invites are refused', { skip: remote ? 'needs a controllable clock' : false }, async () => {
  const { guild, client: ownerClient } = await guildWithOwner();
  const invite = await ownerClient.ask({ t: 'invite.create', max_uses: 1, ttl_s: 60 });
  hub.advance(61_000);
  const guest = await Identity.create(), client = await hub.connect(guild);
  await client.login(guest, guild);
  assert.equal((await client.ask({ t: 'guild.join', invite: invite.code, ...(await guest.encFields(guild)) })).code, 'forbidden');
});

test('only the owner creates invites and channels; new channels reach members', async () => {
  const { guild, client: ownerClient } = await guildWithOwner();
  const { client } = await member(guild, ownerClient);
  assert.equal((await client.ask({ t: 'invite.create' })).code, 'forbidden');
  assert.equal((await client.ask({ t: 'channel.create', name: 'x', type: 'text' })).code, 'forbidden');
  assert.equal((await ownerClient.ask({ t: 'channel.create', name: '', type: 'text' })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'channel.create', name: 'x', type: 'video' })).code, 'bad_request');
  const created = await ownerClient.ask({ t: 'channel.create', name: 'jeux', type: 'text' });
  assert.equal(created.channel.name, 'jeux');
  assert.equal((await client.wait('channel')).channel.id, created.channel.id);
});

test('envelopes are signed, ordered, stored opaque and broadcast to members', async () => {
  const { guild, client: ownerClient } = await guildWithOwner();
  const { identity, client } = await member(guild, ownerClient);
  const body = b64('ciphertext-not-readable-by-the-service');
  const first = await client.ask(await envelope(identity, guild, 'general', 'msg00001', body));
  const second = await client.ask(await envelope(identity, guild, 'general', 'msg00002', body));
  assert.equal(first.t, 'ack');
  assert.equal(second.seq, first.seq + 1);
  const seen = await ownerClient.wait('envelope');
  assert.equal(seen.body, body);
  assert.equal(seen.seq, first.seq);
  assert.equal(seen.from.length, 64);
});

test('forged, duplicate, oversize and misrouted envelopes are refused', async () => {
  const { guild, owner, client: ownerClient } = await guildWithOwner();
  const { identity, client } = await member(guild, ownerClient);
  const body = b64('x');
  const good = await envelope(identity, guild, 'general', 'msg00001', body);
  assert.equal((await client.ask({ ...good, body: b64('y') })).code, 'unauthorized');
  assert.equal((await client.ask({ ...good, id: 'msg00009' })).code, 'unauthorized');
  assert.equal((await client.ask(await envelope(owner, guild, 'general', 'msg00003', body))).code, 'unauthorized');
  assert.equal((await client.ask(good)).t, 'ack');
  assert.equal((await client.ask(good)).code, 'conflict');
  assert.equal((await client.ask(await envelope(identity, guild, 'missing1', 'msg00004', body))).code, 'not_found');
  const huge = Buffer.alloc(LIMITS.envelopeBytes + 3).toString('base64');
  assert.equal((await client.ask(await envelope(identity, guild, 'general', 'msg00005', huge))).code, 'too_large');
  assert.equal((await client.ask(await envelope(identity, guild, 'general', 'bad id!', body))).code, 'bad_request');
});

test('a non-member can neither send nor read history', async () => {
  const { guild } = await guildWithOwner();
  const stranger = await Identity.create(), client = await hub.connect(guild);
  await client.login(stranger, guild);
  assert.equal((await client.ask(await envelope(stranger, guild, 'general', 'msg00001', b64('x')))).code, 'forbidden');
  assert.equal((await client.ask({ t: 'history.fetch', channel: 'general' })).code, 'forbidden');
});

test('history pages go backwards with before, and forwards with after', async () => {
  const { guild, owner, client: ownerClient } = await guildWithOwner();
  for (let i = 1; i <= 7; i++) await ownerClient.ask(await envelope(owner, guild, 'general', `msg${String(i).padStart(5, '0')}`, b64('x')));
  const latest = await ownerClient.ask({ t: 'history.fetch', channel: 'general', limit: 3 });
  assert.deepEqual(latest.items.map((m) => m.seq), [5, 6, 7]);
  assert.equal(latest.more, true);
  const older = await ownerClient.ask({ t: 'history.fetch', channel: 'general', limit: 3, before: 5 });
  assert.deepEqual(older.items.map((m) => m.seq), [2, 3, 4]);
  const newer = await ownerClient.ask({ t: 'history.fetch', channel: 'general', limit: 10, after: 5 });
  assert.deepEqual(newer.items.map((m) => m.seq), [6, 7]);
  assert.equal(newer.more, false);
  const capped = await ownerClient.ask({ t: 'history.fetch', channel: 'general', limit: 9999 });
  assert.ok(capped.items.length <= LIMITS.historyPage);
});

test('malformed frames get an error and oversize frames are refused', async () => {
  const client = await hub.connect(newGuildId());
  await client.wait('hello');
  await client.send('not json');
  assert.equal((await client.wait('error')).code, 'bad_request');
  await client.send(JSON.stringify({ t: 'nope' }));
  assert.equal((await client.wait('error')).code, 'unauthorized');
  await client.send('x'.repeat(LIMITS.frameBytes + 1));
  assert.equal((await client.wait('error')).code, 'too_large');
});

test('a burst beyond the limit is rate limited with a retry delay', async () => {
  const client = await hub.connect(newGuildId());
  await client.wait('hello');
  for (let i = 0; i < LIMITS.burst + 5; i++) await client.send({ t: 'ping' });
  await new Promise((r) => setTimeout(r, 150));
  const replies = client.queue.splice(0);
  const limited = replies.find((m) => m.code === 'rate_limited');
  assert.ok(limited, 'expected a rate_limited reply');
  assert.ok(limited.retry_after >= 1);
});

test('the encryption key must be signed by the identity that publishes it', async () => {
  const guild = newGuildId(), owner = await Identity.create(), other = await Identity.create();
  const client = await hub.connect(guild);
  await client.login(owner, guild);
  const fields = await owner.encFields(guild);
  assert.equal((await client.ask({ t: 'guild.create', name: 'x' })).code, 'bad_request');
  assert.equal((await client.ask({ t: 'guild.create', name: 'x', enc: fields.enc })).code, 'bad_request');
  assert.equal((await client.ask({ t: 'guild.create', name: 'x', enc: fields.enc, encsig: fields.encsig })).code, 'bad_request');   // no signed display name
  assert.equal((await client.ask({ t: 'guild.create', name: 'x', enc: other.enc, encsig: fields.encsig })).code, 'bad_request');   // another key, same signature
  const elsewhere = await owner.encFields(newGuildId());
  assert.equal((await client.ask({ t: 'guild.create', name: 'x', ...elsewhere })).code, 'bad_request');                              // signature for another guild
  assert.equal((await client.ask({ t: 'guild.create', name: 'x', ...fields })).member, true);
});

test('members list carries identity and encryption keys, and who already has the group key', async () => {
  const { guild, owner, client: ownerClient } = await guildWithOwner();
  const { identity } = await member(guild, ownerClient);
  const list = await ownerClient.ask({ t: 'members.list' });
  assert.equal(list.items.length, 2);
  const mine = list.items.find((m) => m.role === 'owner'), theirs = list.items.find((m) => m.role === 'member');
  assert.equal(mine.pub, owner.pub); assert.equal(mine.enc, owner.enc);
  assert.equal(theirs.pub, identity.pub); assert.equal(theirs.enc, identity.enc);
  assert.equal(theirs.keyed, false);
  assert.equal((await ownerClient.wait('member')).enc, identity.enc);
});

test('only the owner hands out group keys; each member reads only their own', async () => {
  const { guild, owner, client: ownerClient } = await guildWithOwner();
  const { identity, client } = await member(guild, ownerClient);
  const list = await ownerClient.ask({ t: 'members.list' });
  const guestFp = list.items.find((m) => m.role === 'member').fp, ownerFp = list.items.find((m) => m.role === 'owner').fp;
  const blob = Buffer.alloc(61, 7).toString('base64');
  assert.equal((await client.ask({ t: 'keys.put', to: guestFp, epoch: 1, blob })).code, 'forbidden');
  assert.deepEqual((await client.ask({ t: 'keys.get' })).items, []);
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: 'f'.repeat(64), epoch: 1, blob })).code, 'not_found');
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: guestFp, epoch: 0, blob })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: guestFp, epoch: 1, blob: 'not base64!' })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: guestFp, epoch: 1, blob: Buffer.alloc(900).toString('base64') })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: guestFp, epoch: 1, blob })).t, 'ack');
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: ownerFp, epoch: 1, blob })).t, 'ack');
  const notice = await client.wait('keys');
  assert.equal(notice.to, guestFp);
  const got = (await client.ask({ t: 'keys.get' })).items;
  assert.deepEqual(got.map((k) => [k.epoch, k.blob, k.from]), [[1, blob, ownerFp]]);
  assert.equal((await ownerClient.ask({ t: 'members.list' })).items.filter((m) => m.keyed).length, 2);
  const stranger = await Identity.create(), outside = await hub.connect(guild);
  await outside.login(stranger, guild);
  assert.equal((await outside.ask({ t: 'keys.get' })).code, 'forbidden');
  assert.equal((await outside.ask({ t: 'members.list' })).code, 'forbidden');
});

test('display names are signed by their owner, validated, and shown to everyone', async () => {
  const { guild, owner, client: ownerClient } = await guildWithOwner();
  const { identity, client } = await member(guild, ownerClient);
  const list = (await ownerClient.ask({ t: 'members.list' })).items;
  assert.equal(list.find((m) => m.role === 'owner').name, owner.name);
  assert.equal(list.find((m) => m.role === 'member').name, identity.name);
  const joined = await ownerClient.wait('member');
  assert.equal(joined.name, identity.name);
  // Rename: accepted with a valid signature, announced to members.
  const renamed = await client.ask({ t: 'profile.set', ...(await identity.nameFields(guild, 'Alice')) });
  assert.equal(renamed.t, 'ack');
  const told = await ownerClient.wait('profile');
  assert.deepEqual([told.name, told.namesig.length], ['Alice', 128]);
  assert.equal((await ownerClient.ask({ t: 'members.list' })).items.find((m) => m.role === 'member').name, 'Alice');
  // Refused: someone else's signature, another server, empty or oversize names, control characters, no signature.
  const base = await identity.nameFields(guild, 'Bob');
  assert.equal((await client.ask({ t: 'profile.set', name: 'Mallory', namesig: base.namesig })).code, 'bad_request');
  assert.equal((await client.ask({ t: 'profile.set', ...(await identity.nameFields(newGuildId(), 'Bob')) })).code, 'bad_request');
  assert.equal((await client.ask({ t: 'profile.set', ...(await owner.nameFields(guild, 'Bob')) })).code, 'bad_request');
  for (const bad of ['', ' Bob', 'Bob ', 'x'.repeat(33), 'a\nb', 'a\u202eb', 'a\u200bb']) {
    assert.equal((await client.ask({ t: 'profile.set', ...(await identity.nameFields(guild, bad)) })).code, 'bad_request', JSON.stringify(bad));
  }
  assert.equal((await client.ask({ t: 'profile.set', name: 'Bob' })).code, 'bad_request');
  const stranger = await Identity.create(), outside = await hub.connect(guild);
  await outside.login(stranger, guild);
  assert.equal((await outside.ask({ t: 'profile.set', ...(await stranger.nameFields(guild, 'Eve')) })).code, 'forbidden');
  assert.equal((await client.ask({ t: 'profile.set', ...(await identity.nameFields(guild, 'Zoé 😀')) })).t, 'ack');   // accents and emoji are fine
});

test('the owner removes a member: access ends, the next epoch opens, the identity cannot return', async () => {
  const { guild, client: ownerClient } = await guildWithOwner();
  const { identity, client } = await member(guild, ownerClient);
  const list = (await ownerClient.ask({ t: 'members.list' })).items;
  const guestFp = list.find((m) => m.role === 'member').fp, ownerFp = list.find((m) => m.role === 'owner').fp;
  const blob = Buffer.alloc(61, 7).toString('base64');
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: guestFp, epoch: 1, blob })).t, 'ack');
  // Only the owner removes; never themselves; the target must exist; the epoch must be exactly the next one.
  assert.equal((await client.ask({ t: 'member.remove', fp: ownerFp, epoch: 2 })).code, 'forbidden');
  assert.equal((await ownerClient.ask({ t: 'member.remove', fp: ownerFp, epoch: 2 })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'member.remove', fp: 'f'.repeat(64), epoch: 2 })).code, 'not_found');
  assert.equal((await ownerClient.ask({ t: 'member.remove', fp: guestFp, epoch: 5 })).code, 'conflict');
  // The next epoch can be staged for the owner only, before the removal.
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: guestFp, epoch: 2, blob })).code, 'bad_request');
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: ownerFp, epoch: 2, blob })).t, 'ack');
  const removal = await ownerClient.ask({ t: 'member.remove', fp: guestFp, epoch: 2 });
  assert.deepEqual([removal.t, removal.epoch], ['ack', 2]);
  assert.equal((await ownerClient.wait('removed')).fp, guestFp);
  assert.equal((await client.wait('removed')).fp, guestFp);                       // the member is told before the door closes
  assert.equal((await ownerClient.ask({ t: 'members.list' })).items.length, 1);
  assert.equal((await ownerClient.ask({ t: 'members.list' })).epoch, 2);
  // Their earlier messages must stay verifiable: the identity key and signed name are still listed.
  const gone = (await ownerClient.ask({ t: 'members.list' })).former;
  assert.deepEqual(gone.map((m) => [m.fp, m.pub, m.name]), [[guestFp, identity.pub, identity.name]]);
  // The removed identity can read nothing and write nothing, even on a fresh connection.
  const again = await hub.connect(guild);
  const ready = await again.login(identity, guild);
  assert.equal(ready.member, false);
  for (const request of [{ t: 'keys.get' }, { t: 'members.list' }, { t: 'history.fetch', channel: 'general' }]) assert.equal((await again.ask(request)).code, 'forbidden');
  assert.equal((await again.ask(await envelope(identity, guild, 'general', 'late00001', Buffer.from('x').toString('base64')))).code, 'forbidden');
  // Their key blobs are gone, and a new invitation cannot bring the same identity back.
  const invite = await ownerClient.ask({ t: 'invite.create', max_uses: 5, ttl_s: 3600 });
  const refused = await again.ask({ t: 'guild.join', invite: invite.code, ...(await identity.encFields(guild)) });
  assert.deepEqual([refused.code, refused.reason], ['forbidden', 'removed']);
  // A different identity can still join and receives every epoch's key addressed to it.
  const { identity: newcomer, client: fresh } = await member(guild, ownerClient);
  const newcomerFp = (await ownerClient.ask({ t: 'members.list' })).items.find((m) => m.fp !== ownerFp).fp;
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: newcomerFp, epoch: 1, blob })).t, 'ack');
  assert.equal((await ownerClient.ask({ t: 'keys.put', to: newcomerFp, epoch: 2, blob })).t, 'ack');
  assert.deepEqual((await fresh.ask({ t: 'keys.get' })).items.map((k) => k.epoch), [1, 2]);
  assert.equal((await ownerClient.ask({ t: 'members.list' })).items.filter((m) => m.keyed).length, 2);
  void newcomer;
});
