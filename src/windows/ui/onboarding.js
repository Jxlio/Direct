// First launch: three short steps (welcome, you, your first circle). It appears only for someone with no name
// and no circle yet, can be skipped, and can be replayed from Settings. Nothing is created until the last step.
(function () {
  'use strict';
  const KEY = 'murmur.onboarded.v1';
  const $ = id => document.getElementById(id);
  const root = $('onboarding');
  if (!root) return;
  const T = window.murmurTheme;
  let step = 0, mode = 'create';
  const done = () => { try { localStorage.setItem(KEY, '1'); } catch { /* shown again next time */ } };

  function close() { root.hidden = true; document.body.classList.remove('onboarding-open'); done(); }
  function show(next) {
    step = Math.max(0, Math.min(2, next));
    root.querySelectorAll('[data-step]').forEach(panel => { panel.hidden = Number(panel.dataset.step) !== step; });
    root.querySelectorAll('.ob-dots i').forEach((dot, index) => dot.classList.toggle('on', index <= step));
    root.querySelector('.ob-art').dataset.art = String(step);
    const focus = [$('ob-start'), $('ob-name'), mode === 'create' ? $('ob-circle') : $('ob-invite')][step];
    focus?.focus();
  }
  function open() {
    const known = window.murmurServers?.api.servers()[0]?.origin;
    if (known && !$('ob-service').value) $('ob-service').value = known;
    $('ob-advanced').open = !$('ob-service').value;
    root.hidden = false; document.body.classList.add('onboarding-open'); $('ob-error').textContent = ''; $('ob-name').value = window.murmurServers?.api.me() || ''; show(0); }

  function swatches(container, colors, current, pick) {
    container.replaceChildren();
    for (const [hex, name] of colors) {
      const button = document.createElement('button');
      button.type = 'button'; button.className = 'ob-swatch'; button.title = name; button.setAttribute('aria-label', name);
      button.setAttribute('aria-checked', String(hex === current())); button.setAttribute('role', 'radio');
      button.style.background = hex;
      button.addEventListener('click', () => { pick(hex); paint(); });
      container.append(button);
    }
  }
  function paint() {
    const look = T.load();
    swatches($('ob-accent'), T.ACCENTS, () => T.load().accent, hex => T.apply({ ...T.load(), accent: hex }));
    swatches($('ob-accent2'), T.ACCENTS, () => T.load().accent2, hex => T.apply({ ...T.load(), accent2: hex }));
    root.querySelectorAll('[data-ob-theme]').forEach(button => button.setAttribute('aria-checked', String(button.dataset.obTheme === look.theme)));
    root.querySelectorAll('[data-mode]').forEach(tab => { tab.setAttribute('aria-selected', String(tab.dataset.mode === mode)); });
    $('ob-create').hidden = mode !== 'create'; $('ob-join').hidden = mode !== 'join';
    $('ob-finish').textContent = mode === 'create' ? 'Créer mon cercle' : 'Rejoindre';
  }

  async function finish() {
    const api = window.murmurServers?.api;
    const error = $('ob-error');
    error.textContent = '';
    if (!api) { error.textContent = 'L’application n’est pas prête, réessayez dans un instant.'; return; }
    const problem = mode === 'create'
      ? ($('ob-service').value.trim() ? api.createServer($('ob-service').value, $('ob-circle').value) : 'Indiquez l’adresse du service qui hébergera le cercle.')
      : api.joinServer($('ob-invite').value);
    if (problem) { error.textContent = problem; return; }
    close();
    window.openPage?.('server');
  }

  $('ob-start').addEventListener('click', () => show(1));
  root.querySelectorAll('[data-skip]').forEach(button => button.addEventListener('click', close));
  root.querySelectorAll('[data-back]').forEach(button => button.addEventListener('click', () => show(step - 1)));
  $('ob-next').addEventListener('click', async () => {
    const name = $('ob-name').value.trim();
    const problem = await window.murmurServers?.api.setName(name);
    if (problem) { $('ob-error').textContent = problem; return; }
    $('ob-error').textContent = '';
    show(2);
  });
  $('ob-name').addEventListener('keydown', event => { if (event.key === 'Enter') { event.preventDefault(); $('ob-next').click(); } });
  root.querySelectorAll('[data-ob-theme]').forEach(button => button.addEventListener('click', () => { T.apply({ ...T.load(), theme: button.dataset.obTheme }); paint(); }));
  root.querySelectorAll('[data-mode]').forEach(tab => tab.addEventListener('click', () => { mode = tab.dataset.mode; $('ob-error').textContent = ''; paint(); show(2); }));
  $('ob-finish').addEventListener('click', finish);
  document.addEventListener('keydown', event => { if (event.key === 'Escape' && !root.hidden) close(); });
  $('onboarding-replay')?.addEventListener('click', () => { paint(); open(); });

  paint();
  window.murmurOnboarding = { open, close };
  // Only for somebody who has nothing yet.
  let seen = false;
  try { seen = !!localStorage.getItem(KEY); } catch { /* treated as new */ }
  const api = window.murmurServers?.api;
  if (!seen && api && !api.me() && api.servers().length === 0) open();
})();
