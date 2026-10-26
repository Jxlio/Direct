// Voice channels: a call between the people who joined a channel, up to eight, as a mesh of direct
// WebRTC connections. The service only introduces them: it relays session descriptions that are
// signed by the sender and encrypted for the server's members (so it can neither read nor alter
// them), and never sees audio. Echo cancellation, noise suppression and automatic gain come from the
// browser engine, switched on or off by each person's own settings.
(() => {
  'use strict';
  const C = window.murmurCrypto;
  const A = () => window.murmurServers.api;
  const POLL = 100, HOLD = 3, STATS_EVERY = 2000, ICE_WAIT = 8000, RETRIES = 3;
  const DEFAULT_ICE = [{ urls: 'stun:stun.cloudflare.com:3478' }];
  let context = null, timer = 0, statsTimer = 0, relay = null;     // shared audio context, level poll, ICE servers from the service
  const $v = id => document.getElementById(id);
  const stateOf = server => server.voice;
  const presence = server => (server.presence = server.presence || new Map());     // channel -> Set of fingerprints in it
  const loudness = peak => Math.min(100, Math.round(100 * Math.sqrt(peak)));

  // ----- level meter on a stream: who is speaking -----
  function meter(stream) {
    context = context || new AudioContext();
    if (context.state === 'suspended') context.resume().catch(() => {});
    const analyser = context.createAnalyser();
    analyser.fftSize = 512;
    const source = context.createMediaStreamSource(stream);
    source.connect(analyser);
    const data = new Uint8Array(analyser.fftSize);
    return { read() { analyser.getByteTimeDomainData(data); let peak = 0; for (const v of data) peak = Math.max(peak, Math.abs(v - 128) / 128); return loudness(peak); }, close() { source.disconnect(); } };
  }

  // ----- signalling -----
  async function iceServers(server) {
    if (relay === null) {
      relay = [];
      try { const answer = await A().request(server, { t: 'turn.get' }); if (Array.isArray(answer.iceServers)) relay = answer.iceServers.map(item => ({ ...item, urls: [].concat(item?.urls || []).filter(u => typeof u === 'string' && /^(stun|turn|turns):[A-Za-z0-9.:_?=&-]+$/.test(u)) })).filter(item => item.urls.length); } catch { /* direct connections only */ }
    }
    if (relay.length) return relay;
    const fields = ['stun-host', 'stun-port', 'turn-host', 'turn-port'].map(id => document.getElementById(id)?.value.trim() || '');
    const own = [];
    if (/^[a-z\d.:-]+$/i.test(fields[0]) && Number(fields[1]) > 0) own.push({ urls: `stun:${fields[0]}:${fields[1]}` });
    if (/^[a-z\d.:-]+$/i.test(fields[2]) && Number(fields[3]) > 0) own.push({ urls: `turn:${fields[2]}:${fields[3]}`, username: document.getElementById('turn-user').value, credential: document.getElementById('turn-password').value });
    return own.length ? own : DEFAULT_ICE;
  }
  async function send(server, fp, payload) {
    const call = stateOf(server), key = server.keys.get(server.epoch);
    if (!call || !key) throw new Error('Pas de clé de groupe.');
    const id = A().randomHex(8), body = await C.encryptPayload(key, server.epoch, server.guild, call.channel, id, payload);
    const sig = await A().sign(`murmur/v2/voice|${server.guild}|${call.channel}|${fp}|${id}|${body}`);
    await A().request(server, { t: 'voice.signal', channel: call.channel, to: fp, id, body, sig });
  }
  async function receive(server, message) {
    const call = stateOf(server), member = server.members.get(message.from);
    if (!call || message.channel !== call.channel || !member) return;
    const signed = await C.verifyEcdsa(member.pub, C.textBytes(`murmur/v2/voice|${server.guild}|${message.channel}|${server.you}|${message.id}|${message.body}`), message.sig);
    if (!signed) return;
    let payload;
    try {
      const key = server.keys.get(C.parseBody(message.body).epoch);
      payload = key && await C.decryptPayload(key, server.guild, message.channel, message.id, message.body);
    } catch { return; }
    if (!payload) return;
    let peer = call.peers.get(message.from);
    if (payload.type === 'offer') {
      if (peer && peer.pc.connectionState === 'connected') return;            // an old offer replayed
      if (peer) closePeer(call, message.from);
      peer = createPeer(server, message.from, false);
      try {
        await peer.pc.setRemoteDescription({ type: 'offer', sdp: payload.sdp });
        await peer.pc.setLocalDescription(await peer.pc.createAnswer());
        await gathered(peer.pc);
        await send(server, message.from, { type: 'answer', sdp: peer.pc.localDescription.sdp });
      } catch (error) { peerFailed(server, message.from, error.message); }
    } else if (payload.type === 'answer' && peer?.initiator && peer.pc.signalingState === 'have-local-offer') {
      try { await peer.pc.setRemoteDescription({ type: 'answer', sdp: payload.sdp }); } catch (error) { peerFailed(server, message.from, error.message); }
    } else if (payload.type === 'state' && peer) {
      peer.muted = !!payload.muted; peer.deafened = !!payload.deafened; draw(server);
    }
  }
  const gathered = pc => new Promise((resolve, reject) => {
    if (pc.iceGatheringState === 'complete') { resolve(); return; }
    const timeout = setTimeout(() => { pc.removeEventListener('icegatheringstatechange', check); resolve(); }, ICE_WAIT);   // what was found so far is used
    function check() { if (pc.iceGatheringState === 'complete') { clearTimeout(timeout); pc.removeEventListener('icegatheringstatechange', check); resolve(); } }
    pc.addEventListener('icegatheringstatechange', check);
  });

  // ----- one connection to one other person -----
  function createPeer(server, fp, initiator) {
    const call = stateOf(server);
    const pc = new RTCPeerConnection({ iceServers: call.ice, bundlePolicy: 'max-bundle' });
    const peer = { fp, pc, initiator, level: 0, muted: false, deafened: false, volume: 1, audio: null, meter: null, stats: null, tries: 0, last: null };
    for (const track of call.stream.getAudioTracks()) pc.addTrack(track, call.stream);
    pc.ontrack = event => {
      const stream = event.streams[0] || new MediaStream([event.track]);
      peer.audio = new Audio(); peer.audio.autoplay = true; peer.audio.srcObject = stream; peer.audio.volume = peer.volume; peer.audio.muted = call.deafened;
      if (call.sink && peer.audio.setSinkId) peer.audio.setSinkId(call.sink).catch(() => {});
      $v('voice-audio').append(peer.audio);
      peer.meter = meter(stream);
      draw(server);
    };
    pc.onconnectionstatechange = () => {
      if (pc.connectionState === 'failed') peerFailed(server, fp, 'connexion impossible');
      draw(server);
    };
    pc.addEventListener('connectionstatechange', () => { if (pc.connectionState === 'connected') window.murmurSound?.play('join'); });
    call.peers.set(fp, peer);
    return peer;
  }
  function closePeer(call, fp) {
    const peer = call.peers.get(fp);
    if (!peer) return;
    call.peers.delete(fp); window.murmurSound?.play('leave');
    peer.meter?.close(); peer.audio?.remove(); if (peer.audio) peer.audio.srcObject = null;
    peer.pc.ontrack = peer.pc.onconnectionstatechange = null; peer.pc.close();
  }
  async function offerTo(server, fp, tries = 0) {
    const call = stateOf(server);
    if (!call || !call.participants.has(fp)) return;
    closePeer(call, fp);
    const peer = createPeer(server, fp, true); peer.tries = tries;
    try {
      await peer.pc.setLocalDescription(await peer.pc.createOffer());
      await gathered(peer.pc);
      await send(server, fp, { type: 'offer', sdp: peer.pc.localDescription.sdp });
    } catch (error) { peerFailed(server, fp, error.message); }
  }
  function peerFailed(server, fp, why) {
    const call = stateOf(server), peer = call?.peers.get(fp);
    if (!call || !peer) return;
    peer.failure = why; draw(server);
    // The person who offered tries again a few times; the other side simply waits for the new offer.
    if (peer.initiator && peer.tries < RETRIES) setTimeout(() => { if (stateOf(server) === call) offerTo(server, fp, peer.tries + 1); }, 3000 * (peer.tries + 1));
  }

  // ----- joining and leaving -----
  async function join(server, channel) {
    if (stateOf(server)) await leave(server);
    const prefs = window.murmurAudioPrefs?.() || {};
    const notice = text => { $v('voice-notice').textContent = text; };
    if (!server.keys.get(server.epoch)) { notice('La clé du groupe n’est pas encore arrivée.'); return; }
    notice('Ouverture du micro…');
    let stream;
    try {
      stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: prefs.echo !== false, noiseSuppression: prefs.noise !== false, autoGainControl: !!prefs.agc, channelCount: 1 } });
    } catch (error) { notice(error.name === 'NotAllowedError' ? 'Le micro est refusé par Windows.' : 'Aucun micro disponible.'); return; }
    const call = { channel, stream, peers: new Map(), participants: new Set(), ice: await iceServers(server), muted: false, deafened: false, hold: 0, level: 0, sink: '' };
    server.voice = call;
    call.meter = meter(stream);
    try {
      const answer = await A().request(server, { t: 'voice.join', channel });
      presence(server).set(channel, new Set([...answer.participants, server.you]));
      for (const fp of answer.participants) call.participants.add(fp);
      notice('');
      // The newcomer calls everybody already there.
      await Promise.all(answer.participants.map(fp => offerTo(server, fp)));
    } catch (error) {
      notice(error.reply?.reason === 'full' ? 'Ce salon est complet (8 personnes).' : error.message);
      await leave(server, false);
      return;
    }
    if (!timer) timer = setInterval(poll, POLL);
    if (!statsTimer) statsTimer = setInterval(gatherStats, STATS_EVERY);
    announce(server);
    A().render();
  }
  async function leave(server, tell = true) {
    const call = stateOf(server);
    if (!call) return;
    server.voice = null;
    for (const fp of [...call.peers.keys()]) closePeer(call, fp);
    call.meter?.close();
    for (const track of call.stream.getTracks()) track.stop();
    if (tell && server.status === 'ready') { try { await A().request(server, { t: 'voice.leave' }); } catch { /* the service drops presence when the connection ends */ } }
    presence(server).get(call.channel)?.delete(server.you);
    if (![...A().servers()].some(s => s.voice)) { clearInterval(timer); clearInterval(statsTimer); timer = statsTimer = 0; }
    $v('voice-notice').textContent = ''; $v('voice-stats').textContent = '';
    A().render();
  }
  const announce = server => {
    const call = stateOf(server);
    if (call) for (const fp of call.peers.keys()) send(server, fp, { type: 'state', muted: call.muted, deafened: call.deafened }).catch(() => {});
  };

  // ----- what is heard and said, 10 times a second -----
  function poll() {
    for (const server of A().servers()) {
      const call = stateOf(server);
      if (!call) continue;
      const gate = window.murmurAudioPrefs?.().gate || 0, level = call.meter.read();
      // The microphone gate: below the person's own threshold nothing is sent, with a short tail so word endings survive.
      call.hold = level >= gate ? HOLD : Math.max(0, call.hold - 1);
      const open = !gate || call.hold > 0;
      for (const track of call.stream.getAudioTracks()) track.enabled = !call.muted && open;
      call.level = call.muted || !open ? 0 : level;
      for (const peer of call.peers.values()) peer.level = peer.deafened || !peer.meter ? 0 : peer.meter.read();
      draw(server);
    }
  }
  async function gatherStats() {
    for (const server of A().servers()) {
      const call = stateOf(server);
      if (!call) continue;
      for (const peer of call.peers.values()) {
        if (peer.pc.connectionState !== 'connected') continue;
        try {
          const report = await peer.pc.getStats(), now = performance.now();
          let rtt = null, lost = 0, received = 0, jitter = 0, bytes = 0, path = '';
          for (const item of report.values()) {
            if (item.type === 'candidate-pair' && item.nominated && item.state === 'succeeded') { rtt = item.currentRoundTripTime; const local = report.get(item.localCandidateId); path = local?.candidateType === 'relay' ? 'relais' : 'direct'; }
            if (item.type === 'inbound-rtp' && item.kind === 'audio') { lost = item.packetsLost || 0; received = item.packetsReceived || 0; jitter = item.jitter || 0; bytes = item.bytesReceived || 0; }
          }
          const rate = peer.last ? Math.round(((bytes - peer.last.bytes) * 8) / (now - peer.last.at)) : 0;
          peer.last = { bytes, at: now };
          peer.stats = { rtt: rtt === null ? null : Math.round(rtt * 1000), loss: received + lost ? Math.round(1000 * lost / (received + lost)) / 10 : 0, jitter: Math.round(jitter * 1000), kbps: rate, path };
        } catch { /* a closing connection has no statistics */ }
      }
      draw(server);
    }
  }

  // ----- drawing -----
  function nameOf(server, fp) { return A().nameOf(server, fp); }
  function draw(server) {
    const selected = A().selected();
    if (selected !== server) return;
    const call = stateOf(server);
    const viewing = server.viewVoice && server.channels.find(c => c.id === server.viewVoice);
    const channel = call ? server.channels.find(c => c.id === call.channel) : viewing;
    $v('voice-panel').hidden = !channel;
    if (!channel) return;
    $v('voice-title').textContent = `🔊 ${channel.name}`;
    const people = $v('voice-people');
    const gone = new Set(presence(server).get(channel.id) || []);
    const rows = [...gone].filter(fp => fp !== server.you || call);
    people.replaceChildren();
    for (const fp of rows) {
      const me = fp === server.you, peer = call?.peers.get(fp);
      const row = document.createElement('div'); row.className = 'voice-person';
      const level = me ? call?.level || 0 : peer?.level || 0;
      const avatar = document.createElement('span'); avatar.className = 'friend-avatar' + (level >= (window.murmurVoicePrefs?.speaking() || 12) ? ' speaking' : '');
      avatar.textContent = (nameOf(server, fp)[0] || '?').toUpperCase();
      const name = document.createElement('span'); name.className = 'voice-name';
      const state = me ? '' : !call ? '' : !peer ? ' · …' : peer.pc.connectionState === 'connected' ? '' : peer.failure ? ' · connexion impossible' : ' · connexion…';
      name.textContent = nameOf(server, fp) + (me && call?.muted ? ' 🔇' : '') + (peer?.muted ? ' 🔇' : '') + (peer?.deafened ? ' 🎧✕' : '') + state;
      row.append(avatar, name);
      if (peer && !me) {
        const volume = document.createElement('input'); volume.type = 'range'; volume.min = 0; volume.max = 150; volume.value = Math.round(peer.volume * 100);
        volume.setAttribute('aria-label', `Volume de ${nameOf(server, fp)}`); volume.title = 'Volume de cette personne (chez vous seulement)';
        volume.addEventListener('input', () => { peer.volume = volume.value / 100; if (peer.audio) peer.audio.volume = Math.min(1, peer.volume); });
        row.append(volume);
      }
      people.append(row);
    }
    $v('voice-join').hidden = !!call; $v('voice-mute').hidden = $v('voice-deafen').hidden = $v('voice-leave').hidden = !call;
    $v('voice-mute').textContent = call?.muted ? '🔇 Micro coupé' : '🎙 Micro';
    $v('voice-deafen').textContent = call?.deafened ? '🎧✕ Son coupé' : '🎧 Son';
    const stats = [...(call?.peers.values() || [])].filter(p => p.stats).map(p => `${nameOf(server, p.fp)} : ${p.stats.path}, ${p.stats.rtt ?? '?'} ms, perte ${p.stats.loss} %, ${p.stats.kbps} kb/s`);
    $v('voice-stats').textContent = stats.join(' · ');
    $v('voice-notice').textContent = $v('voice-notice').textContent || (call && call.peers.size === 0 && call.participants.size === 0 ? 'Vous êtes seul dans le salon.' : '');
  }

  window.murmurVoice = {
    open(server, channel) { server.viewVoice = channel; draw(server); },
    // A voice channel in the list shows who is in it.
    decorate(server, channel, list) {
      const people = presence(server).get(channel.id);
      if (!people || !people.size) return;
      const box = document.createElement('div'); box.className = 'channel-people';
      for (const fp of people) { const line = document.createElement('span'); line.textContent = nameOf(server, fp); box.append(line); }
      list.append(box);
    },
    render(server) { draw(server); },
    onFrame(server, message) {
      if (message.t === 'voice') {
        const set = presence(server).get(message.channel) || new Set();
        if (message.joined) set.add(message.fp); else set.delete(message.fp);
        presence(server).set(message.channel, set);
        const call = stateOf(server);
        if (call && message.channel === call.channel) {
          if (message.joined) call.participants.add(message.fp);
          else { call.participants.delete(message.fp); closePeer(call, message.fp); }
        }
        A().render();
      } else if (message.t === 'voice.signal') receive(server, message);
    },
    // After the connection is (re)established: learn who is in each voice channel, and rejoin the call we were in.
    async onReady(server) {
      const wanted = server.wanted;
      server.wanted = null;
      for (const channel of server.channels.filter(c => c.type === 'voice')) presence(server).set(channel.id, new Set(server.initialPresence?.[channel.id] || []));
      A().render();
      if (wanted && !stateOf(server)) join(server, wanted);
    },
    onClose(server) {
      const call = stateOf(server);
      if (call) { server.wanted = call.channel; leave(server, false); }
      presence(server).clear(); relay = null;
    },
    leave(server) { return leave(server, true); },
    join, stats: () => [...A().servers()].map(s => s.voice && [...s.voice.peers.values()].map(p => ({ fp: p.fp, state: p.pc.connectionState, level: p.level, stats: p.stats, muted: p.muted }))),
    state: server => stateOf(server) && { channel: stateOf(server).channel, muted: stateOf(server).muted, level: stateOf(server).level, peers: stateOf(server).peers.size },
  };

  $v('voice-join').addEventListener('click', () => { const server = A().selected(); if (server?.viewVoice) join(server, server.viewVoice); });
  $v('voice-leave').addEventListener('click', () => { const server = A().selected(); if (server) leave(server); });
  $v('voice-mute').addEventListener('click', () => { const server = A().selected(), call = server && stateOf(server); if (call) { call.muted = !call.muted; window.murmurSound?.play(call.muted ? 'mute' : 'unmute'); announce(server); draw(server); } });
  $v('voice-deafen').addEventListener('click', () => {
    const server = A().selected(), call = server && stateOf(server);
    if (!call) return;
    call.deafened = !call.deafened; window.murmurSound?.play(call.deafened ? 'deafen' : 'undeafen');
    for (const peer of call.peers.values()) if (peer.audio) peer.audio.muted = call.deafened;
    announce(server); draw(server);
  });
})();
