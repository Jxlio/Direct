// Interface sounds, made on the spot with the Web Audio API: no file to ship, nothing to download.
// Each sound is one or two short notes with a soft attack. The person chooses on/off and the volume
// (Settings → Sons de l'interface); the choice stays on this device.
(function (root) {
  'use strict';
  const KEY = 'murmur.sounds.v1';
  let prefs = { on: true, volume: 0.5 };
  try { const saved = JSON.parse(root.localStorage.getItem(KEY)); if (saved) prefs = { on: saved.on !== false, volume: Math.min(1, Math.max(0, Number(saved.volume) || 0)) }; } catch { /* defaults */ }
  let ctx = null, last = 0;
  const context = () => {
    if (!ctx) { const Audio = root.AudioContext || root.webkitAudioContext; if (!Audio) return null; ctx = new Audio({ latencyHint: 'interactive' }); }
    if (ctx.state === 'suspended') ctx.resume().catch(() => {});
    return ctx;
  };
  // [frequency Hz, start s, length s, wave, relative loudness]
  const SOUNDS = {
    send:      [[523, 0, .07, 'sine', 1], [784, .05, .09, 'sine', .8]],
    receive:   [[659, 0, .09, 'sine', 1], [880, .08, .14, 'sine', .9]],
    tick:      [[1200, 0, .035, 'triangle', .5]],
    mention:   [[784, 0, .1, 'sine', 1], [988, .09, .1, 'sine', 1], [1319, .18, .2, 'sine', .9]],
    join:      [[440, 0, .09, 'triangle', 1], [660, .09, .14, 'triangle', 1]],
    leave:     [[660, 0, .09, 'triangle', 1], [392, .09, .16, 'triangle', .9]],
    mute:      [[330, 0, .1, 'square', .35]],
    unmute:    [[494, 0, .1, 'square', .35]],
    deafen:    [[392, 0, .08, 'square', .35], [262, .08, .12, 'square', .35]],
    undeafen:  [[262, 0, .08, 'square', .35], [392, .08, .12, 'square', .35]],
    open:      [[330, 0, .06, 'sine', .6], [494, .05, .1, 'sine', .6]],
    close:     [[494, 0, .05, 'sine', .5], [330, .05, .09, 'sine', .5]],
    click:     [[900, 0, .025, 'triangle', .4]],
    error:     [[196, 0, .12, 'sawtooth', .35], [156, .1, .16, 'sawtooth', .35]],
  };
  function play(name, gain = 1) {
    const notes = SOUNDS[name];
    if (!prefs.on || !prefs.volume || !notes) return;
    const now = root.performance.now();
    if (now - last < 35) return;                       // never a machine-gun of sounds
    last = now;
    const audio = context();
    if (!audio) return;
    const start = audio.currentTime + 0.005;
    for (const [freq, at, length, wave, loud] of notes) {
      const osc = audio.createOscillator(), amp = audio.createGain();
      osc.type = wave; osc.frequency.value = freq;
      const peak = Math.max(0.0002, 0.16 * prefs.volume * prefs.volume * loud * gain);
      amp.gain.setValueAtTime(0.0001, start + at);
      amp.gain.exponentialRampToValueAtTime(peak, start + at + 0.012);
      amp.gain.exponentialRampToValueAtTime(0.0001, start + at + length);
      osc.connect(amp).connect(audio.destination);
      osc.start(start + at); osc.stop(start + at + length + 0.02);
    }
  }
  function set(next) {
    prefs = { on: next.on !== undefined ? !!next.on : prefs.on, volume: next.volume !== undefined ? Math.min(1, Math.max(0, Number(next.volume) || 0)) : prefs.volume };
    try { root.localStorage.setItem(KEY, JSON.stringify(prefs)); } catch { /* kept for this session */ }
  }
  root.murmurSound = { play, set, get: () => ({ ...prefs }), names: Object.keys(SOUNDS) };

  // A tick under the pointer for the controls people press all day, and the settings panel's swish.
  root.document.addEventListener('pointerdown', event => {
    if (event.target.closest?.('.primary-button, .side-link, .channel, .secondary-button, .ss-nav button, .theme-option, .accent-swatch')) play('click');
  }, true);
})(window);
