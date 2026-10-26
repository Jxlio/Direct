// The interface sounds: every name plays something, the person's choices are respected and kept,
// and nothing throws where there is no audio.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function load(storage = {}, withAudio = true) {
  const played = [];
  let clock = 1000;
  class FakeContext {
    constructor() { this.currentTime = 0; this.state = 'running'; this.destination = {}; }
    resume() { return Promise.resolve(); }
    createOscillator() { const o = { frequency: {}, connect(n) { return n; }, start() { played.push(o.frequency.value); }, stop() {} }; return o; }
    createGain() { return { gain: { setValueAtTime() {}, exponentialRampToValueAtTime() {} }, connect(n) { return n; } }; }
  }
  const window = {
    localStorage: { getItem: (k) => storage[k] ?? null, setItem: (k, v) => { storage[k] = v; } },
    performance: { now: () => (clock += 100) },
    document: { addEventListener() {} },
  };
  if (withAudio) window.AudioContext = FakeContext;
  vm.runInNewContext(fs.readFileSync(path.resolve(__dirname, '../../src/windows/ui/sounds.js'), 'utf8'), { window, Math, Number, JSON, Promise });
  return { sound: window.murmurSound, played, storage };
}

test('every sound makes at least one note', () => {
  const { sound, played } = load();
  assert.ok(sound.names.length >= 12);
  for (const name of sound.names) { const before = played.length; sound.play(name); assert.ok(played.length > before, name); }
});

test('off means silent, and the choice is kept on this device', () => {
  const { sound, played, storage } = load();
  sound.set({ on: false });
  sound.play('send');
  assert.equal(played.length, 0);
  assert.equal(JSON.parse(storage['murmur.sounds.v1']).on, false);
  sound.set({ on: true, volume: 0 });
  sound.play('send');
  assert.equal(played.length, 0, 'volume zero is silent too');
  assert.equal(load(storage).sound.get().on, true);
});

test('volume is clamped and saved settings are not trusted', () => {
  const { sound } = load();
  sound.set({ volume: 7 }); assert.equal(sound.get().volume, 1);
  sound.set({ volume: -3 }); assert.equal(sound.get().volume, 0);
  assert.equal(load({ 'murmur.sounds.v1': '{"on":"yes","volume":"abc"}' }).sound.get().volume, 0);
  assert.equal(load({ 'murmur.sounds.v1': 'not json' }).sound.get().on, true);
});

test('without audio support or an unknown name, nothing throws', () => {
  load({}, false).sound.play('send');
  load().sound.play('nope');
});

test('sounds that come too fast are thinned out', () => {
  const clock = load();
  const first = clock.played.length;
  clock.sound.play('tick');
  assert.equal(clock.played.length, first + 1);
});
