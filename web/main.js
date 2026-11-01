// Front-end do emulador: vídeo em canvas, áudio, teclado, controle (Gamepad API), toque e saves.
// Roda Game Boy Advance (núcleo src/) e Game Boy / Game Boy Color (núcleo src/gb/) com a mesma interface.
import { isZip, extractRom } from './zip.js';
import { EmuClient } from './emu-client.js';
import { AudioOut } from './audio.js';
import { smsKind, smsNormalize, smsCode } from '../src/sms/header.js';
import { isNgpRom, ngpInfo } from '../src/ngp/header.js';

const KEY = { A: 0, B: 1, SELECT: 2, START: 3, RIGHT: 4, LEFT: 5, UP: 6, DOWN: 7, R: 8, L: 9 };
import { Renderer, FILTERS } from './renderer.js';
import { library, romId } from './library.js';
import { makeBackup, restoreBackup, isBackup } from './backup.js';
import { CHANGELOG } from './changelog.js';
import { Cloud } from './cloud.js';
import { GamepadInput, ACTIONS, XBOX_NAMES } from './gamepad.js';
import { transferStatus, sendCartridge, sendShelf, receiveCartridge, cancelTransfer } from './transfer.js';
import { ConsoleMenu } from './consolemenu.js';
import { initDownloads } from './downloads.js';

const $ = (id) => document.getElementById(id);
try { initDownloads(); } catch {}  // área de download do app (não pode atrapalhar o emulador)
const img32 = new Uint32Array(240 * 160); // quadro da tela de espera
const mainR = new Renderer($('screen'));
let tvR = null;          // criado ao abrir o Modo TV
let tvOpen = false;
const video = { filter: 'xbr', fit: 'aspect', strength: { xbr: 25, lcd: 70, crt: 70 }, lcdColor: false, frameBlend: false };
try {
  const saved = JSON.parse(localStorage.getItem('gba-video3') || '{}');
  Object.assign(video, saved, { strength: Object.assign(video.strength, saved.strength || {}) });
} catch {}
if (!FILTERS[video.filter]) video.filter = 'xbr';
const PARAM_LABEL = { xbr: 'Suavidade das bordas', lcd: 'Intensidade da grade', crt: 'Intensidade do efeito' };
function applyVideo() {
  for (const r of [mainR, tvR]) {
    if (!r) continue;
    r.filter = video.filter;
    r.lcdColor = video.lcdColor && sys !== 'gb'; r.frameBlend = video.frameBlend;
    for (const k of Object.keys(video.strength)) r.params[k] = video.strength[k] / 100;
  }
  mainR.fit = 'fill';
  if (tvR) tvR.fit = video.fit;
  $('filter').value = video.filter; $('fit').value = video.fit;
  $('lcdColor').checked = video.lcdColor; $('frameBlend').checked = video.frameBlend;
  const hasParam = video.filter in PARAM_LABEL;
  $('paramRow').hidden = !hasParam;
  if (hasParam) { $('paramLabel').textContent = PARAM_LABEL[video.filter]; $('param').value = video.strength[video.filter]; }
  try { localStorage.setItem('gba-video3', JSON.stringify(video)); } catch {}
}
function present(fb, w = 240, h = 160) {
  if (tvOpen && tvR) tvR.draw(fb, w, h); else mainR.draw(fb, w, h);
}
function presentGame() { if (lastFb) present(lastFb, lastW, lastH); }

// ---------------------------------------------------------------- consoles
const SCREEN = { gba: [240, 160], gb: [160, 144], gbc: [160, 144], gg: [160, 144], sms: [256, 192], ngp: [160, 152], ngpc: [160, 152] };
const SYS_NAME = { gba: 'Game Boy Advance', gb: 'Game Boy', gbc: 'Game Boy Color', gg: 'Game Gear', sms: 'Master System', ngp: 'Neo Geo Pocket', ngpc: 'Neo Geo Pocket Color' };
const SYS_TAG = { gba: 'GBA', gb: 'GB', gbc: 'GBC', gg: 'GG', sms: 'SMS', ngp: 'NGP', ngpc: 'NGPC', nds: 'DS' };
const FPS = { gg: 59.922, sms: 59.922, ngp: 59.95, ngpc: 59.95 };  // os demais: 59,7275
let romKind = null;  // console identificado ao abrir (pelo nome do arquivo, quando o cabeçalho não diz)
let sys = 'gba';
// o núcleo roda num Web Worker; aqui ficam só as informações do jogo e o último quadro
let info = { title: '', gameCode: '', saveLabel: '', hasSave: false, frame: 0 };
let lastFb = null, lastW = 240, lastH = 160;
let inFlight = 0, inFlightAt = 0;  // quadros pedidos ao núcleo e ainda não devolvidos
let lastSave = null, saveDirty = false;
const client = new EmuClient(onCoreMessage);
client.ready.then(mode => { document.body.dataset.core = mode; });

// identifica o console pelo cabeçalho (logotipo e campos fixos), com a extensão como desempate
function detectSystem(bytes, name) {
  const n = (name || '').toLowerCase();
  const gbLogo = bytes.length >= 0x150 && bytes[0x104] === 0xCE && bytes[0x105] === 0xED && bytes[0x106] === 0x66 && bytes[0x107] === 0x66;
  const gbaLogo = bytes.length >= 0xC0 && bytes[0x04] === 0x24 && bytes[0x05] === 0xFF && bytes[0x06] === 0xAE && bytes[0x07] === 0x51;
  if (isNgpRom(bytes)) return ngpInfo(bytes).color ? 'ngpc' : 'ngp';
  if (bytes.length >= 0x200 && bytes[0x15C] === 0x56 && bytes[0x15D] === 0xCF) return 'nds';
  if (/\.nds$/.test(n)) return 'nds';
  if (gbaLogo || bytes[0xB2] === 0x96 && /\.(gba|agb|bin)$/.test(n)) return 'gba';
  if (gbLogo) return (bytes[0x143] & 0x80) ? 'gbc' : 'gb';
  const sk = smsKind(bytes, n);
  if (sk) return sk;
  if (/\.(gba|agb)$/.test(n)) return 'gba';
  if (/\.(gbc|cgb)$/.test(n)) return 'gbc';
  if (/\.(gb|sgb)$/.test(n)) return 'gb';
  return null;
}

// título e código do jogo, como o núcleo de cada console lê
function romHeader(bytes, name, kind) {
  const s = kind || detectSystem(bytes, name);
  if (s === 'gg' || s === 'sms') return { sys: s, title: '', gameCode: smsCode(smsNormalize(bytes)) };
  if (s === 'ngp' || s === 'ngpc') { const i = ngpInfo(bytes); return { sys: s, title: i.title, gameCode: i.gameCode }; }
  if (s === 'gba') {
    let title = '';
    for (let k = 0xA0; k < 0xAC; k++) { const c = bytes[k]; if (c) title += String.fromCharCode(c); }
    let gameCode = '';
    for (let k = 0xAC; k < 0xB0; k++) gameCode += String.fromCharCode(bytes[k] || 32);
    return { sys: s, title: title.trim(), gameCode };
  }
  if (s === 'gb' || s === 'gbc') {
    const info = cartInfoOf(bytes);
    return { sys: s, title: info.title, gameCode: info.code };
  }
  return { sys: s, title: '', gameCode: '' };
}
function cartInfoOf(bytes) {
  let title = '';
  const end = (bytes[0x143] & 0x80) ? 0x143 : 0x144;
  for (let k = 0x134; k < end; k++) { const c = bytes[k]; if (!c) break; if (c >= 0x20 && c < 0x7F) title += String.fromCharCode(c); }
  const code = (bytes[0x14D] | (bytes[0x14E] << 8) | (bytes[0x14F] << 16)).toString(16).toUpperCase().padStart(6, '0');
  return { title: title.trim(), code };
}
const SAVE_PREFIX = { gba: 'gba-save:', gb: 'gb-save:', gbc: 'gb-save:', gg: 'sms-save:', sms: 'sms-save:', ngp: 'ngp-save:', ngpc: 'ngp-save:' };
// chave antiga (até a versão 1 da estante): console + código + título. Duas ROMs diferentes com o mesmo
// cabeçalho (homebrews, hacks) dividiam o save. A chave nova é única por ROM; a antiga só serve para migrar.
const saveKeyFor = (h) => (SAVE_PREFIX[h.sys] || 'gba-save:') + h.gameCode + ':' + h.title;
const romSaveKey = (bytes, h) => 'rom:' + romId(bytes, h.gameCode || '', h.title || '');
let running = false;
let paused = false;
let turbo = 1;
const audio = new AudioOut();
let saveKey = null;
let romBytes = null;
let biosBytes = null;

// ---------------------------------------------------------------- armazenamento local
function store(key, value) { try { localStorage.setItem(key, value); return true; } catch { return false; } }
function load(key) { try { return localStorage.getItem(key); } catch { return null; } }
function toB64(u8) { let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000)); return btoa(s); }
function fromB64(b) { const s = atob(b); const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i); return u; }

// registro de atividade deste aparelho (saves gravados/importados, backups, cartuchos adicionados)
function logEvent(text) {
  try {
    const list = JSON.parse(localStorage.getItem('indigo-log') || '[]');
    list.unshift({ at: Date.now(), text });
    localStorage.setItem('indigo-log', JSON.stringify(list.slice(0, 120)));
  } catch { /* sem armazenamento */ }
  if (typeof renderLog === 'function') renderLog();
}
function toast(msg) {
  const t = $('toast');
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toast._t);
  toast._t = setTimeout(() => { t.hidden = true; }, 2600);
}

// ---------------------------------------------------------------- tela de espera
let attractT = 0;
function drawAttract() {
  attractT++;
  for (let y = 0; y < 160; y++) {
    for (let x = 0; x < 240; x++) {
      const v = ((x + attractT) >> 3 ^ (y + (attractT >> 1)) >> 3) & 1;
      const g = 18 + v * 6 + ((y * 3) >> 5);
      img32[y * 240 + x] = (0xFF000000 | ((g + 14) << 16) | (g << 8) | (g + 2)) >>> 0;
    }
  }
  present(img32);
}

// ---------------------------------------------------------------- áudio
// Precisa ser chamada dentro do toque/clique, antes de qualquer await: no celular o som só é liberado no gesto.
let audioRate = 0;
function startAudio() {
  if (audio.running && audioRate) return;
  audio.unlock().then((kind) => {
    if (kind === 'none') return;
    if (audio.sampleRate !== audioRate) { audioRate = audio.sampleRate; client.post({ t: 'rate', rate: audioRate }); }
    applyVolume(); renderSound();
  }).catch(() => toast('O navegador não liberou o áudio.'));
  renderSound();
}
// o contexto pode parar sozinho (ligação, outro app tocando, tela bloqueada): a fila velha é descartada ao voltar
audio.onstate = (st) => { if (st === 'running') audio.clear(); renderSound(); };
const IS_IOS = /iPhone|iPad|iPod/.test(navigator.userAgent) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
function renderSound() {
  const el = $('soundState');
  if (!el) return;
  const on = audio.running;
  const show = running || !!audio.ctx;
  el.hidden = !show;
  el.dataset.on = on ? '1' : '0';
  el.dataset.warn = !on && running ? '1' : '0';
  el.textContent = on ? ($('mute').checked || +$('volume').value === 0 ? 'Som mudo' : 'Som ligado') : 'Som parado: toque aqui';
}
function testSound() {
  startAudio();
  audio.beep(); // o contexto já existe aqui (criado no próprio toque); o beep sai assim que ele estiver rodando
  setTimeout(() => {
    const muted = $('mute').checked || +$('volume').value === 0;
    const parts = [audio.running ? `Som ligado: você deve ter ouvido três notas (${audio.sampleRate} Hz, ${audio.kind === 'worklet' ? 'AudioWorklet' : 'modo compatível'})` : `O navegador ainda não liberou o som (estado: ${audio.state}); toque de novo`];
    if (muted) parts.push('o volume do Índigo está em zero ou "Sem som" está marcado');
    if (IS_IOS && !navigator.audioSession) parts.push('no iPhone, se não ouviu nada, desligue a chave de silencioso e aumente o volume');
    toast(parts.join('. ') + '.');
  }, 400);
}
function applyVolume() { audio.setVolume($('mute').checked ? 0 : +$('volume').value / 100); renderSound(); }

// ---------------------------------------------------------------- entrada
const keyMap = {
  ArrowUp: KEY.UP, ArrowDown: KEY.DOWN, ArrowLeft: KEY.LEFT, ArrowRight: KEY.RIGHT,
  KeyX: KEY.A, KeyZ: KEY.B, KeyA: KEY.L, KeyS: KEY.R,
  Enter: KEY.START, Backspace: KEY.SELECT, ShiftRight: KEY.SELECT,
};
let kbMask = 0, touchMask = 0, padMask = 0;

window.addEventListener('keydown', (e) => {
  if (typeof cmenu !== 'undefined' && cmenu.open) { if (cmenu.key(e.code)) e.preventDefault(); return; }
  if (e.code === 'KeyM' && !(e.target && e.target.tagName === 'INPUT' && e.target.type !== 'checkbox')) { cmenu.show(); e.preventDefault(); return; }
  if (e.target && (e.target.tagName === 'INPUT' && e.target.type !== 'checkbox' && e.target.type !== 'range')) return;
  // sem jogo aberto, setas, Enter e Espaço ficam com a página (rolar, abrir links e botões pelo teclado)
  if (!running && (e.code in keyMap || e.code === 'Space')) return;
  if (e.code in keyMap) { kbMask |= 1 << keyMap[e.code]; e.preventDefault(); }
  else if (e.code === 'Space') { kbTurbo = true; e.preventDefault(); }
  else if (e.code === 'KeyP') { togglePause(); }
});
window.addEventListener('keyup', (e) => {
  if (e.code in keyMap) { kbMask &= ~(1 << keyMap[e.code]); e.preventDefault(); }
  else if (e.code === 'Space') { kbTurbo = false; }
});
window.addEventListener('blur', () => { kbMask = 0; touchMask = 0; kbTurbo = false; });

// ---------------------------------------------------------------- controle
let kbTurbo = false;
const pad = new GamepadInput((action) => {
  if (typeof cmenu !== 'undefined' && cmenu.open) return;
  if (action === 'pause') { togglePause(); toast(paused ? 'Pausado' : 'Continuando'); }
  else if (action === 'tv') { tvOpen ? closeTv() : openTv(); }
});
function pollGamepads() {
  padMask = pad.poll();
  updatePadPanel();
}

let lastPadSig = '';
function updatePadPanel() {
  const p = pad.connected;
  const sig = (p ? p.id : '') + '|' + [...pad.lastPressed].join(',') + '|' + (pad.listener ? 'L' : '');
  if (sig === lastPadSig) return;
  const wasConnected = lastPadSig.split('|')[0] !== '';
  lastPadSig = sig;
  const chip = $('padState');
  document.body.dataset.pad = p ? '1' : '0';
  if (!!p !== wasConnected) updateImm();
  if (p) {
    chip.textContent = 'Controle conectado'; chip.dataset.on = '1';
    $('padName').textContent = p.id.replace(/\s*\(.*?Vendor.*?\)\s*/i, '').slice(0, 60) || 'Controle';
    $('padHint').textContent = p.mapping === 'standard' ? 'Layout padrão reconhecido.' : 'Layout não padrão: confira e remapeie os botões abaixo.';
    if (!wasConnected) { toast('Controle conectado'); pad.rumble(150); }
  } else {
    chip.textContent = 'Nenhum controle'; chip.dataset.on = '0';
    $('padName').textContent = 'Nenhum controle detectado';
    $('padHint').textContent = 'Conecte o controle e aperte qualquer botão: o navegador só mostra o controle depois do primeiro toque.';
  }
  for (const row of document.querySelectorAll('.map-row')) {
    const list = pad.settings.map[row.dataset.action] || [];
    row.classList.toggle('hit', list.some(i => pad.lastPressed.has(i)));
  }
  for (const dot of document.querySelectorAll('.pad-live [data-i]')) dot.classList.toggle('on', pad.lastPressed.has(+dot.dataset.i));
}

function renderPadMap() {
  const box = $('padMap');
  box.innerHTML = ACTIONS.map(([key, label]) => `
    <div class="map-row" data-action="${key}">
      <span class="map-name">${label}</span>
      <span class="map-btn" id="map-${key}">${pad.labelFor(key)}</span>
      <button class="btn small" type="button" data-remap="${key}">Trocar</button>
    </div>`).join('');
  $('padPreset').value = pad.settings.preset in { letters: 1, position: 1 } ? pad.settings.preset : 'custom';
  $('padStick').checked = pad.settings.stick;
  $('padDead').value = Math.round(pad.settings.deadzone * 100);
}

$('padMap').addEventListener('click', (e) => {
  const b = e.target.closest('[data-remap]');
  if (!b) return;
  const action = b.dataset.remap;
  if (!pad.connected) { toast('Conecte o controle e aperte um botão primeiro.'); return; }
  document.querySelectorAll('[data-remap]').forEach(x => { x.textContent = 'Trocar'; });
  b.textContent = 'Aperte…';
  $('map-' + action).textContent = 'Aperte o botão no controle';
  b.closest('.map-row').classList.add('listening');
  const timer = setTimeout(() => { pad.cancelListen(); renderPadMap(); }, 6000);
  pad.listen((idx) => {
    clearTimeout(timer);
    pad.assign(action, idx);
    pad.rumble(80);
    renderPadMap();
    toast(`${ACTIONS.find(a => a[0] === action)[1]} agora é ${XBOX_NAMES[idx] || 'botão ' + idx}`);
  });
});
$('padPreset').addEventListener('change', (e) => {
  if (e.target.value === 'custom') return;
  pad.applyPreset(e.target.value); renderPadMap();
});
$('padStick').addEventListener('change', (e) => { pad.settings.stick = e.target.checked; pad.save(); });
$('padDead').addEventListener('input', (e) => { pad.settings.deadzone = +e.target.value / 100; pad.save(); });
$('padTest').addEventListener('click', () => { if (pad.connected) pad.rumble(300); else toast('Nenhum controle conectado.'); });
renderPadMap();

// botões na tela (toque)
// Direcional por ângulo a partir do centro (diagonais fáceis, dá para deslizar o polegar sem levantar);
// A e B pelo botão mais próximo, com o meio dos dois apertando ambos; vibração curta ao apertar.
let haptic = true;
try { haptic = localStorage.getItem('indigo-haptic') !== '0'; } catch { /* ignorado */ }
function bindTouch() {
  const pressed = new Map(); // toque → { bits, zone }
  const dev = $('touch');
  const dpad = dev.querySelector('.dpad');
  const faces = [...dev.querySelectorAll('.ab .round')];
  const bitsOf = (el) => { let m = 0; for (const k of el.dataset.key.split(' ')) m |= 1 << KEY[k]; return m; };
  const U = 1 << KEY.UP, D = 1 << KEY.DOWN, L = 1 << KEY.LEFT, R = 1 << KEY.RIGHT;
  const DIRS = [R, R | D, D, D | L, L, L | U, U, U | R];
  // -1 = fora do direcional
  const dirAt = (x, y, sticky) => {
    const r = dpad.getBoundingClientRect();
    const rad = r.width / 2, dx = x - (r.left + rad), dy = y - (r.top + rad), dist = Math.hypot(dx, dy);
    if (!sticky && dist > rad * 1.3) return -1;
    if (dist < rad * 0.16) return 0;
    let a = Math.atan2(dy, dx) / (Math.PI / 4); // em oitavos de volta
    const k = Math.round(a), off = Math.abs(a - k);
    // diagonais um pouco mais estreitas que as direções retas (36° contra 54°)
    if (k % 2 !== 0 && off > 0.4) return DIRS[((a > k ? k + 1 : k - 1) + 8) & 7];
    return DIRS[(k + 8) & 7];
  };
  const faceAt = (x, y, sticky) => {
    const d = faces.filter(el => el.offsetParent).map(el => {
      const r = el.getBoundingClientRect();
      return { el, n: Math.hypot(x - (r.left + r.width / 2), y - (r.top + r.height / 2)) / (r.width / 2) };
    }).sort((p, q) => p.n - q.n);
    if (!d.length) return -1;
    const lim = sticky ? 2.4 : 1.35;
    if (d[0].n > lim) return sticky ? 0 : -1;
    if (d[1] && d[1].n < 1.6 && d[1].n - d[0].n < 0.35) return bitsOf(d[0].el) | bitsOf(d[1].el); // entre os dois
    return bitsOf(d[0].el);
  };
  const hit = (x, y, zone) => {
    if (zone === 'dpad') return { bits: dirAt(x, y, true), zone };
    if (zone === 'face') { const f = faceAt(x, y, true); if (f > 0) return { bits: f, zone }; }
    const el = document.elementFromPoint(x, y);
    if (!el || !dev.contains(el) || el.closest('.mini')) return { bits: 0, zone: null };
    const btn = el.closest('[data-key]');
    if (btn && !btn.closest('.dpad') && !btn.closest('.ab')) return { bits: bitsOf(btn), zone: 'key' };
    const dd = dirAt(x, y, false);
    if (dd >= 0) return { bits: dd, zone: 'dpad' };
    const f = faceAt(x, y, false);
    if (f >= 0) return { bits: f, zone: 'face' };
    return { bits: 0, zone: zone || null };
  };
  const recompute = () => {
    let m = 0;
    for (const p of pressed.values()) m |= p.bits;
    if (haptic && (m & ~touchMask) && navigator.vibrate) { try { navigator.vibrate(12); } catch { /* ignorado */ } }
    touchMask = m;
  };
  const update = (e) => {
    let ours = false;
    for (const t of e.changedTouches) {
      if (e.type === 'touchend' || e.type === 'touchcancel') {
        if (pressed.delete(t.identifier)) ours = true;
      } else if (e.type === 'touchstart') {
        // só captura toques que começam num controle; o resto rola a página normalmente
        const h = hit(t.clientX, t.clientY, null);
        if (h.zone) { pressed.set(t.identifier, h); ours = true; }
      } else if (pressed.has(t.identifier)) {
        const prev = pressed.get(t.identifier);
        pressed.set(t.identifier, hit(t.clientX, t.clientY, prev.zone)); ours = true;
      }
    }
    if (!ours) return;
    recompute();
    if (e.cancelable) e.preventDefault();
    if (!audio.running) startAudio();
  };
  for (const ev of ['touchstart', 'touchmove', 'touchend', 'touchcancel']) dev.addEventListener(ev, update, { passive: false });
  // celular: a página perde o foco (notificação, troca de app) sem touchend; solta tudo
  const releaseAll = () => { pressed.clear(); recompute(); };
  window.addEventListener('blur', releaseAll);
  document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); });
  dev.addEventListener('contextmenu', (e) => e.preventDefault());
  // mouse também funciona nos botões da tela
  dev.addEventListener('mousedown', (e) => { const h = hit(e.clientX, e.clientY, null); pressed.set('m', h); recompute(); });
  window.addEventListener('mousemove', (e) => { const p = pressed.get('m'); if (p && e.buttons) { pressed.set('m', hit(e.clientX, e.clientY, p.zone)); recompute(); } });
  window.addEventListener('mouseup', () => { pressed.delete('m'); recompute(); });
  // atalhos do celular deitado
  dev.addEventListener('click', (e) => {
    const b = e.target.closest('.mini');
    if (!b) return;
    if (b.dataset.act === 'menu') cmenu.show();
    else if (b.dataset.act === 'save') saveStateNow();
    else if (b.dataset.act === 'load') loadStateNow();
  });
}
$('haptic').checked = haptic;
$('haptic').addEventListener('change', (e) => { haptic = e.target.checked; try { localStorage.setItem('indigo-haptic', haptic ? '1' : '0'); } catch { /* ignorado */ } if (haptic && navigator.vibrate) navigator.vibrate(20); });
if (!('vibrate' in navigator)) $('hapticRow').hidden = true;
// Tela cheia do jogo no celular: com jogo aberto e o celular deitado (ou depois de pedir "Tela cheia"), o console
// com os botões de toque ocupa a tela toda. O Modo TV (só a imagem, para controle) não serve no toque: ele
// escondia os botões e, quando a tela cheia do navegador falhava, a imagem do jogo ficava parada por baixo.
const mq = (q) => { const m = matchMedia(q); const on = (f) => (m.addEventListener ? m.addEventListener('change', f) : m.addListener(f)); return { m, on }; };
const landscapeMq = mq('(orientation: landscape)');
const phoneLandMq = mq('(orientation: landscape) and (max-height: 540px) and (pointer: coarse)');
const coarseMq = mq('(pointer: coarse)');
function updateImm() {
  const b = document.body.dataset;
  const imm = b.loaded === '1' && b.immersive !== '0' && !tvOpen && landscapeMq.m.matches && (phoneLandMq.m.matches || b.fs === '1');
  b.imm = imm ? '1' : '0';
}
landscapeMq.on(() => { delete document.body.dataset.immersive; updateImm(); });
phoneLandMq.on(updateImm);
window.addEventListener('resize', updateImm);
// celular: tela de toque pequena (o Xbox e tablets seguem com o Modo TV). Com controle conectado (GameSir, Xbox,
// DualSense…), a tela cheia mostra só a imagem do jogo, sem os botões de toque.
const IS_PHONE = coarseMq.m.matches && !/Xbox/i.test(navigator.userAgent) && Math.min(screen.width, screen.height) <= 600;
const touchPlay = () => IS_PHONE;
function enterPlayFs() {
  document.body.dataset.fs = '1';
  delete document.body.dataset.immersive;
  updateImm();
  const el = document.documentElement, req = el.requestFullscreen || el.webkitRequestFullscreen;
  if (req) {
    Promise.resolve(req.call(el, { navigationUI: 'hide' }))
      .then(() => { try { return screen.orientation && screen.orientation.lock && screen.orientation.lock('landscape').catch(() => {}); } catch { /* ignorado */ } })
      .catch(() => {});
  }
  if (!landscapeMq.m.matches) toast('Deite o celular para jogar em tela cheia.');
}
function exitPlayFs() {
  delete document.body.dataset.fs;
  try { if (screen.orientation && screen.orientation.unlock) screen.orientation.unlock(); } catch { /* ignorado */ }
  if (document.fullscreenElement && document.exitFullscreen) document.exitFullscreen().catch(() => {});
  updateImm();
}
document.addEventListener('fullscreenchange', () => { if (!document.fullscreenElement && document.body.dataset.fs) { delete document.body.dataset.fs; updateImm(); } });

// ---------------------------------------------------------------- saves
// o núcleo manda o save junto com o quadro sempre que o jogo grava; aqui ele vai para o banco do navegador.
// Só é dado como gravado depois que o banco confirma; se falhar, tenta de novo e avisa.
let saveWriting = null, saveFailWarned = false;
function persistSave() {
  if (!saveKey || !saveDirty || !lastSave) return saveWriting || Promise.resolve();
  if (saveWriting) return saveWriting;
  const key = saveKey, data = lastSave;
  saveDirty = false;
  saveWriting = (async () => {
    let ok = false;
    if (library.available()) {
      try { await library.putSave(key, data); ok = true; } catch (e) { console.warn('save no banco falhou', e); }
    }
    if (!ok && store(key, toB64(data))) { store(key + ':at', String(Date.now())); ok = true; }
    if (ok) {
      saveFailWarned = false;
      if (key === saveKey) $('saveState').textContent = 'Save gravado às ' + new Date().toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
      savedKeys.add(key);
      cloud.touch(key);
    } else {
      if (key === saveKey && lastSave === data) saveDirty = true; // tenta de novo no próximo ciclo
      if (!saveFailWarned) { saveFailWarned = true; toast('Não foi possível gravar o save neste navegador (armazenamento cheio?). Exporte o .sav para não perder o progresso.'); }
      $('saveState').textContent = 'Save NÃO gravado: exporte o .sav';
    }
  })().finally(() => { saveWriting = null; });
  return saveWriting;
}
// lê o save de um jogo: banco (chave nova) → banco ou localStorage nas chaves antigas, migrando na hora
async function readSave(key, legacy = []) {
  if (library.available()) {
    try { const r = await library.getSave(key); if (r) return r.data; } catch { /* segue para as antigas */ }
  }
  for (const k of legacy) {
    let bytes = null;
    if (library.available()) { try { const r = await library.getSave(k); if (r) bytes = r.data; } catch { /* ignora */ } }
    if (!bytes) { const b = load(k); if (b) bytes = fromB64(b); }
    if (bytes) {
      if (library.available()) { try { await library.putSave(key, bytes, { source: 'migrado', reason: 'migrado da chave antiga', force: true }); } catch { /* fica na antiga */ } }
      return bytes;
    }
  }
  const b = load(key);
  return b ? fromB64(b) : null;
}
// copia para o banco os saves que estavam no localStorage (versões antigas do Índigo); a cópia antiga fica
let savedKeys = new Set();
let saveMeta = new Map(); // chave → { at, source } (para saber se dá para continuar)
async function migrateLegacySaves() {
  if (!library.available()) return;
  try {
    const have = new Set((await library.listSaves()).map(r => r.key));
    let n = 0;
    for (let i = 0; i < localStorage.length; i++) {
      const k = localStorage.key(i);
      if (!k || !/^(gba|gb|sms|ngp)-save:/.test(k) || /:(at|bak)$/.test(k) || have.has(k)) continue;
      const v = localStorage.getItem(k);
      if (!v) continue;
      await library.putSave(k, fromB64(v), { at: Number(localStorage.getItem(k + ':at')) || Date.now(), source: 'migrado', reason: 'vindo do armazenamento antigo', force: true });
      n++;
    }
    if (n) logEvent(`${n} save(s) migrados do armazenamento antigo para o banco do navegador`);
    savedKeys = new Set((await library.listSaves()).map(r => r.key));
  } catch (e) { console.warn('migração de saves', e); }
}
setInterval(persistSave, 2000);
window.addEventListener('pagehide', () => { persistSave(); });
document.addEventListener('visibilitychange', () => { if (document.hidden) persistSave(); });

// ---------------------------------------------------------------- ROM
// ---------------------------------------------------------------- estante (biblioteca local)
let currentId = null;
let currentName = '';

function niceName(fileName, title) {
  const base = (fileName || '').replace(/\.(gba|agb|gbc|gb|sgb|cgb|gg|sms|sg|ngp|ngc|npc|bin|zip)$/i, '').replace(/[_]+/g, ' ').trim();
  return base || title || 'Jogo sem nome';
}

async function startGame(bytes, name, kind) {
  await autoState();
  await persistSave();
  await flushPlayTime();
  romBytes = bytes;
  romKind = kind || detectSystem(bytes, name);
  await boot();
  currentName = name;
  $('romName').textContent = name;
  $('romMeta').textContent = `${SYS_NAME[sys]} · ${info.title || '—'} · ${bytes.length < 1048576 ? Math.round(bytes.length / 1024) + ' KB' : (bytes.length / 1048576).toFixed(1).replace('.', ',') + ' MB'} · save ${saveLabel()}`;
  document.body.dataset.loaded = '1';
  delete document.body.dataset.immersive;
  updateImm();
}

// lê o arquivo (descompacta .zip) e identifica o console; devolve null com o motivo se não for uma ROM
async function prepareRom(file) {
  let bytes = new Uint8Array(await file.arrayBuffer());
  let fileName = file.name;
  if (isZip(bytes)) {
    try { const r = await extractRom(bytes); bytes = r.data; fileName = r.name; }
    catch (e) { return { error: e.message || 'Não foi possível abrir o .zip.' }; }
  }
  const kind = detectSystem(bytes, fileName);
  if (kind === 'nds') return { error: 'Nintendo DS não é suportado.' };
  if (!kind || bytes.length < 0x150 || bytes.length > 0x2000000) return { error: 'não parece uma ROM de console suportado' };
  const h = romHeader(bytes, fileName, kind);
  return { bytes, fileName, kind, h, id: romId(bytes, h.gameCode || '', h.title || ''), name: niceName(fileName, h.title) };
}

// guarda na estante sem abrir o jogo; devolve 'added' ou 'exists'
async function addToShelf(r) {
  const isNew = await library.add({
    id: r.id, name: r.name, fileName: r.fileName, title: r.h.title, gameCode: r.h.gameCode, console: r.kind,
    size: r.bytes.length, saveKey: romSaveKey(r.bytes, r.h),
  }, r.bytes);
  library.persist().then(updateStorageNote);
  return isNew ? 'added' : 'exists';
}

async function openRom(file) {
  const r = await prepareRom(file);
  if (r.error) { toast(`${file.name}: ${r.error}`); return; }
  await startGame(r.bytes, r.name, r.kind);
  currentId = r.id;
  refreshStates();
  if (!library.available()) { toast('ROM carregada. Este navegador não permite guardar a estante.'); return; }
  try {
    const st = await addToShelf(r);
    await library.update(r.id, { lastPlayed: Date.now(), saveKey, console: sys });
    if (st === 'added') logEvent(`Cartucho adicionado: ${r.name} (${SYS_TAG[sys]})`);
    toast(st === 'added' ? 'Cartucho guardado na estante deste aparelho.' : 'Esse cartucho já estava na estante. Bom jogo!');
  } catch {
    toast('ROM carregada, mas não coube no armazenamento deste navegador.');
  }
  renderShelf();
}

// várias ROMs de uma vez: só guarda na estante, sem abrir
async function addRomsToShelf(files) {
  if (!library.available()) { toast('Este navegador não permite guardar a estante.'); return; }
  const res = { added: [], exists: [], bad: [] };
  let i = 0;
  for (const f of files) {
    i++;
    toast(`Adicionando ${i} de ${files.length}: ${f.name}`);
    try {
      const r = await prepareRom(f);
      if (r.error) { res.bad.push(`${f.name} (${r.error})`); continue; }
      (await addToShelf(r)) === 'added' ? res.added.push(r.name) : res.exists.push(r.name);
    } catch { res.bad.push(`${f.name} (sem espaço no armazenamento)`); }
  }
  if (res.added.length) logEvent(`${res.added.length} cartucho(s) adicionados: ${res.added.join(', ')}`);
  renderShelf();
  dialogOpen(`<h3>Estante atualizada</h3>
    <p class="note">${res.added.length} adicionado(s)${res.exists.length ? ` · ${res.exists.length} já estava(m) na estante` : ''}${res.bad.length ? ` · ${res.bad.length} ignorado(s)` : ''}.</p>
    ${res.bad.length ? `<ul class="dlg-list">${res.bad.map(b => `<li>${esc(b)}</li>`).join('')}</ul>` : ''}
    <div class="row"><button class="btn primary" type="button" data-xclose>Fechar</button></div>`);
}

async function playFromShelf(id) {
  startAudio(); // ainda dentro do toque: depois do primeiro await o celular não libera mais o som
  try {
    const rec = await library.get(id);
    if (!rec) { toast('Esse cartucho não está mais na estante.'); renderShelf(); return; }
    if (rec.console === 'nds') { toast('Nintendo DS não é suportado. Você pode remover este cartucho da estante.'); return; }
    startAudio();
    // estados de versões antigas ficaram na chave do save antiga: o boot migra o save sozinho
    await startGame(new Uint8Array(rec.data), rec.name, rec.console && rec.console !== 'nds' ? rec.console : null);
    currentId = id;
    refreshStates();
    await library.update(id, { lastPlayed: Date.now(), saveKey, console: sys });
    renderShelf();
    dialogClose();
    $('touch').scrollIntoView({ behavior: 'smooth', block: 'start' });
  } catch (e) {
    console.error('abrir cartucho', e);
    toast('Não foi possível abrir esse cartucho: ' + ((e && e.message) || 'erro'));
  }
}

// capa do cartucho: tirada sozinha uma vez, na primeira tela com conteúdo (nos primeiros segundos, quase sempre
// a tela-título), e depois fica fixa. Dá para trocar em Detalhes: tela atual, imagem própria ou remover.
function frameUrl(q = 0.82) {
  if (!lastFb) return null;
  const w = lastW, h = lastH;
  const c = document.createElement('canvas');
  c.width = w; c.height = h;
  const x = c.getContext('2d');
  const img = x.createImageData(w, h);
  new Uint32Array(img.data.buffer).set(lastFb.subarray(0, w * h));
  x.putImageData(img, 0, 0);
  return c.toDataURL('image/jpeg', q);
}
// tela com conteúdo: várias cores e nenhuma cobrindo quase tudo (evita logotipo em fundo liso e tela preta)
function interesting(fb, n) {
  const count = new Map();
  const step = Math.max(1, Math.floor(n / 4000));
  let total = 0;
  for (let i = 0; i < n; i += step) { const c = fb[i] & 0xF0F0F0; count.set(c, (count.get(c) || 0) + 1); total++; }
  if (count.size < 3) return false;
  return Math.max(...count.values()) / total < 0.9;
}
// tempo de jogo por cartucho (conta só com o jogo rodando, sem pausa e com a aba visível)
let playPending = 0;
setInterval(() => { if (running && !paused && !document.hidden && currentId) playPending += 5; }, 5000);
async function flushPlayTime() {
  if (!playPending || !currentId) return;
  const id = currentId, add = playPending;
  playPending = 0;
  try { const m = await library.meta(id); if (m) await library.update(id, { playTime: (m.playTime || 0) + add }); } catch { playPending += add; }
}
setInterval(flushPlayTime, 60000);
document.addEventListener('visibilitychange', () => { if (document.hidden) flushPlayTime(); });

let coverTry = 0;
async function setCover(id, url, how) {
  await library.update(id, { cover: url, coverBy: how });
  const r = shelfCache.find(x => x.id === id); if (r) { r.cover = url; r.coverBy = how; }
  renderShelf();
}
function autoCover() {
  if (!currentId || !running || !lastFb || paused) return;
  const r = shelfCache.find(x => x.id === currentId);
  if (!r || r.cover) return;
  if (!interesting(lastFb, lastW * lastH) && info.frame < 3600) return; // depois de 1 min aceita qualquer tela
  try { setCover(currentId, frameUrl(), 'auto'); } catch { /* sem capa */ }
}
// imagem escolhida pelo usuário: recorta no centro em 3:2 e reduz para 360×240
function imageToCover(file) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const im = new Image();
    im.onload = () => {
      const W = 360, H = 240, c = document.createElement('canvas'); c.width = W; c.height = H;
      const s = Math.max(W / im.width, H / im.height), w = im.width * s, h = im.height * s;
      c.getContext('2d').drawImage(im, (W - w) / 2, (H - h) / 2, w, h);
      URL.revokeObjectURL(url);
      resolve(c.toDataURL('image/jpeg', 0.85));
    };
    im.onerror = () => { URL.revokeObjectURL(url); reject(new Error('Não deu para ler essa imagem.')); };
    im.src = url;
  });
}

const rtf = (() => { try { return new Intl.RelativeTimeFormat('pt-BR', { numeric: 'auto' }); } catch { return null; } })();
function ago(t) {
  if (!t) return 'Nunca jogado';
  const s = (t - Date.now()) / 1000;
  const units = [['year', 31536000], ['month', 2592000], ['week', 604800], ['day', 86400], ['hour', 3600], ['minute', 60]];
  for (const [u, v] of units) if (Math.abs(s) >= v) return rtf ? 'Jogado ' + rtf.format(Math.round(s / v), u) : new Date(t).toLocaleDateString('pt-BR');
  return 'Jogado agora';
}

function esc(t) { return String(t).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }

let shelfCache = [];
const shelfView = { q: '', sys: 'all', sort: 'recent' };
try { Object.assign(shelfView, JSON.parse(localStorage.getItem('indigo-shelf-view') || '{}'), { q: '' }); } catch { /* padrão */ }
const fmtDate = (t) => t ? new Date(t).toLocaleString('pt-BR', { day: '2-digit', month: '2-digit', year: '2-digit', hour: '2-digit', minute: '2-digit' }) : '—';
const fmtSize = (n) => n >= 1048576 ? (n / 1048576).toFixed(1).replace('.', ',') + ' MB' : Math.max(1, Math.round(n / 1024)) + ' KB';
function fmtPlay(sec) {
  if (!sec) return 'ainda não contado';
  const h = Math.floor(sec / 3600), m = Math.round((sec % 3600) / 60);
  return h ? `${h} h ${m} min` : `${m || 1} min`;
}
const sysGroup = (c) => c === 'gbc' ? 'gb' : c === 'ngpc' ? 'ngp' : (c || 'gba');
const GROUP_NAME = { gba: 'GBA', gb: 'GB / GBC', gg: 'Game Gear', sms: 'Master System', ngp: 'Neo Geo Pocket' };
// filtros da estante: todos, favoritos, cada console presente e cada coleção criada
function filterOptions() {
  const opts = [{ v: 'all', label: 'Todos' }];
  if (shelfCache.some(r => r.fav)) opts.push({ v: 'fav', label: '★ Favoritos' });
  for (const g of [...new Set(shelfCache.map(r => sysGroup(r.console)))]) opts.push({ v: g, label: GROUP_NAME[g] || g });
  for (const t of allTags()) opts.push({ v: 'tag:' + t, label: '# ' + t });
  return opts;
}
function allTags() { return [...new Set(shelfCache.flatMap(r => r.tags || []))].sort((a, b) => a.localeCompare(b, 'pt-BR')); }
function matchFilter(r) {
  const f = shelfView.sys;
  if (f === 'all') return true;
  if (f === 'fav') return !!r.fav;
  if (f.startsWith('tag:')) return (r.tags || []).includes(f.slice(4));
  return sysGroup(r.console) === f;
}

async function renderShelf() {
  const grid = $('shelfGrid');
  if (!library.available()) { $('shelfEmpty').hidden = false; $('shelfEmpty').textContent = 'Este navegador não permite guardar jogos.'; return; }
  try { shelfCache = await library.list(); } catch { shelfCache = []; }
  try { const ls = await library.listSaves(); saveMeta = new Map(ls.map(x => [x.key, x])); savedKeys = new Set(ls.map(x => x.key)); } catch { /* mantém */ }
  // continuar: o jogo com o estado automático mais recente (fora o que está aberto)
  const resume = shelfCache.filter(r => canResume(r) && !(r.id === currentId && running)).sort((a, b) => b.autoAt - a.autoAt)[0];
  $('resumeBar').hidden = !resume;
  if (resume) $('resumeBar').innerHTML = `${resume.cover ? `<img src="${resume.cover}" alt="">` : ''}<span><b>Continuar ${esc(resume.name)}</b><small>Parou ${esc(ago(resume.autoAt).replace('Jogado ', ''))} · ${esc(SYS_NAME[resume.console] || '')}</small></span><button class="btn primary" type="button" data-resume="${esc(resume.id)}">Continuar</button>`;
  // filtros só com os consoles que existem na estante
  const fopts = filterOptions();
  if (!fopts.some(o => o.v === shelfView.sys)) shelfView.sys = 'all';
  $('shelfTools').hidden = shelfCache.length < 2;
  $('shelfChips').innerHTML = fopts.length > 2 || shelfView.sys !== 'all' ? fopts.map(o =>
    `<button type="button" class="chip${shelfView.sys === o.v ? ' on' : ''}" data-sys="${esc(o.v)}">${esc(o.label)}</button>`).join('') : '';
  $('shelfSort').value = shelfView.sort;
  const q = shelfView.q.trim().toLowerCase();
  let items = shelfCache.filter(r => matchFilter(r) &&
    (!q || (r.name + ' ' + (r.title || '') + ' ' + (r.gameCode || '')).toLowerCase().includes(q)));
  const by = {
    recent: (a, b) => (b.lastPlayed || b.added || 0) - (a.lastPlayed || a.added || 0),
    name: (a, b) => a.name.localeCompare(b.name, 'pt-BR', { sensitivity: 'base' }),
    added: (a, b) => (b.added || 0) - (a.added || 0),
    time: (a, b) => (b.playTime || 0) - (a.playTime || 0),
    console: (a, b) => sysGroup(a.console).localeCompare(sysGroup(b.console)) || a.name.localeCompare(b.name, 'pt-BR'),
  }[shelfView.sort] || ((a, b) => 0);
  items = items.sort(by);
  $('shelfEmpty').hidden = shelfCache.length > 0;
  $('shelfNone').hidden = !shelfCache.length || items.length > 0;
  $('shelfCount').textContent = shelfCache.length ? `${shelfCache.length} ${shelfCache.length === 1 ? 'cartucho' : 'cartuchos'}` : '';
  grid.innerHTML = items.map(r => {
    const hasSave = !!r.saveKey && (savedKeys.has(r.saveKey) || load(r.saveKey) !== null);
    const playing = r.id === currentId && running;
    const cover = r.cover
      ? `<img src="${r.cover}" alt="" width="240" height="160">`
      : `<span class="nocover">${esc((r.name || '?').slice(0, 2).toUpperCase())}</span>`;
    return `<div class="cart-item" data-id="${esc(r.id)}">
      <button class="cart-fav${r.fav ? ' on' : ''}" type="button" data-fav="${esc(r.id)}" aria-pressed="${r.fav ? 'true' : 'false'}" aria-label="${r.fav ? 'Tirar dos favoritos' : 'Marcar como favorito'}">${r.fav ? '★' : '☆'}</button>
      <button class="cart-play" type="button" data-play="${esc(r.id)}" aria-label="Jogar ${esc(r.name)}">
        <span class="cart-shell"><span class="cart-label">${cover}</span><span class="cart-ridge"></span></span>
        <span class="cart-name">${esc(r.name)}</span>
      </button>
      <span class="cart-meta"><b class="tag sys-${esc(r.console || 'gba')}">${SYS_TAG[r.console || 'gba'] || 'GBA'}</b> ${playing ? '<b class="tag on">Jogando</b>' : esc(ago(r.lastPlayed))}${hasSave ? ' <b class="tag">Save</b>' : ''}</span>
      <span class="cart-actions">${canResume(r) && !playing ? `<button class="cart-send" type="button" data-resume="${esc(r.id)}">Continuar</button>` : ''}<button class="cart-send" type="button" data-info="${esc(r.id)}">Detalhes</button><button class="cart-send xfer-only" type="button" data-send="${esc(r.id)}">Enviar</button></span>
    </div>`;
  }).join('');
  updateStorageNote();
}

async function updateStorageNote() {
  const u = await library.usage();
  let persisted = null;
  try { if (navigator.storage && navigator.storage.persisted) persisted = await navigator.storage.persisted(); } catch { /* sem informação */ }
  $('shelfUsage').textContent = u && u.usage ? `${fmtSize(u.usage)} usados neste aparelho` : '';
  const last = Number(load('indigo-last-backup') || 0);
  const old = !last || Date.now() - last > 14 * 86400000;
  const warn = $('storageWarn');
  const cs = cloud.statusText();
  $('cloudNote').hidden = !cs;
  $('cloudNote').innerHTML = cs ? `${esc(cs)} ${cloud.state === 'locked' ? '<button class="linkbtn" type="button" data-cloud-pin>Digitar PIN</button> · ' : ''}<button class="linkbtn" type="button" data-backup>Opções</button>` : '';
  $('cloudNote').dataset.state = cloud.state;
  warn.hidden = !shelfCache.length || ((persisted === true || cloud.state === 'idle') && !old);
  warn.innerHTML = persisted === true
    ? `Seu último backup foi ${last ? esc(ago(last).replace('Jogado ', '')) : 'nunca'}. Um backup guarda ROMs, saves e estados num arquivo só. <button class="linkbtn" type="button" data-backup>Fazer backup</button>`
    : `Este navegador pode apagar a estante quando faltar espaço${isXbox ? ' (o Xbox costuma limpar dados de sites)' : ''}.${cloud.state === 'idle' ? ' Os saves estão na nuvem, mas as ROMs e os estados só ficam aqui.' : ''} Faça backup de vez em quando${last ? `; o último foi ${esc(ago(last).replace('Jogado ', ''))}` : ''}. <button class="linkbtn" type="button" data-backup>Fazer backup</button>`;
}

document.addEventListener('click', (e) => { const r = e.target.closest('[data-resume]'); if (r) { startAudio(); resumeGame(r.dataset.resume); } });
$('shelfGrid').addEventListener('click', (e) => {
  const play = e.target.closest('[data-play]');
  if (play) { playFromShelf(play.dataset.play); return; }
  const send = e.target.closest('[data-send]');
  if (send) { sendFromShelf(send.dataset.send); return; }
  const info = e.target.closest('[data-info]');
  if (info) { openDetails(info.dataset.info); return; }
  const fav = e.target.closest('[data-fav]');
  if (fav) toggleFav(fav.dataset.fav);
});
async function toggleFav(id) {
  const r = shelfCache.find(x => x.id === id) || await library.meta(id);
  if (!r) return;
  await library.update(id, { fav: !r.fav });
  toast(r.fav ? `${r.name} saiu dos favoritos.` : `${r.name} está nos favoritos.`);
  await renderShelf();
}
async function setTags(id, tags) {
  const clean = [...new Set(tags.map(t => t.trim().slice(0, 30)).filter(Boolean))];
  await library.update(id, { tags: clean });
  const r = shelfCache.find(x => x.id === id); if (r) r.tags = clean;
  await renderShelf();
}
$('shelfSearch').addEventListener('input', () => { shelfView.q = $('shelfSearch').value; renderShelf(); });
const saveView = () => { try { localStorage.setItem('indigo-shelf-view', JSON.stringify({ sys: shelfView.sys, sort: shelfView.sort })); } catch { /* ignorado */ } };
$('shelfChips').addEventListener('click', (e) => { const c = e.target.closest('[data-sys]'); if (!c) return; shelfView.sys = c.dataset.sys; saveView(); renderShelf(); });
$('shelfSort').addEventListener('change', () => { shelfView.sort = $('shelfSort').value; saveView(); renderShelf(); });
$('addRoms').addEventListener('change', (e) => { const fs = [...e.target.files]; e.target.value = ''; if (fs.length) addRomsToShelf(fs); });
document.addEventListener('click', (e) => {
  if (e.target.closest('[data-backup]')) openBackup();
  if (e.target.closest('[data-cloud-pin]')) cloud.unlock().then(r => { if (r) toast(`PIN aceito. Nuvem sincronizada: ${r.up} enviado(s), ${r.down} recebido(s).`); else if (cloud.state === 'locked') toast('PIN incorreto ou não digitado.'); });
});
$('backupBtn').addEventListener('click', openBackup);

// ---------------------------------------------------------------- estante pelo controle (menu do console)
const SORT_NAME = { recent: 'Jogados recentemente', name: 'Nome (A–Z)', added: 'Adicionados por último', time: 'Mais tempo de jogo', console: 'Console' };
function shelfSorted() {
  const by = {
    recent: (a, b) => (b.lastPlayed || b.added || 0) - (a.lastPlayed || a.added || 0),
    name: (a, b) => a.name.localeCompare(b.name, 'pt-BR', { sensitivity: 'base' }),
    added: (a, b) => (b.added || 0) - (a.added || 0),
    time: (a, b) => (b.playTime || 0) - (a.playTime || 0),
    console: (a, b) => sysGroup(a.console).localeCompare(sysGroup(b.console)) || a.name.localeCompare(b.name, 'pt-BR'),
  }[shelfView.sort] || (() => 0);
  return shelfCache.filter(matchFilter).sort(by);
}
async function shelfForMenu() {
  await renderShelf();
  return shelfSorted().map(r => Object.assign({}, r, { tag: SYS_TAG[r.console] || '', resume: canResume(r) && !(r.id === currentId && running) }));
}
async function cartInfoForMenu(id) {
  const rec = shelfCache.find(r => r.id === id) || await library.meta(id).catch(() => null);
  if (!rec) return null;
  const current = id === currentId && running;
  if (current) await persistSave();
  const states = (await library.listStates(id).catch(() => [])).filter(s => s.slot > 0).sort((a, b) => a.slot - b.slot);
  const sm = (await library.listSaves().catch(() => [])).find(x => x.key === rec.saveKey);
  const hist = rec.saveKey ? await library.saveHistory(rec.saveKey).catch(() => []) : [];
  return {
    name: rec.name, cover: rec.cover, current, fav: !!rec.fav,
    sub: `${SYS_NAME[rec.console] || ''} · ${rec.lastPlayed ? ago(rec.lastPlayed) : 'Nunca jogado'} · tempo de jogo: ${fmtPlay(rec.playTime)}`,
    resume: !current && canResume(rec), resumeHint: rec.autoAt ? 'Parou ' + ago(rec.autoAt).replace('Jogado ', '') : '',
    saveHint: sm ? `Save de ${fmtDate(sm.at)}` : 'Sem save ainda',
    states: states.map(s => ({ slot: s.slot, hint: (s.name ? s.name + ' · ' : '') + fmtDate(s.at) })),
    versions: hist.map(v => ({ hid: v.hid, label: fmtDate(v.at), hint: `${fmtSize(v.size || v.data.length)} · ${v.reason || ''}` })),
  };
}
async function restoreVersionById(id, hid) {
  const rec = shelfCache.find(r => r.id === id) || await library.meta(id);
  if (!rec || !rec.saveKey) return;
  const v = (await library.saveHistory(rec.saveKey)).find(x => x.hid === hid);
  if (!v) return;
  const isCur = id === currentId && running;
  if (isCur) await persistSave();
  await library.snapshotSave(rec.saveKey, 'antes de restaurar versão').catch(() => {});
  await library.putSave(rec.saveKey, v.data, { source: 'restaurado', reason: 'versão restaurada', force: true });
  logEvent(`Save de ${rec.name} restaurado para a versão de ${fmtDate(v.at)}`);
  if (isCur) await boot();
  toast(`Save de ${fmtDate(v.at)} restaurado.`);
  renderShelf();
}
async function removeCart(id, all) {
  const rec = shelfCache.find(r => r.id === id) || await library.meta(id);
  if (!rec) return;
  if (id === currentId) await persistSave();
  if (all) await library.removeAll(id, rec.saveKey); else await library.remove(id);
  if (id === currentId) currentId = null;
  logEvent(`Cartucho removido: ${rec.name}${all ? ' (com save e estados)' : ' (save mantido)'}`);
  toast(all ? 'Cartucho, save e estados removidos.' : 'Cartucho removido. O save continua guardado.');
  renderShelf();
}

// ---------------------------------------------------------------- saves na nuvem
let shelfTimer = 0;
const cloud = new Cloud({
  library,
  saveInfo: (key) => { const r = shelfCache.find(x => x.saveKey === key); return r ? { name: r.name, console: r.console } : null; },
  isBusy: (key) => running && key === saveKey,
  log: (t) => logEvent(t),
  onChange: () => {
    clearTimeout(shelfTimer);
    shelfTimer = setTimeout(async () => {
      try { savedKeys = new Set((await library.listSaves()).map(r => r.key)); } catch { /* mantém */ }
      renderShelf();
      if (running && cloud.waiting.has(saveKey) && !cloud.warned) {
        cloud.warned = true;
        const w = cloud.waiting.get(saveKey);
        toast(`Há um save mais novo deste jogo na nuvem (${w.from || 'outro aparelho'}). Ele fica nas versões da nuvem; veja em Detalhes.`);
      }
    }, 300);
  },
});
setInterval(() => { if (!document.hidden) cloud.sync('periódica'); }, 5 * 60 * 1000);
document.addEventListener('visibilitychange', () => { if (document.hidden) cloud.flush(); else cloud.sync('volta à aba'); });

// ---------------------------------------------------------------- detalhes do cartucho
let detailId = null;
async function openDetails(id) {
  detailId = id;
  if (id === currentId) await persistSave();
  const rec = await library.get(id).catch(() => null);
  if (!rec) { toast('Esse cartucho não está mais na estante.'); renderShelf(); return; }
  const bytes = new Uint8Array(rec.data);
  const h = romHeader(bytes, rec.fileName, rec.console);
  const key = romSaveKey(bytes, h);
  const sv = await readSave(key, [rec.saveKey, saveKeyFor(h)].filter(k => k && k !== key));
  if (rec.saveKey !== key) { await library.update(id, { saveKey: key }); rec.saveKey = key; }
  const cur = await library.getSave(key).catch(() => null);
  const hist = await library.saveHistory(key).catch(() => []);
  const states = await library.listStates(id).catch(() => []);
  const isCur = id === currentId && running;
  const cover = rec.cover ? `<img src="${rec.cover}" alt="">` : `<span class="nocover">${esc((rec.name || '?').slice(0, 2).toUpperCase())}</span>`;
  dialogOpen(`<div class="dlg-wide">
    <div class="det-top">
      <div class="det-cover">${cover}</div>
      <div class="det-head">
        <label class="sr" for="detName">Nome na estante</label>
        <div class="det-rename"><input id="detName" value="${esc(rec.name)}" maxlength="80" autocomplete="off"><button class="btn small" type="button" data-rename>Renomear</button></div>
        <div class="cart-meta"><b class="tag sys-${esc(rec.console || 'gba')}">${esc(SYS_NAME[rec.console] || rec.console || '')}</b>${isCur ? ' <b class="tag on">Jogando</b>' : ''}</div>
        <dl class="det-info">
          <dt>Título interno</dt><dd>${esc(rec.title || '—')}</dd>
          <dt>Código</dt><dd>${esc(rec.gameCode || '—')}</dd>
          <dt>Tamanho</dt><dd>${fmtSize(rec.size || bytes.length)}</dd>
          <dt>Na estante desde</dt><dd>${fmtDate(rec.added)}</dd>
          <dt>Última vez</dt><dd>${rec.lastPlayed ? fmtDate(rec.lastPlayed) : 'nunca'}</dd>
          <dt>Tempo de jogo</dt><dd>${fmtPlay(rec.playTime)}</dd>
        </dl>
        <div class="row"><button class="btn small${rec.fav ? ' on' : ''}" type="button" data-det-fav>${rec.fav ? '★ Favorito' : '☆ Marcar como favorito'}</button></div>
      </div>
    </div>
    <h4>Coleções</h4>
    <div class="det-cols chips">${allTags().map(t => `<button type="button" class="chip${(rec.tags || []).includes(t) ? ' on' : ''}" data-tag="${esc(t)}">${esc(t)}</button>`).join('')}
      <span class="det-newtag"><input id="detNewTag" maxlength="30" placeholder="Nova coleção (ex.: Zerados)" aria-label="Nova coleção"><button class="btn small" type="button" data-tag-add>Adicionar</button></span></div>
    <h4>Capa</h4>
    <div class="row">
      ${isCur ? '<button class="btn small" type="button" data-cover-screen>Usar a tela atual</button>' : ''}
      <label class="btn small">Escolher imagem<input type="file" id="detCover" accept="image/*"></label>
      ${rec.cover ? '<button class="btn small" type="button" data-cover-del>Remover capa</button>' : ''}
      <span class="note">${rec.cover ? (rec.coverBy === 'imagem' ? 'Imagem escolhida por você.' : rec.coverBy === 'tela' ? 'Tela escolhida por você.' : 'Tirada sozinha na primeira tela do jogo.') : 'Sem capa: ela é tirada sozinha nos primeiros segundos de jogo.'}</span>
    </div>
    <h4>Save do cartucho</h4>
    <p class="note">${cur ? `${fmtSize(cur.size || cur.data.length)}, gravado em ${fmtDate(cur.at)}${cur.source && cur.source !== 'jogo' ? ` (${esc(cur.source)})` : ''}.` : sv ? 'Save guardado (formato antigo).' : 'Ainda não há save deste jogo neste aparelho.'}</p>
    <div class="row">
      ${cur ? '<button class="btn small" type="button" data-save-export>Exportar save</button>' : ''}
      <label class="btn small">Importar save<input type="file" id="detSaveFile" accept=".sav,.srm,.ngf,.bin"></label>
      ${cur ? '<button class="btn small danger" type="button" data-save-del>Apagar save</button>' : ''}
    </div>
    ${hist.length ? `<details class="det-hist"><summary>Versões anteriores do save (${hist.length})</summary>
      <ul class="dlg-list">${hist.map(v => `<li><span>${fmtDate(v.at)} · ${fmtSize(v.size || v.data.length)} · ${esc(v.reason || '')}</span>
        <span><button class="linkbtn" type="button" data-hist-restore="${esc(v.hid)}">Restaurar</button> <button class="linkbtn" type="button" data-hist-dl="${esc(v.hid)}">Baixar</button></span></li>`).join('')}</ul>
      <p class="note">O Índigo guarda até 12 versões: uma a cada 10 minutos de jogo e sempre antes de importar, restaurar ou apagar.</p></details>` : ''}
    <div id="detCloud"></div>
    <h4>Estados salvos</h4>
    <div class="det-states">${SLOTS.map(n => { const m = states.find(x => x.slot === n); return `<div class="det-state">
      ${m && m.thumb ? `<img src="${m.thumb}" alt="">` : '<span class="nothumb">vazio</span>'}
      <span class="note">${m && m.name ? `<b>${esc(m.name)}</b> · ` : ''}Espaço ${n}${m ? ' · ' + fmtDate(m.at) : ''}</span>
      ${m ? `<span><button class="linkbtn" type="button" data-state-load="${n}">Jogar daqui</button> <button class="linkbtn" type="button" data-state-name="${n}">Nomear</button> <button class="linkbtn" type="button" data-state-del="${n}">Apagar</button></span>` : ''}
    </div>`; }).join('')}</div>
    <div class="row det-actions">
      ${!isCur && canResume(Object.assign({}, rec, { saveKey: key })) ? `<button class="btn primary" type="button" data-resume="${esc(id)}">Continuar de onde parou</button>` : ''}
      <button class="btn ${!isCur && canResume(Object.assign({}, rec, { saveKey: key })) ? '' : 'primary'}" type="button" data-xplay="${esc(id)}">${isCur ? 'Voltar ao jogo' : 'Jogar do início'}</button>
      <button class="btn xfer-only" type="button" data-detsend>Enviar para outro aparelho</button>
      <button class="btn" type="button" data-xclose>Fechar</button>
    </div>
    <div class="det-remove">
      <label class="opt-inline"><input type="checkbox" id="detRemoveAll"> apagar também o save e os estados</label>
      <button class="btn small danger" type="button" data-remove>Remover da estante</button>
    </div>
  </div>`);
  const ctx = { rec, key, hist, isCur, h };
  $('xferBody').onclick = (e) => detailAction(e, ctx);
  if (cloud.usable) cloud.versions(key).then(vs => {
    const box = $('detCloud');
    if (!box || detailId !== id) return;
    box.innerHTML = vs.length ? `<details class="det-hist"><summary>Na nuvem (${vs.length} ${vs.length > 1 ? 'versões' : 'versão'}; a mais nova de ${fmtDate(vs[0].at)}, ${esc(vs[0].from || '?')})</summary>
      <ul class="dlg-list">${vs.map(v => `<li><span>${fmtDate(v.at)} · ${fmtSize(v.size)} · ${esc(v.from || '')}${cur && cur.at === v.at ? ' · igual ao deste aparelho' : ''}</span>
      <span>${cur && cur.at === v.at ? '' : `<button class="linkbtn" type="button" data-cloud-use="${v.at}" data-cloud-from="${esc(v.from || '')}">Usar esta</button>`}</span></li>`).join('')}</ul></details>`
      : '<p class="note">Este save ainda não está na nuvem.</p>';
  });
  const cf = $('detCover');
  if (cf) cf.addEventListener('change', async () => {
    const file = cf.files[0]; if (!file) return;
    try { await setCover(id, await imageToCover(file), 'imagem'); toast('Capa trocada.'); openDetails(id); }
    catch (err) { toast(err.message); }
  });
  const f = $('detSaveFile');
  if (f) f.addEventListener('change', async () => {
    const file = f.files[0]; if (!file) return;
    const buf = new Uint8Array(await file.arrayBuffer());
    if (!buf.length || buf.length > 0x100000) { toast('Esse arquivo não parece um save.'); return; }
    await library.snapshotSave(key, 'antes de importar').catch(() => {});
    await library.putSave(key, buf, { source: 'importado', reason: 'importado de arquivo', force: true });
    savedKeys.add(key);
    logEvent(`Save importado (${file.name}) para ${rec.name}`);
    if (isCur) await boot();
    toast(isCur ? 'Save importado e jogo reiniciado.' : 'Save importado.');
    openDetails(id);
  });
}

function download(bytes, name) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(bytes instanceof Blob ? bytes : new Blob([bytes], { type: 'application/octet-stream' }));
  a.download = name;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 8000);
}
const saveExt = (c) => c === 'ngp' || c === 'ngpc' ? '.ngf' : '.sav';
const stripRtc = (c, d) => c === 'gba' && d.length % 1024 === 16 ? d.slice(0, d.length - 16) : d;

async function detailAction(e, { rec, key, hist, isCur }) {
  const t = e.target;
  const id = rec.id;
  if (t.closest('[data-rename]')) {
    const name = ($('detName').value || '').trim();
    if (!name) { toast('Dê um nome ao cartucho.'); return; }
    await library.update(id, { name });
    if (id === currentId) { currentName = name; $('romName').textContent = name; }
    toast('Nome alterado.'); renderShelf(); return;
  }
  if (t.closest('[data-det-fav]')) { await toggleFav(id); openDetails(id); return; }
  const tg = t.closest('[data-tag]');
  if (tg) {
    const cur = rec.tags || [], name = tg.dataset.tag;
    await setTags(id, cur.includes(name) ? cur.filter(x => x !== name) : [...cur, name]);
    openDetails(id); return;
  }
  if (t.closest('[data-tag-add]')) {
    const name = ($('detNewTag').value || '').trim();
    if (!name) { toast('Digite o nome da coleção.'); return; }
    await setTags(id, [...(rec.tags || []), name]);
    logEvent(`${rec.name} entrou na coleção "${name}"`);
    openDetails(id); return;
  }
  if (t.closest('[data-cover-screen]')) { if (lastFb) { await setCover(id, frameUrl(), 'tela'); toast('Capa trocada pela tela atual.'); } openDetails(id); return; }
  if (t.closest('[data-cover-del]')) { await setCover(id, null, null); openDetails(id); return; }
  const sn = t.closest('[data-state-name]');
  if (sn) {
    const n = Number(sn.dataset.stateName);
    const name = window.prompt(`Nome do espaço ${n} (ex.: "antes do chefe"). Deixe vazio para tirar o nome.`, '');
    if (name === null) return;
    await library.renameState(id, n, name.trim().slice(0, 40));
    if (id === currentId) refreshStates();
    openDetails(id); return;
  }
  if (t.closest('[data-save-export]')) {
    const cur = await library.getSave(key); if (cur) download(stripRtc(rec.console, cur.data), rec.name + saveExt(rec.console));
    return;
  }
  if (t.closest('[data-save-del]')) {
    if (t.dataset.confirm !== '1') { t.dataset.confirm = '1'; t.textContent = 'Confirmar: apagar save'; return; }
    await library.deleteSave(key); savedKeys.delete(key);
    logEvent(`Save apagado de ${rec.name} (cópia guardada nas versões anteriores)`);
    if (isCur) await boot();
    toast('Save apagado. Uma cópia ficou nas versões anteriores.'); openDetails(id); renderShelf(); return;
  }
  const hr = t.closest('[data-hist-restore]');
  if (hr) {
    const v = hist.find(x => x.hid === hr.dataset.histRestore); if (!v) return;
    if (isCur) await persistSave();
    await library.snapshotSave(key, 'antes de restaurar versão').catch(() => {});
    await library.putSave(key, v.data, { source: 'restaurado', reason: 'versão restaurada', force: true });
    savedKeys.add(key);
    logEvent(`Save de ${rec.name} restaurado para a versão de ${fmtDate(v.at)}`);
    if (isCur) await boot();
    toast(`Save de ${fmtDate(v.at)} restaurado${isCur ? ' e jogo reiniciado' : ''}.`); openDetails(id); return;
  }
  const cu = t.closest('[data-cloud-use]');
  if (cu) {
    if (isCur) await persistSave();
    try {
      await cloud.download(key, Number(cu.dataset.cloudUse), cu.dataset.cloudFrom);
      savedKeys.add(key);
      logEvent(`Save de ${rec.name} trocado pela versão da nuvem de ${fmtDate(Number(cu.dataset.cloudUse))}`);
      if (isCur) await boot();
      toast('Versão da nuvem aplicada. A anterior ficou nas versões deste aparelho.');
    } catch (err) { toast('Não foi possível baixar da nuvem: ' + (err.message || 'erro')); }
    openDetails(id); return;
  }
  const hd = t.closest('[data-hist-dl]');
  if (hd) { const v = hist.find(x => x.hid === hd.dataset.histDl); if (v) download(stripRtc(rec.console, v.data), `${rec.name} (${fmtDate(v.at).replace(/[/:]/g, '-')})${saveExt(rec.console)}`); return; }
  const sl = t.closest('[data-state-load]');
  if (sl) {
    const n = Number(sl.dataset.stateLoad);
    dialogClose();
    if (!(id === currentId && running)) await playFromShelf(id);
    stateSlot = n; refreshStates(); await loadStateNow(); return;
  }
  const sd = t.closest('[data-state-del]');
  if (sd) {
    if (sd.dataset.confirm !== '1') { sd.dataset.confirm = '1'; sd.textContent = 'Confirmar'; return; }
    await library.deleteState(id, Number(sd.dataset.stateDel));
    if (id === currentId) refreshStates();
    openDetails(id); return;
  }
  if (t.closest('[data-detsend]')) { sendFromShelf(id); return; }
  if (t.closest('[data-remove]')) {
    const all = $('detRemoveAll').checked;
    if (t.dataset.confirm !== '1') { t.dataset.confirm = '1'; t.textContent = all ? 'Confirmar: remover tudo' : 'Confirmar remoção'; return; }
    if (id === currentId) await persistSave();
    if (all) await library.removeAll(id, key); else await library.remove(id);
    if (all) savedKeys.delete(key);
    if (id === currentId) currentId = null;
    logEvent(`Cartucho removido: ${rec.name}${all ? ' (com save e estados)' : ' (save mantido)'}`);
    dialogClose();
    toast(all ? 'Cartucho, save e estados removidos. Uma cópia do save ficou no histórico.' : 'Cartucho removido. O save continua guardado e volta se você abrir a ROM de novo.');
    renderShelf();
  }
}

// ---------------------------------------------------------------- backup da estante
function openBackup() {
  dialogOpen(`<h3>Backup da estante</h3>
    <p class="note">Guarde tudo num arquivo só e restaure neste ou em outro aparelho. Restaurar nunca apaga nada: junta com o que já existe e, quando os dois lados têm save, fica o mais novo (o outro vai para as versões anteriores).</p>
    <label class="opt-inline"><input type="checkbox" id="bkRoms" checked> incluir as ROMs (arquivo maior)</label>
    <div class="row"><button class="btn primary" type="button" data-bk-make>Baixar backup</button>
      <label class="btn">Restaurar backup<input type="file" id="bkFile" accept=".indigo,application/octet-stream"></label></div>
    <p class="note">${Number(load('indigo-last-backup')) ? 'Último backup: ' + fmtDate(Number(load('indigo-last-backup'))) : 'Nenhum backup feito neste aparelho ainda.'}</p>
    ${cloud.state !== 'unavailable' ? `<h4>Saves na nuvem</h4>
    <p class="note">Cada save gravado sobe sozinho para a nuvem deste site (só os saves, nunca as ROMs) e os saves mais novos feitos em outros aparelhos descem para cá. Ficam as 6 versões mais recentes de cada jogo. ${esc(cloud.statusText())}</p>
    <label class="opt-inline"><input type="checkbox" id="cloudOn" ${cloud.enabled ? 'checked' : ''}> guardar saves na nuvem automaticamente</label>
    <div class="row"><button class="btn" type="button" data-cloud-sync ${cloud.enabled ? '' : 'disabled'}>Sincronizar agora</button></div>` : ''}
    <div class="row"><button class="btn" type="button" data-xclose>Fechar</button></div>`);
  const co = $('cloudOn');
  if (co) co.addEventListener('change', () => { cloud.enabled = co.checked; logEvent(`Saves na nuvem ${co.checked ? 'ligados' : 'desligados'}`); openBackup(); });
  $('xferBody').onclick = async (e) => {
    if (e.target.closest('[data-cloud-sync]')) {
      await persistSave(); await cloud.flush();
      const r = await cloud.sync('manual', true);
      toast(r ? `Nuvem sincronizada: ${r.up} enviado(s), ${r.down} recebido(s).` : 'Não foi possível sincronizar: ' + cloud.lastError);
      if (r && r.down && running) toast('Saves recebidos valem quando você abrir cada jogo.');
      openBackup(); return;
    }
    if (!e.target.closest('[data-bk-make]')) return;
    downloadBackup($('bkRoms').checked);
  };
  $('bkFile').addEventListener('change', (e) => { const f = e.target.files[0]; e.target.value = ''; if (f) doRestore(f); });
}

async function downloadBackup(roms) {
    try {
      await persistSave(); await flushPlayTime();
      dialogProgress('Preparando backup', 0, 'lendo a estante');
      const { blob, counts } = await makeBackup({ roms }, (f) => dialogProgress('Preparando backup', f, 'lendo a estante'));
      const d = new Date(), pad = (n) => String(n).padStart(2, '0');
      download(blob, `indigo-backup-${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}${roms ? '' : '-saves'}.indigo`);
      store('indigo-last-backup', String(Date.now()));
      const msg = `${counts.roms} cartucho(s)${roms ? '' : ' (só os dados, sem as ROMs)'}, ${counts.saves} save(s), ${counts.states} estado(s), ${fmtSize(blob.size)}`;
      logEvent('Backup baixado: ' + msg);
      dialogOpen(`<h3>Backup pronto</h3><p class="note">${esc(msg)}. Guarde o arquivo <b>.indigo</b> num lugar seguro (pendrive, nuvem). Para restaurar, use “Backup” → “Restaurar backup”.</p>
        <div class="row"><button class="btn primary" type="button" data-xclose>Fechar</button></div>`);
      updateStorageNote();
    } catch (err) { dialogOpen(`<h3>Não deu para fazer o backup</h3><p class="note">${esc(err.message || 'erro')}</p><div class="row"><button class="btn" type="button" data-xclose>Fechar</button></div>`); }
}

async function doRestore(file) {
  try {
    if (!(await isBackup(file))) { toast('Esse arquivo não é um backup do Índigo (.indigo).'); return; }
    await persistSave();
    dialogProgress('Restaurando backup', 0, 'lendo o arquivo');
    const r = await restoreBackup(file, (f) => dialogProgress('Restaurando backup', f, 'gravando na estante'));
    savedKeys = new Set((await library.listSaves()).map(x => x.key));
    const parts = [`${r.romsAdded} cartucho(s) novo(s)`, `${r.romsKept} já estava(m) na estante`,
      `${r.savesNew + r.savesNewer} save(s) restaurado(s)`, `${r.savesKeptLocal} save(s) mantido(s) porque o deste aparelho era mais novo`, `${r.states} estado(s)`];
    if (r.romsNoData) parts.push(`${r.romsNoData} cartucho(s) sem ROM no backup (abra a ROM para jogar; o save já está aqui)`);
    logEvent('Backup restaurado: ' + parts.join(', '));
    renderShelf();
    if (running && romBytes) await boot();
    dialogOpen(`<h3>Backup restaurado</h3><ul class="dlg-list">${parts.map(p => `<li>${esc(p)}</li>`).join('')}</ul>
      <div class="row"><button class="btn primary" type="button" data-xclose>Fechar</button></div>`);
  } catch (err) {
    dialogOpen(`<h3>Não deu para restaurar</h3><p class="note">${esc(err.message || 'erro')}</p><div class="row"><button class="btn" type="button" data-xclose>Fechar</button></div>`);
  }
}

// ---------------------------------------------------------------- enviar para outro aparelho
transferStatus().then(st => { document.body.dataset.xfer = st; });

function xferOpen(html) { $('xferBody').onclick = null; $('xferBody').classList.remove('wide'); $('xferBody').innerHTML = html; if (html.includes('dlg-wide')) $('xferBody').classList.add('wide'); $('xfer').hidden = false; const f = $('xferBody').querySelector('button, input:not([type=file])'); if (f) f.focus({ preventScroll: true }); }
function xferClose() { $('xfer').hidden = true; $('xferBody').innerHTML = ''; $('xferBody').onclick = null; }
const dialogOpen = (html) => xferOpen(html), dialogClose = () => { if (!$('xfer').hidden) xferClose(); };
const dialogProgress = (t, f, d) => xferProgress(t, f, d);
function xferProgress(title, frac, detail) {
  const pct = Math.round(frac * 100);
  xferOpen(`<h3>${esc(title)}</h3><div class="bar"><span style="width:${pct}%"></span></div><p class="note">${pct}% · ${esc(detail || '')}</p>`);
}
$('xfer').addEventListener('click', (e) => {
  if (e.target === $('xfer') || e.target.closest('[data-xclose]')) xferClose();
  const c = e.target.closest('[data-xcancel]');
  if (c) { cancelTransfer(c.dataset.xcancel); xferClose(); toast('Envio cancelado e apagado do servidor.'); }
  if (e.target.closest('[data-shelfgo]')) { runSendShelf(); return; }
  const pl = e.target.closest('[data-xplay]');
  if (pl) { xferClose(); if (pl.dataset.xplay === currentId && running) { paused = false; $('pause').textContent = 'Pausar'; $('touch').scrollIntoView({ behavior: 'smooth', block: 'start' }); } else playFromShelf(pl.dataset.xplay); }
});
window.addEventListener('keydown', (e) => { if (e.code === 'Escape' && !$('xfer').hidden) xferClose(); });

async function sendById(id, onProgress) {
  if (id === currentId) persistSave();
  const rec = await library.get(id);
  if (!rec) throw new Error('Esse cartucho não está mais na estante.');
  const sv = rec.saveKey ? await library.getSave(rec.saveKey).catch(() => null) : null;
  const save = sv ? toB64(sv.data) : (rec.saveKey ? load(rec.saveKey) : null);
  const saveAt = sv ? sv.at : (rec.saveKey ? Number(load(rec.saveKey + ':at') || 0) : 0);
  const code = await sendCartridge({
    bytes: new Uint8Array(rec.data), name: rec.name, fileName: rec.fileName, title: rec.title, gameCode: rec.gameCode, save, saveAt,
  }, onProgress);
  return { code, rec, save };
}

async function sendFromShelf(id) {
  try {
    xferProgress('Enviando', 0, 'preparando');
    const { code, rec, save } = await sendById(id, (f) => xferProgress('Enviando', f, f < 1 ? 'subindo o cartucho' : 'pronto'));
    xferOpen(`<h3>Pronto para receber</h3>
      <p class="note">No outro aparelho, abra o Índigo, toque em <b>Receber por código</b> e digite:</p>
      <div class="code" aria-label="Código">${code.slice(0, 3)}&nbsp;${code.slice(3)}</div>
      <p class="note">${esc(rec.name)}${save ? ' + save' : ''} · vale por 1 hora e é apagado assim que o outro aparelho receber.</p>
      <div class="row"><button class="btn" type="button" data-xcancel="${code}">Cancelar envio</button><button class="btn primary" type="button" data-xclose>Fechar</button></div>`);
  } catch (err) {
    xferOpen(`<h3>Não deu para enviar</h3><p class="note">${esc(err.message || 'Erro desconhecido.')}</p><div class="row"><button class="btn" type="button" data-xclose>Fechar</button></div>`);
  }
}

// ---- estante inteira por um código só (o mesmo arquivo do Backup, levado pelo servidor de transferências)
const fmtMB = (n) => n < 1048576 ? Math.max(1, Math.round(n / 1024)) + ' KB' : (n / 1048576).toFixed(n < 104857600 ? 1 : 0).replace('.', ',') + ' MB';
async function sendWholeShelf(onProgress) {
  await persistSave();
  const { blob, counts } = await makeBackup({ roms: true }, (f) => onProgress(f * 0.12, 'juntando os jogos'));
  if (!counts.roms) throw new Error('A estante deste aparelho está vazia.');
  const code = await sendShelf(blob, counts, (f) => onProgress(0.12 + f * 0.88, 'subindo a estante'));
  logEvent(`Estante inteira enviada por código (${counts.roms} cartucho(s), ${counts.saves} save(s), ${counts.states} estado(s), ${fmtMB(blob.size)})`);
  return { code, counts, size: blob.size };
}
async function openSendShelf() {
  const metas = await library.list();
  if (!metas.length) { toast('A estante deste aparelho está vazia.'); return; }
  const total = metas.reduce((n, m) => n + (m.size || 0), 0);
  xferOpen(`<h3>Enviar estante inteira</h3>
    <p class="note">${metas.length} jogo(s), uns ${esc(fmtMB(total))}, com os saves e os estados salvos. Gera um código de 6 números para digitar no outro aparelho em <b>Receber por código</b>.</p>
    <p class="note">O outro aparelho junta com o que já tem: nada é apagado e, quando os dois têm save do mesmo jogo, fica o mais novo. No servidor, a cópia é apagada quando o outro aparelho recebe, ou em 1 hora.</p>
    <div class="row"><button class="btn" type="button" data-xclose>Cancelar</button><button class="btn primary" type="button" data-shelfgo>Enviar</button></div>`);
}
async function runSendShelf() {
  try {
    xferProgress('Enviando a estante', 0, 'preparando');
    const r = await sendWholeShelf((f, what) => xferProgress('Enviando a estante', f, what));
    xferOpen(`<h3>Pronto para receber</h3>
      <p class="note">No outro aparelho, abra o Índigo, toque em <b>Receber por código</b> e digite:</p>
      <div class="code" aria-label="Código">${r.code.slice(0, 3)}&nbsp;${r.code.slice(3)}</div>
      <p class="note">Estante inteira: ${r.counts.roms} jogo(s), ${r.counts.saves} save(s), ${r.counts.states} estado(s) · ${esc(fmtMB(r.size))} · vale por 1 hora e é apagada do servidor assim que o outro aparelho receber.</p>
      <div class="row"><button class="btn" type="button" data-xcancel="${r.code}">Cancelar envio</button><button class="btn primary" type="button" data-xclose>Fechar</button></div>`);
  } catch (err) {
    xferOpen(`<h3>Não deu para enviar a estante</h3><p class="note">${esc(err.message || 'Erro desconhecido.')}</p><div class="row"><button class="btn" type="button" data-xclose>Fechar</button></div>`);
  }
}
async function receiveShelf(code, m, blob, onProgress) {
  await persistSave();
  const r = await restoreBackup(blob, (f) => onProgress && onProgress(1, f));
  cancelTransfer(code); // recebido: apaga do servidor
  savedKeys = new Set((await library.listSaves()).map(x => x.key));
  library.persist();
  renderShelf();
  if (running && romBytes) await boot();
  const parts = [`${r.romsAdded} jogo(s) novo(s)`];
  if (r.romsKept) parts.push(`${r.romsKept} já estava(m) aqui`);
  parts.push(`${r.savesNew + r.savesNewer} save(s) atualizado(s)`);
  if (r.savesKeptLocal) parts.push(`${r.savesKeptLocal} save(s) daqui mantido(s) por serem mais novos`);
  if (r.states) parts.push(`${r.states} estado(s)`);
  logEvent(`Estante inteira recebida${m.from ? ' do ' + m.from : ''}: ` + parts.join(', '));
  return { id: null, name: 'A estante', note: `Veio${m.from ? ' do ' + m.from : ''}: ${parts.join(', ')}.` };
}

async function receiveByCode(code, onProgress) {
  const got = await receiveCartridge(code, onProgress);
  if (got.manifest.kind === 'shelf') return receiveShelf(code, got.manifest, got.blob, null);
  const { manifest: m, bytes } = got;
  const h = romHeader(bytes, m.fileName);
  if (!h.sys || h.sys === 'nds') throw new Error('Esse cartucho não é de um console suportado.');
  const id = romId(bytes, h.gameCode, h.title);
  const sk = romSaveKey(bytes, h);
  await library.add({ id, name: m.name, fileName: m.fileName, title: h.title, gameCode: h.gameCode, size: bytes.length, saveKey: sk, console: h.sys }, bytes);
  library.persist();
  let saveMsg = '';
  if (m.save) {
    const local = await readSave(sk, [saveKeyFor(h)]);
    const localRec = await library.getSave(sk).catch(() => null);
    const localAt = localRec ? localRec.at : 0;
    if (!local || (m.saveAt || 0) >= localAt) {
      if (local) await library.snapshotSave(sk, 'antes de receber de outro aparelho').catch(() => {});
      await library.putSave(sk, fromB64(m.save), { at: m.saveAt || Date.now(), source: 'recebido', reason: 'recebido de outro aparelho', force: true });
      savedKeys.add(sk);
      saveMsg = 'O save veio junto.';
      if (sk === saveKey && running) boot();
    } else saveMsg = 'O save deste aparelho é mais recente e foi mantido.';
  }
  renderShelf();
  return { id, name: m.name, note: `Guardado na estante deste aparelho${m.from ? ', vindo do ' + m.from : ''}. ${saveMsg}`.trim() };
}

function openReceive() {
  xferOpen(`<h3>Receber por código</h3>
    <p class="note">Digite o código de 6 dígitos que apareceu no outro aparelho (um cartucho ou a estante inteira).</p>
    <form id="recvForm" class="recv">
      <input id="recvCode" inputmode="numeric" autocomplete="off" maxlength="7" placeholder="000 000" aria-label="Código de 6 dígitos">
      <button class="btn primary" type="submit">Receber</button>
    </form>
    <div class="row"><button class="btn" type="button" data-xclose>Cancelar</button></div>`);
  $('recvForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const code = ($('recvCode').value || '').replace(/\D/g, '');
    if (code.length !== 6) { toast('O código tem 6 números.'); return; }
    try {
      xferProgress('Recebendo', 0, 'buscando');
      const r = await receiveByCode(code, (f) => xferProgress('Recebendo', f, f < 1 ? 'baixando' : 'guardando na estante'));
      xferOpen(`<h3>${esc(r.name)} chegou</h3>
        <p class="note">${esc(r.note)}</p>
        <div class="row">${r.id ? `<button class="btn primary" type="button" data-xplay="${esc(r.id)}">Jogar agora</button>` : ''}<button class="btn${r.id ? '' : ' primary'}" type="button" data-xclose>Fechar</button></div>`);
    } catch (err) {
      xferOpen(`<h3>Não deu para receber</h3><p class="note">${esc(err.message || 'Erro desconhecido.')}</p><div class="row"><button class="btn" type="button" data-xclose>Fechar</button></div>`);
    }
  });
}
$('receiveBtn').addEventListener('click', openReceive);
$('sendShelfBtn').addEventListener('click', openSendShelf);
migrateLegacySaves().then(() => { renderShelf(); cloud.init(); });
renderShelf();

// ---------------------------------------------------------------- menu do console (controle)
// Edge do Xbox (versão antiga): entrega o controle à página em vez de mover o cursor
try { if ('gamepadInputEmulation' in navigator) navigator.gamepadInputEmulation = 'gamepad'; } catch { /* sem suporte */ }
const isXbox = /Xbox/i.test(navigator.userAgent);
if (isXbox) document.body.dataset.xbox = '1';

const cmenu = new ConsoleMenu($('cmenu'), {
  isImmersive: () => document.body.dataset.imm === '1' || document.body.dataset.fs === '1',
  exitImmersive: () => { document.body.dataset.immersive = '0'; exitPlayFs(); },
  isTouchPlay: () => touchPlay(),
  isPaused: () => paused,
  setPaused: (v) => { if (running) { paused = v; $('pause').textContent = paused ? 'Continuar' : 'Pausar'; } },
  isRunning: () => running,
  currentName: () => currentName,
  canTransfer: () => document.body.dataset.xfer === 'ready',
  isTv: () => tvOpen,
  toggleTv: () => { tvOpen ? closeTv() : openTv(); },
  filterName: () => FILTERS[video.filter],
  shellName: () => SHELLS[shellTheme],
  cycleShell: (d) => { const o = Object.keys(SHELLS); applyShell(o[(o.indexOf(shellTheme) + d + o.length) % o.length]); },
  cycleFilter: (d) => {
    const order = Object.keys(FILTERS);
    video.filter = order[(order.indexOf(video.filter) + d + order.length) % order.length];
    applyVideo(); if (!running) present(img32);
  },
  reset: () => { if (romBytes) { persistSave(); boot(); } },
  stateSlot: () => stateSlot,
  stateInfo: () => stateInfoText(),
  cycleSlot: (d) => { stateSlot = ((stateSlot - 1 + d + 9) % 9) + 1; refreshStates(); },
  slotList: () => SLOTS.map(n => { const m = stateMeta[n]; return { slot: n, used: !!m, thumb: m ? m.thumb || null : null, hint: m ? `${m.name ? m.name + ' · ' : ''}${fmtDate(m.at)}` : 'vazio' }; }),
  saveSlot: (n) => { stateSlot = n; refreshStates(); return saveStateNow(); },
  loadSlotHere: (n) => { stateSlot = n; refreshStates(); return loadStateNow(n); },
  saveState: () => saveStateNow(),
  loadState: () => loadStateNow(),
  isGameBoy: () => sys === 'gb' || sys === 'gbc',
  paletteName: () => GB_PALETTES[gbPalette].label,
  cyclePalette: (d) => { const o = Object.keys(GB_PALETTES); setPalette(o[(o.indexOf(gbPalette) + d + o.length) % o.length]); },
  listShelf: () => shelfForMenu(),
  shelfView: () => ({ groups: filterOptions().map(o => ({ label: o.label, on: shelfView.sys === o.v })), sortLabel: SORT_NAME[shelfView.sort] || '' }),
  cycleShelfSys: (d) => {
    const opts = filterOptions().map(o => o.v);
    shelfView.sys = opts[(opts.indexOf(shelfView.sys) + d + opts.length) % opts.length]; saveView(); renderShelf();
  },
  toggleFav: (id) => toggleFav(id),
  coverFromScreen: (id) => { if (id === currentId && running && lastFb) { setCover(id, frameUrl(), 'tela'); toast('Capa trocada pela tela atual.'); } },
  cycleShelfSort: () => { const o = Object.keys(SORT_NAME); shelfView.sort = o[(o.indexOf(shelfView.sort) + 1) % o.length]; saveView(); renderShelf(); },
  cartInfo: (id) => cartInfoForMenu(id),
  resumeTarget: () => { if (running) return null; const r = shelfCache.filter(canResume).sort((a, b) => b.autoAt - a.autoAt)[0]; return r ? { id: r.id, name: r.name } : null; },
  resume: (id) => { startAudio(); resumeGame(id); },
  loadSlot: async (id, n) => { if (!(id === currentId && running)) await playFromShelf(id); if (currentId === id) { stateSlot = n; refreshStates(); loadStateNow(n); } },
  restoreVersion: (id, hid) => restoreVersionById(id, hid),
  remove: (id, all) => removeCart(id, all),
  cloudAvailable: () => cloud.state !== 'unavailable',
  cloudEnabled: () => cloud.enabled,
  cloudStatus: () => cloud.statusText(),
  cloudToggle: () => { cloud.enabled = !cloud.enabled; logEvent(`Saves na nuvem ${cloud.enabled ? 'ligados' : 'desligados'}`); },
  cloudSync: async () => { await persistSave(); await cloud.flush(); return cloud.sync('manual', true); },
  backup: (roms) => downloadBackup(roms),
  play: (id) => playFromShelf(id),
  send: async (id, onProgress) => (await sendById(id, onProgress)).code,
  sendShelf: async (onProgress) => sendWholeShelf((f) => onProgress(f)),
  receive: (code, onProgress) => receiveByCode(code, onProgress),
  mount: (el) => { const host = tvOpen ? $('tv') : document.body; if (el.parentNode !== host) host.appendChild(el); },
});
$('menuBtn').addEventListener('click', () => cmenu.show());
let menuPrev = new Set();
function menuFrame() {
  const p = pad.lastPressed;
  const fresh = (i) => p.has(i) && !menuPrev.has(i);
  // View + Menu juntos abrem/fecham o menu
  if (p.has(8) && p.has(9) && (fresh(8) || fresh(9))) { cmenu.toggle(); cmenu.prevPressed = new Set(p); }
  // sem jogo rodando, qualquer A ou Menu no controle abre o menu
  else if (!cmenu.open && !running && (fresh(0) || fresh(9))) { cmenu.show(); cmenu.prevPressed = new Set(p); }
  else if (cmenu.open) cmenu.input(p, pad.axes || [0, 0]);
  menuPrev = new Set(p);
  return cmenu.open;
}

function saveLabel() { return info.saveLabel || '—'; }

// carrega a ROM atual no núcleo (com o save guardado neste navegador)
let bootSeq = 0;
async function boot() {
  await persistSave();
  const seq = ++bootSeq;
  running = false; lastFb = null; inFlight = 0; lastSave = null; saveDirty = false;
  audio.clear();
  const h = romHeader(romBytes, '', romKind);
  saveKey = romSaveKey(romBytes, h);
  if (cloud.usable) { $('saveState').textContent = 'Conferindo o save na nuvem…'; await cloud.pullBeforeBoot(saveKey); }
  const prev = await readSave(saveKey, [saveKeyFor(h)]);
  if (seq !== bootSeq) return;
  let res;
  try {
    const rom = romBytes.slice();
    res = await client.request({ t: 'load', kind: romKind, rom: rom.buffer, save: prev ? prev.slice() : null, bios: biosBytes ? biosBytes.slice() : null,
      rate: audio.sampleRate, palette: GB_PALETTES[gbPalette] }, [rom.buffer]);
  } catch (e) { toast('Não foi possível iniciar o jogo: ' + (e.message || 'erro')); return; }
  if (seq !== bootSeq) return;
  lastFb = null; lastSave = null; saveDirty = false; inFlight = 0; coverTry = 0;
  sys = res.sys;
  info = { title: res.title, gameCode: res.gameCode, saveLabel: res.saveLabel, hasSave: res.hasSave, frame: 0 };
  document.body.dataset.sys = sys;
  $('bezelTag').textContent = { gba: '32-BIT ARM7', gb: 'DOT MATRIX', gbc: 'COLOR', gg: '8-BIT COLOR', sms: '8-BIT Z80', ngp: '16-BIT TLCS', ngpc: '16-BIT COLOR' }[sys];
  FRAME_MS = 1000 / (FPS[sys] || 59.7275);
  $('resTag').textContent = SCREEN[sys].join('×');
  $('startLabel').textContent = sys === 'ngp' || sys === 'ngpc' ? 'OPTION' : 'START';
  for (const r of [mainR, tvR]) if (r) r.profile = sys === 'gbc' || sys === 'gg' || sys === 'ngpc' ? 'gbc' : 'gba';
  applyVideo();
  $('saveState').textContent = prev ? 'Save anterior restaurado' : (!info.hasSave ? 'Este cartucho não tem save (use os estados)' : 'Sem save ainda');
  running = true; renderSound();
  paused = false;
  $('pause').textContent = 'Pausar';
  frameDebt = 0; lastTime = performance.now();
}

// mensagens do núcleo: quadro novo (com som e, se mudou, o save)
function onCoreMessage(m) {
  if (m.t === 'frame') {
    inFlight = 0;
    if (!running) return; // quadro de um jogo que já foi trocado
    lastFb = m.fb; lastW = m.w; lastH = m.h;
    info.frame = m.frame;
    if (m.L && !paused) audio.push(m.L, m.R);
    if (m.save) { lastSave = m.save; saveDirty = true; }
    if (info.frame >= 240 && info.frame - coverTry >= 60) { coverTry = info.frame; autoCover(); }
    presentGame();
  } else if (m.t === 'error') {
    console.error('núcleo:', m.error);
    toast('O emulador encontrou um erro: ' + m.error);
    running = false;
  }
}

async function importSave(file) {
  if (!romBytes || !saveKey) { toast('Abra a ROM antes de importar o save.'); return; }
  const buf = new Uint8Array(await file.arrayBuffer());
  if (!buf.length || buf.length > 0x100000) { toast('Esse arquivo não parece um save.'); return; }
  await persistSave();
  try {
    await library.snapshotSave(saveKey, 'antes de importar');
    await library.putSave(saveKey, buf, { source: 'importado', reason: 'importado de arquivo', force: true });
  } catch { store(saveKey, toB64(buf)); }
  await boot();
  logEvent(`Save importado (${file.name}) para ${currentName || 'o jogo atual'}`);
  toast('Save importado e jogo reiniciado. O save anterior ficou no histórico.');
}

async function exportSave() {
  if (!running) { toast('Abra um jogo primeiro.'); return; }
  let data = await client.request({ t: 'saveGet' }).catch(() => null);
  if (!data) { toast('Este jogo não tem save de cartucho. Use “Salvar estado”.'); return; }
  // o ajuste do relógio do GBA fica guardado depois do save; o .sav exportado sai no tamanho padrão
  if (sys === 'gba' && data.length % 1024 === 16) data = data.slice(0, data.length - 16);
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([data], { type: 'application/octet-stream' }));
  a.download = (currentName || info.title || 'jogo') + (sys === 'ngp' || sys === 'ngpc' ? '.ngf' : '.sav');
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 4000);
}

// ---------------------------------------------------------------- save states (3 espaços por jogo)
const SLOTS = [1, 2, 3, 4, 5, 6, 7, 8, 9];
let stateSlot = 1;
let stateMeta = {};
async function refreshStates() {
  stateMeta = {};
  if (currentId && library.available()) {
    try { for (const m of await library.listStates(currentId)) stateMeta[m.slot] = m; } catch { /* sem estados */ }
  }
  const sel = $('stateSlot');
  if (sel) {
    if (sel.options.length !== SLOTS.length) sel.innerHTML = SLOTS.map(n => `<option value="${n}">Espaço ${n}</option>`).join('');
    sel.value = String(stateSlot);
    for (const o of sel.options) {
      const m = stateMeta[o.value];
      o.textContent = `Espaço ${o.value}` + (m ? `${m.name ? ' · ' + m.name : ''} · ${new Date(m.at).toLocaleString('pt-BR', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' })}` : ' · vazio');
    }
  }
}
function stateInfoText() {
  const m = stateMeta[stateSlot];
  return m ? `Espaço ${stateSlot}${m.name ? ' · ' + m.name : ''} · ${new Date(m.at).toLocaleString('pt-BR', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' })}` : `Espaço ${stateSlot} · vazio`;
}
function thumbOf() {
  try {
    if (!lastFb) return null;
    const w = lastW, h = lastH;
    const c = document.createElement('canvas'); c.width = w; c.height = h;
    const x = c.getContext('2d'); const img = x.createImageData(w, h);
    new Uint32Array(img.data.buffer).set(lastFb.subarray(0, w * h)); x.putImageData(img, 0, 0);
    return c.toDataURL('image/jpeg', 0.7);
  } catch { return null; }
}
async function saveStateNow() {
  if (!running || !currentId) { toast('Abra um jogo da estante para salvar estados.'); return; }
  try {
    const st = await client.request({ t: 'stateSave' });
    st.rom = currentId;
    await library.putState(currentId, stateSlot, st, thumbOf());
    persistSave();
    toast(`Estado salvo no espaço ${stateSlot}.`);
    refreshStates();
  } catch (e) { toast('Não foi possível salvar o estado: ' + (e.message || 'erro')); }
}
async function loadStateNow(slot = stateSlot) {
  if (!running || !currentId) { toast('Abra um jogo da estante primeiro.'); return false; }
  try {
    const rec = await library.getState(currentId, slot);
    if (!rec) { toast(slot ? `O espaço ${slot} está vazio.` : 'Não há ponto para continuar.'); return false; }
    await client.request({ t: 'stateLoad', state: rec.state });
    audio.clear();
    paused = false; $('pause').textContent = 'Pausar';
    frameDebt = 0; lastTime = performance.now();
    presentGame();
    toast(slot ? `Estado do espaço ${slot} carregado.` : 'Continuando de onde você parou.');
    return true;
  } catch (e) { toast('Não foi possível carregar: ' + (e.message || 'erro')); return false; }
}

// ---------------------------------------------------------------- continuar de onde parou
// Um estado automático (espaço 0) é gravado a cada minuto de jogo, ao trocar de jogo e ao sair da aba.
// Só vale enquanto for mais novo que o save do cartucho que veio de fora (nuvem, importação, restauração):
// continuar de um estado velho traria de volta a memória antiga do cartucho.
let autoBusy = false;
async function autoState() {
  if (!running || !currentId || autoBusy || info.frame < 120) return;
  autoBusy = true;
  const id = currentId;
  try {
    const st = await client.request({ t: 'stateSave' });
    st.rom = id;
    const at = await library.putState(id, 0, st, thumbOf());
    await library.update(id, { autoAt: at });
    const r = shelfCache.find(x => x.id === id); if (r) r.autoAt = at;
  } catch { /* fica o anterior */ }
  finally { autoBusy = false; }
}
setInterval(() => { if (running && !paused && !document.hidden) autoState(); }, 60000);
document.addEventListener('visibilitychange', () => { if (document.hidden) autoState(); });
window.addEventListener('pagehide', () => { autoState(); });

function canResume(r) {
  if (!r || !r.autoAt) return false;
  const sm = saveMeta.get(r.saveKey);
  if (sm && sm.at > r.autoAt && sm.source && sm.source !== 'jogo') return false;
  return true;
}
async function resumeGame(id) {
  if (!(id === currentId && running)) {
    await playFromShelf(id);
    if (currentId !== id) return;
  }
  dialogClose();
  await loadStateNow(0);
}

// ---------------------------------------------------------------- paletas do Game Boy (DMG)
const hex = (c) => (0xFF000000 | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF)) >>> 0; // #RRGGBB → ABGR
const pal4 = (...c) => c.map(hex);
const GB_PALETTES = {
  verde: { label: 'Verde clássico', bg: pal4(0xE0F8D0, 0x88C070, 0x346856, 0x081820) },
  pocket: { label: 'Pocket (cinza esverdeado)', bg: pal4(0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F) },
  cinza: { label: 'Tons de cinza', bg: pal4(0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000) },
  light: { label: 'Light (tela acesa)', bg: pal4(0x7CF0D0, 0x3CBF9C, 0x1A7A66, 0x08332D) },
  indigo: { label: 'Índigo', bg: pal4(0xEEECFF, 0xA9A1EE, 0x5D50B8, 0x19133F) },
  ambar: { label: 'Âmbar', bg: pal4(0xFFE9B8, 0xE6A456, 0x8C4B1E, 0x2B1206) },
  cores: { label: 'Colorida (estilo GBC)', bg: pal4(0xFFFFFF, 0x7BFF31, 0x0063C6, 0x000000), obj0: pal4(0xFFFFFF, 0xFF8484, 0x943939, 0x000000), obj1: pal4(0xFFFFFF, 0x63A5FF, 0x0000FF, 0x000000) },
};
let gbPalette = 'verde';
try { const v = localStorage.getItem('gb-palette'); if (v && GB_PALETTES[v]) gbPalette = v; } catch { /* padrão */ }
function applyPalette() { client.post({ t: 'palette', palette: GB_PALETTES[gbPalette] }); }
function setPalette(v) {
  gbPalette = GB_PALETTES[v] ? v : 'verde';
  try { localStorage.setItem('gb-palette', gbPalette); } catch { /* ignorado */ }
  $('gbPalette').value = gbPalette;
  applyPalette();
  if (running && sys === 'gb') toast('Paleta: ' + GB_PALETTES[gbPalette].label + ' (vale a partir do próximo quadro)');
}

async function loadBiosFile(file) {
  const buf = new Uint8Array(await file.arrayBuffer());
  if (buf.length !== 0x4000) { toast('A BIOS do GBA tem 16 KB exatos.'); return; }
  biosBytes = buf;
  $('biosState').textContent = 'BIOS original carregada';
  if (romBytes) boot();
}

function togglePause() {
  if (!running) return;
  paused = !paused;
  $('pause').textContent = paused ? 'Continuar' : 'Pausar';
}

// ---------------------------------------------------------------- laço principal
let FRAME_MS = 1000 / 59.7275;
let lastTime = performance.now(), frameDebt = 0;
let fpsCount = 0, fpsTime = performance.now();

// acende os botões do aparelho conforme o que está apertado (teclado, controle ou toque)
const keyEls = Array.from(document.querySelectorAll('[data-key]')).map(el => {
  let m = 0;
  for (const k of el.dataset.key.split(' ')) m |= 1 << KEY[k];
  return [el, m];
});
let lastShown = -1;
function showPressed(mask) {
  if (mask === lastShown) return;
  lastShown = mask;
  for (const [el, m] of keyEls) el.classList.toggle('on', (mask & m) === m);
}

function loop(now) {
  requestAnimationFrame(loop);
  pollGamepads();
  if (menuFrame()) { padMask = 0; pad.turbo = false; }
  turbo = (kbTurbo || pad.turbo) ? 4 : 1;
  if (!running) { drawAttract(); lastTime = now; return; }
  if (paused) { lastTime = now; return; }
  frameDebt += Math.min(100, now - lastTime) * turbo;
  lastTime = now;
  // o áudio corrige pequenas derivas de relógio
  if (audio.running && turbo === 1) {
    const buffered = audio.buffered, sr = audio.sampleRate;
    if (buffered < sr * 0.04) frameDebt += 2;
    else if (buffered > sr * 0.14) frameDebt -= 2;
  }
  const keys = kbMask | touchMask | padMask;
  showPressed(keys);
  if (inFlight && now - inFlightAt > 1000) inFlight = 0; // núcleo não respondeu: pede de novo
  if (frameDebt > FRAME_MS * 8 * turbo) frameDebt = FRAME_MS * 8 * turbo;
  // pede ao núcleo os quadros devidos; enquanto ele trabalha, a página segue livre para desenhar
  if (!inFlight && frameDebt >= FRAME_MS) {
    const n = Math.min(Math.floor(frameDebt / FRAME_MS), 4 * turbo);
    frameDebt -= n * FRAME_MS;
    inFlight = n; inFlightAt = now;
    fpsCount += n;
    client.post({ t: 'run', n, keys, audio: turbo === 1 });
  }
  if (now - fpsTime >= 1000) {
    $('fps').textContent = Math.round(fpsCount * 1000 / (now - fpsTime)) + ' qps';
    fpsCount = 0; fpsTime = now;
  }
}

// ---------------------------------------------------------------- UI
$('volume').addEventListener('input', applyVolume);
$('mute').addEventListener('change', applyVolume);
$('romFile').addEventListener('change', (e) => { const f = e.target.files[0]; if (f) { startAudio(); openRom(f); } e.target.value = ''; });
$('saveFile').addEventListener('change', (e) => { const f = e.target.files[0]; if (f) importSave(f); e.target.value = ''; });
$('biosFile').addEventListener('change', (e) => { const f = e.target.files[0]; if (f) loadBiosFile(f); e.target.value = ''; });
$('pause').addEventListener('click', togglePause);
$('saveExport').addEventListener('click', exportSave);
$('stateSave').addEventListener('click', saveStateNow);
$('stateLoad').addEventListener('click', loadStateNow);
$('stateSlot').addEventListener('change', (e) => { stateSlot = +e.target.value; refreshStates(); });
$('gbPalette').innerHTML = Object.entries(GB_PALETTES).map(([k, v]) => `<option value="${k}">${v.label}</option>`).join('');
$('gbPalette').value = gbPalette;
$('gbPalette').addEventListener('change', (e) => setPalette(e.target.value));
window.addEventListener('keydown', (e) => {
  if (cmenu.open || (e.target && e.target.tagName === 'INPUT' && e.target.type !== 'checkbox' && e.target.type !== 'range')) return;
  if (e.code === 'F5') { e.preventDefault(); saveStateNow(); }
  else if (e.code === 'F8') { e.preventDefault(); loadStateNow(); }
});
$('reset').addEventListener('click', () => { if (romBytes) { persistSave(); boot(); toast('Jogo reiniciado.'); } });
// ---------------------------------------------------------------- Modo TV (1080p e acima)
let hudTimer = 0;
function updateHud() {
  const c = $('tvScreen');
  const r = tvR ? tvR.lastRect : { w: 0, h: 0, scale: 0 };
  const names = FILTERS;
  $('tvInfo').textContent = `Saída ${c.width}×${c.height} · imagem ${r.w}×${r.h} · ${r.scale.toFixed(2).replace('.', ',')}× · ${names[video.filter]}`;
}
function pokeHud() {
  const tv = $('tv');
  tv.dataset.idle = '0';
  updateHud();
  clearTimeout(hudTimer);
  hudTimer = setTimeout(() => { tv.dataset.idle = '1'; }, 2500);
}
function openTv() {
  if (touchPlay()) { enterPlayFs(); return; }
  const tv = $('tv');
  tv.hidden = false;
  if (!tvR) tvR = new Renderer($('tvScreen'));
  tvOpen = true;
  updateImm();
  applyVideo();
  const req = tv.requestFullscreen || tv.webkitRequestFullscreen;
  if (req) Promise.resolve(req.call(tv)).catch(() => {});
  if (!running) present(img32); else presentGame();
  setTimeout(pokeHud, 300);
}
function closeTv() {
  tvOpen = false;
  updateImm();
  $('tv').hidden = true;
  if (document.fullscreenElement && document.exitFullscreen) document.exitFullscreen().catch(() => {});
  if (running) presentGame();
}
document.addEventListener('fullscreenchange', () => { if (!document.fullscreenElement && tvOpen) closeTv(); else if (tvOpen) setTimeout(pokeHud, 200); });
$('fullscreen').addEventListener('click', openTv);
if (touchPlay()) $('fullscreen').textContent = 'Tela cheia';
$('tvClose').addEventListener('click', closeTv);
$('tv').addEventListener('mousemove', pokeHud);
$('tv').addEventListener('touchstart', pokeHud, { passive: true });
$('tvFilter').addEventListener('click', () => {
  const order = Object.keys(FILTERS);
  video.filter = order[(order.indexOf(video.filter) + 1) % order.length];
  applyVideo(); pokeHud();
});
$('tvFit').addEventListener('click', () => {
  const order = ['integer', 'aspect', 'stretch'];
  video.fit = order[(order.indexOf(video.fit) + 1) % order.length];
  applyVideo(); pokeHud();
});
window.addEventListener('keydown', (e) => {
  if (cmenu.open) return;
  if (e.code === 'KeyF') { tvOpen ? closeTv() : openTv(); }
  else if (e.code === 'Escape' && tvOpen) closeTv();
});
window.addEventListener('resize', () => { if (tvOpen) pokeHud(); });
$('filter').addEventListener('change', (e) => { video.filter = e.target.value; applyVideo(); if (!running) present(img32); });
$('fit').addEventListener('change', (e) => { video.fit = e.target.value; applyVideo(); });
$('param').addEventListener('input', (e) => { video.strength[video.filter] = +e.target.value; applyVideo(); if (!running) present(img32); });
$('lcdColor').addEventListener('change', (e) => { video.lcdColor = e.target.checked; applyVideo(); });
$('frameBlend').addEventListener('change', (e) => { video.frameBlend = e.target.checked; applyVideo(); });

// ---------------------------------------------------------------- tema da carcaça
const SHELLS = { indigo: 'Índigo', grafite: 'Grafite fosco', gelo: 'Gelo', fume: 'Fumê translúcido', agua: 'Água translúcido' };
let shellTheme = 'indigo';
try { const t = localStorage.getItem('gba-shell'); if (t && SHELLS[t]) shellTheme = t; } catch { /* padrão */ }
function applyShell(t) {
  shellTheme = SHELLS[t] ? t : 'indigo';
  document.body.dataset.shell = shellTheme;
  $('shellTheme').value = shellTheme;
  try { localStorage.setItem('gba-shell', shellTheme); } catch { /* ignorado */ }
}
$('shellTheme').addEventListener('change', (e) => applyShell(e.target.value));
applyShell(shellTheme);
applyVideo();
// qualquer toque com um jogo aberto religa o som (iPhone depois de ligação, tela bloqueada etc.)
for (const ev of ['pointerdown', 'touchend', 'click', 'keydown']) {
  document.addEventListener(ev, () => { if (running && !audio.running) startAudio(); }, { capture: true, passive: true });
}
document.addEventListener('visibilitychange', () => {
  if (!audio.ctx) return;
  if (document.hidden) { if (audio.running) audio.ctx.suspend().catch(() => {}); }
  else if (running && !paused) audio.ctx.resume().catch(() => {}); // sem gesto pode falhar no iPhone: o próximo toque resolve
});
$('soundState').addEventListener('click', testSound);
$('soundTest').addEventListener('click', testSound);

// arrastar e soltar
const drop = $('screenWrap');
['dragenter', 'dragover'].forEach(ev => window.addEventListener(ev, (e) => { e.preventDefault(); drop.dataset.drag = '1'; }));
['dragleave', 'drop'].forEach(ev => window.addEventListener(ev, (e) => { e.preventDefault(); drop.dataset.drag = '0'; }));
window.addEventListener('drop', (e) => {
  const files = e.dataTransfer ? [...e.dataTransfer.files] : [];
  const f = files[0];
  if (!f) return;
  startAudio();
  const n = f.name.toLowerCase();
  if (n.endsWith('.indigo')) { doRestore(f); return; }
  if (files.length > 1) { addRomsToShelf(files); return; }
  if (n.endsWith('.sav') || n.endsWith('.ngf')) importSave(f);
  else if (n.endsWith('.bin') && f.size === 0x4000) loadBiosFile(f);
  else if (n.endsWith('.state')) toast('Use os espaços de estado do Índigo (Salvar/Carregar estado).');
  else openRom(f);
});

bindTouch();
requestAnimationFrame(loop);

// ------------------------------------------------------------ versão nova publicada
// Navegadores de console (Xbox) costumam manter a página antiga aberta ou em cache. A página confere a versão
// publicada ao abrir e ao voltar para a aba; sem jogo rodando, recarrega sozinha, e com jogo avisa.
const BUILD = typeof __BUILD__ !== 'undefined' ? __BUILD__ : 'dev';
let updateWarned = false;
async function checkUpdate() {
  if (BUILD === 'dev' || !/^https?:$/.test(location.protocol)) return;
  try {
    const r = await fetch('version.txt?t=' + Date.now(), { cache: 'no-store' });
    if (!r.ok) return;
    const v = (await r.text()).trim();
    if (!v || v === BUILD) return;
    let tried = null;
    try { tried = sessionStorage.getItem('indigo-reload'); } catch { /* sem armazenamento */ }
    if (!running && tried !== v) { // só uma tentativa por versão (evita recarregar em laço se o cache insistir)
      try { sessionStorage.setItem('indigo-reload', v); } catch { /* sem armazenamento */ }
      persistSave(); location.reload(); return;
    }
    if (!updateWarned) { updateWarned = true; toast('Há uma versão nova do Índigo. Salve e recarregue a página para atualizar.'); }
  } catch { /* sem rede: segue com a versão atual */ }
}
checkUpdate();
document.addEventListener('visibilitychange', () => { if (!document.hidden) checkUpdate(); });
setInterval(checkUpdate, 30 * 60 * 1000);

// ------------------------------------------------------------ registro (melhorias + atividade)
let logTab = 'changes';
function renderLog() {
  const el = $('logList');
  if (!el) return;
  if (logTab === 'changes') {
    el.innerHTML = CHANGELOG.map(c => `<li><time>${esc(c.date.split('-').reverse().join('/'))}</time><b>${esc(c.title)}</b>
      <ul>${c.items.map(i => `<li>${esc(i)}</li>`).join('')}</ul>${c.tests ? `<span class="note"><b>Testes:</b> ${esc(c.tests)}</span>` : ''}</li>`).join('');
  } else {
    let list = [];
    try { list = JSON.parse(localStorage.getItem('indigo-log') || '[]'); } catch { /* vazio */ }
    el.innerHTML = list.length ? list.map(e => `<li><time>${esc(fmtDate(e.at))}</time><span>${esc(e.text)}</span></li>`).join('')
      : '<li class="note">Nada registrado ainda neste aparelho.</li>';
  }
}
$('logCard').addEventListener('click', (e) => {
  const t = e.target.closest('[data-logtab]'); if (!t) return;
  logTab = t.dataset.logtab;
  for (const b of $('logCard').querySelectorAll('[data-logtab]')) b.classList.toggle('on', b === t);
  renderLog();
});
renderLog();
refreshStates();
