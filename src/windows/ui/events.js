// The message model of a server channel. Everything that happens in a channel is an envelope; most are
// messages, some change an earlier message (edit, delete, reaction). Clients, not the service, decide
// who may change what, because the service cannot read them: an edit counts only from the author, a
// deletion from the author or from staff, a reaction from anyone. Changes can only point to earlier
// messages, by the number the service gave them, which nobody can forge or reuse.
(function (root) {
  'use strict';
  const MAX_REACTIONS = 20, MAX_SNIPPET = 90;
  const MODS = new Set(['edit', 'del', 'react']);
  const createStore = () => ({ base: new Map(), mods: new Map(), seen: new Set() });
  const kindOf = payload => (MODS.has(payload?.t) ? payload.t : 'msg');
  const text = value => (typeof value === 'string' ? value.slice(0, 4000) : '');

  // ev = { seq, id, from, ts, payload }. Returns the sequence number of the message whose row
  // changed, or null when the event is not valid (it is then ignored, never shown).
  function ingest(store, ev) {
    if (!ev || !Number.isInteger(ev.seq) || typeof ev.from !== 'string' || typeof ev.payload !== 'object' || !ev.payload || store.seen.has(ev.seq)) return null;
    const kind = kindOf(ev.payload);
    if (kind === 'msg') { store.seen.add(ev.seq); store.base.set(ev.seq, ev); return ev.seq; }
    const target = ev.payload.target;
    if (!Number.isInteger(target) || target >= ev.seq) return null;
    if (kind === 'edit' && typeof ev.payload.text !== 'string') return null;
    if (kind === 'react' && (typeof ev.payload.emoji !== 'string' || ev.payload.emoji.length === 0 || ev.payload.emoji.length > 16)) return null;
    store.seen.add(ev.seq);
    const mods = store.mods.get(target) || { edits: [], dels: [], reacts: [] };
    (kind === 'edit' ? mods.edits : kind === 'del' ? mods.dels : mods.reacts).push(ev);
    store.mods.set(target, mods);
    return target;
  }

  const snippet = value => { const flat = text(value).replace(/\s+/g, ' ').trim(); return flat.length > MAX_SNIPPET ? `${flat.slice(0, MAX_SNIPPET - 1)}…` : flat; };
  function current(store, seq, ctx) {
    const base = store.base.get(seq);
    if (!base) return null;
    const mods = store.mods.get(seq) || { edits: [], dels: [], reacts: [] };
    const deleted = mods.dels.some(d => d.from === base.from || ctx.isStaff(d.from));
    const edit = [...mods.edits].reverse().find(e => e.from === base.from);
    return { base, mods, deleted, text: deleted ? '' : text(edit ? edit.payload.text : base.payload.text), edited: !deleted && !!edit };
  }
  // The row to display for a message: the original, with valid edits, deletions and reactions folded in.
  // ctx = { me, isStaff(fp), nameOf(fp) }
  function row(store, seq, ctx) {
    const state = current(store, seq, ctx);
    if (!state) return null;
    const { base, mods, deleted } = state;
    const when = new Date(base.ts), remote = base.from !== ctx.me;
    const counts = new Map();
    for (const reaction of mods.reacts) {
      const who = counts.get(reaction.payload.emoji) || new Map();
      who.set(reaction.from, reaction.payload.on !== false);
      counts.set(reaction.payload.emoji, who);
    }
    const reactions = [];
    for (const [emoji, who] of counts) {
      const people = [...who].filter(([, on]) => on).map(([fp]) => fp);
      if (people.length && reactions.length < MAX_REACTIONS) reactions.push({ emoji, count: people.length, mine: people.includes(ctx.me), names: people.map(ctx.nameOf) });
    }
    let reply = null;
    const target = !deleted && Number.isInteger(base.payload.reply) ? base.payload.reply : null;
    if (target !== null) {
      const quoted = current(store, target, ctx);
      reply = quoted ? { seq: target, author: ctx.nameOf(quoted.base.from), text: quoted.deleted ? 'Message supprimé' : snippet(quoted.text) || '(pièce jointe)' } : { seq: target, unknown: true };
    }
    const files = !deleted && Array.isArray(base.payload.files) ? base.payload.files.filter(f => f && typeof f.blob === 'string').slice(0, 4) : [];
    const mentioned = !deleted && Array.isArray(base.payload.mentions) && base.payload.mentions.includes(ctx.me);
    return { uid: seq, seq, id: base.id, from: base.from, text: deleted ? '🗑 Message supprimé' : state.text, deleted, edited: state.edited, reactions: deleted ? [] : reactions,
      reply, files, mentionsMe: mentioned, author: remote ? ctx.nameOf(base.from) : 'Vous', remote: remote ? 1 : 0,
      hour: when.getHours(), minute: when.getMinutes(), room: 0, mediaId: 0, mediaKind: 0, mediaPending: 0, actions: true };
  }
  const ordered = store => [...store.base.keys()].sort((a, b) => a - b);

  // Plain-text search: case and accents do not matter. Deleted messages are skipped.
  const fold = value => text(value).normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
  function search(store, query, ctx, limit = 50) {
    const needle = fold(query).trim();
    if (!needle) return [];
    const hits = [];
    for (const seq of ordered(store).reverse()) {
      const state = current(store, seq, ctx);
      if (!state || state.deleted || !fold(state.text).includes(needle)) continue;
      hits.push({ seq, from: state.base.from, text: state.text, ts: state.base.ts });
      if (hits.length >= limit) break;
    }
    return hits;
  }

  const api = { createStore, ingest, row, ordered, search, kindOf, snippet };
  root.murmurEvents = api;
  if (typeof module !== 'undefined') module.exports = api;
})(typeof window !== 'undefined' ? window : globalThis);
