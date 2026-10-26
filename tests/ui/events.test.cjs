// The rules about who may change a message, and what a row shows. Run with: node --test tests/ui/
const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto').webcrypto;
globalThis.crypto = globalThis.crypto || crypto;
const E = require('../../src/windows/ui/events.js');
const e2ee = require('../../src/windows/ui/e2ee.js');

const ME = 'me'.repeat(32), ALICE = 'al'.repeat(32), BOB = 'bo'.repeat(32), OWNER = 'ow'.repeat(32), MOD = 'md'.repeat(32);
const names = { [ME]: 'Moi', [ALICE]: 'Alice', [BOB]: 'Bob', [OWNER]: 'Chef', [MOD]: 'Modo' };
const ctx = { me: ME, isStaff: fp => fp === OWNER || fp === MOD, nameOf: fp => names[fp] || fp.slice(0, 8) };
let n = 0;
const ev = (from, payload, seq = ++n) => ({ seq, id: `id${seq}`, from, ts: Date.UTC(2026, 0, 1, 12, 30), payload: { v: 1, ...payload } });
const feed = (store, ...events) => events.forEach(e => E.ingest(store, e));

test('a plain message becomes a row, mine labelled "Vous"', () => {
  const s = E.createStore(); feed(s, ev(ALICE, { text: 'bonjour' }), ev(ME, { text: 'salut' }));
  const [a, m] = E.ordered(s).map(seq => E.row(s, seq, ctx));
  assert.deepEqual([a.text, a.author, a.remote, m.author, m.remote, a.actions], ['bonjour', 'Alice', 1, 'Vous', 0, true]);
});

test('only the author can edit; the latest valid edit wins and is marked', () => {
  const s = E.createStore(), m = ev(ALICE, { text: 'v1' });
  feed(s, m, ev(BOB, { t: 'edit', target: m.seq, text: 'forgé' }), ev(ALICE, { t: 'edit', target: m.seq, text: 'v2' }), ev(ALICE, { t: 'edit', target: m.seq, text: 'v3' }), ev(BOB, { t: 'edit', target: m.seq, text: 'encore forgé' }));
  const r = E.row(s, m.seq, ctx);
  assert.deepEqual([r.text, r.edited], ['v3', true]);
});

test('the author, the owner and moderators can delete; nobody else', () => {
  for (const [who, deleted] of [[ALICE, true], [OWNER, true], [MOD, true], [BOB, false], [ME, false]]) {
    const s = E.createStore(), m = ev(ALICE, { text: 'x' });
    feed(s, m, ev(who, { t: 'del', target: m.seq }));
    const r = E.row(s, m.seq, ctx);
    assert.equal(r.deleted, deleted, `${names[who]} deleting Alice's message`);
    if (deleted) assert.deepEqual([r.text, r.reactions, r.files], ['🗑 Message supprimé', [], []]);
  }
});

test('a deleted message cannot be edited back, and its replies say it is gone', () => {
  const s = E.createStore(), m = ev(ALICE, { text: 'secret' }), r = ev(BOB, { text: 'réponse', reply: m.seq });
  feed(s, m, r, ev(ALICE, { t: 'del', target: m.seq }), ev(ALICE, { t: 'edit', target: m.seq, text: 'revenu' }));
  assert.equal(E.row(s, m.seq, ctx).deleted, true);
  assert.deepEqual(E.row(s, r.seq, ctx).reply, { seq: m.seq, author: 'Alice', text: 'Message supprimé' });
});

test('reactions: anyone adds, each person once per emoji, removing works, empty ones vanish', () => {
  const s = E.createStore(), m = ev(ALICE, { text: 'x' });
  feed(s, m, ev(BOB, { t: 'react', target: m.seq, emoji: '👍', on: true }), ev(ME, { t: 'react', target: m.seq, emoji: '👍', on: true }), ev(BOB, { t: 'react', target: m.seq, emoji: '👍', on: true }),
    ev(ALICE, { t: 'react', target: m.seq, emoji: '❤️', on: true }), ev(ALICE, { t: 'react', target: m.seq, emoji: '❤️', on: false }));
  const r = E.row(s, m.seq, ctx);
  assert.deepEqual(r.reactions.map(x => [x.emoji, x.count, x.mine]), [['👍', 2, true]]);
  assert.deepEqual(r.reactions[0].names.sort(), ['Bob', 'Moi']);
});

test('changes that point forward, nowhere or at nonsense are ignored; events are counted once', () => {
  const s = E.createStore(), m = ev(ALICE, { text: 'x' });
  feed(s, m);
  const edit = ev(ALICE, { t: 'edit', target: m.seq, text: 'ok' });
  assert.equal(E.ingest(s, edit), m.seq);
  assert.equal(E.ingest(s, edit), null);                                              // replayed after a reconnection
  assert.equal(E.ingest(s, ev(ALICE, { t: 'edit', target: 999, text: 'future' }, 5)), null);
  assert.equal(E.ingest(s, ev(ALICE, { t: 'edit', target: 'x', text: 'y' })), null);
  assert.equal(E.ingest(s, ev(ALICE, { t: 'edit', target: m.seq })), null);
  assert.equal(E.ingest(s, ev(ALICE, { t: 'react', target: m.seq, emoji: '' })), null);
  assert.equal(E.ingest(s, ev(ALICE, { t: 'react', target: m.seq, emoji: 'x'.repeat(40) })), null);
  assert.equal(E.ingest(s, null), null);
  assert.equal(E.row(s, 12345, ctx), null);
});

test('a change that arrives before the message it concerns still applies once the message is known', () => {
  const s = E.createStore(), m = ev(ALICE, { text: 'v1' }, 10), edit = ev(ALICE, { t: 'edit', target: 10, text: 'v2' }, 11);
  feed(s, edit);                                                                      // older page not loaded yet
  assert.equal(E.row(s, 10, ctx), null);
  feed(s, m);
  assert.equal(E.row(s, 10, ctx).text, 'v2');
});

test('replies quote the current text, mentions and files are kept, bad files are dropped', () => {
  const s = E.createStore(), m = ev(ALICE, { text: 'Voici le plan\n\nen deux lignes' }), edit = ev(ALICE, { t: 'edit', target: m.seq, text: 'Plan v2' });
  const r = ev(BOB, { text: '@Moi regarde', reply: m.seq, mentions: [ME], files: [{ blob: 'a'.repeat(32), name: 'a.png' }, { name: 'sans blob' }, null] });
  feed(s, m, edit, r);
  const row = E.row(s, r.seq, ctx);
  assert.deepEqual(row.reply, { seq: m.seq, author: 'Alice', text: 'Plan v2' });
  assert.equal(row.mentionsMe, true);
  assert.equal(row.files.length, 1);
  const stranger = E.row(s, r.seq, { ...ctx, me: BOB });
  assert.equal(stranger.mentionsMe, false);
  const orphan = E.createStore(), lonely = ev(BOB, { text: 'orphelin', reply: 3 });
  feed(orphan, lonely);
  assert.deepEqual(E.row(orphan, lonely.seq, ctx).reply, { seq: 3, unknown: true });
});

test('long quotes are shortened to one line', () => {
  const s = E.createStore(), m = ev(ALICE, { text: 'mot '.repeat(80) }), r = ev(BOB, { text: 'ok', reply: m.seq });
  feed(s, m, r);
  const quote = E.row(s, r.seq, ctx).reply.text;
  assert.ok(quote.length <= 90 && quote.endsWith('…') && !quote.includes('\n'));
});

test('search ignores case and accents, skips deleted messages and shows the newest first', () => {
  const s = E.createStore(), a = ev(ALICE, { text: 'Rendez-vous à la Café du coin' }), b = ev(BOB, { text: 'le CAFE est fermé' }), c = ev(ALICE, { text: 'café secret' }), d = ev(BOB, { text: 'autre sujet' });
  feed(s, a, b, c, d, ev(ALICE, { t: 'del', target: c.seq }), ev(ALICE, { t: 'edit', target: a.seq, text: 'Rendez-vous au parc' }));
  assert.deepEqual(E.search(s, 'CAFÉ', ctx).map(h => h.seq), [b.seq]);
  assert.deepEqual(E.search(s, 'parc', ctx).map(h => h.seq), [a.seq]);               // search sees the edited text
  assert.deepEqual(E.search(s, '   ', ctx), []);
  assert.deepEqual(E.search(s, 'introuvable', ctx), []);
});

test('payloads round-trip through encryption, and a stranger cannot read or rewrite them', async () => {
  const key = e2ee.newGroupKey(), payload = { text: 'salut', reply: 4, mentions: [ALICE], files: [{ blob: 'b'.repeat(32), name: 'photo.png', size: 100, mime: 'image/png', key: 'AAAA', chunks: 1 }] };
  const body = await e2ee.encryptPayload(key, 2, 'g'.repeat(32), 'general', 'id000001', payload);
  assert.deepEqual(await e2ee.decryptPayload(key, 'g'.repeat(32), 'general', 'id000001', body), { v: 1, ...payload });
  await assert.rejects(e2ee.decryptPayload(e2ee.newGroupKey(), 'g'.repeat(32), 'general', 'id000001', body));
  await assert.rejects(e2ee.decryptPayload(key, 'g'.repeat(32), 'general', 'id000002', body));
  await assert.rejects(e2ee.encryptPayload(key, 1, 'g'.repeat(32), 'general', 'id000003', { text: 'x'.repeat(16000) }));
  assert.equal(await e2ee.decryptMessage(key, 'g'.repeat(32), 'general', 'id000004', await e2ee.encryptMessage(key, 1, 'g'.repeat(32), 'general', 'id000004', 'ancien format')), 'ancien format');
});

test('files are encrypted in chunks that fit the service, and only the right key and order open them', async () => {
  const bytes = new Uint8Array(130000), id = 'f'.repeat(32);
  for (let at = 0; at < bytes.length; at += 60000) crypto.getRandomValues(bytes.subarray(at, Math.min(at + 60000, bytes.length)));
  const { key, chunks } = await e2ee.encryptFile(bytes, id);
  assert.equal(chunks.length, 3);
  assert.ok(chunks.every(c => c.length <= 64000), 'each chunk fits a service frame');
  assert.deepEqual(await e2ee.decryptFile(key, id, chunks), bytes);
  await assert.rejects(e2ee.decryptFile(key, 'e'.repeat(32), chunks));                          // another file id
  await assert.rejects(e2ee.decryptFile(key, id, [chunks[1], chunks[0], chunks[2]]));           // reordered
  await assert.rejects(e2ee.decryptFile(key, id, chunks.slice(0, 2)));                          // truncated
  await assert.rejects(e2ee.decryptFile((await e2ee.encryptFile(bytes, id)).key, id, chunks));  // another key
  const empty = await e2ee.encryptFile(new Uint8Array(0), id);
  assert.equal(empty.chunks.length, 1);
  assert.equal((await e2ee.decryptFile(empty.key, id, empty.chunks)).length, 0);
  await assert.rejects(e2ee.encryptFile(new Uint8Array(1200000), id));                          // over the 24-chunk limit
});

test('only real pictures are recognised, by their first bytes', () => {
  const png = Uint8Array.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0]);
  assert.equal(e2ee.sniffImage(png), 'image/png');
  assert.equal(e2ee.sniffImage(Uint8Array.from([0xff, 0xd8, 0xff, 0xe0])), 'image/jpeg');
  assert.equal(e2ee.sniffImage(Uint8Array.from([0x47, 0x49, 0x46, 0x38, 0x39, 0x61])), 'image/gif');
  assert.equal(e2ee.sniffImage(Uint8Array.from([0x52, 0x49, 0x46, 0x46, 0, 0, 0, 0, 0x57, 0x45, 0x42, 0x50])), 'image/webp');
  assert.equal(e2ee.sniffImage(Uint8Array.from([0x3c, 0x73, 0x76, 0x67])), null);              // an SVG could carry scripts
  assert.equal(e2ee.sniffImage(Uint8Array.from([0x4d, 0x5a, 0x90, 0])), null);                 // a program
  assert.equal(e2ee.sniffImage(new Uint8Array(0)), null);
});
