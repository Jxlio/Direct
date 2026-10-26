const bridge = window.chrome?.webview;
const $ = id => document.getElementById(id);
const send = value => bridge?.postMessage(value);
let state = null;
let page = 'chat';
let friendSignature = '';
let audioSignature = '';
const pages = {
  chat: ['', 'Messages', 'Conversation privée'],
  friends: ['', 'Amis', 'Contacts et invitations'],
  voice: ['', 'Appels', 'Salon vocal privé'],
  network: ['', 'Réseau', 'Connexion et options manuelles'],
  servers: ['', 'Cercles', 'Créer ou rejoindre'],
  server: ['', 'Cercle', 'Salons et messages'],
  settings: ['', 'Paramètres', 'Son et préférences']
};
function openPage(next) {
  if (!pages[next]) return;
  page = next;
  document.querySelectorAll('.page').forEach(element => element.classList.toggle('active', element.id === 'page-' + next));
  document.querySelectorAll('[data-page]').forEach(element => {
    element.classList.toggle('selected', element.dataset.page === next && element.classList.contains('dock-button'));
    element.classList.toggle('active', element.dataset.page === next && element.classList.contains('side-link'));
  });
  [$('top-icon').textContent, $('top-title').textContent, $('top-subtitle').textContent] = pages[next];
  if (next === 'settings') send('audio-enumerate');
}
document.addEventListener('click', event => {
  const navigation = event.target.closest('[data-page]');
  if (navigation) openPage(navigation.dataset.page);
  const action = event.target.closest('[data-action]');
  if (action && !action.disabled) {
    if (action.dataset.action.startsWith('friend-forget:') && action.dataset.confirm !== 'yes') {
      action.dataset.confirm = 'yes';
      action.textContent = 'Confirmer ?';
      return;
    }
    send(action.dataset.action);
  }
});
function copy(text, button) {
  if (!text) return;
  const fallback = () => {
    const textarea = button.id === 'copy-code' ? $('generated-code') : $('network-outgoing');
    textarea.select();
    if (document.execCommand('copy')) button.textContent = 'Copié ✓';
  };
  if (navigator.clipboard?.writeText) navigator.clipboard.writeText(text)
    .then(() => { button.textContent = 'Copié ✓'; }).catch(fallback);
  else fallback();
}
$('copy-code').addEventListener('click', () => copy($('generated-code').value, $('copy-code')));
$('copy-network').addEventListener('click', () => copy($('network-outgoing').value, $('copy-network')));
$('create-friend').addEventListener('click', () => {
  const name = $('friend-name').value.trim();
  if (!name) { $('friend-name').focus(); return; }
  send('friend-create:' + name);
});
$('add-friend').addEventListener('click', () => {
  const name = $('friend-name').value.trim();
  const code = $('friend-invite').value.trim();
  if (!name) { $('friend-name').focus(); return; }
  if (!code) { $('friend-invite').focus(); return; }
  send('friend-add:' + name + '\n' + code);
  $('friend-invite').value = '';
});
$('call-button').addEventListener('click', () => send('voice'));
$('mute-button').addEventListener('click', () => send('mute'));
$('screen-button').addEventListener('click', () => screenLocal ? stopScreenShare() : startScreenShare());
$('screen-fullscreen').addEventListener('click', () => {
  if (document.fullscreenElement) document.exitFullscreen();
  else $('screen-stage').requestFullscreen?.();
});
$('attach-button').addEventListener('click', () => send('attach'));
$('disconnect-friend').addEventListener('click', () => send('friend-disconnect'));
$('reconnect-friend').addEventListener('click', () => openPage('friends'));
$('test-button').addEventListener('click', () => {
  const button = $('test-button');
  button.disabled = true;
  button.textContent = 'Ouverture de deux fenêtres…';
  send('test');
  setTimeout(() => {
    button.textContent = 'Lancer le test solo';
    button.disabled = false;
  }, 3000);
});
$('window-minimize').addEventListener('click', () => send('window-minimize'));
$('window-maximize').addEventListener('click', () => send('window-maximize'));
$('window-close').addEventListener('click', () => send('window-close'));
for (const header of document.querySelectorAll('.sidebar-brand, .topbar')) {
  header.addEventListener('mousedown', event => {
    if (event.button !== 0 || event.target.closest('button, a, input, select, textarea')) return;
    send(event.detail === 2 ? 'window-maximize' : 'window-drag');
  });
}
$('update-check').addEventListener('click', () => send('update-check'));
$('update-download').addEventListener('click', () => send('update-download'));
function networkFields() {
  return [$('stun-host').value.trim(), $('stun-port').value.trim(), $('turn-host').value.trim(),
    $('turn-port').value.trim(), $('turn-user').value.trim(), $('turn-password').value].join('\n');
}
$('net-invite').addEventListener('click', () => send('net-invite:' + networkFields()));
$('net-reply').addEventListener('click', () => {
  const code = $('network-received').value.trim();
  if (code) send('net-reply:' + code + '\n' + networkFields()); else $('network-received').focus();
});
$('net-apply').addEventListener('click', () => {
  const code = $('network-received').value.trim();
  if (code) send('net-apply:' + code); else $('network-received').focus();
});
$('audio-input').addEventListener('change', () => send('audio-input:' + $('audio-input').value));
$('audio-output').addEventListener('change', () => send('audio-output:' + $('audio-output').value));
for (const [id,label,action] of [['input-volume','input-volume-value','audio-in-volume'],['output-volume','output-volume-value','audio-out-volume']]) {
  $(id).addEventListener('input', () => { $(label).textContent = $(id).value + ' %'; });
  $(id).addEventListener('change', () => send(action + ':' + $(id).value));
}
$('test-mic').addEventListener('click', () => send('audio-preview-in'));
$('test-output').addEventListener('click', () => send('audio-preview-out'));
$('composer').addEventListener('submit', event => {
  event.preventDefault();
  const input = $('message-input');
  if (!state?.connected || !input.value.trim()) return;
  send('send:' + input.value);
  input.value = '';
  input.focus();
});
$('message-input').addEventListener('keydown', event => {
  if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
    event.preventDefault();
    $('composer').requestSubmit();
  }
});
const urlPattern = /https?:\/\/[^\s<>"']+/gi;
function linkInfo(raw) {
  try {
    const url = new URL(raw);
    if (url.protocol !== 'https:' && url.protocol !== 'http:') return null;
    return url;
  } catch { return null; }
}
function youtubeId(url) {
  if (url.protocol !== 'https:') return null;
  if (url.hostname === 'youtu.be') return /^[\w-]{11}$/.test(url.pathname.slice(1)) ? url.pathname.slice(1) : null;
  if (url.hostname === 'youtube.com' || url.hostname === 'www.youtube.com') {
    const id = url.pathname === '/watch' ? url.searchParams.get('v') : url.pathname.startsWith('/shorts/') ? url.pathname.split('/')[2] : null;
    return /^[\w-]{11}$/.test(id || '') ? id : null;
  }
  return null;
}
function appendLinkedText(target, text) {
  let cursor = 0, first = null;
  for (const match of text.matchAll(urlPattern)) {
    const label = match[0].replace(/[.,!?;:)\]]+$/, '');
    const url = linkInfo(label);
    if (!url) continue;
    target.append(document.createTextNode(text.slice(cursor, match.index)));
    const anchor = document.createElement('a');
    anchor.href = url.href; anchor.textContent = label;
    anchor.addEventListener('click', event => { event.preventDefault(); send('open-url:' + url.href); });
    target.append(anchor);
    cursor = match.index + label.length;
    if (!first) first = url;
  }
  target.append(document.createTextNode(text.slice(cursor)));
  return first;
}
function makeLinkPreview(url) {
  const card = document.createElement('div');
  card.className = 'link-preview';
  const host = document.createElement('span'); host.className = 'link-host'; host.textContent = url.hostname;
  const title = document.createElement('strong'); title.textContent = url.pathname === '/' ? url.hostname : (url.pathname.split('/').filter(Boolean).pop() || url.hostname).replace(/[-_]/g, ' ');
  const detail = document.createElement('small'); detail.textContent = url.href;
  card.append(host, title, detail);
  const video = youtubeId(url);
  if (video) {
    const play = document.createElement('button'); play.type = 'button'; play.textContent = 'Lire la vidéo ici';
    play.addEventListener('click', () => {
      if (card.querySelector('iframe')) return;
      const frame = document.createElement('iframe');
      frame.src = `https://www.youtube-nocookie.com/embed/${video}`;
      frame.title = 'Vidéo YouTube'; frame.allow = 'autoplay; encrypted-media; picture-in-picture';
      frame.allowFullscreen = true; frame.referrerPolicy = 'no-referrer';
      card.append(frame); play.remove();
    });
    card.append(play);
  } else {
    card.setAttribute('role', 'link'); card.tabIndex = 0;
    card.addEventListener('click', () => send('open-url:' + url.href));
    card.addEventListener('keydown', event => { if (event.key === 'Enter') send('open-url:' + url.href); });
  }
  return card;
}
// Message list: the page owns the full list; the app sends only what changed.
// Only a window of at most MAX_ROWS rows exists in the DOM, so a long history stays cheap.
const MAX_ROWS = 200, PAGE_ROWS = 50, INITIAL_ROWS = 100;
// Words that start with @ are drawn as mentions; the rest of the text keeps its links.
function appendRichText(target, text, mention) {
  let first = null, cursor = 0;
  for (const match of text.matchAll(/@[^\s@]{1,32}/g)) {
    const before = text.slice(cursor, match.index);
    if (before) first = first || appendLinkedText(target, before);
    const tag = document.createElement('span'); tag.className = 'mention-tag'; tag.textContent = match[0];
    target.append(tag); cursor = match.index + match[0].length;
  }
  const rest = text.slice(cursor);
  if (rest) first = first || appendLinkedText(target, rest);
  return first;
}
// A row is plain data. Server rows also carry what the page needs to act on them (data-action
// buttons, handled by server.js): reply, edit, delete, react, and the files to load on request.
function messageRow(message) {
  const row = document.createElement('article');
  row.className = 'message' + (message.remote ? '' : ' local') + (message.mentionsMe ? ' mentioned' : '') + (message.deleted ? ' deleted' : '');
  row.dataset.uid = message.uid;
  const avatar = document.createElement('span');
  avatar.className = 'friend-avatar';
  avatar.textContent = message.remote ? (message.author?.[0] || 'A').toUpperCase() : 'V';
  const body = document.createElement('div');
  body.className = 'message-body';
  const meta = document.createElement('div');
  meta.className = 'message-meta';
  meta.textContent = `${message.author} · ${String(message.hour).padStart(2, '0')}:${String(message.minute).padStart(2, '0')}${message.edited ? ' · modifié' : ''}`;
  const bubble = document.createElement('div');
  bubble.className = 'bubble';
  let preview = null;
  if (message.reply) {
    const quote = document.createElement('button'); quote.type = 'button'; quote.className = 'reply-quote'; quote.dataset.action = 'jump'; quote.dataset.target = message.reply.seq;
    const who = document.createElement('strong'); who.textContent = message.reply.unknown ? 'Message plus ancien' : message.reply.author;
    const what = document.createElement('span'); what.textContent = message.reply.unknown ? 'Cliquer pour le retrouver' : message.reply.text;
    quote.append(who, what); body.append(meta, quote);
  } else body.append(meta);
  if (message.mediaKind) {
    if (message.mediaId) {
      const picture = document.createElement('img');
      picture.className = 'message-media'; picture.src = `https://murmur.local/media/${message.mediaId}`;
      picture.alt = message.text; picture.loading = 'lazy';
      bubble.append(picture);
    }
    const caption = document.createElement('span');
    caption.className = 'media-caption' + (message.mediaPending < 0 ? ' error' : '');
    caption.textContent = message.text + (message.mediaPending > 0 ? ' · Envoi en cours…' : message.mediaPending < 0 ? ' · Échec de l’envoi' : !message.mediaId ? ' · Aperçu expiré' : '');
    bubble.append(caption);
    body.append(bubble);
  } else {
    if (message.text || !message.files?.length) preview = appendRichText(bubble, message.text, !!message.mentionsMe);
    body.append(bubble);
    for (const file of message.files || []) {
      const card = document.createElement('div'); card.className = 'file-card';
      if (file.url) {
        const picture = document.createElement('img'); picture.className = 'message-media'; picture.src = file.url; picture.alt = file.name || 'image';
        card.append(picture);
      } else {
        const load = document.createElement('button'); load.type = 'button'; load.dataset.action = 'file'; load.dataset.blob = file.blob;
        load.textContent = file.error ? `⚠ ${file.error}` : file.loading ? 'Chargement…' : `🖼 ${file.name || 'image'} · ${Math.max(1, Math.round((file.size || 0) / 1024))} Ko — afficher`;
        load.disabled = !!file.loading;
        card.append(load);
      }
      body.append(card);
    }
    if (preview) body.append(makeLinkPreview(preview));
  }
  if (message.reactions?.length) {
    const bar = document.createElement('div'); bar.className = 'reactions';
    for (const reaction of message.reactions) {
      const chip = document.createElement('button'); chip.type = 'button'; chip.className = 'reaction' + (reaction.mine ? ' mine' : '');
      chip.dataset.action = 'react'; chip.dataset.emoji = reaction.emoji; chip.title = reaction.names.join(', ');
      chip.textContent = `${reaction.emoji} ${reaction.count}`;
      bar.append(chip);
    }
    body.append(bar);
  }
  if (message.actions && !message.deleted) {
    const tools = document.createElement('div'); tools.className = 'message-tools';
    for (const [action, label, glyph] of [['react', 'Réagir', '😊'], ['reply', 'Répondre', '↩'], ...(!message.remote ? [['edit', 'Modifier', '✎']] : []), ['delete', 'Supprimer', '🗑']]) {
      if (action === 'delete' && message.remote && !message.canModerate) continue;
      const button = document.createElement('button'); button.type = 'button'; button.dataset.action = action; button.title = label; button.setAttribute('aria-label', label); button.textContent = glyph;
      tools.append(button);
    }
    body.append(tools);
  }
  row.append(avatar, body);
  return row;
}

// One windowed list per conversation. `change` is {reset} or {drop, update, append}.
function createThread(list, scroll, onChange = () => {}) {
  const thread = { items: [], first: 0, end: 0 };
  const rowsFor = (from, to) => {
    const fragment = document.createDocumentFragment();
    for (let i = from; i < to; i++) fragment.append(messageRow(thread.items[i]));
    return fragment;
  };
  const atBottom = () => thread.end === thread.items.length && scroll.scrollHeight - scroll.scrollTop - scroll.clientHeight < 120;
  function reset(items) {
    thread.items = items;
    thread.end = items.length;
    thread.first = Math.max(0, items.length - INITIAL_ROWS);
    list.replaceChildren(rowsFor(thread.first, thread.end));
    onChange(thread.items);
    scroll.scrollTop = scroll.scrollHeight;
  }
  function apply(change) {
    if (change.reset) { reset(change.reset); return; }
    const follow = atBottom();
    if (change.drop) {
      thread.items.splice(0, change.drop);
      const fromDom = Math.max(0, Math.min(change.drop, thread.end) - thread.first);
      for (let i = 0; i < fromDom; i++) list.firstElementChild.remove();
      thread.first = Math.max(0, thread.first - change.drop);
      thread.end = Math.max(0, thread.end - change.drop);
    }
    for (const message of change.update || []) {
      const index = thread.items.findIndex(item => item.uid === message.uid);
      if (index < 0) continue;
      thread.items[index] = message;
      if (index >= thread.first && index < thread.end) { const row = messageRow(message); row.classList.add('changed'); list.children[index - thread.first]?.replaceWith(row); }
    }
    const wasEnd = thread.end === thread.items.length;
    for (const message of change.append || []) thread.items.push(message);
    if (wasEnd) {
      const added = thread.items.length - thread.end;
      list.append(rowsFor(thread.end, thread.items.length));
      // A message that arrives while you look slides in; a whole page of history does not.
      if (added > 0 && added <= 3 && document.visibilityState === 'visible') for (let i = 0; i < added; i++) list.children[list.children.length - 1 - i]?.classList.add('fresh');
      thread.end = thread.items.length;
      while (thread.end - thread.first > MAX_ROWS && follow) { list.firstElementChild.remove(); thread.first++; }
    }
    onChange(thread.items);
    if (follow) scroll.scrollTop = scroll.scrollHeight;
  }
  // Older rows (history) go in front: the visible row must not move.
  function prepend(items) {
    if (!items.length) return;
    const before = scroll.scrollHeight, shift = items.length;
    thread.items.unshift(...items);
    if (thread.first === 0) {
      list.prepend(rowsFor(0, shift));
      thread.end += shift;
    } else { thread.first += shift; thread.end += shift; }
    while (thread.end - thread.first > MAX_ROWS) { list.lastElementChild.remove(); thread.end--; }
    scroll.scrollTop += scroll.scrollHeight - before;
    onChange(thread.items);
  }
  function loadEarlier() {
    const count = Math.min(PAGE_ROWS, thread.first);
    if (!count) return;
    const before = scroll.scrollHeight;
    list.prepend(rowsFor(thread.first - count, thread.first));
    thread.first -= count;
    scroll.scrollTop += scroll.scrollHeight - before;
    while (thread.end - thread.first > MAX_ROWS) { list.lastElementChild.remove(); thread.end--; }
  }
  function loadLater() {
    const count = Math.min(PAGE_ROWS, thread.items.length - thread.end);
    if (!count) return;
    const before = scroll.scrollHeight - scroll.scrollTop;
    list.append(rowsFor(thread.end, thread.end + count));
    thread.end += count;
    let removed = 0;
    while (thread.end - thread.first > MAX_ROWS) { list.firstElementChild.remove(); thread.first++; removed++; }
    if (removed) scroll.scrollTop = scroll.scrollHeight - before;
  }
  scroll.addEventListener('scroll', () => {
    if (scroll.scrollTop < 300) { if (thread.first > 0) loadEarlier(); else thread.onTop?.(); }
    else if (scroll.scrollHeight - scroll.scrollTop - scroll.clientHeight < 300) loadLater();
  }, { passive: true });
  return { thread, reset, apply, prepend };
}
const chatThread = createThread($('messages'), $('chat-scroll'), items => {
  $('chat-empty').hidden = items.length > 0;
  $('message-count').textContent = items.length; $('message-count').hidden = !items.length;
});
const thread = chatThread.thread;
const applyMessages = change => chatThread.apply(change);
function renderFriends(friends) {
  const signature = JSON.stringify(friends);
  if (signature === friendSignature) return;
  friendSignature = signature;
  const list = $('friends-list'), side = $('side-friends');
  list.replaceChildren(); side.replaceChildren();
  if (!friends.length) {
    const empty = document.createElement('p');
    empty.textContent = 'Aucun ami enregistré.';
    side.append(empty);
  }
  friends.forEach((friend, index) => {
    const row = document.createElement('div');
    row.className = 'friend-row';
    const avatar = document.createElement('span');
    avatar.className = 'friend-avatar';
    avatar.textContent = (friend.name[0] || 'A').toUpperCase();
    const info = document.createElement('div');
    info.className = 'friend-info';
    const title = document.createElement('strong');
    title.textContent = friend.name;
    const presence = document.createElement('small');
    presence.textContent = friend.online ? '● Disponible' : '○ En attente';
    info.append(title, presence);
    const connect = document.createElement('button');
    connect.textContent = 'Retrouver';
    connect.dataset.action = 'friend-connect:' + index;
    const code = document.createElement('button');
    code.textContent = 'Code';
    code.dataset.action = 'friend-code:' + index;
    const forget = document.createElement('button');
    forget.textContent = 'Oublier';
    forget.className = 'danger';
    forget.dataset.action = 'friend-forget:' + index;
    row.append(avatar, info, connect, code, forget);
    list.append(row);
    const shortcut = document.createElement('button');
    shortcut.className = 'side-friend';
    shortcut.dataset.page = 'chat';
    const mini = avatar.cloneNode(true);
    const name = document.createElement('span');
    name.textContent = friend.name;
    shortcut.append(mini, name);
    side.append(shortcut);
  });
}
function renderAudio(audio) {
  const signature = JSON.stringify([audio.inputs, audio.outputs]);
  if (signature !== audioSignature) {
    audioSignature = signature;
    for (const [id, names] of [['audio-input', audio.inputs], ['audio-output', audio.outputs]]) {
      const select = $(id);
      select.replaceChildren();
      names.forEach((name, index) => {
        const option = document.createElement('option');
        option.value = String(index);
        option.textContent = name;
        select.append(option);
      });
    }
  }
  $('audio-input').value = String(audio.inputChoice);
  $('audio-output').value = String(audio.outputChoice);
  $('input-volume').value = audio.inputVolume;
  $('output-volume').value = audio.outputVolume;
  $('input-volume-value').textContent = audio.inputVolume + ' %';
  $('output-volume-value').textContent = audio.outputVolume + ' %';
  $('mic-meter').style.width = audio.previewLevel + '%';
  $('gate-meter').style.width = audio.previewLevel + '%';
  if (document.activeElement !== $('gate-slider')) $('gate-slider').value = audio.gate;
  showGate(audio.gate);
  for (const [id, key] of [['audio-echo', 'echo'], ['audio-noise', 'noise'], ['audio-agc', 'agc']]) { $(id).checked = !!audio[key]; $(id).disabled = !audio.cleanupAvailable; }
  $('cleanup-note').hidden = !!audio.cleanupAvailable;
  window.murmurAudioPrefs = () => ({ echo: !!audio.echo, noise: !!audio.noise, agc: !!audio.agc, gate: audio.gate || 0 });
  $('audio-notice').textContent = audio.notice;
  for (const id of ['audio-input','audio-output','test-mic','test-output']) $(id).disabled = !!(audio.locked || audio.sandbox);
  $('test-mic').textContent = audio.previewActive ? 'Arrêter le test' : 'Tester le micro';
  $('test-output').textContent = audio.previewActive ? 'Arrêter le test' : 'Écouter un son';
}
// Who is speaking: the app sends a loudness (0..100) for our microphone and for the other side
// whenever one moves. Above this level a person counts as speaking.
const VOICE_KEY = 'murmur.voice.v1';
let speakingLevel = 12;
try { const saved = JSON.parse(localStorage.getItem(VOICE_KEY) || 'null'); if (Number.isInteger(saved?.speaking) && saved.speaking >= 3 && saved.speaking <= 50) speakingLevel = saved.speaking; } catch { /* default */ }
function showSpeaking(self, peer) {
  const mine = self >= speakingLevel, theirs = peer >= speakingLevel;
  $('self-participant').classList.toggle('speaking', mine);
  $('peer-participant').classList.toggle('speaking', theirs);
  const indicator = document.querySelector('.call-indicator');
  indicator.classList.toggle('speaking', mine || theirs);
  const note = $('voice-speaking');
  note.textContent = theirs ? `${$('voice-peer').textContent} parle` : mine ? 'Vous parlez' : '';
  note.classList.toggle('speaking-note', mine || theirs);
}
function render(data) {
  state = data;
  window.murmurServers?.onState(data);
  $('self-name').textContent = data.self;
  $('identity-fingerprint').value = data.identity || 'Indisponible';
  $('identity-card').hidden = !!data.testMode;
  $('friend-count').textContent = `${data.friends.length} / 16`;
  $('friend-card-count').textContent = `${data.friends.length} ami${data.friends.length > 1 ? 's' : ''}`;
  $('dock-online').classList.toggle('online', data.friends.some(friend => friend.online));
  $('friend-online-dot').classList.toggle('online', data.friends.some(friend => friend.online));
  $('friends-notice').textContent = data.friendsNotice;
  $('connection-pill').textContent = data.connected ? 'Connecté' : data.connecting ? 'Connexion…' : 'Hors ligne';
  $('connection-pill').classList.toggle('online', !!data.connected);
  const displayPeer = data.connected ? data.peer : data.friends.length === 1 ? data.friends[0].name : 'Messages privés';
  $('chat-peer').textContent = displayPeer;
  $('peer-avatar').textContent = (displayPeer[0] || 'A').toUpperCase();
  document.querySelector('.chat-heading').classList.toggle('empty-peer', !data.connected && data.friends.length !== 1);
  $('chat-presence').textContent = data.connected ? 'En ligne · conversation chiffrée' : data.connection || (data.friends.length ? 'En attente de connexion' : 'Aucun ami connecté');
  $('empty-title').textContent = data.connected ? 'La conversation est prête' : data.friends.length ? 'En attente de connexion' : 'Aucune conversation';
  $('empty-description').textContent = data.connected ? 'Envoyez le premier message.' : data.friends.length ? 'Vous pourrez écrire dès que votre ami sera connecté.' : 'Ajoutez un ami pour lui écrire ou l’appeler.';
  $('empty-action').hidden = !!data.connected;
  $('empty-action').textContent = data.friends.length ? 'Voir les amis' : 'Ajouter un ami';
  $('voice-small-status').textContent = data.voice;
  $('voice-status').textContent = data.voice;
  $('voice-detail').textContent = data.callIncoming && !data.callRequested ? `${data.peer} vous appelle.` : data.callRequested ? 'La conversation est ouverte à deux.' : 'Connectez un ami pour lancer un appel.';
  $('voice-peer').textContent = data.peer;
  $('voice-peer-avatar').textContent = (data.peer?.[0] || 'A').toUpperCase();
  $('peer-voice').textContent = data.connected ? 'Connecté' : 'Hors ligne';
  $('self-voice').textContent = data.muted ? 'Micro coupé' : data.callRequested ? 'Dans le salon' : 'Prêt à rejoindre';
  document.querySelector('.call-indicator').classList.toggle('live', !!data.callRequested);
  if (!data.callRequested) showSpeaking(0, 0);
  $('call-button').textContent = data.callRequested ? 'Raccrocher' : data.callIncoming ? 'Répondre' : data.testMode ? 'Tester l’appel' : 'Appeler';
  $('call-button').disabled = !data.connected;
  $('mute-button').textContent = data.muted ? 'Activer le micro' : 'Couper le micro';
  $('mute-button').disabled = !data.callRequested;
  $('screen-button').disabled = !data.screenEnabled || !data.callRequested || !data.callIncoming;
  $('screen-button').textContent = screenLocal ? 'Arrêter le partage' : 'Partager l’écran';
  if ((!data.connected || !data.callRequested) && screenPeer) stopScreenShare(true);
  $('send-button').disabled = !data.connected;
  $('attach-button').disabled = !data.connected || !!data.mediaBusy;
  $('composer-status').textContent = data.connected ? 'Entrée pour envoyer · Maj+Entrée pour une nouvelle ligne' : 'Connectez un ami pour écrire.';
  $('network-status').textContent = data.connection || (data.connected ? 'Connecté à ton ami.' : 'Hors ligne.');
  $('network-notice').textContent = data.networkNotice || (data.internetEnabled ? 'La reconnexion automatique est configurée.' : 'Ajoutez un ami pour le retrouver sur le même réseau.');
  $('network-outgoing').value = data.networkToken || '';
  $('invite-result').hidden = !data.inviteCode;
  $('generated-code').value = data.inviteCode || '';
  $('test-button').hidden = !!data.testMode;
  $('update-card').hidden = !!data.testMode;
  $('update-status').textContent = data.updateStatus || 'Aucune vérification en cours.';
  $('update-check').disabled = !!data.updateBusy;
  $('update-download').disabled = !data.updateAvailable || !!data.updateBusy;
  renderFriends(data.friends);
  renderAudio(data.audio);
}
let screenPeer = null, screenLocal = null, screenSession = '', screenStatsTimer = null;
const screenSignal = payload => send('screen-signal:' + JSON.stringify(payload));
const screenNotice = text => { $('screen-notice').textContent = text; };
function closeScreenPeer() {
  if (screenStatsTimer) clearInterval(screenStatsTimer);
  screenStatsTimer = null;
  if (screenPeer) { screenPeer.ontrack = null; screenPeer.onconnectionstatechange = null; screenPeer.close(); }
  screenPeer = null;
  $('screen-video').srcObject = null;
  $('screen-quality').textContent = '';
}
function stopScreenShare(silent = false) {
  if (!silent && screenSession) screenSignal({kind: 'stop', id: screenSession});
  if (screenLocal) screenLocal.getTracks().forEach(track => track.stop());
  screenLocal = null; screenSession = '';
  closeScreenPeer();
  $('screen-stage').hidden = true;
  $('screen-button').textContent = 'Partager l’écran';
}
function waitForIce(peer) {
  if (peer.iceGatheringState === 'complete') return Promise.resolve();
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => { peer.removeEventListener('icegatheringstatechange', changed); reject(new Error('Recherche réseau trop longue')); }, 12000);
    function changed() {
      if (peer.iceGatheringState !== 'complete') return;
      clearTimeout(timeout); peer.removeEventListener('icegatheringstatechange', changed); resolve();
    }
    peer.addEventListener('icegatheringstatechange', changed);
  });
}
function newScreenPeer() {
  const iceServers = [];
  const stunHost = $('stun-host').value.trim(), stunPort = Number($('stun-port').value);
  if (/^[a-z\d.:-]+$/i.test(stunHost) && stunPort > 0 && stunPort < 65536)
    iceServers.push({urls: `stun:${stunHost}:${stunPort}`});
  const turnHost = $('turn-host').value.trim(), turnPort = Number($('turn-port').value);
  if (/^[a-z\d.:-]+$/i.test(turnHost) && turnPort > 0 && turnPort < 65536)
    iceServers.push({urls: `turn:${turnHost}:${turnPort}`, username: $('turn-user').value, credential: $('turn-password').value});
  const peer = new RTCPeerConnection({iceServers, bundlePolicy: 'max-bundle'});
  peer.ontrack = event => {
    $('screen-video').srcObject = event.streams[0] || new MediaStream([event.track]);
    $('screen-video').play().catch(() => {});
    $('screen-stage').hidden = false;
    screenNotice('Vidéo reçue en direct.');
  };
  peer.onconnectionstatechange = () => {
    if (peer !== screenPeer) return;
    if (peer.connectionState === 'failed') screenNotice('Connexion vidéo interrompue. Relancez le partage.');
    else if (peer.connectionState === 'connected') screenNotice(screenLocal ? 'Votre écran est transmis en direct.' : 'Écran de votre ami en direct.');
  };
  screenPeer = peer;
  screenStatsTimer = setInterval(async () => {
    if (peer !== screenPeer || peer.connectionState !== 'connected') return;
    try {
      const reports = await peer.getStats();
      for (const report of reports.values()) {
        if ((screenLocal && report.type !== 'outbound-rtp') || (!screenLocal && report.type !== 'inbound-rtp') || report.kind !== 'video') continue;
        const width = report.frameWidth || 0, height = report.frameHeight || 0;
        const fps = report.framesPerSecond || 0;
        $('screen-quality').textContent = width && height ? `${width} × ${height} · ${Math.round(fps)} i/s` : 'Connexion vidéo active';
        break;
      }
    } catch (_) { /* A closed connection has no stats. */ }
  }, 2000);
  return peer;
}
async function startScreenShare() {
  if (!state?.connected || !state.callRequested || !state.callIncoming || !navigator.mediaDevices?.getDisplayMedia) {
    screenNotice('Le partage vidéo est indisponible sur cette version de Windows.');
    $('screen-stage').hidden = false; return;
  }
  try {
    const stream = await navigator.mediaDevices.getDisplayMedia({audio: false, video: {
      frameRate: {ideal: 60, max: 60}, width: {ideal: 1920}, height: {ideal: 1080}
    }});
    if (!state?.connected || !state.callRequested) { stream.getTracks().forEach(track => track.stop()); return; }
    stopScreenShare(true);
    screenLocal = stream; screenSession = crypto.randomUUID();
    const track = stream.getVideoTracks()[0];
    track.contentHint = 'detail';
    track.addEventListener('ended', () => { if (screenLocal === stream) stopScreenShare(); }, {once: true});
    const peer = newScreenPeer();
    const sender = peer.addTrack(track, stream);
    const transceiver = peer.getTransceivers().find(item => item.sender === sender);
    const codecs = RTCRtpSender.getCapabilities?.('video')?.codecs || [];
    const h264 = codecs.filter(codec => codec.mimeType.toLowerCase() === 'video/h264');
    if (transceiver && h264.length) transceiver.setCodecPreferences([
      ...h264, ...codecs.filter(codec => codec.mimeType.toLowerCase() !== 'video/h264')
    ]);
    const params = sender.getParameters();
    params.degradationPreference = 'maintain-resolution';
    if (params.encodings?.length) {
      params.encodings[0].maxBitrate = 8000000;
      params.encodings[0].maxFramerate = 60;
      try { await sender.setParameters(params); } catch (_) { /* Encoder may choose its own limit. */ }
    }
    $('screen-stage').hidden = false;
    $('screen-video').srcObject = stream;
    screenNotice('Connexion vidéo en cours…');
    $('screen-button').textContent = 'Arrêter le partage';
    await peer.setLocalDescription(await peer.createOffer());
    await waitForIce(peer);
    if (peer === screenPeer) screenSignal({kind: 'offer', id: screenSession, sdp: peer.localDescription.sdp});
  } catch (error) {
    stopScreenShare(true);
    if (error.name !== 'NotAllowedError') {
      $('screen-stage').hidden = false;
      screenNotice(`Partage impossible : ${error.message || 'erreur de capture'}.`);
    }
  }
}
async function receiveScreenSignal(raw) {
  if (!state?.connected || !state.callRequested || typeof raw !== 'string' || raw.length > 8188) return;
  let message;
  try { message = JSON.parse(raw); } catch (_) { return; }
  if (!message || typeof message.id !== 'string' || message.id.length > 80) return;
  if (message.kind === 'stop') { if (message.id === screenSession && !screenLocal) stopScreenShare(true); return; }
  if (typeof message.sdp !== 'string' || message.sdp.length > 7900) return;
  try {
    if (message.kind === 'offer') {
      if (screenLocal) return;
      stopScreenShare(true);
      screenSession = message.id;
      const peer = newScreenPeer();
      $('screen-stage').hidden = false;
      screenNotice('Connexion à l’écran de votre ami…');
      await peer.setRemoteDescription({type: 'offer', sdp: message.sdp});
      await peer.setLocalDescription(await peer.createAnswer());
      await waitForIce(peer);
      if (peer === screenPeer) screenSignal({kind: 'answer', id: screenSession, sdp: peer.localDescription.sdp});
    } else if (message.kind === 'answer' && screenLocal && message.id === screenSession && screenPeer) {
      await screenPeer.setRemoteDescription({type: 'answer', sdp: message.sdp});
    }
  } catch (error) { screenNotice(`Connexion vidéo impossible : ${error.message || 'erreur réseau'}.`); }
}
function handle(data) {
  if (data?.type === 'state') render(data);
  else if (data?.type === 'messages') applyMessages(data);
  else if (data?.type === 'screen-signal') receiveScreenSignal(data.payload);
  else if (data?.type === 'levels') showSpeaking(data.self, data.peer);
  else if (data?.type === 'service' || data?.type === 'signature' || data?.type === 'ecdh') window.murmurServers?.handle(data);
}
bridge?.addEventListener('message', event => handle(event.data));
// Outside the app (a plain browser tab) there is no bridge: expose the entry point for UI tests.
if (!bridge) window.murmurUi = { handle, thread };
if (pages[location.hash.slice(1)]) openPage(location.hash.slice(1));
send('ready');

// ----- appearance -----
(function appearance() {
  const T = window.murmurTheme;
  if (!T) return;
  let look = T.load();
  const choose = patch => { look = T.apply({ ...look, ...patch }); paint(); };
  function radio(container, items, current, make, pick) {
    container.replaceChildren();
    for (const item of items) {
      const button = make(item);
      button.setAttribute('role', 'radio'); button.type = 'button';
      button.setAttribute('aria-checked', String(current(item)));
      button.addEventListener('click', () => pick(item));
      container.append(button);
    }
  }
  function paint() {
    radio($('theme-grid'), T.THEMES, ([id]) => id === look.theme, ([id, name]) => {
      const button = document.createElement('button');
      button.className = 'theme-option theme-preview'; button.dataset.theme = id;
      const preview = document.createElement('span'); preview.className = 'tp';
      const side = document.createElement('span'); side.className = 'tp-side';
      const main = document.createElement('span'); main.className = 'tp-main';
      main.append(document.createElement('i'), document.createElement('i'));
      preview.append(side, main);
      const label = document.createElement('span'); label.textContent = name;
      button.append(preview, label);
      return button;
    }, ([id, , signature]) => choose(signature ? { theme: id, accent: signature } : { theme: id }));
    radio($('accent-row'), T.ACCENTS, ([hex]) => hex === look.accent, ([hex, name]) => {
      const button = document.createElement('button');
      button.className = 'accent-swatch'; button.title = name; button.setAttribute('aria-label', name);
      button.style.background = hex;
      return button;
    }, ([hex]) => choose({ accent: hex }));
    if (!$('accent-custom')) {
      const wrap = document.createElement('label'); wrap.className = 'accent-custom'; wrap.textContent = 'Autre';
      const input = document.createElement('input'); input.type = 'color'; input.id = 'accent-custom'; input.setAttribute('aria-label', 'Couleur personnalisée');
      input.addEventListener('input', () => choose({ accent: input.value }));
      wrap.append(input); $('accent-row').after(wrap); wrap.style.marginTop = '10px';
    }
    radio($('accent2-row'), T.ACCENTS, ([hex]) => hex === look.accent2, ([hex, name]) => {
      const button = document.createElement('button');
      button.className = 'accent-swatch'; button.title = name; button.setAttribute('aria-label', name);
      button.style.background = hex;
      return button;
    }, ([hex]) => choose({ accent2: hex }));
    radio($('logo-row'), T.LOGOS, ([id]) => id === look.logo, ([id, name]) => {
      const button = document.createElement('button');
      button.className = 'logo-choice'; button.title = name; button.setAttribute('aria-label', name);
      const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); svg.setAttribute('class', 'logo-preview');
      const use = document.createElementNS('http://www.w3.org/2000/svg', 'use'); use.setAttribute('href', `#logo-${id}`);
      svg.append(use); button.append(svg);
      return button;
    }, ([id]) => choose({ logo: id }));
    $('accent-custom').value = look.accent;
    radio($('scale-row'), T.SCALES, ([scale]) => scale === look.scale, ([, name]) => { const b = document.createElement('button'); b.textContent = name; return b; }, ([scale]) => choose({ scale }));
  }
  $('appearance-reset').addEventListener('click', () => choose({ ...T.DEFAULTS }));
  $('theme-open').addEventListener('click', () => send('theme-open'));
  $('theme-reload').addEventListener('click', () => { $('user-css').href = `user.css?v=${Date.now()}`; });
  paint();
})();

// ----- personal voice thresholds -----
function showGate(value) {
  $('gate-value').textContent = Number(value) ? `${value} %` : 'Désactivé';
  $('gate-marker').style.display = Number(value) ? 'block' : 'none';
  $('gate-marker').style.left = `${Number(value)}%`;
}
function sendCleanup() {
  send(`audio-cleanup:${$('audio-echo').checked ? 1 : 0}|${$('audio-noise').checked ? 1 : 0}|${$('audio-agc').checked ? 1 : 0}|${$('gate-slider').value}`);
}
for (const id of ['audio-echo', 'audio-noise', 'audio-agc']) $(id).addEventListener('change', sendCleanup);
$('gate-slider').addEventListener('input', () => showGate($('gate-slider').value));
$('gate-slider').addEventListener('change', sendCleanup);
$('speaking-slider').value = speakingLevel; $('speaking-value').textContent = speakingLevel;
$('speaking-slider').addEventListener('input', () => {
  speakingLevel = Number($('speaking-slider').value); $('speaking-value').textContent = speakingLevel;
  try { localStorage.setItem(VOICE_KEY, JSON.stringify({ speaking: speakingLevel })); } catch { /* kept for this session */ }
});
window.murmurVoicePrefs = { speaking: () => speakingLevel };

// ----- interface sounds -----
(function sounds() {
  const S = window.murmurSound;
  if (!S) return;
  const prefs = S.get();
  $('sound-on').checked = prefs.on;
  $('sound-volume').value = String(Math.round(prefs.volume * 100));
  const label = () => { $('sound-volume-value').textContent = `${$('sound-volume').value} %`; $('sound-volume').disabled = !$('sound-on').checked; };
  label();
  $('sound-on').addEventListener('change', () => { S.set({ on: $('sound-on').checked }); label(); S.play('receive'); });
  $('sound-volume').addEventListener('input', () => { S.set({ volume: $('sound-volume').value / 100 }); label(); });
  $('sound-volume').addEventListener('change', () => S.play('receive'));
  $('sound-test').addEventListener('click', () => { for (const [i, name] of ['send', 'receive', 'mention', 'join', 'leave'].entries()) setTimeout(() => S.play(name), i * 450); });
})();
