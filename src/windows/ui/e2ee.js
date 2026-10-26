// End-to-end encryption for servers (see docs/protocole.md, section 5).
//
// One secret group key per server. The owner wraps it for each member with a key derived from
// an ECDH agreement between their encryption keys; messages are AES-256-GCM under a per-channel
// subkey. Only standard WebCrypto primitives are used. Works in the page and in Node (tests).
(function (root) {
  'use strict';
  const subtle = root.crypto.subtle;
  const encoder = new TextEncoder(), decoder = new TextDecoder();
  const VERSION = 1, NONCE = 12, HEADER = 1 + 4 + NONCE;
  const zero = new Uint8Array(32);

  const toBase64 = bytes => btoa(Array.from(bytes, b => String.fromCharCode(b)).join(''));
  const fromBase64 = text => {
    if (typeof text !== 'string' || !/^[A-Za-z0-9+/]+={0,2}$/.test(text) || text.length % 4) throw new Error('bad base64');
    return Uint8Array.from(atob(text), c => c.charCodeAt(0));
  };
  const toHex = bytes => Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
  const fromHex = text => {
    if (typeof text !== 'string' || text.length % 2 || !/^[0-9a-f]*$/.test(text)) throw new Error('bad hex');
    return Uint8Array.from(text.match(/../g) || [], pair => parseInt(pair, 16));
  };

  async function hkdf(secret, info, length = 32) {
    const key = await subtle.importKey('raw', secret, 'HKDF', false, ['deriveBits']);
    return new Uint8Array(await subtle.deriveBits({ name: 'HKDF', hash: 'SHA-256', salt: zero, info: encoder.encode(info) }, key, length * 8));
  }
  const aesKey = (bytes, usage) => subtle.importKey('raw', bytes, 'AES-GCM', false, usage);
  const newGroupKey = () => root.crypto.getRandomValues(new Uint8Array(32));

  // ----- group key delivery -----
  // `shared` is the ECDH secret between the sender's and the recipient's encryption keys.
  const wrapInfo = (guild, epoch, senderEnc, recipientEnc) => `murmur/v2/keywrap|${guild}|${epoch}|${senderEnc}|${recipientEnc}`;
  async function wrapKey(shared, guild, epoch, senderEnc, recipientEnc, groupKey) {
    const info = wrapInfo(guild, epoch, senderEnc, recipientEnc);
    const key = await aesKey(await hkdf(shared, info), ['encrypt']);
    const nonce = root.crypto.getRandomValues(new Uint8Array(NONCE));
    const sealed = new Uint8Array(await subtle.encrypt({ name: 'AES-GCM', iv: nonce, additionalData: encoder.encode(info) }, key, groupKey));
    return toBase64(Uint8Array.from([...nonce, ...sealed]));
  }
  async function unwrapKey(shared, guild, epoch, senderEnc, recipientEnc, blob) {
    const bytes = fromBase64(blob);
    if (bytes.length !== NONCE + 32 + 16) throw new Error('bad key blob');
    const info = wrapInfo(guild, epoch, senderEnc, recipientEnc);
    const key = await aesKey(await hkdf(shared, info), ['decrypt']);
    return new Uint8Array(await subtle.decrypt({ name: 'AES-GCM', iv: bytes.slice(0, NONCE), additionalData: encoder.encode(info) }, key, bytes.slice(NONCE)));
  }

  // ----- messages -----
  // Body layout: version (1) | epoch (4, big endian) | nonce (12) | AES-GCM ciphertext and tag.
  // The additional data ties a ciphertext to its server, channel, message id and epoch, so a
  // body cannot be replayed elsewhere.
  const messageAad = (guild, channel, id, epoch) => encoder.encode(`murmur/v2/body|${guild}|${channel}|${id}|${epoch}`);
  const channelKey = async (groupKey, guild, channel, usage) => aesKey(await hkdf(groupKey, `murmur/v2/msg|${guild}|${channel}`), usage);
  function parseBody(body) {
    const bytes = fromBase64(body);
    if (bytes.length < HEADER + 16 || bytes[0] !== VERSION) throw new Error('unsupported body');
    return { epoch: new DataView(bytes.buffer, bytes.byteOffset).getUint32(1), bytes };
  }
  // The payload is a small JSON object: {v:1, text} for plain messages, plus optional fields for replies,
  // files and mentions, or {v:1, t:'edit'|'del'|'react', target, ...} for changes to an earlier message.
  async function encryptPayload(groupKey, epoch, guild, channel, id, payload) {
    const key = await channelKey(groupKey, guild, channel, ['encrypt']);
    const nonce = root.crypto.getRandomValues(new Uint8Array(NONCE));
    const plain = encoder.encode(JSON.stringify({ v: 1, ...payload }));
    if (plain.length > 15000) throw new Error('payload too large');
    const sealed = new Uint8Array(await subtle.encrypt({ name: 'AES-GCM', iv: nonce, additionalData: messageAad(guild, channel, id, epoch) }, key, plain));
    const out = new Uint8Array(HEADER + sealed.length);
    out[0] = VERSION; new DataView(out.buffer).setUint32(1, epoch); out.set(nonce, 5); out.set(sealed, HEADER);
    return toBase64(out);
  }
  async function decryptPayload(groupKey, guild, channel, id, body) {
    const { epoch, bytes } = parseBody(body);
    const key = await channelKey(groupKey, guild, channel, ['decrypt']);
    const plain = await subtle.decrypt({ name: 'AES-GCM', iv: bytes.slice(5, HEADER), additionalData: messageAad(guild, channel, id, epoch) }, key, bytes.slice(HEADER));
    const parsed = JSON.parse(decoder.decode(plain));
    if (parsed?.v !== 1 || typeof parsed !== 'object') throw new Error('unsupported payload');
    return parsed;
  }
  const encryptMessage = (groupKey, epoch, guild, channel, id, text) => encryptPayload(groupKey, epoch, guild, channel, id, { text });
  async function decryptMessage(groupKey, guild, channel, id, body) {
    const parsed = await decryptPayload(groupKey, guild, channel, id, body);
    if (typeof parsed.text !== 'string') throw new Error('unsupported payload');
    return parsed.text;
  }

  // ----- files -----
  // A file is encrypted in chunks under its own random key; the key travels inside the (encrypted)
  // message that announces the file, so the service stores chunks it can never read. A chunk is
  // nonce (12) + ciphertext + tag, at most 48,000 bytes, which is 64,000 characters in base64.
  const CHUNK_PLAIN = 48000 - NONCE - 16;
  const fileAad = (id, index, total) => encoder.encode(`murmur/v2/file|${id}|${index}|${total}`);
  async function encryptFile(bytes, id) {
    const raw = root.crypto.getRandomValues(new Uint8Array(32)), key = await aesKey(raw, ['encrypt']);
    const total = Math.max(1, Math.ceil(bytes.length / CHUNK_PLAIN)), chunks = [];
    if (total > 24) throw new Error('file too large');
    for (let index = 0; index < total; index++) {
      const nonce = root.crypto.getRandomValues(new Uint8Array(NONCE));
      const sealed = new Uint8Array(await subtle.encrypt({ name: 'AES-GCM', iv: nonce, additionalData: fileAad(id, index, total) }, key, bytes.slice(index * CHUNK_PLAIN, (index + 1) * CHUNK_PLAIN)));
      chunks.push(toBase64(Uint8Array.from([...nonce, ...sealed])));
    }
    return { key: toBase64(raw), chunks };
  }
  async function decryptFile(keyBase64, id, chunks) {
    const key = await aesKey(fromBase64(keyBase64), ['decrypt']), total = chunks.length, parts = [];
    for (let index = 0; index < total; index++) {
      const bytes = fromBase64(chunks[index]);
      if (bytes.length < NONCE + 16) throw new Error('bad chunk');
      parts.push(new Uint8Array(await subtle.decrypt({ name: 'AES-GCM', iv: bytes.slice(0, NONCE), additionalData: fileAad(id, index, total) }, key, bytes.slice(NONCE))));
    }
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let at = 0; for (const part of parts) { out.set(part, at); at += part.length; }
    return out;
  }
  // Only a few picture formats are ever shown, recognised by their first bytes rather than by name.
  function sniffImage(bytes) {
    const is = (...signature) => signature.every((b, i) => bytes[i] === b);
    if (is(0xff, 0xd8, 0xff)) return 'image/jpeg';
    if (is(0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)) return 'image/png';
    if (is(0x47, 0x49, 0x46, 0x38) && (bytes[4] === 0x37 || bytes[4] === 0x39) && bytes[5] === 0x61) return 'image/gif';
    if (is(0x52, 0x49, 0x46, 0x46) && bytes[8] === 0x57 && bytes[9] === 0x45 && bytes[10] === 0x42 && bytes[11] === 0x50) return 'image/webp';
    return null;
  }

  // ----- identities of other members -----
  const sha256Hex = async bytes => toHex(new Uint8Array(await subtle.digest('SHA-256', bytes)));
  async function verifyEcdsa(pubHex, messageBytes, sigHex) {
    try {
      const key = await subtle.importKey('raw', fromHex(pubHex), { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
      return await subtle.verify({ name: 'ECDSA', hash: 'SHA-256' }, key, fromHex(sigHex), messageBytes);
    } catch { return false; }
  }

  root.murmurCrypto = { toBase64, fromBase64, toHex, fromHex, hkdf, newGroupKey, wrapKey, unwrapKey, encryptMessage, decryptMessage, encryptPayload, decryptPayload, encryptFile, decryptFile, sniffImage,
    parseBody, sha256Hex, verifyEcdsa, textBytes: text => encoder.encode(text) };
  if (typeof module !== 'undefined') module.exports = root.murmurCrypto;
})(typeof window !== 'undefined' ? window : globalThis);
