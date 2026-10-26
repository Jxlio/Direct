// Unit tests for src/windows/ui/e2ee.js. Run with: node --test tests/ui/
const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto').webcrypto;
globalThis.crypto = globalThis.crypto || crypto;
const e2ee = require('../../src/windows/ui/e2ee.js');

const guild = 'a'.repeat(32), channel = 'general', id = 'msg00001';
async function pair() {
  const a = await crypto.subtle.generateKey({ name: 'ECDH', namedCurve: 'P-256' }, true, ['deriveBits']);
  const b = await crypto.subtle.generateKey({ name: 'ECDH', namedCurve: 'P-256' }, true, ['deriveBits']);
  const pub = async (k) => e2ee.toHex(new Uint8Array(await crypto.subtle.exportKey('raw', k.publicKey)));
  const agree = async (mine, theirs) => new Uint8Array(await crypto.subtle.deriveBits({ name: 'ECDH', public: await crypto.subtle.importKey('raw', theirs, { name: 'ECDH', namedCurve: 'P-256' }, false, []) }, mine.privateKey, 256));
  return { a, b, aPub: await pub(a), bPub: await pub(b), agree };
}

test('a message round-trips, including emoji and accents', async () => {
  const key = e2ee.newGroupKey(), text = 'Salut — é à ü 😀 \n ligne 2';
  const body = await e2ee.encryptMessage(key, 3, guild, channel, id, text);
  assert.equal(await e2ee.decryptMessage(key, guild, channel, id, body), text);
  assert.equal(e2ee.parseBody(body).epoch, 3);
});
test('the body does not contain the text and differs each time', async () => {
  const key = e2ee.newGroupKey();
  const one = await e2ee.encryptMessage(key, 1, guild, channel, id, 'secret secret secret');
  const two = await e2ee.encryptMessage(key, 1, guild, channel, id, 'secret secret secret');
  assert.notEqual(one, two);
  assert.ok(!Buffer.from(one, 'base64').toString('latin1').includes('secret'));
});
test('a wrong key, channel, id or server, or any altered byte, is refused', async () => {
  const key = e2ee.newGroupKey(), body = await e2ee.encryptMessage(key, 1, guild, channel, id, 'hello');
  await assert.rejects(e2ee.decryptMessage(e2ee.newGroupKey(), guild, channel, id, body));
  await assert.rejects(e2ee.decryptMessage(key, guild, 'jeux0001', id, body));
  await assert.rejects(e2ee.decryptMessage(key, guild, channel, 'msg00002', body));
  await assert.rejects(e2ee.decryptMessage(key, 'b'.repeat(32), channel, id, body));
  const bytes = Buffer.from(body, 'base64');
  for (const index of [0, 3, 6, 20, bytes.length - 1]) {
    const changed = Buffer.from(bytes); changed[index] ^= 1;
    await assert.rejects(e2ee.decryptMessage(key, guild, channel, id, changed.toString('base64')), `byte ${index}`);
  }
  await assert.rejects(e2ee.decryptMessage(key, guild, channel, id, bytes.subarray(0, 20).toString('base64')));
  await assert.rejects(e2ee.decryptMessage(key, guild, channel, id, 'not base64!'));
});
test('the epoch is authenticated: relabelling a body as another epoch fails', async () => {
  const key = e2ee.newGroupKey(), body = Buffer.from(await e2ee.encryptMessage(key, 1, guild, channel, id, 'hello'), 'base64');
  body.writeUInt32BE(2, 1);
  await assert.rejects(e2ee.decryptMessage(key, guild, channel, id, body.toString('base64')));
});
test('the group key travels wrapped for one recipient and nobody else', async () => {
  const { a, b, aPub, bPub, agree } = await pair();
  const group = e2ee.newGroupKey();
  const sharedA = await agree(a, e2ee.fromHex(bPub)), sharedB = await agree(b, e2ee.fromHex(aPub));
  assert.deepEqual(sharedA, sharedB);
  const blob = await e2ee.wrapKey(sharedA, guild, 1, aPub, bPub, group);
  assert.deepEqual(await e2ee.unwrapKey(sharedB, guild, 1, aPub, bPub, blob), group);
  const { b: c, bPub: cPub } = await pair();
  const sharedC = await agree(c, e2ee.fromHex(aPub));
  await assert.rejects(e2ee.unwrapKey(sharedC, guild, 1, aPub, bPub, blob));          // a third party
  await assert.rejects(e2ee.unwrapKey(sharedB, guild, 2, aPub, bPub, blob));          // another epoch
  await assert.rejects(e2ee.unwrapKey(sharedB, 'b'.repeat(32), 1, aPub, bPub, blob)); // another server
  await assert.rejects(e2ee.unwrapKey(sharedB, guild, 1, aPub, cPub, blob));          // another recipient label
  await assert.rejects(e2ee.unwrapKey(sharedB, guild, 1, aPub, bPub, 'AAAA'));
  const bytes = Buffer.from(blob, 'base64'); bytes[30] ^= 1;
  await assert.rejects(e2ee.unwrapKey(sharedB, guild, 1, aPub, bPub, bytes.toString('base64')));
});
test('the wrapped key and bodies fit the service limits', async () => {
  const { a, aPub, bPub, agree } = await pair();
  const blob = await e2ee.wrapKey(await agree(a, e2ee.fromHex(bPub)), guild, 1, aPub, bPub, e2ee.newGroupKey());
  assert.ok(blob.length <= 700, `blob ${blob.length}`);
  const text = 'é'.repeat(2000);        // composer limit; 2 bytes each in UTF-8
  const body = await e2ee.encryptMessage(e2ee.newGroupKey(), 1, guild, channel, id, text);
  assert.ok(body.length <= Math.ceil(16384 / 3) * 4, `body ${body.length}`);
});
test('signatures of other members are checked against their published key', async () => {
  const pair = await crypto.subtle.generateKey({ name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign', 'verify']);
  const pub = e2ee.toHex(new Uint8Array(await crypto.subtle.exportKey('raw', pair.publicKey)));
  const message = e2ee.textBytes('murmur/v2/envelope|x');
  const sig = e2ee.toHex(new Uint8Array(await crypto.subtle.sign({ name: 'ECDSA', hash: 'SHA-256' }, pair.privateKey, message)));
  assert.equal(await e2ee.verifyEcdsa(pub, message, sig), true);
  assert.equal(await e2ee.verifyEcdsa(pub, e2ee.textBytes('other'), sig), false);
  assert.equal(await e2ee.verifyEcdsa('zz', message, sig), false);
  assert.equal(await e2ee.verifyEcdsa(pub, message, '00'.repeat(64)), false);
  assert.equal(await e2ee.sha256Hex(new Uint8Array(0)), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
});
