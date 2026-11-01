// Núcleo do emulador fora da página: roda num Web Worker (ou, se o navegador não deixar, na própria página
// com a mesma interface). Recebe comandos por mensagem e devolve quadros, som e saves.
//
// página → núcleo                                   núcleo → página
//   load {rom, save, bios, rate, palette}            loaded {sys, title, gameCode, saveLabel, hasSave}
//   run {n, keys, audio}                             frame {fb, w, h, L, R, frame, save?}
//   rate {rate} · palette {bg, obj0, obj1}
//   stateSave {id} · stateLoad {id, state}           reply {id, ok, value | error}
//   saveGet {id} · saveImport {id, bytes}

import { GBA } from '../src/gba.js';
import { GB } from '../src/gb/gb.js';
import { gbaSaveState, gbaLoadState } from '../src/snapshot.js';
import { SMS } from '../src/sms/sms.js';
import { NGP } from '../src/ngp/ngp.js';

const RTC_MAGIC = [0x52, 0x54, 0x43, 0x31]; // "RTC1": ajuste do relógio do GBA guardado depois do save

export function detectKind(bytes) {
  const gbLogo = bytes.length >= 0x150 && bytes[0x104] === 0xCE && bytes[0x105] === 0xED && bytes[0x106] === 0x66 && bytes[0x107] === 0x66;
  return gbLogo ? 'gb' : 'gba';
}

export function createCore(post) {
  let emu = null, sys = 'gba', rate = 48000;

  // consoles cujo núcleo cuida do próprio save (hasSave/exportSave/importSave)
  const isSega = () => sys === 'gg' || sys === 'sms' || sys === 'ngp' || sys === 'ngpc';
  function hasSave() {
    if (!emu) return false;
    if (isSega()) return emu.hasSave();
    return sys === 'gba' ? true : emu.cart.hasSave();
  }
  function isDirty() {
    if (!emu) return false;
    if (isSega()) return emu.dirty;
    if (sys === 'gba') return emu.bus.save.dirty;
    return emu.cart.dirty && emu.cart.hasSave();
  }
  function exportSave() {
    if (isSega()) return emu.exportSave();
    if (sys === 'gba') {
      const s = emu.bus.save; s.dirty = false;
      const rtc = emu.bus.gpio;
      if (!rtc || !rtc.offset) return s.data.slice();
      const out = new Uint8Array(s.data.length + 16);
      out.set(s.data);
      out.set(RTC_MAGIC, s.data.length);
      new DataView(out.buffer).setFloat64(s.data.length + 4, rtc.offset, true);
      out[s.data.length + 12] = rtc.status;
      return out;
    }
    const c = emu.cart; c.dirty = false;
    return c.hasSave() ? c.exportSave() : null;
  }
  function importSave(bytes) {
    if (!bytes || !bytes.length) return;
    if (isSega()) { emu.importSave(bytes); return; }
    if (sys === 'gba') {
      emu.bus.save.load(bytes);
      const n = emu.bus.save.data.length, rtc = emu.bus.gpio;
      if (rtc && bytes.length >= n + 16 && RTC_MAGIC.every((v, i) => bytes[n + i] === v)) {
        rtc.offset = new DataView(bytes.buffer, bytes.byteOffset).getFloat64(n + 4, true) || 0;
        rtc.status = bytes[n + 12] || 0x40;
      }
    } else emu.cart.importSave(bytes);
  }
  function saveLabel() {
    if (sys === 'ngp' || sys === 'ngpc') return 'flash do cartucho';
    if (isSega()) return emu.hasSave() ? 'RAM do cartucho' : 'nenhum por enquanto';
    if (sys === 'gba') {
      const t = emu.bus.save.type;
      const base = { sram: 'SRAM', flash64: 'Flash 64K', flash128: 'Flash 128K', eeprom: 'EEPROM' }[t] || t;
      return emu.bus.gpio ? base + ' + relógio' : base;
    }
    const c = emu.cart;
    if (!c.hasSave()) return 'nenhum';
    return (c.ram.length ? `RAM ${c.ram.length >= 1024 ? c.ram.length / 1024 + ' KB' : c.ram.length + ' B'}` : '') + (c.info.rtc ? ' + relógio' : '');
  }
  function setPalette(p) {
    if (!p || sys === 'gba' || isSega() || !emu) return;
    emu.ppu.dmgBg.set(p.bg); emu.ppu.dmgObj0.set(p.obj0 || p.bg); emu.ppu.dmgObj1.set(p.obj1 || p.obj0 || p.bg);
  }
  const reply = (id, ok, value, error) => post({ t: 'reply', id, ok, value, error });

  return function handle(m) {
    try {
      switch (m.t) {
        case 'ping': reply(m.id, true, 'pong'); break;
        case 'load': {
          const rom = new Uint8Array(m.rom);
          if (m.kind === 'ngp' || m.kind === 'ngpc') {
            emu = new NGP();
            emu.loadRom(rom);
            sys = emu.color ? 'ngpc' : 'ngp';
          } else if (m.kind === 'gg' || m.kind === 'sms') {
            emu = new SMS(m.kind === 'gg');
            emu.loadRom(rom);
            sys = m.kind;
          } else if (detectKind(rom) === 'gb') {
            emu = new GB();
            emu.loadRom(rom, null, 'auto');
            sys = emu.cgb ? 'gbc' : 'gb';
          } else {
            emu = new GBA();
            if (m.bios) emu.loadBios(new Uint8Array(m.bios));
            emu.loadRom(rom);
            sys = 'gba';
          }
          if (m.rate) rate = m.rate;
          emu.apu.setSampleRate(rate);
          if (m.save) importSave(new Uint8Array(m.save));
          setPalette(m.palette);
          post({ t: 'loaded', id: m.id, sys, title: emu.title, gameCode: emu.gameCode, saveLabel: saveLabel(), hasSave: hasSave() });
          break;
        }
        case 'run': {
          if (!emu) return;
          emu.setKeys(m.keys | 0);
          for (let k = 0; k < m.n; k++) emu.runFrame();
          const n = emu.apu.available();
          const L = new Float32Array(n), R = new Float32Array(n);
          if (n) emu.apu.pull(L, R);
          const [w, h] = sys === 'gba' ? [240, 160] : sys === 'ngp' || sys === 'ngpc' ? [160, 152] : isSega() ? [emu.vdp.w, emu.vdp.h] : [160, 144];
          const fb = new Uint32Array(emu.ppu.fb.subarray(0, w * h));
          const msg = { t: 'frame', fb, w, h, frame: emu.frame, L: m.audio ? L : null, R: m.audio ? R : null };
          const tr = [fb.buffer];
          if (m.audio) tr.push(L.buffer, R.buffer);
          if (isDirty()) { const s = exportSave(); if (s) { msg.save = s; tr.push(s.buffer); } }
          post(msg, tr);
          break;
        }
        case 'rate': rate = m.rate; if (emu) emu.apu.setSampleRate(rate); break;
        case 'palette': setPalette(m.palette); break;
        case 'stateSave': {
          const st = sys === 'gba' ? gbaSaveState(emu) : emu.saveState();
          st.kind = sys;
          st.sys = sys;
          reply(m.id, true, st);
          break;
        }
        case 'stateLoad': {
          if (sys === 'gba') gbaLoadState(emu, m.state); else emu.loadState(m.state);
          emu.apu.setSampleRate(rate);
          reply(m.id, true, null);
          break;
        }
        case 'saveGet': reply(m.id, true, emu && hasSave() ? exportSave() : null); break;
        case 'saveImport': importSave(new Uint8Array(m.bytes)); reply(m.id, true, null); break;
        default: break;
      }
    } catch (e) {
      if (m.id !== undefined) reply(m.id, false, null, String((e && e.message) || e));
      else post({ t: 'error', error: String((e && e.message) || e) });
    }
  };
}
