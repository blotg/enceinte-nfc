'use strict';

/* ================= Outils ================= */

const $ = (sel, el = document) => el.querySelector(sel);
const root = $('#root');

function h(tag, props, ...kids) {
  const el = document.createElement(tag);
  if (props) {
    for (const [k, v] of Object.entries(props)) {
      if (v === null || v === undefined || v === false) continue;
      if (k === 'class') el.className = v;
      else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
      else if (k in el && typeof v !== 'string') el[k] = v;
      else if (k === 'value') el.value = v;
      else el.setAttribute(k, v === true ? '' : v);
    }
  }
  for (const kid of kids.flat()) {
    if (kid === null || kid === undefined || kid === false) continue;
    el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  }
  return el;
}

/* replaceChildren sans les enfants vides (null deviendrait le texte « null »). */
function setKids(el, ...kids) {
  el.replaceChildren(...kids.flat().filter((k) => k !== null && k !== undefined && k !== false));
}

class AuthError extends Error {}

async function api(path, opts = {}) {
  const init = { method: opts.method || 'GET', headers: {}, credentials: 'same-origin', cache: 'no-store' };
  if (init.method !== 'GET') init.headers['X-Requested-With'] = 'enceinte';
  if (opts.body !== undefined) {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(opts.body);
  }
  let res;
  try {
    res = await fetch(path, init);
  } catch (e) {
    throw new Error('Enceinte injoignable');
  }
  let data = {};
  try { data = await res.json(); } catch (e) { /* réponse vide */ }
  if (res.status === 401 && !opts.public) {
    showLogin();
    throw new AuthError(data.error || 'Connexion requise');
  }
  if (res.status === 403 && data.https) {
    location.href = data.https; /* HTTPS activé : on bascule */
    throw new AuthError('Passage en HTTPS');
  }
  if (!res.ok) throw new Error(data.error || ('Erreur ' + res.status));
  return data;
}

const post = (path, body) => api(path, { method: 'POST', body: body || {} });

let toastTimer = null;
function toast(msg, bad = false) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'toast show' + (bad ? ' bad' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = 'toast'; }, bad ? 5000 : 2500);
}

function reportError(e) {
  if (!(e instanceof AuthError)) toast(e.message || String(e), true);
}

/* Exécute une action asynchrone en désactivant le bouton pendant ce temps. */
async function busy(btn, fn) {
  if (btn) btn.disabled = true;
  try { return await fn(); } catch (e) { reportError(e); } finally { if (btn) btn.disabled = false; }
}

function fmtTime(s) {
  s = Math.max(0, Math.floor(s || 0));
  const hh = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
  const mm = hh ? String(m).padStart(2, '0') : String(m);
  return (hh ? hh + ':' : '') + mm + ':' + String(r).padStart(2, '0');
}

function fmtSize(b) {
  if (b < 1024) return b + ' o';
  const u = ['Ko', 'Mo', 'Go', 'To'];
  let i = -1;
  do { b /= 1024; i++; } while (b >= 1024 && i < u.length - 1);
  return b.toFixed(b < 10 ? 1 : 0).replace('.', ',') + ' ' + u[i];
}

function fmtDelay(s) {
  if (!s) return 'sans limite';
  if (s < 3600) return Math.round(s / 60) + ' min';
  const hh = s / 3600;
  return (Number.isInteger(hh) ? hh : hh.toFixed(1).replace('.', ',')) + ' h';
}

const DELAY_CHOICES = [0, 60, 300, 600, 1800, 3600, 10800, 86400];
const SOUND_LEVELS = ['désactivée', 'légère', 'moyenne', 'forte'];
const GENERAL_DEFAULTS = { resume_s: 600, resume_after_other: false, shuffle: false, repeat: false, normalize: 2, compress: 2 };
const KIND_ICON = { folder: '📁', radio: '📻', podcast: '🎙' };
const KIND_LABEL = { radio: 'Webradio', podcast: 'Podcast' };
const PODCAST_KEEP = { min: 1, max: 50, def: 10 };

function fmtDate(epoch) {
  return new Date(epoch * 1000).toLocaleString('fr-FR', { dateStyle: 'medium', timeStyle: 'short' });
}

const isUrl = (u) => /^https?:\/\/\S+$/i.test(u);

const isSet = (v) => v !== null && v !== undefined;

/* Réglages propres à une carte, en clair (vide : réglages généraux). */
function cardRules(c) {
  const rules = [];
  if (isSet(c.shuffle)) rules.push(c.shuffle ? 'ordre aléatoire' : 'dans l\'ordre');
  if (isSet(c.repeat)) rules.push(c.repeat ? 'playlist en boucle' : 's\'arrête à la fin');
  const other = !isSet(c.resume_other) ? '' : c.resume_other ? 'même après une autre carte' : 'sauf si une autre carte est posée';
  if (isSet(c.resume_s)) rules.push(`progression conservée ${fmtDelay(c.resume_s)}` + (other ? ', ' + other : ''));
  else if (other) rules.push('progression conservée ' + other);
  if (isSet(c.normalize)) rules.push('normalisation ' + SOUND_LEVELS[c.normalize]);
  if (isSet(c.compress)) rules.push('compression ' + SOUND_LEVELS[c.compress]);
  if (c.sleep_tracks) rules.push(`mode sommeil : ${plural(c.sleep_tracks, 'morceau', 'morceaux')}`);
  else if (c.sleep_minutes) rules.push(`mode sommeil : ${fmtMinutes(c.sleep_minutes)}`);
  return rules;
}

function plural(n, one, many) { return `${n} ${n > 1 ? many : one}`; }
function fmtMinutes(m) {
  return m < 60 ? `${m} min` : `${Math.floor(m / 60)} h` + (m % 60 ? ' ' + String(m % 60).padStart(2, '0') : '');
}

const SLEEP_MAX = { tracks: 999, minutes: 720 };
const SLEEP_DEFAULT = { tracks: 3, minutes: 30 };

function joinPath(a, b) { return a ? a + '/' + b : b; }
function baseName(p) { return p.split('/').pop(); }

const AUDIO_EXT = ['mp3', 'aac', 'm4a', 'mp4', 'flac', 'wav', 'ogg', 'oga', 'opus'];
const EXTRA_EXT = ['jpg', 'jpeg', 'png', 'txt', 'm3u'];
function uploadable(name) {
  if (name.startsWith('.')) return false;
  const ext = name.split('.').pop().toLowerCase();
  return AUDIO_EXT.includes(ext) || EXTRA_EXT.includes(ext);
}

function passwordInput(autocomplete) {
  return h('input', { type: 'password', autocomplete, autocapitalize: 'off', spellcheck: false });
}

/* ================= Version de l'interface ================= */

/* Identifiant du firmware qui a servi cette page (balise meta insérée par l'enceinte). */
const PAGE_BUILD = (document.querySelector('meta[name=build]') || {}).content || '';

/*
 * L'enceinte a changé de firmware (mise à jour) depuis le chargement de la page : on
 * recharge pour obtenir la nouvelle interface. Une seule fois par firmware, au cas où le
 * navigateur servirait encore l'ancienne page.
 */
function reloadIfUpdated(build) {
  if (!build || !PAGE_BUILD || PAGE_BUILD.startsWith('{{') || build === PAGE_BUILD) return false;
  try {
    if (sessionStorage.getItem('reloaded-for') === build) return false;
    sessionStorage.setItem('reloaded-for', build);
  } catch (e) { /* stockage indisponible : on recharge quand même */ }
  stopPolling();
  toast('Nouvelle version de l\'enceinte : rechargement de l\'interface…');
  setTimeout(() => location.reload(), 800);
  return true;
}

/* ================= État global ================= */

const app = {
  state: null,
  status: null,
  tab: location.hash.slice(1) || 'play',
  path: '',
  timer: null,
  view: null, // éléments de la vue courante mis à jour par le rafraîchissement
  uploads: [],
  uploading: false,
  selected: new Set(), // chemins sélectionnés dans l'onglet Musique
};

function stopPolling() {
  clearInterval(app.timer);
  app.timer = null;
}

function startPolling() {
  stopPolling();
  refreshStatus();
  app.timer = setInterval(refreshStatus, 2000);
}

async function refreshStatus() {
  if (app.uploading) return; // le serveur traite les envois un par un
  try {
    app.status = await api('/api/status');
  } catch (e) {
    if (!(e instanceof AuthError)) setChips(null);
    return;
  }
  if (!app.timer) return; // déconnecté entre-temps
  if (reloadIfUpdated(app.status.build)) return;
  setChips(app.status);
  setAlerts(app.status);
  if (app.view && app.view.update) app.view.update(app.status);
}

/* Messages affichés sur tous les onglets : réseau Wi-Fi de nouveau à portée, adresse IP à l'essai. */
function setAlerts(st) {
  const box = $('#alerts');
  const w = st && st.wifi;
  const list = [];
  if (w && w.ip_test_remaining > 0) {
    list.push(h('div', { class: 'notice warn' },
      `Nouvelle adresse IP à l'essai${w.ip_test_address ? ' (' + w.ip_test_address + ')' : ''}. `,
      'Elle sera conservée dès qu\'une connexion administrateur aura lieu à cette adresse ; sinon l\'enceinte reviendra à l\'ancienne configuration dans ',
      h('b', null, fmtTime(w.ip_test_remaining)), '.'));
  }
  if (w && w.sta_available && w.on_ap && !app.switchDismissed) {
    const go = h('button', { class: 'small primary' }, 'Basculer maintenant');
    go.addEventListener('click', () => busy(go, async () => {
      await post('/api/wifi/switch');
      showSwitching(w);
    }));
    const later = h('button', { class: 'small', onclick: () => { app.switchDismissed = true; setAlerts(app.status); } }, 'Plus tard');
    list.push(h('div', { class: 'notice' },
      `Le Wi-Fi « ${w.ssid} » est de nouveau disponible. L'enceinte s'y reconnectera d'elle-même dès que plus aucun appareil ne sera connecté à son point d'accès et qu'aucun envoi ne sera en cours. Basculer maintenant ?`,
      h('div', { class: 'row actions' }, go, later)));
  }
  setKids(box, ...list);
}

function showSwitching(w) {
  stopPolling();
  $('#tabs').hidden = true;
  setKids($('#alerts'));
  app.view = null;
  setKids(root, h('div', { class: 'card stack' },
    h('h2', null, 'Bascule vers le Wi-Fi de la maison'),
    h('p', { class: 'pulse' }, `L'enceinte se connecte au réseau « ${w.ssid} » et va quitter son point d'accès.`),
    h('p', null, 'Reconnectez ce téléphone ou cet ordinateur au Wi-Fi « ', h('b', null, w.ssid), ' », puis ouvrez ',
      h('b', null, `http://${w.hostname}.local`), '.'),
    h('button', { onclick: boot }, 'Recharger')));
}

function setChips(st) {
  const chips = $('#chips');
  if (!st) {
    setChipList(chips, [h('span', { class: 'chip bad' }, 'Hors ligne')]);
    return;
  }
  const w = st.wifi;
  const list = [];
  if (w.connected) list.push(h('span', { class: 'chip ok', title: w.ip }, 'Wi-Fi'));
  else if (w.ap) list.push(h('span', { class: 'chip warn' }, 'Point d\'accès'));
  else list.push(h('span', { class: 'chip bad' }, 'Wi-Fi'));
  list.push(h('span', { class: 'chip ' + (st.sd.mounted ? 'ok' : 'bad') }, 'SD'));
  const reader = st.card.reader_ok
    ? 'Lecteur NFC : ' + (st.card.reader || 'détecté')
    : 'Aucun lecteur NFC détecté (PN5180 ou PN532)';
  list.push(h('span', { class: 'chip ' + (st.card.reader_ok ? 'ok' : 'bad'), title: reader }, 'NFC'));
  setChipList(chips, list);
  $('#dev-name').textContent = w.hostname || 'Enceinte';
}

/* Les puces ne sont remplacées que si elles changent : une info-bulle survolée reste affichée. */
function setChipList(chips, list) {
  const sig = list.map((c) => c.outerHTML).join('');
  if (chips.dataset.sig === sig) return;
  chips.dataset.sig = sig;
  setKids(chips, ...list);
}

/* ================= Démarrage ================= */

async function boot() {
  stopPolling();
  try {
    app.state = await api('/api/state', { public: true });
  } catch (e) {
    setKids(root, h('div', { class: 'card' },
      h('p', null, 'L\'enceinte ne répond pas.'),
      h('button', { onclick: boot }, 'Réessayer')));
    return;
  }
  if (reloadIfUpdated(app.state.build)) return;
  $('#dev-name').textContent = app.state.hostname;
  if (app.state.setup_required) showSetup();
  else if (!app.state.logged_in) showLogin();
  else showApp();
}

/* ================= Première configuration ================= */

function wifiPicker() {
  const select = h('select');
  const manual = h('input', { type: 'text', placeholder: 'Nom du réseau', autocapitalize: 'off', spellcheck: false, hidden: true });
  const pass = passwordInput('off');
  const scanBtn = h('button', { type: 'button', class: 'small' }, 'Rechercher');
  const fill = (nets) => {
    const opts = [h('option', { value: '' }, nets ? '— Choisir un réseau —' : 'Recherche en cours…')];
    for (const n of nets || []) {
      opts.push(h('option', { value: n.ssid }, `${n.ssid}  (${n.rssi > -60 ? 'excellent' : n.rssi > -72 ? 'bon' : 'faible'}${n.secure ? '' : ', ouvert'})`));
    }
    opts.push(h('option', { value: '__manual' }, 'Autre réseau…'));
    setKids(select, ...opts);
  };
  const scan = () => busy(scanBtn, async () => {
    fill(null);
    try {
      const r = await api('/api/wifi/scan');
      fill(r.networks);
      if (!r.networks.length) toast('Aucun réseau trouvé, réessayez');
    } catch (e) {
      fill([]);
      throw e;
    }
  });
  scanBtn.addEventListener('click', scan);
  select.addEventListener('change', () => { manual.hidden = select.value !== '__manual'; if (!manual.hidden) manual.focus(); });
  fill([]);
  return {
    el: h('div', null,
      h('label', null, 'Réseau'),
      h('div', { class: 'row' }, h('div', { class: 'grow' }, select), scanBtn),
      manual,
      h('label', null, 'Mot de passe du Wi-Fi'), pass),
    scan,
    ssid: () => (select.value === '__manual' ? manual.value.trim() : select.value),
    password: () => pass.value,
  };
}

function showSetup() {
  $('#tabs').hidden = true;
  app.view = null;
  const pw = passwordInput('new-password');
  const pw2 = passwordInput('new-password');
  const host = h('input', { type: 'text', value: app.state.hostname, autocapitalize: 'off', spellcheck: false });
  const wifi = wifiPicker();
  const err = h('p', { class: 'error-text' });
  const submit = h('button', { class: 'primary' }, 'Terminer la configuration');
  submit.addEventListener('click', () => busy(submit, async () => {
    err.textContent = '';
    if (pw.value.length < 6) { err.textContent = 'Le mot de passe doit contenir au moins 6 caractères.'; return; }
    if (pw.value !== pw2.value) { err.textContent = 'Les deux mots de passe sont différents.'; return; }
    const ssid = wifi.ssid();
    try {
      await post('/api/setup', { password: pw.value, hostname: host.value.trim(), ssid, wifi_password: wifi.password() });
    } catch (e) {
      err.textContent = e.message;
      return;
    }
    if (ssid) showConnecting(ssid, host.value.trim().toLowerCase());
    else boot();
  }));
  setKids(root, 
    h('h1', null, 'Bienvenue'),
    h('p', { class: 'muted' }, 'Deux étapes pour configurer votre enceinte.'),
    h('div', { class: 'card' },
      h('h2', null, '1. Mot de passe administrateur'),
      h('p', { class: 'muted small' }, 'Il protège cette page de configuration.'),
      h('label', null, 'Mot de passe ', h('span', { class: 'muted' }, '(6 caractères minimum)')), pw,
      h('label', null, 'Confirmation'), pw2,
      h('label', null, 'Nom de l\'enceinte ', h('span', { class: 'muted' }, '(adresse nom.local)')), host),
    h('div', { class: 'card' },
      h('h2', null, '2. Réseau Wi-Fi'),
      h('p', { class: 'muted small' }, 'Le réseau de la maison. Vous pourrez aussi le régler plus tard.'),
      wifi.el),
    err,
    h('div', { class: 'row end' }, submit));
  wifi.scan();
}

function showConnecting(ssid, hostname) {
  const msg = h('p', { class: 'pulse' }, `Connexion au réseau « ${ssid} »…`);
  const info = h('div');
  setKids(root, h('div', { class: 'card stack' },
    h('h2', null, 'Configuration enregistrée'), msg, info,
    h('p', { class: 'muted small' },
      'Si cette page ne répond plus, reconnectez votre téléphone à votre Wi-Fi habituel, puis ouvrez ',
      h('b', null, `http://${hostname}.local`), '.')));
  let tries = 0;
  const poll = async () => {
    tries++;
    try {
      const st = await api('/api/status');
      if (st.wifi.connected) {
        msg.className = 'ok-text';
        msg.textContent = `Connectée à « ${ssid} ».`;
        setKids(info, 
          h('p', null, 'L\'enceinte est accessible à l\'adresse ', h('b', null, `http://${st.wifi.hostname}.local`),
            ` ou http://${st.wifi.ip}`),
          h('button', { class: 'primary', onclick: boot }, 'Continuer'));
        return;
      }
    } catch (e) { /* le point d'accès change de canal pendant la connexion */ }
    if (tries > 30) {
      msg.className = 'error-text';
      msg.textContent = 'La connexion n\'a pas abouti. Vérifiez le mot de passe du Wi-Fi.';
      setKids(info, h('button', { onclick: boot }, 'Revenir aux réglages'));
      return;
    }
    setTimeout(poll, 2000);
  };
  setTimeout(poll, 3000);
}

/* ================= Connexion ================= */

function showLogin() {
  stopPolling();
  $('#tabs').hidden = true;
  app.view = null;
  const pw = passwordInput('current-password');
  const err = h('p', { class: 'error-text' });
  const btn = h('button', { class: 'primary', type: 'submit' }, 'Se connecter');
  const form = h('form', { class: 'card' },
    h('h2', null, 'Connexion'),
    h('label', null, 'Mot de passe administrateur'), pw, err,
    h('div', { class: 'row end actions' }, btn));
  form.addEventListener('submit', (ev) => {
    ev.preventDefault();
    busy(btn, async () => {
      try {
        await post('/api/login', { password: pw.value });
        boot();
      } catch (e) {
        err.textContent = e.message;
      }
    });
  });
  setKids(root, form);
  pw.focus();
}

/* ================= Application ================= */

function showApp() {
  $('#tabs').hidden = false;
  switchTab(app.tab);
  startPolling();
}

const TABS = ['play', 'cards', 'music', 'settings'];

function switchTab(tab) {
  clearInterval(app.touchTimer);
  if (!TABS.includes(tab)) tab = 'play';
  app.tab = tab;
  if (location.hash !== '#' + tab) history.replaceState(null, '', '#' + tab);
  for (const b of document.querySelectorAll('#tabs button')) b.classList.toggle('active', b.dataset.tab === tab);
  app.view = null;
  const views = { play: renderPlay, cards: renderCards, music: renderMusic, settings: renderSettings };
  Promise.resolve(views[tab]()).catch(reportError);
}

for (const b of document.querySelectorAll('#tabs button')) {
  b.addEventListener('click', () => switchTab(b.dataset.tab));
}

/* ---------------- Lecture ---------------- */

function renderPlay() {
  const v = {};
  v.title = h('div', { class: 'now-title' }, '—');
  v.sub = h('div', { class: 'now-sub' });
  v.elapsed = h('span', null, '0:00');
  v.duration = h('span', null, '0:00');
  v.seek = h('input', { type: 'range', min: 0, max: 1, step: 1, value: 0, 'aria-label': 'Position' });
  v.seek.addEventListener('input', () => { v.dragging = true; v.elapsed.textContent = fmtTime(v.seek.value); });
  v.seek.addEventListener('change', () => {
    v.dragging = false;
    post('/api/player', { action: 'seek', value: Number(v.seek.value) }).catch(reportError);
  });
  const cmd = (action) => () => post('/api/player', { action }).then(refreshStatus).catch(reportError);
  v.toggle = h('button', { class: 'main', 'aria-label': 'Lecture / pause', onclick: cmd('toggle') }, '▶');
  /* Curseur de 0 au volume maximum : il ne peut pas aller au-delà. */
  v.vol = h('input', { type: 'range', min: 0, max: 100, step: 1, 'aria-label': 'Volume' });
  v.volLabel = h('span', { class: 'vol-val' });
  v.volMax = h('span', { title: 'Volume maximum, réglable dans Réglages → Enceinte' });
  v.sound = h('p', { class: 'small muted', hidden: true });
  v.sleep = h('p', { class: 'small muted', hidden: true });
  let volTimer = null;
  v.vol.addEventListener('input', () => {
    v.volDragging = true;
    v.volLabel.textContent = v.vol.value;
    clearTimeout(volTimer);
    volTimer = setTimeout(() => {
      post('/api/player', { action: 'volume', value: Number(v.vol.value) }).catch(reportError);
      v.volDragging = false;
    }, 150);
  });
  v.card = h('div', { class: 'notice' });
  v.err = h('div', { class: 'notice bad', hidden: true });

  v.update = (st) => {
    const p = st.player;
    const playing = p.state === 'play';
    v.title.textContent = p.state === 'stop' && !p.file ? 'Aucune lecture' : (p.title || baseName(p.file));
    v.sub.textContent = [p.artist, p.album].filter(Boolean).join(' — ') ||
      (p.queue_len ? `Morceau ${p.song + 1} sur ${p.queue_len}` : '');
    if (p.queue_len && (p.artist || p.album)) v.sub.textContent += ` · ${p.song + 1}/${p.queue_len}`;
    v.toggle.textContent = playing ? '❚❚' : '▶';
    v.seek.disabled = !p.seekable || p.state === 'stop';
    v.seek.max = Math.max(1, Math.round(p.duration));
    if (!v.dragging) {
      v.seek.value = Math.round(p.elapsed);
      v.elapsed.textContent = fmtTime(p.elapsed);
    }
    v.duration.textContent = p.duration ? fmtTime(p.duration) : '–:––';
    if (p.stream) {
      v.duration.textContent = 'en direct';
      v.sub.textContent = 'Webradio' + (st.card.folder ? ' · ' + baseName(st.card.folder) : '');
    }
    if (!v.volDragging) {
      if (Number(v.vol.max) !== p.max_volume) v.vol.max = p.max_volume;
      v.vol.value = p.volume;
      v.volLabel.textContent = p.volume;
      v.volMax.textContent = p.max_volume < 100 ? `max ${p.max_volume}` : 'max 100';
    }
    const sound = [p.normalize ? 'normalisation ' + SOUND_LEVELS[p.normalize] : '',
      p.compress ? 'compression ' + SOUND_LEVELS[p.compress] : ''].filter(Boolean);
    v.sound.hidden = !sound.length;
    v.sound.textContent = 'Son : ' + sound.join(', ');
    let sleep = '';
    if (p.sleep_done) sleep = 'Mode sommeil : lecture en pause. Retirez et reposez la carte pour continuer.';
    else if (p.sleep_tracks === 1) sleep = 'Mode sommeil : pause à la fin de ce morceau.';
    else if (p.sleep_tracks > 1) sleep = `Mode sommeil : pause dans ${p.sleep_tracks} morceaux, celui-ci compris.`;
    else if (p.sleep_s > 0) sleep = `Mode sommeil : pause dans ${fmtTime(p.sleep_s)} d'écoute.`;
    v.sleep.hidden = !sleep;
    v.sleep.textContent = sleep;
    v.err.hidden = !p.error;
    v.err.textContent = p.error;
    const c = st.card;
    let text, cls = 'notice';
    if (!c.reader_ok) { text = 'Lecteur NFC non détecté : vérifiez son branchement.'; cls += ' bad'; }
    else if (c.present && c.present === c.last_unknown && c.present !== c.session) {
      text = `Carte inconnue (${c.present}). Associez-la dans l'onglet Cartes.`; cls += ' warn';
    } else if (c.present && c.session === c.present) text = `Carte posée : ${c.folder}`;
    else if (c.resume_remaining < 0) {
      text = 'Carte retirée : reposez-la pour reprendre où vous en étiez.';
    } else if (c.resume_remaining > 0) {
      text = `Carte retirée : reposez-la avant ${fmtTime(c.resume_remaining)} pour reprendre où vous en étiez.`;
    } else if (!st.sd.mounted) { text = 'Carte SD absente ou illisible.'; cls += ' bad'; }
    else text = 'Posez une carte sur l\'enceinte pour écouter sa musique.';
    v.card.className = cls;
    v.card.textContent = text;
  };

  setKids(root, 
    h('div', { class: 'card' },
      v.title, v.sub,
      h('div', { class: 'progress' }, v.elapsed, v.seek, v.duration),
      h('div', { class: 'transport' },
        h('button', { 'aria-label': 'Précédent', onclick: cmd('prev') }, '⏮'),
        v.toggle,
        h('button', { 'aria-label': 'Suivant', onclick: cmd('next') }, '⏭')),
      h('div', { class: 'row', style: 'justify-content:center' },
        h('button', { class: 'small', onclick: cmd('stop') }, '■ Arrêter')),
      h('div', { class: 'vol' }, h('span', { 'aria-hidden': 'true' }, '🔈'),
        h('div', { class: 'vol-track' }, v.vol, h('div', { class: 'vol-scale' }, h('span', null, '0'), v.volMax)),
        v.volLabel),
      v.sound, v.sleep),
    v.card, v.err);
  app.view = v;
  if (app.status) v.update(app.status);
}

/* ---------------- Cartes ---------------- */

async function topFolders() {
  const r = await api('/api/files?path=');
  return r.entries.filter((e) => e.dir).map((e) => e.name);
}

function folderSelect(folders, current) {
  const sel = h('select');
  const opts = [h('option', { value: '' }, '— Choisir un dossier —')];
  const all = current && !folders.includes(current) ? [current, ...folders] : folders;
  for (const f of all) opts.push(h('option', { value: f, selected: f === current }, f));
  setKids(sel, ...opts);
  return sel;
}

async function renderCards() {
  const [data, folders, defaults] = await Promise.all([api('/api/cards'), topFolders().catch(() => []),
    api('/api/settings').catch(() => GENERAL_DEFAULTS)]);
  const list = h('ul', { class: 'list' });
  if (!data.cards.length) list.append(h('li', { class: 'muted' }, 'Aucune carte associée pour l\'instant.'));
  for (const c of data.cards) {
    const del = h('button', { class: 'small danger', 'aria-label': 'Supprimer', title: 'Supprimer' }, '✕');
    del.addEventListener('click', () => {
      if (!confirm(`Supprimer l'association de la carte ${c.uid} ?`)) return;
      busy(del, async () => { await post('/api/cards/delete', { uid: c.uid }); toast('Association supprimée'); renderCards(); });
    });
    const edit = h('button', { class: 'small', 'aria-label': 'Réglages de la carte', title: 'Réglages de la carte' }, '☰');
    edit.addEventListener('click', () => learnFlow(c.folder, c.uid, folders, c, defaults));
    const rules = cardRules(c);
    list.append(h('li', null,
      h('span', { class: 'ico', title: KIND_LABEL[c.kind] || 'Dossier' }, KIND_ICON[c.kind] || '▣'),
      h('div', { class: 'name' },
        c.folder, !c.exists ? h('span', { class: 'error-text small' }, '  (dossier introuvable)') : null,
        h('small', { class: 'mono' }, c.uid),
        rules.length ? h('small', null, rules.join(' · ')) : null),
      edit, del));
  }
  const add = h('button', { class: 'primary' }, '+ Associer une carte');
  add.addEventListener('click', () => learnFlow('', '', folders, null, defaults));
  const quick = data.present && !data.cards.some((c) => c.uid === data.present)
    ? h('div', { class: 'notice warn' }, `Une carte inconnue est posée (${data.present}). `,
        h('button', { class: 'linkish', onclick: () => learnFlow('', data.present, folders, null, defaults) }, 'L\'associer'))
    : null;
  setKids(root, 
    !data.reader_ok ? h('div', { class: 'notice bad' }, 'Lecteur NFC non détecté.') : null,
    quick,
    h('div', { class: 'card' },
      h('div', { class: 'row' }, h('h2', { class: 'grow' }, 'Cartes associées'), add),
      list));
  app.view = null;
}

const RADIO_HELP = 'Adresse d\'un flux MP3, AAC ou Ogg, souvent donnée sur le site de la radio (elle finit '
  + 'souvent par .mp3, .aac, .m3u ou .pls). Les flux HLS (.m3u8 découpé en segments) ne sont pas pris en charge.';
const PODCAST_HELP = 'Adresse du flux RSS du podcast (« flux RSS » ou « RSS feed » sur son site ou son '
  + 'application). Les nouveaux épisodes sont téléchargés chaque nuit ; seuls les plus récents sont gardés. '
  + 'La carte les joue du plus ancien au plus récent, avec la reprise habituelle.';

/* Champs d'une webradio : nom et adresse du flux. */
function radioFields() {
  const name = h('input', { type: 'text', placeholder: 'Ex. : France Inter', maxlength: 80 });
  const url = h('input', { type: 'url', placeholder: 'https://…', autocapitalize: 'off', spellcheck: false });
  const el = h('div', { class: 'stack' },
    h('label', null, 'Nom de la webradio'), name,
    h('label', null, 'Adresse du flux'), url,
    h('p', { class: 'muted small' }, RADIO_HELP));
  return {
    el, name, url,
    check(needName) {
      if (needName && !name.value.trim()) { toast('Donnez un nom à la webradio', true); return false; }
      if (!isUrl(url.value.trim())) { toast('Adresse du flux : http:// ou https://', true); return false; }
      return true;
    },
  };
}

/* Champs d'un podcast : flux RSS, nom facultatif, épisodes gardés. */
function podcastFields() {
  const url = h('input', { type: 'url', placeholder: 'https://…/rss', autocapitalize: 'off', spellcheck: false });
  const name = h('input', { type: 'text', placeholder: 'Facultatif : titre du podcast', maxlength: 80 });
  const keep = h('input', { type: 'number', class: 'short', min: PODCAST_KEEP.min, max: PODCAST_KEEP.max,
    step: 1, value: PODCAST_KEEP.def, inputmode: 'numeric', 'aria-label': 'Épisodes gardés' });
  const info = h('div', { class: 'small', hidden: true });
  const el = h('div', { class: 'stack' },
    h('label', null, 'Adresse du flux RSS'), url,
    h('label', null, 'Nom ', h('span', { class: 'muted' }, '(sans nom : le titre du podcast)')), name,
    h('label', null, 'Épisodes gardés'), h('div', { class: 'row' }, keep, h('span', null, 'les plus récents')),
    info,
    h('p', { class: 'muted small' }, PODCAST_HELP));
  return {
    el, url, name, keep, info,
    keepValue: () => Number(keep.value),
    check() {
      if (!isUrl(url.value.trim())) { toast('Adresse du flux RSS : http:// ou https://', true); return false; }
      const k = Number(keep.value);
      if (!(Number.isInteger(k) && k >= PODCAST_KEEP.min && k <= PODCAST_KEEP.max)) {
        toast(`Épisodes gardés : ${PODCAST_KEEP.min} à ${PODCAST_KEEP.max}`, true);
        return false;
      }
      return true;
    },
  };
}

function podcastSummary(pi) {
  const parts = [`${plural(pi.episodes, 'épisode', 'épisodes')} sur la carte SD`];
  if (pi.syncing) parts.push('téléchargement en cours' + (pi.progress >= 0 ? ` (${pi.progress} %)` : '…'));
  else parts.push(pi.last_check ? `vérifié le ${fmtDate(pi.last_check)}` : 'pas encore vérifié');
  return parts.join(' · ');
}

/*
 * Association : capture d'une carte (mode association de l'enceinte, la musique ne
 * démarre pas) puis choix du dossier. knownUid permet de sauter la capture.
 */
function learnFlow(presetFolder, knownUid, folders, card = null, defaults = null, onDone = renderCards) {
  let stopped = false;
  const box = h('div', { class: 'card stack' });
  const cancel = h('button', null, 'Annuler');
  cancel.addEventListener('click', () => {
    stopped = true;
    post('/api/cards/learn', { action: 'cancel' }).catch(() => {});
    onDone();
  });
  setKids(root, box);
  app.view = null;

  const chooseFolder = async (uid) => {
    const sel = folderSelect(folders, presetFolder);
    if (!defaults) defaults = await api('/api/settings').catch(() => GENERAL_DEFAULTS);
    const curDelay = card && isSet(card.resume_s) ? card.resume_s : null;
    const curOther = card && isSet(card.resume_other) ? card.resume_other : null;
    const curShuffle = card && isSet(card.shuffle) ? card.shuffle : null;
    const curRepeat = card && isSet(card.repeat) ? card.repeat : null;
    const repeatSel = h('select', null,
      h('option', { value: '', selected: curRepeat === null },
        `Réglage général (${defaults.repeat ? 'recommencer' : 's\'arrêter'})`),
      h('option', { value: '0', selected: curRepeat === false }, 'S\'arrêter'),
      h('option', { value: '1', selected: curRepeat === true }, 'Recommencer la playlist'));
    /* Contenu joué : dossier de la carte SD, webradio ou podcast */
    const kind0 = (card && card.kind) || 'folder';
    const kindSel = h('select', null,
      h('option', { value: 'folder', selected: kind0 === 'folder' }, 'Un dossier de la carte SD'),
      h('option', { value: 'radio', selected: kind0 === 'radio' }, 'Une webradio'),
      h('option', { value: 'podcast', selected: kind0 === 'podcast' }, 'Un podcast'));
    const folderBox = h('div', { class: 'stack' },
      h('label', null, 'Dossier de la carte SD'), sel,
      h('p', { class: 'muted small' }, 'Pour un sous-dossier, utilisez « Associer une carte » depuis l\'onglet Musique.'));
    const radio = radioFields();
    const podcast = podcastFields();
    const syncKind = () => {
      folderBox.hidden = kindSel.value !== 'folder';
      radio.el.hidden = kindSel.value !== 'radio';
      podcast.el.hidden = kindSel.value !== 'podcast';
    };
    kindSel.addEventListener('change', syncKind);
    syncKind();
    if (card && kind0 === 'radio') {
      const r = await api('/api/radio?folder=' + encodeURIComponent(card.folder)).catch(() => null);
      if (r) { radio.name.value = r.name; radio.url.value = r.url; }
      radio.name.disabled = true; /* nom : celui du dossier (onglet Musique) */
    }
    if (card && kind0 === 'podcast') {
      const pi = await api('/api/podcast?folder=' + encodeURIComponent(card.folder)).catch(() => null);
      if (pi) {
        podcast.url.value = pi.url;
        podcast.name.value = pi.title;
        podcast.keep.value = pi.keep;
        podcast.info.hidden = false;
        setKids(podcast.info, podcastSummary(pi), pi.last_error ? h('div', { class: 'error-text' }, pi.last_error) : null);
      }
      podcast.name.disabled = true;
    }
    const shuffleSel = h('select', null,
      h('option', { value: '', selected: curShuffle === null },
        `Réglage général (${defaults.shuffle ? 'aléatoire' : 'dans l\'ordre'})`),
      h('option', { value: '0', selected: curShuffle === false }, 'Dans l\'ordre'),
      h('option', { value: '1', selected: curShuffle === true }, 'Aléatoire'));
    const delays = DELAY_CHOICES.includes(curDelay) || curDelay === null ? DELAY_CHOICES : [...DELAY_CHOICES, curDelay].sort((a, b) => a - b);
    const delaySel = h('select', null,
      h('option', { value: '', selected: curDelay === null }, `Réglage général (${fmtDelay(defaults.resume_s)})`),
      ...delays.map((d) => h('option', { value: String(d), selected: d === curDelay },
        d === 0 ? 'Sans limite' : fmtDelay(d))));
    const otherSel = h('select', null,
      h('option', { value: '', selected: curOther === null },
        `Réglage général (${defaults.resume_after_other ? 'conserver' : 'recommencer'})`),
      h('option', { value: '1', selected: curOther === true }, 'Conserver la progression'),
      h('option', { value: '0', selected: curOther === false }, 'Recommencer au début'));
    const levelSel = (cur, general) => h('select', null,
      h('option', { value: '', selected: !isSet(cur) }, `Réglage général (${SOUND_LEVELS[general || 0]})`),
      ...SOUND_LEVELS.map((name, i) => h('option', { value: String(i), selected: cur === i },
        name[0].toUpperCase() + name.slice(1))));
    const normSel = levelSel(card ? card.normalize : null, defaults.normalize);
    const compSel = levelSel(card ? card.compress : null, defaults.compress);
    const cur = { tracks: (card && card.sleep_tracks) || 0, minutes: (card && card.sleep_minutes) || 0 };
    const sleepSel = h('select', null,
      h('option', { value: '', selected: !cur.tracks && !cur.minutes }, 'Désactivé'),
      h('option', { value: 'tracks', selected: cur.tracks > 0 }, 'Pause après un nombre de morceaux'),
      h('option', { value: 'minutes', selected: !cur.tracks && cur.minutes > 0 }, 'Pause après une durée d\'écoute'));
    const sleepNum = h('input', { type: 'number', class: 'short', min: 1, step: 1, inputmode: 'numeric',
      'aria-label': 'Mode sommeil', value: cur.tracks || cur.minutes || '' });
    const sleepUnit = h('span');
    const sleepRow = h('div', { class: 'row' }, sleepNum, sleepUnit);
    const syncSleep = () => {
      const m = sleepSel.value;
      sleepRow.hidden = !m;
      if (!m) return;
      sleepNum.max = SLEEP_MAX[m];
      sleepUnit.textContent = m === 'tracks' ? 'morceau(x)' : 'minutes d\'écoute';
    };
    sleepSel.addEventListener('change', () => {
      if (sleepSel.value) sleepNum.value = cur[sleepSel.value] || SLEEP_DEFAULT[sleepSel.value];
      syncSleep();
    });
    syncSleep();
    const save = h('button', { class: 'primary' }, card ? 'Enregistrer' : 'Associer');
    save.addEventListener('click', () => busy(save, async () => {
      const kind = kindSel.value;
      const same = card && kind === kind0; /* même webradio ou même podcast : réglages modifiés */
      if (kind === 'folder' && !sel.value) { toast('Choisissez un dossier', true); return; }
      if (kind === 'radio' && !radio.check(!same)) return;
      if (kind === 'podcast' && !podcast.check()) return;
      const sleepMode = sleepSel.value;
      const sleepN = sleepMode ? Number(sleepNum.value) : 0;
      if (sleepMode && !(Number.isInteger(sleepN) && sleepN >= 1 && sleepN <= SLEEP_MAX[sleepMode])) {
        toast(`Mode sommeil : nombre entre 1 et ${SLEEP_MAX[sleepMode]}`, true);
        return;
      }
      const settings = {
        uid,
        resume_s: delaySel.value === '' ? null : Number(delaySel.value),
        resume_other: otherSel.value === '' ? null : otherSel.value === '1',
        shuffle: shuffleSel.value === '' ? null : shuffleSel.value === '1',
        normalize: normSel.value === '' ? null : Number(normSel.value),
        compress: compSel.value === '' ? null : Number(compSel.value),
        repeat: repeatSel.value === '' ? null : repeatSel.value === '1',
        sleep_tracks: sleepMode === 'tracks' ? sleepN : 0,
        sleep_minutes: sleepMode === 'minutes' ? sleepN : 0,
      };
      if (kind === 'folder') {
        await post('/api/cards', { ...settings, folder: sel.value });
      } else if (kind === 'radio' && same) {
        await post('/api/radio', { folder: card.folder, url: radio.url.value.trim() });
        await post('/api/cards', { ...settings, folder: card.folder });
      } else if (kind === 'radio') {
        await post('/api/radio', { name: radio.name.value.trim(), url: radio.url.value.trim(), card: settings });
      } else if (same) {
        await post('/api/podcast', { folder: card.folder, action: 'update', url: podcast.url.value.trim(),
          keep: podcast.keepValue() });
        await post('/api/cards', { ...settings, folder: card.folder });
      } else {
        await post('/api/podcasts', { url: podcast.url.value.trim(), name: podcast.name.value.trim(),
          keep: podcast.keepValue(), card: settings });
        toast('Abonnement enregistré : les épisodes se téléchargent');
        onDone();
        return;
      }
      toast(card ? 'Carte modifiée' : 'Carte associée');
      onDone();
    }));
    setKids(box,
      h('h2', null, card ? 'Modifier la carte' : 'Choisir la musique'),
      h('div', { class: 'big-uid mono' }, uid),
      h('label', null, 'Que joue cette carte ?'), kindSel,
      folderBox, radio.el, podcast.el,
      h('label', null, 'Ordre de lecture'), shuffleSel,
      h('label', null, 'À la fin de la playlist'), repeatSel,
      h('label', null, 'Après le retrait, conserver la progression pendant'), delaySel,
      h('label', null, 'Si une autre carte est posée entre-temps'), otherSel,
      h('label', null, 'Normalisation ', h('span', { class: 'muted' }, '(volume égalisé entre les morceaux)')), normSel,
      h('label', null, 'Compression ', h('span', { class: 'muted' }, '(écarts de volume réduits dans un morceau)')), compSel,
      h('label', null, 'Mode sommeil ', h('span', { class: 'muted' }, '(compté depuis la pose de la carte)')), sleepSel,
      sleepRow,
      h('p', { class: 'muted small' }, 'En nombre de morceaux, la pause tombe entre deux morceaux ; en durée, le son baisse '
        + 'doucement pendant les 15 dernières secondes. Retirer et reposer la carte reprend là où la lecture s\'était '
        + 'arrêtée, avec un nouveau décompte.'),
      h('div', { class: 'row end actions' }, cancel, save));
  };

  if (knownUid) { chooseFolder(knownUid).catch(reportError); return; }

  setKids(box, 
    h('h2', null, 'Associer une carte'),
    h('p', { class: 'big-uid pulse' }, 'Posez la carte sur l\'enceinte…'),
    h('p', { class: 'muted small' }, 'La musique ne démarrera pas pendant l\'association.'),
    h('div', { class: 'row end' }, cancel));

  (async () => {
    let previous = '';
    try { previous = (await api('/api/cards')).learned; } catch (e) { /* ignoré */ }
    await post('/api/cards/learn', { action: 'start' });
    let seenLearning = false;
    const started = Date.now();
    while (!stopped) {
      await new Promise((r) => setTimeout(r, 700));
      if (stopped || app.tab !== 'cards') return;
      const d = await api('/api/cards');
      if (d.learning) seenLearning = true;
      if (d.learned && !d.learning && (seenLearning || d.learned !== previous)) {
        const known = d.cards.find((c) => c.uid === d.learned) || null;
        if (known && !card) { card = known; presetFolder = presetFolder || known.folder; }
        await chooseFolder(d.learned);
        return;
      }
      if (!d.learning && (seenLearning || Date.now() - started > 4000)) {
        setKids(box, h('h2', null, 'Aucune carte détectée'),
          h('p', { class: 'muted' }, 'Le délai d\'une minute est écoulé.'),
          h('div', { class: 'row end' }, cancel, h('button', { class: 'primary', onclick: () => learnFlow(presetFolder, '', folders, card, defaults, onDone) }, 'Réessayer')));
        return;
      }
    }
  })().catch(reportError);
}

/* ---------------- Musique ---------------- */

async function renderMusic(path = app.path) {
  let data;
  const cardsReq = api('/api/cards').catch(() => ({ cards: [] }));
  try {
    data = await api('/api/files?path=' + encodeURIComponent(path));
  } catch (e) {
    if (path) { app.path = ''; return renderMusic(''); }
    setKids(root, h('div', { class: 'notice bad' }, e.message));
    return;
  }
  if (app.path !== data.path) app.selected.clear();
  app.path = data.path;
  /* FAT ignore la casse des noms */
  const allCards = (await cardsReq).cards || [];
  const cardsOf = (folder) => allCards.filter((c) => c.folder.toLowerCase() === folder.toLowerCase());

  const crumbs = h('div', { class: 'crumbs' });
  crumbs.append(h('button', { class: 'small', onclick: () => renderMusic('') }, 'Carte SD'));
  let acc = '';
  for (const part of data.path ? data.path.split('/') : []) {
    acc = joinPath(acc, part);
    const target = acc;
    crumbs.append(h('span', { class: 'muted' }, '›'), h('button', { class: 'small', onclick: () => renderMusic(target) }, part));
  }

  const used = data.total ? (data.total - data.free) / data.total : 0;
  const usage = h('div', { class: 'small muted' },
    h('div', { class: 'bar' }, h('span', { style: `width:${(used * 100).toFixed(1)}%` })),
    `${fmtSize(data.free)} libres sur ${fmtSize(data.total)}`);

  const list = h('ul', { class: 'list' });
  if (!data.entries.length) list.append(h('li', { class: 'muted' }, 'Dossier vide.'));
  const selBar = h('div', { class: 'selbar', hidden: true });
  const drawSelBar = () => {
    const n = app.selected.size;
    selBar.hidden = n === 0;
    if (!n) return;
    const moveBtn = h('button', { class: 'small primary', onclick: () => moveFlow([...app.selected], data.path) }, '↦ Déplacer…');
    const delBtn = h('button', { class: 'small danger' }, '✕ Supprimer');
    delBtn.addEventListener('click', () => {
      if (!confirm(`Supprimer ${n} élément(s) (les dossiers avec tout leur contenu) ?`)) return;
      busy(delBtn, async () => {
        let failed = 0;
        for (const path of app.selected) {
          try { await post('/api/files/delete', { path }); } catch (err) { failed++; }
        }
        app.selected.clear();
        toast(failed ? `${failed} suppression(s) impossible(s)` : 'Supprimé', failed > 0);
        renderMusic();
      });
    });
    setKids(selBar, h('span', { class: 'grow' }, `${n} sélectionné${n > 1 ? 's' : ''}`), moveBtn, delBtn,
      h('button', { class: 'small', 'aria-label': 'Annuler la sélection', onclick: () => { app.selected.clear(); renderMusic(); } }, '✕'));
  };
  for (const e of data.entries) {
    const full = joinPath(data.path, e.name);
    const check = h('input', { type: 'checkbox', class: 'sel', checked: app.selected.has(full), 'aria-label': 'Sélectionner ' + e.name });
    check.addEventListener('change', () => { if (check.checked) app.selected.add(full); else app.selected.delete(full); drawSelBar(); });
    const mv = h('button', { class: 'small', 'aria-label': 'Déplacer', title: 'Déplacer', onclick: () => moveFlow([full], data.path) }, '↦');
    const ren = h('button', { class: 'small', 'aria-label': 'Renommer' }, '✎');
    ren.addEventListener('click', () => {
      const name = prompt('Nouveau nom :', e.name);
      if (!name || name === e.name) return;
      busy(ren, async () => { await post('/api/files/rename', { from: full, to: joinPath(data.path, name.trim()) }); renderMusic(); });
    });
    const del = h('button', { class: 'small danger', 'aria-label': 'Supprimer' }, '✕');
    del.addEventListener('click', () => {
      const linked = e.dir ? allCards.filter((c) => c.folder.toLowerCase() === full.toLowerCase() ||
        c.folder.toLowerCase().startsWith(full.toLowerCase() + '/')).length : 0;
      const warnCards = linked ? `\n\n${linked} carte${linked > 1 ? 's' : ''} associée${linked > 1 ? 's' : ''} à ce dossier ne joueront plus rien (association supprimée).` : '';
      if (!confirm(e.dir ? `Supprimer le dossier « ${e.name} » et tout son contenu ?${warnCards}` : `Supprimer « ${e.name} » ?`)) return;
      busy(del, async () => { await post('/api/files/delete', { path: full }); toast('Supprimé'); renderMusic(); });
    });
    if (e.dir) {
      const nCards = cardsOf(full).length;
      list.append(h('li', null, check,
        h('span', { class: 'ico', title: KIND_LABEL[e.kind] || 'Dossier' }, KIND_ICON[e.kind] || '📁'),
        h('div', { class: 'name' }, h('button', { class: 'linkish', onclick: () => renderMusic(full) }, e.name),
          nCards ? h('small', null, `▣ ${nCards} carte${nCards > 1 ? 's' : ''} associée${nCards > 1 ? 's' : ''}`) : null),
        h('button', { class: 'small', 'aria-label': 'Lire', title: 'Lire', onclick: () => playFolder(full) }, '▶'),
        ren, mv, del));
    } else {
      list.append(h('li', null, check,
        h('span', { class: 'ico' }, e.audio ? '♪' : '·'),
        h('div', { class: 'name' }, e.name, h('small', null, fmtSize(e.size))),
        ren, mv, del));
    }
  }
  drawSelBar();

  const filesInput = h('input', { type: 'file', multiple: true, hidden: true });
  const dirInput = h('input', { type: 'file', multiple: true, hidden: true, webkitdirectory: true });
  filesInput.addEventListener('change', () => { queueFiles([...filesInput.files].map((f) => ({ file: f, rel: f.name })), data.path); filesInput.value = ''; });
  dirInput.addEventListener('change', () => { queueFiles([...dirInput.files].map((f) => ({ file: f, rel: f.webkitRelativePath || f.name })), data.path); dirInput.value = ''; });

  const mkdir = h('button', { class: 'small' }, '+ Dossier');
  mkdir.addEventListener('click', () => {
    const name = prompt('Nom du nouveau dossier :');
    if (!name) return;
    busy(mkdir, async () => { await post('/api/files/mkdir', { path: joinPath(data.path, name.trim()) }); renderMusic(); });
  });

  const drop = h('div', { class: 'drop' }, 'Glissez ici des fichiers ou des dossiers à envoyer');
  drop.addEventListener('dragover', (ev) => { ev.preventDefault(); drop.classList.add('over'); });
  drop.addEventListener('dragleave', () => drop.classList.remove('over'));
  drop.addEventListener('drop', async (ev) => {
    ev.preventDefault();
    drop.classList.remove('over');
    const items = [...ev.dataTransfer.items].map((i) => i.webkitGetAsEntry && i.webkitGetAsEntry()).filter(Boolean);
    const files = [];
    for (const entry of items) await collectEntry(entry, '', files);
    queueFiles(files, data.path);
  });

  const uploadsBox = h('div', { class: 'uploads' });
  const content = data.kind === 'podcast' ? podcastBox(data.path) : data.kind === 'radio' ? radioBox(data.path) : null;
  app.view = { uploadsBox, update: content && content.refresh ? () => content.refresh() : null };

  const folderActions = data.path ? h('div', { class: 'row' },
    h('button', { class: 'small', onclick: () => playFolder(data.path) }, '▶ Lire ce dossier'),
    h('button', { class: 'small', onclick: async () => learnFlowFromMusic(data.path) }, '▣ Associer une carte')) : null;

  let folderCards = null;
  if (data.path) {
    const here = cardsOf(data.path);
    const cl = h('ul', { class: 'list' });
    for (const c of here) {
      const rules = cardRules(c);
      cl.append(h('li', null,
        h('span', { class: 'ico' }, '▣'),
        h('div', { class: 'name' }, h('span', { class: 'mono' }, c.uid),
          h('small', null, rules.length ? rules.join(' · ') : 'réglages généraux')),
        h('button', { class: 'small', 'aria-label': 'Réglages de la carte', title: 'Réglages de la carte',
          onclick: () => learnFlowFromMusic(data.path, c) }, '☰')));
    }
    folderCards = h('div', { class: 'folder-cards' },
      h('div', { class: 'small muted' }, here.length ? 'Cartes associées à ce dossier :' : 'Aucune carte associée à ce dossier.'),
      here.length ? cl : null);
  }

  setKids(root, 
    h('div', { class: 'card' },
      crumbs, folderActions, content, folderCards,
      h('div', { class: 'row actions' },
        mkdir,
        h('button', { class: 'small', onclick: () => filesInput.click() }, '↑ Fichiers'),
        h('button', { class: 'small', onclick: () => dirInput.click() }, '↑ Dossier'),
        h('button', { class: 'small', onclick: () => newContentFlow('radio') }, '+ Webradio'),
        h('button', { class: 'small', onclick: () => newContentFlow('podcast') }, '+ Podcast')),
      filesInput, dirInput,
      h('div', { class: 'actions' }, drop),
      uploadsBox),
    h('div', { class: 'card' }, selBar, list, h('div', { class: 'actions' }, usage)));
  drawUploads();
}

/* Encadré d'un dossier de webradio : adresse du flux. */
function radioBox(folder) {
  const info = h('div', { class: 'small mono' }, '…');
  const edit = h('button', { class: 'small' }, '✎ Modifier l\'adresse');
  let url = '';
  edit.addEventListener('click', () => {
    const u = prompt('Adresse du flux de la webradio :', url);
    if (!u || u.trim() === url) return;
    if (!isUrl(u.trim())) { toast('Adresse du flux : http:// ou https://', true); return; }
    busy(edit, async () => {
      await post('/api/radio', { folder, url: u.trim() });
      toast('Adresse enregistrée');
      renderMusic(folder);
    });
  });
  api('/api/radio?folder=' + encodeURIComponent(folder))
    .then((r) => { url = r.url; info.textContent = r.url || '(aucune adresse)'; })
    .catch(reportError);
  return h('div', { class: 'content-box' },
    h('div', { class: 'small muted' }, '📻 Webradio'), info,
    h('div', { class: 'row' }, edit));
}

/* Encadré d'un dossier de podcast : état des téléchargements, réglages, désabonnement. */
function podcastBox(folder) {
  const info = h('div', { class: 'small' }, 'Chargement…');
  const sync = h('button', { class: 'small' }, '↻ Vérifier maintenant');
  const edit = h('button', { class: 'small' }, '☰ Réglages');
  const unsub = h('button', { class: 'small danger' }, 'Se désabonner');
  let last = null;
  const draw = (pi) => {
    const was = last && last.syncing;
    last = pi;
    setKids(info,
      h('div', null, h('b', null, pi.title || 'Podcast'), ` · les ${pi.keep} épisodes les plus récents sont gardés`),
      h('div', { class: pi.syncing ? '' : 'muted' }, podcastSummary(pi)),
      pi.last_error && !pi.syncing ? h('div', { class: 'error-text' }, pi.last_error) : null);
    sync.disabled = pi.syncing;
    if (was && !pi.syncing && app.tab === 'music' && app.path === folder) renderMusic(folder); /* nouveaux épisodes */
  };
  const load = () => api('/api/podcast?folder=' + encodeURIComponent(folder)).then(draw).catch(reportError);
  sync.addEventListener('click', () => busy(sync, async () => {
    await post('/api/podcast', { folder, action: 'sync' });
    toast('Vérification lancée');
    setTimeout(load, 1500);
  }));
  edit.addEventListener('click', () => {
    if (!last) return;
    const url = prompt('Adresse du flux RSS :', last.url);
    if (url === null) return;
    const keep = prompt(`Nombre d'épisodes gardés (${PODCAST_KEEP.min} à ${PODCAST_KEEP.max}) :`, String(last.keep));
    if (keep === null) return;
    busy(edit, async () => {
      await post('/api/podcast', { folder, action: 'update', url: url.trim(), keep: Number(keep) });
      toast('Réglages du podcast enregistrés');
      load();
    });
  });
  unsub.addEventListener('click', () => {
    if (!confirm('Se désabonner ? Les épisodes déjà téléchargés restent dans le dossier.')) return;
    busy(unsub, async () => {
      await post('/api/podcast', { folder, action: 'unsubscribe' });
      toast('Désabonné');
      renderMusic(folder);
    });
  });
  load();
  const box = h('div', { class: 'content-box' },
    h('div', { class: 'small muted' }, '🎙 Podcast'), info,
    h('div', { class: 'row' }, sync, edit, unsub));
  box.refresh = () => { if (last && last.syncing) load(); };
  return box;
}

/* Nouvelle webradio ou nouveau podcast (sans carte : on l'associera ensuite). */
function newContentFlow(kind) {
  const f = kind === 'radio' ? radioFields() : podcastFields();
  const save = h('button', { class: 'primary' }, kind === 'radio' ? 'Créer' : 'S\'abonner');
  const cancel = h('button', { onclick: () => renderMusic() }, 'Annuler');
  save.addEventListener('click', () => busy(save, async () => {
    if (kind === 'radio' ? !f.check(true) : !f.check()) return;
    const r = kind === 'radio'
      ? await post('/api/radio', { name: f.name.value.trim(), url: f.url.value.trim() })
      : await post('/api/podcasts', { url: f.url.value.trim(), name: f.name.value.trim(), keep: f.keepValue() });
    toast(kind === 'radio' ? 'Webradio créée' : 'Abonnement enregistré : les épisodes se téléchargent');
    renderMusic(r.folder);
  }));
  setKids(root, h('div', { class: 'card stack' },
    h('h2', null, kind === 'radio' ? 'Nouvelle webradio' : 'Nouveau podcast'),
    f.el,
    h('p', { class: 'muted small' }, 'Associez ensuite une carte depuis le dossier créé (« ▣ Associer une carte »).'),
    h('div', { class: 'row end actions' }, cancel, save)));
  app.view = null;
}

/*
 * Déplacement : choix du dossier de destination en naviguant dans la carte SD.
 * Les éléments déplacés (et leurs sous-dossiers) ne sont pas proposés comme destination.
 */
function moveFlow(items, backPath) {
  const inMoved = (p) => items.some((it) => p === it || p.startsWith(it + '/'));
  const parentOf = (p) => (p.includes('/') ? p.slice(0, p.lastIndexOf('/')) : '');
  const box = h('div', { class: 'card stack' });
  setKids(root, box);
  app.view = null;
  const back = () => { renderMusic(backPath); };
  const show = async (cur) => {
    let data;
    try {
      data = await api('/api/files?path=' + encodeURIComponent(cur));
    } catch (e) { reportError(e); return show(''); }
    const crumbs = h('div', { class: 'crumbs' }, h('button', { class: 'small', onclick: () => show('') }, 'Carte SD'));
    let acc = '';
    for (const part of cur ? cur.split('/') : []) {
      acc = joinPath(acc, part);
      const target = acc;
      crumbs.append(h('span', { class: 'muted' }, '›'), h('button', { class: 'small', onclick: () => show(target) }, part));
    }
    const list = h('ul', { class: 'list' });
    const dirs = data.entries.filter((e) => e.dir && !inMoved(joinPath(cur, e.name)));
    if (!dirs.length) list.append(h('li', { class: 'muted' }, 'Aucun sous-dossier.'));
    for (const e of dirs) {
      const full = joinPath(cur, e.name);
      list.append(h('li', null, h('span', { class: 'ico' }, '📁'),
        h('div', { class: 'name' }, h('button', { class: 'linkish', onclick: () => show(full) }, e.name))));
    }
    const sameDir = items.every((it) => parentOf(it) === cur);
    const go = h('button', { class: 'primary', disabled: sameDir }, 'Déplacer ici');
    go.addEventListener('click', () => busy(go, async () => {
      const errors = [];
      for (const from of items) {
        try {
          await post('/api/files/rename', { from, to: joinPath(cur, baseName(from)) });
        } catch (e) { errors.push(`${baseName(from)} : ${e.message}`); }
      }
      app.selected.clear();
      if (errors.length) toast(errors.join(' ; '), true);
      else toast(`${items.length} élément(s) déplacé(s)`);
      renderMusic(backPath);
    }));
    const mk = h('button', { class: 'small' }, '+ Nouveau dossier');
    mk.addEventListener('click', () => {
      const name = prompt('Nom du nouveau dossier :');
      if (!name) return;
      busy(mk, async () => { await post('/api/files/mkdir', { path: joinPath(cur, name.trim()) }); show(joinPath(cur, name.trim())); });
    });
    setKids(box,
      h('h2', null, items.length > 1 ? `Déplacer ${items.length} éléments` : `Déplacer « ${baseName(items[0])} »`),
      h('p', { class: 'small muted' }, 'Choisissez le dossier de destination :'),
      crumbs, list,
      h('div', { class: 'row' }, mk),
      h('div', { class: 'row end actions' }, h('button', { onclick: back }, 'Annuler'), go));
  };
  show(backPath).catch(reportError);
}

async function collectEntry(entry, prefix, out) {
  if (entry.isFile) {
    const file = await new Promise((res, rej) => entry.file(res, rej));
    out.push({ file, rel: prefix + entry.name });
  } else if (entry.isDirectory) {
    const reader = entry.createReader();
    for (;;) {
      const batch = await new Promise((res, rej) => reader.readEntries(res, rej));
      if (!batch.length) break;
      for (const e of batch) await collectEntry(e, prefix + entry.name + '/', out);
    }
  }
}

/* Association ou réglages d'une carte depuis l'onglet Musique ; on revient ensuite au dossier. */
async function learnFlowFromMusic(folder, card = null) {
  app.tab = 'cards';
  for (const b of document.querySelectorAll('#tabs button')) b.classList.toggle('active', b.dataset.tab === 'cards');
  const folders = await topFolders().catch(() => []);
  const back = () => switchTab('music');
  learnFlow(folder, card ? card.uid : '', folders.includes(folder) ? folders : [folder, ...folders], card, null, back);
}

async function playFolder(folder) {
  try {
    await post('/api/player', { action: 'play_folder', folder });
    toast('Lecture de « ' + baseName(folder) + ' »');
  } catch (e) { reportError(e); }
}

function queueFiles(items, basePath) {
  let skipped = 0;
  for (const it of items) {
    const parts = it.rel.split('/');
    if (parts.some((p) => p.startsWith('.')) || !uploadable(parts[parts.length - 1])) { skipped++; continue; }
    app.uploads.push({ file: it.file, path: joinPath(basePath, it.rel), progress: 0, state: 'attente' });
  }
  if (skipped) toast(`${skipped} fichier(s) ignoré(s) (format non pris en charge)`);
  drawUploads();
  processUploads();
}

function drawUploads() {
  const box = app.view && app.view.uploadsBox;
  if (!box) return;
  const pending = app.uploads.filter((u) => u.state !== 'ok');
  setKids(box, ...pending.slice(0, 20).map((u) => h('div', { class: 'u' },
    h('div', { class: 'row' }, h('span', { class: 'grow' }, baseName(u.path)),
      h('span', { class: u.state === 'erreur' ? 'error-text small' : 'muted small' }, u.state === 'envoi' ? Math.round(u.progress * 100) + ' %' : u.state)),
    h('div', { class: 'bar' }, h('span', { style: `width:${(u.progress * 100).toFixed(0)}%` })))),
  pending.length > 20 ? h('p', { class: 'muted small' }, `… et ${pending.length - 20} autre(s)`) : null);
}

function uploadOne(u) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('PUT', '/api/upload?path=' + encodeURIComponent(u.path));
    xhr.setRequestHeader('X-Requested-With', 'enceinte');
    xhr.upload.onprogress = (ev) => { if (ev.lengthComputable) { u.progress = ev.loaded / ev.total; drawUploads(); } };
    xhr.onload = () => {
      if (xhr.status === 200) resolve();
      else {
        let msg = 'erreur ' + xhr.status;
        try { msg = JSON.parse(xhr.responseText).error || msg; } catch (e) { /* ignoré */ }
        reject(new Error(msg));
      }
    };
    xhr.onerror = () => reject(new Error('connexion perdue'));
    xhr.send(u.file);
  });
}

async function processUploads() {
  if (app.uploading) return;
  app.uploading = true;
  let done = 0, failed = 0;
  try {
    for (const u of app.uploads) {
      if (u.state !== 'attente') continue;
      u.state = 'envoi';
      drawUploads();
      try {
        await uploadOne(u);
        u.state = 'ok';
        u.progress = 1;
        done++;
      } catch (e) {
        u.state = 'erreur';
        failed++;
        toast(`${baseName(u.path)} : ${e.message}`, true);
      }
      drawUploads();
    }
  } finally {
    app.uploading = false;
  }
  app.uploads = app.uploads.filter((u) => u.state === 'erreur');
  if (done) toast(`${done} fichier(s) envoyé(s)` + (failed ? `, ${failed} en erreur` : ''));
  if (app.tab === 'music') renderMusic();
}

/* ---------------- Réglages ---------------- */

const OTA_STATES = ['Inactif', 'Vérification', 'En attente (enceinte occupée)', 'Téléchargement', 'Redémarrage', 'Erreur'];

async function renderSettings() {
  const [s, st] = await Promise.all([api('/api/settings'), api('/api/status')]);
  app.status = st;
  const w = st.wifi;

  /* Wi-Fi */
  const wifi = wifiPicker();
  const saveWifi = h('button', { class: 'primary' }, 'Se connecter');
  saveWifi.addEventListener('click', () => busy(saveWifi, async () => {
    const ssid = wifi.ssid();
    if (!ssid) { toast('Choisissez un réseau', true); return; }
    await post('/api/wifi', { ssid, password: wifi.password() });
    toast('Connexion en cours à « ' + ssid + ' »');
  }));
  const wifiState = w.connected
    ? `Connectée à « ${w.ssid} » (${w.ip}, signal ${w.rssi} dBm)`
    : (w.ssid ? `Non connectée (réseau « ${w.ssid} »)` : 'Aucun réseau configuré');

  /* Enceinte */
  const host = h('input', { type: 'text', value: s.hostname, autocapitalize: 'off', spellcheck: false });
  const maxVol = h('input', { type: 'range', min: 10, max: 100, step: 5, value: s.max_volume });
  const maxLbl = h('span', { class: 'small muted' }, s.max_volume + ' %');
  maxVol.addEventListener('input', () => { maxLbl.textContent = maxVol.value + ' %'; });
  const saveDev = h('button', { class: 'primary' }, 'Enregistrer');
  saveDev.addEventListener('click', () => busy(saveDev, async () => {
    await post('/api/settings', { hostname: host.value.trim(), max_volume: Number(maxVol.value) });
    toast('Réglages enregistrés');
    refreshStatus();
  }));

  /* Lecture des cartes */
  const shuffle = h('input', { type: 'checkbox', checked: !!s.shuffle });
  const repeat = h('input', { type: 'checkbox', checked: !!s.repeat });
  const resumeMin = h('input', { type: 'number', class: 'short', min: 0, max: 43200, step: 1, value: Math.round(s.resume_s / 60),
    'aria-label': 'Durée de conservation de la progression, en minutes' });
  const resumeOther = h('input', { type: 'checkbox', checked: !!s.resume_after_other, id: 'resume-other' });
  const saveResume = h('button', { class: 'primary' }, 'Enregistrer');
  saveResume.addEventListener('click', () => busy(saveResume, async () => {
    const m = Number(resumeMin.value);
    if (!Number.isFinite(m) || m < 0) { toast('Durée invalide', true); return; }
    await post('/api/settings', { shuffle: shuffle.checked, repeat: repeat.checked, resume_s: Math.round(m * 60),
      resume_after_other: resumeOther.checked });
    toast('Réglages de lecture enregistrés');
  }));

  /* Son */
  const levelSelect = (value) => h('select', null, ...SOUND_LEVELS.map((name, i) =>
    h('option', { value: String(i), selected: value === i }, name[0].toUpperCase() + name.slice(1))));
  const normalize = levelSelect(s.normalize || 0);
  const compress = levelSelect(s.compress || 0);
  const saveSound = h('button', { class: 'primary' }, 'Enregistrer');
  saveSound.addEventListener('click', () => busy(saveSound, async () => {
    await post('/api/settings', { normalize: Number(normalize.value), compress: Number(compress.value) });
    toast('Réglages du son enregistrés');
  }));

  /* Adresse IP */
  const ipCard = networkCard(s, w);
  const ctlCard = controlsCard(s);

  /* Sauvegarde */
  const withSecrets = h('input', { type: 'checkbox', checked: true });
  const exportBtn = h('button', null, '↓ Exporter les réglages');
  exportBtn.addEventListener('click', () => {
    const a = h('a', { href: '/api/config/export' + (withSecrets.checked ? '?secrets=1' : ''), download: `reglages-${s.hostname}.json` });
    document.body.append(a);
    a.click();
    a.remove();
  });
  const importInput = h('input', { type: 'file', accept: '.json,application/json', hidden: true });
  const importBtn = h('button', null, '↑ Importer des réglages…');
  importBtn.addEventListener('click', () => importInput.click());
  importInput.addEventListener('change', () => {
    const f = importInput.files[0];
    importInput.value = '';
    if (f) busy(importBtn, () => importSettings(f));
  });

  /* HTTPS */
  const httpsBox = h('input', { type: 'checkbox', checked: !!s.https_enabled });
  const httpsMsg = h('p', { class: 'small' });
  const httpsUrl = `https://${location.hostname}/#settings`;
  const showHttpsState = (st) => {
    if (st.https_pending) httpsMsg.textContent = 'Application en cours…';
    else if (st.https_active) {
      setKids(httpsMsg, 'HTTPS actif. ',
        location.protocol === 'https:' ? 'Cette page est chiffrée.' : h('a', { href: httpsUrl }, 'Ouvrir la version sécurisée'));
    } else httpsMsg.textContent = 'HTTPS désactivé : l\'interface est en HTTP.';
  };
  showHttpsState(s);
  const saveHttps = h('button', { class: 'primary' }, 'Enregistrer');
  saveHttps.addEventListener('click', () => busy(saveHttps, async () => {
    const enable = httpsBox.checked;
    await post('/api/https', { enabled: enable });
    httpsMsg.textContent = enable ? 'Création du certificat et démarrage de HTTPS…' : 'Arrêt de HTTPS…';
    for (let i = 0; i < 30; i++) {
      await new Promise((r) => setTimeout(r, 1000));
      let st;
      try { st = await api('/api/settings'); } catch (e) { if (!enable) break; continue; }
      if (!st.https_pending && st.https_active === enable) { showHttpsState(st); break; }
    }
    if (enable && location.protocol !== 'https:') {
      toast('Ouverture de la version sécurisée : acceptez l\'avertissement du navigateur.');
      setTimeout(() => { location.href = httpsUrl; }, 1500);
    } else if (!enable && location.protocol === 'https:') {
      setTimeout(() => { location.href = `http://${location.hostname}/#settings`; }, 1500);
    }
  }));

  /* Sécurité */
  const cur = passwordInput('current-password'), nw = passwordInput('new-password'), nw2 = passwordInput('new-password');
  const savePw = h('button', { class: 'primary' }, 'Changer le mot de passe');
  savePw.addEventListener('click', () => busy(savePw, async () => {
    if (nw.value.length < 6) { toast('6 caractères minimum', true); return; }
    if (nw.value !== nw2.value) { toast('Les deux mots de passe sont différents', true); return; }
    await post('/api/password', { current: cur.value, new: nw.value });
    cur.value = nw.value = nw2.value = '';
    toast('Mot de passe modifié');
  }));
  const mpdPw = passwordInput('new-password');
  const saveMpd = h('button', { class: 'primary' }, 'Enregistrer');
  saveMpd.addEventListener('click', () => busy(saveMpd, async () => {
    await post('/api/mpd', { password: mpdPw.value });
    toast(mpdPw.value ? 'Mot de passe MPD enregistré' : 'Mot de passe MPD supprimé');
    renderSettings();
  }));

  /* Mises à jour */
  const otaUrl = h('input', { type: 'url', value: s.ota_url, placeholder: s.ota_default_url || 'https://github.com/compte/depot', autocapitalize: 'off', spellcheck: false });
  const otaDefault = h('button', { type: 'button', class: 'small', hidden: !s.ota_default_url }, 'Valeur d\'usine');
  otaDefault.addEventListener('click', () => { otaUrl.value = s.ota_default_url; otaUrl.focus(); });
  const otaInt = h('input', { type: 'number', min: 1, max: 720, value: s.ota_interval_h });
  const saveOta = h('button', { class: 'primary' }, 'Enregistrer');
  saveOta.addEventListener('click', () => busy(saveOta, async () => {
    await post('/api/settings', { ota_url: otaUrl.value.trim(), ota_interval_h: Number(otaInt.value) });
    toast('Réglages de mise à jour enregistrés');
  }));
  const check = h('button', null, 'Vérifier maintenant');
  check.addEventListener('click', () => busy(check, async () => { await post('/api/ota/check'); toast('Vérification lancée'); }));
  const otaMsg = h('p', { class: 'small' });
  const otaBar = h('div', { class: 'bar', hidden: true }, h('span'));
  const fw = h('input', { type: 'file', accept: '.bin', hidden: true });
  const fwBtn = h('button', null, 'Installer un fichier .bin…');
  fwBtn.addEventListener('click', () => fw.click());
  fw.addEventListener('change', () => {
    const f = fw.files[0];
    fw.value = '';
    if (!f || !confirm(`Installer « ${f.name} » ? L'enceinte redémarrera.`)) return;
    uploadFirmware(f, otaBar, otaMsg);
  });
  const updateOta = (status) => {
    const o = status.ota;
    otaMsg.textContent = `Version ${o.current} · ${OTA_STATES[o.state] || ''}${o.message ? ' : ' + o.message : ''}`;
    otaMsg.className = 'small' + (o.state === 5 ? ' error-text' : '');
    if (!app.firmwareUpload) {
      otaBar.hidden = o.state !== 3;
      otaBar.firstChild.style.width = o.progress + '%';
    }
  };
  updateOta(st);

  /* Système */
  const reboot = h('button', null, 'Redémarrer');
  reboot.addEventListener('click', () => {
    if (!confirm('Redémarrer l\'enceinte ?')) return;
    busy(reboot, async () => { await post('/api/reboot'); toast('Redémarrage…'); setTimeout(boot, 8000); });
  });
  const reset = h('button', { class: 'danger' }, 'Réinitialisation usine');
  reset.addEventListener('click', () => {
    const pw = prompt('Tous les réglages et les associations de cartes seront effacés (la musique est conservée).\n\nMot de passe administrateur :');
    if (!pw) return;
    busy(reset, async () => { await post('/api/factory-reset', { password: pw }); toast('Réinitialisation…'); setTimeout(boot, 10000); });
  });
  const logout = h('button', null, 'Se déconnecter');
  logout.addEventListener('click', () => busy(logout, async () => { await post('/api/logout'); boot(); }));

  setKids(root, 
    h('div', { class: 'card' },
      h('h2', null, 'Wi-Fi'),
      h('p', { class: w.connected ? 'ok-text small' : 'small error-text' }, wifiState),
      w.ap ? h('p', { class: 'small muted' }, `Point d'accès de configuration actif : « ${w.ap_ssid} »`) : null,
      h('details', null, h('summary', null, 'Changer de réseau'), wifi.el, h('div', { class: 'row end actions' }, saveWifi))),
    ipCard,
    h('div', { class: 'card' },
      h('h2', null, 'Enceinte'),
      h('label', null, 'Nom ', h('span', { class: 'muted' }, '(adresse http://nom.local)')), host,
      h('label', null, 'Volume maximum'), h('div', { class: 'vol' }, maxVol, maxLbl),
      h('p', { class: 'small muted' }, 'Le curseur de l\'onglet Lecture, les boutons de l\'enceinte et les applications MPD ne peuvent pas dépasser ce volume.'),
      h('div', { class: 'row end actions' }, saveDev)),
    ctlCard,
    h('div', { class: 'card' },
      h('h2', null, 'Lecture des cartes'),
      h('p', { class: 'small muted' }, 'Réglages généraux, modifiables carte par carte (bouton ☰ de l\'onglet Cartes).'),
      h('label', { class: 'check' }, shuffle, ' Lire les morceaux dans un ordre aléatoire'),
      h('label', { class: 'check' }, repeat, ' Recommencer la playlist quand elle est finie'),
      h('label', null, 'Après le retrait de la carte, conserver la progression pendant'),
      h('div', { class: 'row' }, resumeMin, h('span', null, 'minutes')),
      h('p', { class: 'small muted' }, '0 : sans limite, la carte reprend toujours où elle en était.'),
      h('label', { class: 'check' }, resumeOther,
        ' Conserver la progression même si une autre carte est posée entre-temps'),
      h('div', { class: 'row end actions' }, saveResume)),
    h('div', { class: 'card' },
      h('h2', null, 'Son'),
      h('p', { class: 'small muted' }, 'Réglages généraux, modifiables carte par carte (bouton ☰ de l\'onglet Cartes).'),
      h('label', null, 'Normalisation'), normalize,
      h('p', { class: 'small muted' }, 'Égalise le volume d\'une playlist et d\'un morceau à l\'autre : les morceaux trop forts sont baissés en moins d\'une seconde, les plus calmes remontés en quelques secondes. Plus elle est forte, plus l\'écart est corrigé.'),
      h('label', null, 'Compression'), compress,
      h('p', { class: 'small muted' }, 'Réduit les écarts de volume à l\'intérieur d\'un morceau : passages calmes remontés, passages forts atténués, sans remonter le souffle des silences. Pratique pour les histoires et les livres audio.'),
      h('div', { class: 'row end actions' }, saveSound)),
    h('div', { class: 'card' },
      h('h2', null, 'Mot de passe administrateur'),
      h('label', null, 'Mot de passe actuel'), cur,
      h('label', null, 'Nouveau mot de passe'), nw,
      h('label', null, 'Confirmation'), nw2,
      h('div', { class: 'row end actions' }, savePw)),
    h('div', { class: 'card' },
      h('h2', null, 'Accès sécurisé (HTTPS)'),
      h('p', { class: 'small muted' },
        'Chiffre les échanges avec l\'interface, mot de passe compris. Le certificat est créé par l\'enceinte (auto-signé) : ',
        'votre navigateur affichera un avertissement la première fois, qu\'il faut accepter. Le Wi-Fi de configuration reste en HTTP.'),
      h('label', { class: 'check' }, httpsBox, ' Activer HTTPS'),
      httpsMsg,
      h('div', { class: 'row end actions' }, saveHttps)),
    h('div', { class: 'card' },
      h('h2', null, 'Accès MPD'),
      h('p', { class: 'small muted' },
        'Pilotez l\'enceinte avec une application MPD (M.A.L.P., mpc, Cantata…) à l\'adresse ',
        h('b', { class: 'mono' }, `${w.ip || location.hostname}:${s.mpd_port}`), '. ',
        'L\'adresse IP est attribuée par votre box : réservez-la dans ses réglages pour qu\'elle ne change pas. ',
        'Le protocole MPD ne connaît qu\'un mot de passe, sans identifiant.'),
      h('p', { class: 'small' }, s.mpd_password_set ? 'Un mot de passe est défini.' : 'Aucun mot de passe : accès libre depuis le réseau local.'),
      h('label', null, 'Mot de passe MPD ', h('span', { class: 'muted' }, '(vide = aucun)')), mpdPw,
      h('div', { class: 'row end actions' }, saveMpd)),
    h('div', { class: 'card' },
      h('h2', null, 'Mises à jour'),
      otaMsg, otaBar,
      h('label', null, 'Source des mises à jour'),
      h('div', { class: 'row' }, h('div', { class: 'grow' }, otaUrl), otaDefault),
      h('p', { class: 'small muted' }, 'Un dépôt GitHub (dernière release stable) ou l\'adresse d\'un manifeste JSON. Vide : pas de mise à jour automatique. ',
        s.ota_default_url ? `Valeur d'usine : ${s.ota_default_url}` : ''),
      h('label', null, 'Vérifier toutes les (heures)'), otaInt,
      h('div', { class: 'row end actions' }, check, saveOta),
      h('div', { class: 'row actions' }, fwBtn), fw),
    h('div', { class: 'card' },
      h('h2', null, 'Sauvegarde des réglages'),
      h('p', { class: 'small muted' },
        'Les réglages sont aussi enregistrés sur la carte SD (fichier .enceinte.json à la racine) avec, dans le dossier de chaque playlist, ses cartes associées (fichier .cartes.json). ',
        'Au démarrage, l\'enceinte les recharge : une copie de la carte SD placée dans une autre enceinte s\'y comporte exactement comme ici.'),
      h('label', { class: 'check' }, withSecrets, ' Inclure les mots de passe (Wi-Fi en clair, administrateur et MPD chiffrés)'),
      h('div', { class: 'row actions' }, exportBtn, importBtn), importInput),
    h('div', { class: 'card' },
      h('h2', null, 'Système'),
      h('p', { class: 'small muted' }, `Allumée depuis ${fmtTime(st.uptime)} · mémoire libre ${fmtSize(st.heap)}`),
      h('div', { class: 'row' }, reboot, logout, reset),
      journalBox()));
  app.view = { update: updateOta };
}

/*
 * Commandes de volume de l'enceinte : boutons poussoirs ou touches tactiles (pièces sous le
 * bois). Pour les touches, jauges en direct : l'écart mesuré quand on pose le doigt doit
 * dépasser nettement le seuil, et rester proche de zéro sans le doigt.
 */
function controlsCard(s) {
  clearInterval(app.touchTimer);
  const touch = h('input', { type: 'radio', name: 'volctl', checked: !!s.vol_touch });
  const buttons = h('input', { type: 'radio', name: 'volctl', checked: !s.vol_touch });
  const thr = h('input', { type: 'range', min: 0.3, max: 10, step: 0.1, value: s.touch_threshold_pct, 'aria-label': 'Seuil' });
  const thrLbl = h('span', { class: 'small muted' });
  const showThr = () => { thrLbl.textContent = Number(thr.value).toFixed(1).replace('.', ',') + ' %'; };
  showThr();
  const HOLDS = [300, 500, 800, 1000, 1500, 2000];
  const holds = HOLDS.includes(s.touch_hold_ms) ? HOLDS : [...HOLDS, s.touch_hold_ms].sort((a, b) => a - b);
  const hold = h('select', null, ...holds.map((ms) => h('option', { value: String(ms), selected: ms === s.touch_hold_ms },
    (ms / 1000).toString().replace('.', ',') + ' s' + (ms === 800 ? ' (conseillé)' : ''))));
  const meters = [0, 1].map((i) => {
    const fill = h('span');
    const val = h('span', { class: 'small mono' }, '–');
    return { fill, val, el: h('div', { class: 'touch-key' },
      h('b', null, i === 0 ? 'Touche +' : 'Touche −'),
      h('div', { class: 'meter' }, fill, h('i')), val) };
  });
  const live = h('p', { class: 'small muted' });
  const touchBox = h('div', { hidden: !s.vol_touch },
    h('label', null, 'Seuil de déclenchement'), h('div', { class: 'vol' }, thr, thrLbl),
    h('p', { class: 'small muted' }, 'Posez le doigt sur chaque pièce : la jauge doit dépasser franchement le trait (seuil). Sans le doigt, elle doit rester près de zéro. Plus le bois est épais, plus le seuil doit être bas.'),
    ...meters.map((m) => m.el), live,
    h('label', null, 'Maintenir le doigt avant que le volume change'), hold,
    h('p', { class: 'small muted' }, 'Un effleurement ne fait rien : utile contre les gestes involontaires des enfants. Les deux touches à la fois (main posée à plat) sont ignorées.'));
  const refresh = async () => {
    if (touchBox.hidden || !document.body.contains(touchBox)) return;
    let d;
    try { d = await api('/api/touch'); } catch (e) { return; }
    const t = Number(thr.value);
    if (!d.ok) { live.textContent = d.touch ? 'Capteur tactile indisponible (broches non tactiles ?).' : 'Enregistrez pour activer les touches tactiles.'; }
    else live.textContent = '';
    d.keys.forEach((k, i) => {
      const m = meters[i];
      /* jauge de 0 à deux fois le seuil : le trait du seuil est au milieu */
      m.fill.style.width = Math.max(0, Math.min(100, (k.delta_pct / (2 * t)) * 100)) + '%';
      m.fill.classList.toggle('on', k.delta_pct > t);
      m.val.textContent = d.ok ? `${k.delta_pct.toFixed(1).replace('.', ',')} %${k.touched ? ' · touchée' : ''}` : '–';
    });
  };
  thr.addEventListener('input', () => { showThr(); refresh(); });
  const toggle = () => { touchBox.hidden = !touch.checked; refresh(); };
  touch.addEventListener('change', toggle);
  buttons.addEventListener('change', toggle);
  app.touchTimer = setInterval(refresh, 400);
  refresh();
  const save = h('button', { class: 'primary' }, 'Enregistrer');
  save.addEventListener('click', () => busy(save, async () => {
    await post('/api/settings', { vol_touch: touch.checked, touch_threshold_pct: Number(thr.value), touch_hold_ms: Number(hold.value) });
    toast('Commandes de volume enregistrées');
  }));
  return h('div', { class: 'card' },
    h('h2', null, 'Commandes de volume sur l\'enceinte'),
    h('label', { class: 'check' }, touch, ' Touches tactiles (pièces ou disques de métal sous le bois)'),
    h('label', { class: 'check' }, buttons, ' Boutons poussoirs'),
    touchBox,
    h('div', { class: 'row end actions' }, save));
}

/* Adresse IP : DHCP ou fixe. Une nouvelle adresse est essayée 5 minutes avant d'être gardée. */
function networkCard(s, w) {
  const testMin = Math.round((s.ip_test_s || 300) / 60);
  const isStatic = s.ip && s.ip.mode === 'static';
  const cur = s.ip_current || {};
  const pick = (k) => (isStatic && s.ip[k]) || cur[k] || '';
  const field = (k, ph) => h('input', { type: 'text', inputmode: 'decimal', value: pick(k), placeholder: ph,
    autocapitalize: 'off', spellcheck: false });
  const address = field('address', '192.168.1.50'), netmask = field('netmask', '255.255.255.0');
  const gateway = field('gateway', '192.168.1.1'), dns = field('dns', 'facultatif : la passerelle');
  if (!isStatic) dns.value = '';
  const dhcp = h('input', { type: 'radio', name: 'ipmode', value: 'dhcp', checked: !isStatic });
  const fixed = h('input', { type: 'radio', name: 'ipmode', value: 'static', checked: isStatic });
  const fields = h('div', { hidden: !isStatic },
    h('label', null, 'Adresse IP'), address,
    h('label', null, 'Masque de sous-réseau'), netmask,
    h('label', null, 'Passerelle par défaut ', h('span', { class: 'muted' }, '(la box)')), gateway,
    h('label', null, 'Serveur DNS'), dns);
  const toggle = () => { fields.hidden = !fixed.checked; };
  dhcp.addEventListener('change', toggle);
  fixed.addEventListener('change', toggle);
  const apply = h('button', { class: 'primary' }, 'Appliquer');
  apply.addEventListener('click', () => busy(apply, async () => {
    const body = fixed.checked
      ? { mode: 'static', address: address.value.trim(), netmask: netmask.value.trim(), gateway: gateway.value.trim(), dns: dns.value.trim() }
      : { mode: 'dhcp' };
    const target = fixed.checked ? body.address : `${w.hostname}.local`;
    if (!confirm(`L'enceinte va passer ${fixed.checked ? 'à l\'adresse ' + body.address : 'en adresse automatique (DHCP)'}.\n\n` +
      `Connectez-vous à l'interface à http://${target} dans les ${testMin} minutes : sans connexion administrateur à cette adresse, l'enceinte reviendra d'elle-même à l'ancienne configuration.`)) return;
    await post('/api/network', body);
    showIpTest(target, s.ip_test_s || 300);
  }));
  return h('div', { class: 'card' },
    h('h2', null, 'Adresse IP'),
    h('p', { class: 'small' }, w.connected ? `Adresse actuelle : ${w.ip}` : 'Wi-Fi non connecté.'),
    h('label', { class: 'check' }, dhcp, ' Automatique (DHCP, attribuée par la box)'),
    h('label', { class: 'check' }, fixed, ' Fixe'),
    fields,
    h('p', { class: 'notice warn small' },
      `Après un changement, connectez-vous à l'interface à la nouvelle adresse dans les ${testMin} minutes. `,
      'Sans connexion administrateur à cette adresse dans ce délai, l\'enceinte revient d\'elle-même à l\'ancienne configuration.'),
    h('div', { class: 'row end actions' }, apply));
}

function showIpTest(target, seconds) {
  stopPolling();
  $('#tabs').hidden = true;
  app.view = null;
  const scheme = location.protocol === 'https:' ? 'https' : 'http';
  const url = `${scheme}://${target}/`;
  const left = h('b', null, fmtTime(seconds));
  const end = Date.now() + seconds * 1000;
  const timer = setInterval(() => {
    const r = Math.max(0, Math.round((end - Date.now()) / 1000));
    left.textContent = fmtTime(r);
    if (!r) clearInterval(timer);
  }, 1000);
  const same = target === location.hostname;
  setKids(root, h('div', { class: 'card stack' },
    h('h2', null, 'Nouvelle adresse à l\'essai'),
    same
      ? h('p', null, 'L\'adresse de cette page ne change pas : dans quelques secondes, revenez à l\'interface pour confirmer la nouvelle configuration.')
      : h('p', null, 'Ouvrez l\'interface à la nouvelle adresse et connectez-vous : ', h('a', { href: url }, url)),
    h('p', null, 'Temps restant : ', left, '. Sans connexion administrateur à cette adresse d\'ici là, l\'enceinte revient à l\'ancienne configuration, et cette page fonctionnera de nouveau.'),
    h('p', { class: 'small muted' }, 'Le mot de passe administrateur sera sans doute redemandé : le navigateur ne garde pas la connexion d\'une adresse à l\'autre.'),
    h('button', { onclick: () => { clearInterval(timer); boot(); } }, 'Revenir à l\'interface')));
}

async function importSettings(file) {
  let doc;
  try {
    doc = JSON.parse(await file.text());
  } catch (e) {
    toast('Ce fichier n\'est pas un fichier JSON valide', true);
    return;
  }
  if (!doc || doc.format !== 'enceinte-reglages') {
    toast('Ce fichier n\'est pas un fichier de réglages d\'enceinte', true);
    return;
  }
  const st = doc.settings || {};
  const parts = [];
  if (doc.settings) parts.push('les réglages généraux' + (st.hostname ? ` (enceinte « ${st.hostname} »)` : ''));
  if (Array.isArray(doc.cards)) parts.push(`les associations de cartes (${doc.cards.length}), qui remplaceront les actuelles`);
  if (st.wifi && 'password' in st.wifi) parts.push(`le Wi-Fi « ${st.wifi.ssid} »`);
  if (st.admin_password_hash) parts.push('le mot de passe administrateur');
  if (st.ip && st.ip.mode === 'static') parts.push(`l'adresse IP fixe ${st.ip.address} (à confirmer sous 5 minutes)`);
  if (!confirm('Importer ' + parts.join(', ') + ' ?')) return;
  const r = await api('/api/config/import', { method: 'POST', body: doc });
  toast(r.message || 'Réglages importés');
  if (r.ip_test) showIpTest(st.ip.address, 300);
  else renderSettings();
}

/*
 * Journal de l'enceinte (messages aussi envoyés sur le port série) et état de la mémoire, lus
 * pendant que le panneau est ouvert. Les temps « depuis le démarrage » sont convertis en heures.
 */
function journalBox() {
  const pre = h('pre', { class: 'log' });
  const mem = h('p', { class: 'small muted' });
  const follow = h('input', { type: 'checkbox', checked: true });
  const details = h('details', null, h('summary', null, 'Journal de l\'enceinte'));
  let next = 0, text = '', timer = null;
  const clock = (r, ms) => {
    if (!r.time) return fmtTime(ms / 1000);
    return new Date(r.time * 1000 - (r.uptime_ms - ms)).toLocaleTimeString('fr-FR');
  };
  const load = async () => {
    if (!details.open || app.tab !== 'settings' || !document.body.contains(pre)) {
      clearInterval(timer);
      timer = null;
      return;
    }
    const r = await api('/api/logs?since=' + next);
    if (r.reset) text = '';
    text += r.text.replace(/^([EWIDV]) \((\d+)\) /gm, (m, lvl, ms) => `${lvl} ${clock(r, Number(ms))} `);
    if (text.length > 300000) text = text.slice(text.indexOf('\n', text.length - 200000) + 1);
    next = r.next;
    pre.textContent = text || '(vide)';
    if (follow.checked) pre.scrollTop = pre.scrollHeight;
    const m = r.memory;
    mem.textContent = `Mémoire interne : ${fmtSize(m.internal_free)} libres (plus grand bloc ${fmtSize(m.internal_largest)}, `
      + `minimum depuis le démarrage ${fmtSize(m.internal_min)})`
      + (m.psram_total ? ` · PSRAM : ${fmtSize(m.psram_free)} libres sur ${fmtSize(m.psram_total)}` : '');
  };
  details.addEventListener('toggle', () => {
    if (details.open && !timer) {
      load().catch(reportError);
      timer = setInterval(() => load().catch(() => {}), 3000);
    }
  });
  const save = h('button', { class: 'small' }, '↓ Télécharger');
  save.addEventListener('click', () => {
    const a = h('a', { href: URL.createObjectURL(new Blob([text], { type: 'text/plain' })),
      download: `journal-enceinte-${new Date().toISOString().slice(0, 16).replace(':', 'h')}.txt` });
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });
  details.append(mem, pre,
    h('div', { class: 'row' }, h('label', { class: 'check', style: 'margin-top:0' }, follow, ' Suivre'), save));
  return details;
}

function uploadFirmware(file, bar, msg) {
  app.firmwareUpload = true;
  app.uploading = true;
  bar.hidden = false;
  const xhr = new XMLHttpRequest();
  xhr.open('PUT', '/api/ota/upload');
  xhr.setRequestHeader('X-Requested-With', 'enceinte');
  xhr.upload.onprogress = (ev) => { if (ev.lengthComputable) bar.firstChild.style.width = (ev.loaded * 100 / ev.total).toFixed(0) + '%'; };
  xhr.onloadend = () => {
    app.firmwareUpload = false;
    app.uploading = false;
    let err = '';
    try { err = JSON.parse(xhr.responseText).error || ''; } catch (e) { /* ignoré */ }
    if (xhr.status === 200) {
      msg.textContent = 'Firmware installé, redémarrage…';
      setTimeout(boot, 12000);
    } else {
      toast(err || 'Échec de l\'installation', true);
      bar.hidden = true;
    }
  };
  xhr.send(file);
}

boot();
