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
  if (!s) return 'toujours';
  if (s < 3600) return Math.round(s / 60) + ' min';
  const hh = s / 3600;
  return (Number.isInteger(hh) ? hh : hh.toFixed(1).replace('.', ',')) + ' h';
}

const DELAY_CHOICES = [0, 60, 300, 600, 1800, 3600, 10800, 86400];

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
  setChips(app.status);
  if (app.view && app.view.update) app.view.update(app.status);
}

function setChips(st) {
  const chips = $('#chips');
  if (!st) {
    setKids(chips, h('span', { class: 'chip bad' }, 'Hors ligne'));
    return;
  }
  const w = st.wifi;
  const list = [];
  if (w.connected) list.push(h('span', { class: 'chip ok', title: w.ip }, 'Wi-Fi'));
  else if (w.ap) list.push(h('span', { class: 'chip warn' }, 'Point d\'accès'));
  else list.push(h('span', { class: 'chip bad' }, 'Wi-Fi'));
  list.push(h('span', { class: 'chip ' + (st.sd.mounted ? 'ok' : 'bad') }, 'SD'));
  list.push(h('span', { class: 'chip ' + (st.card.reader_ok ? 'ok' : 'bad') }, 'NFC'));
  setKids(chips, ...list);
  $('#dev-name').textContent = w.hostname || 'Enceinte';
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
  v.vol = h('input', { type: 'range', min: 0, max: 100, step: 1, 'aria-label': 'Volume' });
  v.volLabel = h('span', { class: 'small muted' });
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
    if (!v.volDragging) {
      v.vol.value = p.volume;
      v.volLabel.textContent = p.volume + (p.max_volume < 100 ? ` (max ${p.max_volume})` : '');
    }
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
      h('div', { class: 'vol' }, h('span', { 'aria-hidden': 'true' }, '🔈'), v.vol, v.volLabel)),
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
    api('/api/settings').catch(() => ({ resume_s: 600, resume_after_other: false }))]);
  const list = h('ul', { class: 'list' });
  if (!data.cards.length) list.append(h('li', { class: 'muted' }, 'Aucune carte associée pour l\'instant.'));
  for (const c of data.cards) {
    const del = h('button', { class: 'small danger', 'aria-label': 'Supprimer', title: 'Supprimer' }, '✕');
    del.addEventListener('click', () => {
      if (!confirm(`Supprimer l'association de la carte ${c.uid} ?`)) return;
      busy(del, async () => { await post('/api/cards/delete', { uid: c.uid }); toast('Association supprimée'); renderCards(); });
    });
    const edit = h('button', { class: 'small', 'aria-label': 'Modifier', title: 'Modifier' }, '✎');
    edit.addEventListener('click', () => learnFlow(c.folder, c.uid, folders, c, defaults));
    const rules = [];
    if (c.resume_s !== null && c.resume_s !== undefined) rules.push(`reprise : ${fmtDelay(c.resume_s)}`);
    if (c.resume_other !== null && c.resume_other !== undefined) {
      rules.push(c.resume_other ? 'reprend même après une autre carte' : 'recommence après une autre carte');
    }
    list.append(h('li', null,
      h('span', { class: 'ico' }, '▣'),
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

/*
 * Association : capture d'une carte (mode association de l'enceinte, la musique ne
 * démarre pas) puis choix du dossier. knownUid permet de sauter la capture.
 */
function learnFlow(presetFolder, knownUid, folders, card = null, defaults = null) {
  let stopped = false;
  const box = h('div', { class: 'card stack' });
  const cancel = h('button', null, 'Annuler');
  cancel.addEventListener('click', () => {
    stopped = true;
    post('/api/cards/learn', { action: 'cancel' }).catch(() => {});
    renderCards();
  });
  setKids(root, box);
  app.view = null;

  const chooseFolder = async (uid) => {
    const sel = folderSelect(folders, presetFolder);
    if (!defaults) defaults = await api('/api/settings').catch(() => ({ resume_s: 600, resume_after_other: false }));
    const curDelay = card && card.resume_s !== null && card.resume_s !== undefined ? card.resume_s : null;
    const curOther = card && card.resume_other !== null && card.resume_other !== undefined ? card.resume_other : null;
    const delays = DELAY_CHOICES.includes(curDelay) || curDelay === null ? DELAY_CHOICES : [...DELAY_CHOICES, curDelay].sort((a, b) => a - b);
    const delaySel = h('select', null,
      h('option', { value: '', selected: curDelay === null }, `Réglage général (${fmtDelay(defaults.resume_s)})`),
      ...delays.map((d) => h('option', { value: String(d), selected: d === curDelay },
        d === 0 ? 'Toujours reprendre' : fmtDelay(d))));
    const otherSel = h('select', null,
      h('option', { value: '', selected: curOther === null },
        `Réglage général (${defaults.resume_after_other ? 'reprendre' : 'recommencer'})`),
      h('option', { value: '1', selected: curOther === true }, 'Reprendre où on en était'),
      h('option', { value: '0', selected: curOther === false }, 'Recommencer au début'));
    const save = h('button', { class: 'primary' }, card ? 'Enregistrer' : 'Associer');
    save.addEventListener('click', () => busy(save, async () => {
      if (!sel.value) { toast('Choisissez un dossier', true); return; }
      await post('/api/cards', {
        uid, folder: sel.value,
        resume_s: delaySel.value === '' ? null : Number(delaySel.value),
        resume_other: otherSel.value === '' ? null : otherSel.value === '1',
      });
      toast(card ? 'Carte modifiée' : 'Carte associée');
      renderCards();
    }));
    setKids(box,
      h('h2', null, card ? 'Modifier la carte' : 'Choisir la musique'),
      h('div', { class: 'big-uid mono' }, uid),
      h('label', null, 'Dossier de la carte SD'), sel,
      h('p', { class: 'muted small' }, 'Pour un sous-dossier, utilisez « Associer une carte » depuis l\'onglet Musique.'),
      h('label', null, 'Reprise après retrait de la carte'), delaySel,
      h('label', null, 'Si une autre carte a été posée entre-temps'), otherSel,
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
          h('div', { class: 'row end' }, cancel, h('button', { class: 'primary', onclick: () => learnFlow(presetFolder, '', folders, card, defaults) }, 'Réessayer')));
        return;
      }
    }
  })().catch(reportError);
}

/* ---------------- Musique ---------------- */

async function renderMusic(path = app.path) {
  let data;
  try {
    data = await api('/api/files?path=' + encodeURIComponent(path));
  } catch (e) {
    if (path) { app.path = ''; return renderMusic(''); }
    setKids(root, h('div', { class: 'notice bad' }, e.message));
    return;
  }
  app.path = data.path;

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
  for (const e of data.entries) {
    const full = joinPath(data.path, e.name);
    const ren = h('button', { class: 'small', 'aria-label': 'Renommer' }, '✎');
    ren.addEventListener('click', () => {
      const name = prompt('Nouveau nom :', e.name);
      if (!name || name === e.name) return;
      busy(ren, async () => { await post('/api/files/rename', { from: full, to: joinPath(data.path, name.trim()) }); renderMusic(); });
    });
    const del = h('button', { class: 'small danger', 'aria-label': 'Supprimer' }, '✕');
    del.addEventListener('click', () => {
      if (!confirm(e.dir ? `Supprimer le dossier « ${e.name} » et tout son contenu ?` : `Supprimer « ${e.name} » ?`)) return;
      busy(del, async () => { await post('/api/files/delete', { path: full }); toast('Supprimé'); renderMusic(); });
    });
    if (e.dir) {
      list.append(h('li', null,
        h('span', { class: 'ico' }, '📁'),
        h('div', { class: 'name' }, h('button', { class: 'linkish', onclick: () => renderMusic(full) }, e.name)),
        h('button', { class: 'small', 'aria-label': 'Lire', onclick: () => playFolder(full) }, '▶'),
        ren, del));
    } else {
      list.append(h('li', null,
        h('span', { class: 'ico' }, e.audio ? '♪' : '·'),
        h('div', { class: 'name' }, e.name, h('small', null, fmtSize(e.size))),
        ren, del));
    }
  }

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
  app.view = { uploadsBox, update: null };

  const folderActions = data.path ? h('div', { class: 'row' },
    h('button', { class: 'small', onclick: () => playFolder(data.path) }, '▶ Lire ce dossier'),
    h('button', { class: 'small', onclick: async () => learnFlowFromMusic(data.path) }, '▣ Associer une carte')) : null;

  setKids(root, 
    h('div', { class: 'card' },
      crumbs, folderActions,
      h('div', { class: 'row actions' },
        mkdir,
        h('button', { class: 'small', onclick: () => filesInput.click() }, '↑ Fichiers'),
        h('button', { class: 'small', onclick: () => dirInput.click() }, '↑ Dossier')),
      filesInput, dirInput,
      h('div', { class: 'actions' }, drop),
      uploadsBox),
    h('div', { class: 'card' }, list, h('div', { class: 'actions' }, usage)));
  drawUploads();
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

async function learnFlowFromMusic(folder) {
  app.tab = 'cards';
  for (const b of document.querySelectorAll('#tabs button')) b.classList.toggle('active', b.dataset.tab === 'cards');
  const folders = await topFolders().catch(() => []);
  learnFlow(folder, '', folders.includes(folder) ? folders : [folder, ...folders]);
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

  /* Reprise */
  const resumeMin = h('input', { type: 'number', min: 0, max: 43200, step: 1, value: Math.round(s.resume_s / 60) });
  const resumeOther = h('input', { type: 'checkbox', checked: !!s.resume_after_other, id: 'resume-other' });
  const saveResume = h('button', { class: 'primary' }, 'Enregistrer');
  saveResume.addEventListener('click', () => busy(saveResume, async () => {
    const m = Number(resumeMin.value);
    if (!Number.isFinite(m) || m < 0) { toast('Délai invalide', true); return; }
    await post('/api/settings', { resume_s: Math.round(m * 60), resume_after_other: resumeOther.checked });
    toast('Réglages de reprise enregistrés');
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
  const otaUrl = h('input', { type: 'url', value: s.ota_url, placeholder: 'https://github.com/compte/depot', autocapitalize: 'off', spellcheck: false });
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
    h('div', { class: 'card' },
      h('h2', null, 'Enceinte'),
      h('label', null, 'Nom ', h('span', { class: 'muted' }, '(adresse http://nom.local)')), host,
      h('label', null, 'Volume maximum'), h('div', { class: 'vol' }, maxVol, maxLbl),
      h('div', { class: 'row end actions' }, saveDev)),
    h('div', { class: 'card' },
      h('h2', null, 'Reprise de la lecture'),
      h('p', { class: 'small muted' }, 'Réglage général, modifiable carte par carte dans l\'onglet Cartes.'),
      h('label', null, 'Délai pour reprendre après le retrait d\'une carte (minutes)'), resumeMin,
      h('p', { class: 'small muted' }, '0 : toujours reprendre où on en était, quel que soit le délai.'),
      h('label', { class: 'check' }, resumeOther,
        ' Reprendre même si une autre carte a été posée entre-temps'),
      h('div', { class: 'row end actions' }, saveResume)),
    h('div', { class: 'card' },
      h('h2', null, 'Mot de passe administrateur'),
      h('label', null, 'Mot de passe actuel'), cur,
      h('label', null, 'Nouveau mot de passe'), nw,
      h('label', null, 'Confirmation'), nw2,
      h('div', { class: 'row end actions' }, savePw)),
    h('div', { class: 'card' },
      h('h2', null, 'Accès MPD'),
      h('p', { class: 'small muted' },
        `Pilotez l'enceinte avec une application MPD (M.A.L.P., mpc, Cantata…) : serveur ${w.hostname}.local, port ${s.mpd_port}. `,
        'Le protocole MPD ne connaît qu\'un mot de passe, sans identifiant.'),
      h('p', { class: 'small' }, s.mpd_password_set ? 'Un mot de passe est défini.' : 'Aucun mot de passe : accès libre depuis le réseau local.'),
      h('label', null, 'Mot de passe MPD ', h('span', { class: 'muted' }, '(vide = aucun)')), mpdPw,
      h('div', { class: 'row end actions' }, saveMpd)),
    h('div', { class: 'card' },
      h('h2', null, 'Mises à jour'),
      otaMsg, otaBar,
      h('label', null, 'Source des mises à jour'), otaUrl,
      h('p', { class: 'small muted' }, 'Un dépôt GitHub (dernière release stable) ou l\'adresse d\'un manifeste JSON. Vide : pas de mise à jour automatique.'),
      h('label', null, 'Vérifier toutes les (heures)'), otaInt,
      h('div', { class: 'row end actions' }, check, saveOta),
      h('div', { class: 'row actions' }, fwBtn), fw),
    h('div', { class: 'card' },
      h('h2', null, 'Système'),
      h('p', { class: 'small muted' }, `Allumée depuis ${fmtTime(st.uptime)} · mémoire libre ${fmtSize(st.heap)}`),
      h('div', { class: 'row' }, reboot, logout, reset)));
  app.view = { update: updateOta };
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
