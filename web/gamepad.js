// Controles (Gamepad API): mapeamento configurável, atalhos e modo "aperte um botão" para remapear
// Índices do layout padrão (Xbox): 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT, 8 View, 9 Menu,
// 10 analógico esquerdo (clique), 11 analógico direito (clique), 12-15 direcional, 16 botão Xbox

import { KEY } from '../src/gba.js';

export const XBOX_NAMES = ['A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'View', 'Menu', 'LS (clique)', 'RS (clique)',
  'Direcional ↑', 'Direcional ↓', 'Direcional ←', 'Direcional →', 'Botão Xbox'];

// ações configuráveis, na ordem em que aparecem na tela
export const ACTIONS = [
  ['UP', 'Cima'], ['DOWN', 'Baixo'], ['LEFT', 'Esquerda'], ['RIGHT', 'Direita'],
  ['A', 'A'], ['B', 'B'], ['L', 'L'], ['R', 'R'], ['START', 'Start'], ['SELECT', 'Select'],
  ['turbo', 'Acelerar (segurar)'], ['pause', 'Pausar'], ['tv', 'Modo TV'],
];

export const PRESETS = {
  // letras iguais: o A do Xbox é o A do GBA
  letters: { UP: [12], DOWN: [13], LEFT: [14], RIGHT: [15], A: [0], B: [1, 2], L: [4], R: [5], START: [9], SELECT: [8], turbo: [7], pause: [10], tv: [11] },
  // posição Nintendo: o botão da direita é o A, o de baixo é o B
  position: { UP: [12], DOWN: [13], LEFT: [14], RIGHT: [15], A: [1, 3], B: [0, 2], L: [4], R: [5], START: [9], SELECT: [8], turbo: [7], pause: [10], tv: [11] },
};

// Alguns Android mostram o leitor de digital ou sensores como "controle" (ex.: uinput-fpc); esses são ignorados.
function isRealPad(p) {
  if (/uinput-fpc|fingerprint|goodix|gf_input|uinput-goodix|sensor/i.test(p.id || '')) return false;
  return (p.buttons ? p.buttons.length : 0) >= 6;
}

export class GamepadInput {
  constructor(onAction) {
    this.onAction = onAction; // (nome) => void, disparado ao apertar pause/tv
    this.settings = { preset: 'letters', map: JSON.parse(JSON.stringify(PRESETS.letters)), stick: true, deadzone: 0.45 };
    try {
      const s = JSON.parse(localStorage.getItem('gba-pad') || 'null');
      if (s && s.map) this.settings = Object.assign(this.settings, s);
    } catch { /* sem preferências salvas */ }
    this.prevPressed = new Set();
    this.listener = null;
    this.turbo = false;
    this.connected = null;
    this.lastPressed = new Set();
  }

  save() { try { localStorage.setItem('gba-pad', JSON.stringify(this.settings)); } catch { /* ignorado */ } }

  applyPreset(name) {
    this.settings.preset = name;
    this.settings.map = JSON.parse(JSON.stringify(PRESETS[name]));
    this.save();
  }

  // espera o próximo botão apertado e devolve o índice
  listen(cb) { this.listener = { cb, armed: false }; }
  cancelListen() { this.listener = null; }

  assign(action, index) {
    const map = this.settings.map;
    // um botão físico só faz uma coisa: remove de outras ações
    for (const k of Object.keys(map)) map[k] = (map[k] || []).filter(i => i !== index);
    map[action] = [index];
    this.settings.preset = 'custom';
    this.save();
  }

  labelFor(action) {
    const list = this.settings.map[action] || [];
    if (!list.length) return '—';
    return list.map(i => XBOX_NAMES[i] || `Botão ${i}`).join(' ou ');
  }

  // lê os controles; devolve a máscara de teclas do GBA
  poll() {
    let pads = [];
    try { pads = navigator.getGamepads ? Array.from(navigator.getGamepads()) : []; } catch { pads = []; }
    const pressed = new Set();
    let ax = 0, ay = 0, found = null;
    for (const p of pads) {
      if (!p || !p.connected || !isRealPad(p)) continue;
      if (!found) found = p;
      p.buttons.forEach((b, i) => { if (b && (b.pressed || b.value > 0.5)) pressed.add(i); });
      // controles fora do layout padrão (alguns Android em modo "Other"/DirectInput): direcional como eixos 6/7
      if (p.mapping !== 'standard' && p.axes.length >= 8) {
        const hx = p.axes[6] || 0, hy = p.axes[7] || 0;
        if (hy < -0.5) pressed.add(12); if (hy > 0.5) pressed.add(13);
        if (hx < -0.5) pressed.add(14); if (hx > 0.5) pressed.add(15);
      }
      if (Math.abs(p.axes[0] || 0) > Math.abs(ax)) ax = p.axes[0] || 0;
      if (Math.abs(p.axes[1] || 0) > Math.abs(ay)) ay = p.axes[1] || 0;
    }
    this.connected = found;
    this.lastPressed = pressed;
    this.axes = [ax, ay];

    // modo de remapeamento: espera soltar tudo, depois pega o primeiro botão novo
    if (this.listener) {
      if (!this.listener.armed) { if (pressed.size === 0) this.listener.armed = true; }
      else if (pressed.size) {
        const idx = [...pressed][0];
        const l = this.listener; this.listener = null;
        l.cb(idx);
      }
      this.prevPressed = pressed;
      return 0;
    }

    const map = this.settings.map;
    let mask = 0;
    for (const [action] of ACTIONS) {
      const list = map[action] || [];
      const down = list.some(i => pressed.has(i));
      if (action in KEY) { if (down) mask |= 1 << KEY[action]; }
      else if (action === 'turbo') this.turbo = down;
      else if (down && !list.some(i => this.prevPressed.has(i))) this.onAction(action);
    }
    if (this.settings.stick) {
      const dz = this.settings.deadzone;
      if (ay < -dz) mask |= 1 << KEY.UP;
      if (ay > dz) mask |= 1 << KEY.DOWN;
      if (ax < -dz) mask |= 1 << KEY.LEFT;
      if (ax > dz) mask |= 1 << KEY.RIGHT;
    }
    this.prevPressed = pressed;
    return mask;
  }

  rumble(ms = 120) {
    const p = this.connected;
    try {
      if (p && p.vibrationActuator && p.vibrationActuator.playEffect) {
        p.vibrationActuator.playEffect('dual-rumble', { duration: ms, strongMagnitude: 0.6, weakMagnitude: 0.4 });
      }
    } catch { /* sem vibração */ }
  }
}
