// Murmur service protocol v2. The service never reads message contents: it checks
// signatures, sizes and permissions, orders envelopes and stores them opaque.
export const PROTOCOL = 2;
export const LIMITS = Object.freeze({
  envelopeBytes: 16384,        // raw body, before base64
  nameLength: 64,
  displayName: 32,
  epochs: 32,                  // group keys a member can fetch at once
  channels: 200,
  historyPage: 50,
  inviteUses: 100,
  inviteTtlSeconds: 7 * 24 * 3600,
  burst: 20,                   // token bucket: 5 requests/second, bursts of 20
  refillPerSecond: 5,
  members: 100,                // encrypted channels are capped until MLS cost is measured
  frameBytes: 24000,
  keyBlobBase64: 700,          // a wrapped group key: nonce + 32-byte key + tag, base64
  blobFrameBytes: 70000,       // only file chunks may use frames this large
  blobChunkBase64: 64000,      // one encrypted chunk of an attachment (48 KB before base64)
  blobChunks: 24,              // at most ~1.1 MB per attachment
  blobBytes: 64 * 1024 * 1024, // all attachments of one server, as stored
  voiceParticipants: 8,        // audio is a mesh between participants, which stops scaling beyond this
  voiceSignalBase64: 16000,    // one encrypted session description
});

const encoder = new TextEncoder();
export const bytes = (text) => encoder.encode(text);

export function hexToBytes(hex, length) {
  if (typeof hex !== 'string' || hex.length !== length * 2 || !/^[0-9a-f]+$/.test(hex)) return null;
  const out = new Uint8Array(length);
  for (let i = 0; i < length; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}
export const bytesToHex = (array) => Array.from(array, (b) => b.toString(16).padStart(2, '0')).join('');
export const randomHex = (length) => bytesToHex(crypto.getRandomValues(new Uint8Array(length)));

export async function fingerprint(publicKey) {
  return bytesToHex(new Uint8Array(await crypto.subtle.digest('SHA-256', publicKey)));
}
// Signatures are raw r||s over SHA-256, the format produced by the Windows client.
export async function verify(publicKey, message, signature) {
  if (publicKey.length !== 65 || publicKey[0] !== 4 || signature.length !== 64) return false;
  try {
    const key = await crypto.subtle.importKey('raw', publicKey, { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
    return await crypto.subtle.verify({ name: 'ECDSA', hash: 'SHA-256' }, key, signature, message);
  } catch {
    return false;
  }
}
export const authMessage = (guild, nonce) => bytes(`murmur/v2/auth|${guild}|${nonce}`);
export const nameMessage = (guild, name) => bytes(`murmur/v2/name|${guild}|${name}`);
export const encMessage = (guild, enc) => bytes(`murmur/v2/enc|${guild}|${enc}`);
export const roleMessage = (guild, fp, role, serial) => bytes(`murmur/v2/role|${guild}|${fp}|${role}|${serial}`);
export const ownerMessage = (guild, to, serial) => bytes(`murmur/v2/owner|${guild}|${to}|${serial}`);
export const voiceMessage = (guild, channel, to, id, body) => bytes(`murmur/v2/voice|${guild}|${channel}|${to}|${id}|${body}`);
export const envelopeMessage = (guild, channel, id, body) => bytes(`murmur/v2/envelope|${guild}|${channel}|${id}|${body}`);

export const isId = (value) => typeof value === 'string' && /^[0-9A-Za-z]{8,40}$/.test(value);
export const isChannelId = (value) => typeof value === 'string' && /^[0-9A-Za-z]{3,40}$/.test(value);
export const isDisplayName = (value) => typeof value === 'string' && value.trim() === value && value.length > 0 && [...value].length <= LIMITS.displayName && !/[\u0000-\u001f\u007f\u200b-\u200f\u2028-\u202e\u2066-\u2069]/.test(value);
export const isName = (value) => typeof value === 'string' && value.trim().length > 0 && value.length <= LIMITS.nameLength && !/[\u0000-\u001f\u007f]/.test(value);
export function isBase64(value, maxLength = Math.ceil(LIMITS.envelopeBytes / 3) * 4) {
  return typeof value === 'string' && value.length > 0 && value.length <= maxLength && /^[A-Za-z0-9+/]+={0,2}$/.test(value) && value.length % 4 === 0;
}
