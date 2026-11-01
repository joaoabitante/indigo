// BIOS própria do Índigo para o Neo Geo Pocket (não usa nenhum byte da BIOS da SNK).
// Monta uma ROM de 64 KB com a tabela de chamadas do sistema em FFFE00 apontando para pequenos trechos com o
// opcode 0x1F, que a CPU entrega ao sistema; aqui cada chamada é feita em JavaScript.
// A fonte do sistema é desenho próprio (ASCII 0x20–0x7E, 8×8).

const CALLS = 0x1B;              // VECT_SHUTDOWN … VECT_COMGETBUFDATA
export const STUB_BASE = 0xFF1000; // trecho de cada chamada: STUB_BASE + n*16
export const SWI1_STUB = 0xFF0F00; // SWI 1: chamada do sistema pelo número em W3
export const DEFAULT_IRQ = 0xFF23DF; // tratador padrão de interrupção (RETI)

// fonte própria: 8 bytes por caractere, bit 7 = coluna da esquerda
const GLYPHS = {
  ' ': '0000000000000000', '!': '1010101010001000', '"': '2828000000000000', '#': '287C28287C280000',
  '$': '103C503814781000', '%': '64640810204C4C00', '&': '3048305448340000', "'": '1010000000000000',
  '(': '0810202020100800', ')': '2010080808102000', '*': '0028107C10280000', '+': '0010107C10100000',
  ',': '0000000000101020', '-': '0000007C00000000', '.': '0000000000303000', '/': '0404081020404000',
  '0': '38444C5464443800', '1': '1030101010103800', '2': '3844041820407C00', '3': '7C08100804443800',
  '4': '081828487C080800', '5': '7C40780404443800', '6': '1820407844443800', '7': '7C04081020202000',
  '8': '3844443844443800', '9': '3844443C04083000', ':': '0030300030300000', ';': '0030300030102000',
  '<': '0810204020100800', '=': '00007C007C000000', '>': '2010080408102000', '?': '3844040810001000',
  '@': '38445C545C403800', 'A': '3844447C44444400', 'B': '7844447844447800', 'C': '3844404040443800',
  'D': '7048444444487000', 'E': '7C40407840407C00', 'F': '7C40407840404000', 'G': '3844405C44443C00',
  'H': '4444447C44444400', 'I': '3810101010103800', 'J': '1C08080808483000', 'K': '4448506050484400',
  'L': '4040404040407C00', 'M': '446C545444444400', 'N': '444464544C444400', 'O': '3844444444443800',
  'P': '7844447840404000', 'Q': '3844444454483400', 'R': '7844447850484400', 'S': '3C40403804047800',
  'T': '7C10101010101000', 'U': '4444444444443800', 'V': '4444444444281000', 'W': '4444445454542800',
  'X': '4444281028444400', 'Y': '4444281010101000', 'Z': '7C04081020407C00', '[': '3820202020203800',
  '\\': '4040201008040400', ']': '3808080808083800', '^': '1028440000000000', '_': '0000000000007C00',
  '`': '2010080000000000', 'a': '000038043C443C00', 'b': '4040586444447800', 'c': '0000384040443800',
  'd': '0404344C44443C00', 'e': '000038447C403800', 'f': '1824207020202000', 'g': '00003C44443C0438',
  'h': '4040586444444400', 'i': '1000301010103800', 'j': '0800180808084830', 'k': '4040485060504800',
  'l': '3010101010103800', 'm': '0000685454544400', 'n': '0000586444444400', 'o': '0000384444443800',
  'p': '0000784444784040', 'q': '0000344C443C0404', 'r': '0000586440404000', 's': '00003C4038047800',
  't': '2020702020241800', 'u': '00004444444C3400', 'v': '0000444444281000', 'w': '0000444454542800',
  'x': '0000442810284400', 'y': '00004444443C0438', 'z': '00007C0810207C00', '{': '0C10102010100C00',
  '|': '1010101010101000', '}': '6010100810106000', '~': '0000205408000000',
};
export const FONT = new Uint8Array(0x800);
for (const [ch, hex] of Object.entries(GLYPHS)) {
  const c = ch.charCodeAt(0);
  for (let r = 0; r < 8; r++) FONT[c * 8 + r] = parseInt(hex.substr(r * 2, 2), 16);
}
// bloco cheio no caractere 1 (usado para molduras/cursores)
for (let r = 0; r < 8; r++) FONT[8 + r] = 0xFF;

export function buildBios() {
  const b = new Uint8Array(0x10000);
  const put32 = (o, v) => { b[o] = v & 0xFF; b[o + 1] = (v >> 8) & 0xFF; b[o + 2] = (v >> 16) & 0xFF; b[o + 3] = (v >>> 24) & 0xFF; };
  for (let n = 0; n < CALLS; n++) {
    const a = STUB_BASE + n * 16;
    put32(0xFE00 + n * 4, a);
    b[a & 0xFFFF] = 0x1F;
  }
  // vetores de SWI / interrupções do hardware: SWI 1 = chamada do sistema; o resto volta direto
  for (let n = 0; n < 64; n++) put32(0xFF00 + n * 4, DEFAULT_IRQ);
  put32(0xFF04, SWI1_STUB);
  b[SWI1_STUB & 0xFFFF] = 0x1F;
  b[DEFAULT_IRQ & 0xFFFF] = 0x07;   // RETI
  b[0x23C3] = 0x07;
  b[0xFFFE] = 0x68; b[0xFFFF] = 0xFE; // JR $ (sem cartucho)
  return b;
}

// tamanhos dos blocos da flash (chip com o bloco de partida no topo): 64 KB… depois 32K, 8K, 8K, 16K
export function flashBlocks(chipSize) {
  const out = [];
  let a = 0;
  while (a < chipSize - 0x10000) { out.push([a, 0x10000]); a += 0x10000; }
  out.push([a, 0x8000]); out.push([a + 0x8000, 0x2000]); out.push([a + 0xA000, 0x2000]); out.push([a + 0xC000, 0x4000]);
  return out;
}

// executa a chamada número n; devolve 'ret' (volta com RET), 'stay' (fica parado) ou 'none'
export function biosCall(sys, n) {
  const cpu = sys.cpu;
  const RA3 = 0x30, RW3 = 0x31, RC3 = 0x34, RB3 = 0x35, XDE3 = 0x38, XHL3 = 0x3C;
  switch (n) {
    case 0x00: return 'stay';                                   // VECT_SHUTDOWN
    case 0x01: break;                                           // VECT_CLOCKGEARSET
    case 0x02: {                                                // VECT_RTCGET
      const d = cpu.getL(XHL3);
      if (d < 0xC000) for (let i = 0; i < 7; i++) sys.write8(d + i, sys.read8(0x91 + i));
      break;
    }
    case 0x04: {                                                // VECT_INTLVSET (RC3 = fonte, RB3 = nível)
      const lv = cpu.getB(RB3) & 7, src = cpu.getB(RC3);
      const map = [[0x70, 0], [0x71, 4], [0x73, 0], [0x73, 4], [0x74, 0], [0x74, 4], [0x79, 0], [0x79, 4], [0x7A, 0], [0x7A, 4]];
      if (src < map.length) {
        const [reg, sh] = map[src];
        sys.write8(reg, (sys.intPrio[reg - 0x70] & ~(0x0F << sh)) | (lv << sh));
      }
      break;
    }
    case 0x05: {                                                // VECT_SYSFONTSET: RA3 = cor do traço (bits 0–1) e do fundo (bits 4–5)
      const v = cpu.getB(RA3), fg = v & 3, bg = (v >> 4) & 3;
      const chr = sys.gfx.chr;
      for (let i = 0; i < 0x800; i++) {
        const c = FONT[i];
        let w = 0;
        for (let k = 0; k < 8; k++) w |= ((c & (0x80 >> k)) ? fg : bg) << (14 - k * 2);
        chr[i * 2] = w & 0xFF; chr[i * 2 + 1] = w >> 8;
      }
      break;
    }
    case 0x06: {                                                // VECT_FLASHWRITE: RA3 banco, BC3 blocos de 256, XHL3 origem, XDE3 destino
      const base = cpu.getB(RA3) === 1 ? 0x800000 : 0x200000;
      const count = cpu.getW(RC3) * 256, src = cpu.getL(XHL3), dst = cpu.getL(XDE3);
      for (let i = 0; i < count; i++) sys.flashProgram(base + dst + i, sys.read8(src + i));
      cpu.setB(RA3, 0);
      break;
    }
    case 0x07: {                                                // VECT_FLASHALLERS
      const base = cpu.getB(RA3) === 1 ? 0x800000 : 0x200000;
      sys.flashErase(base, 0, sys.chipSize(base));
      cpu.setB(RA3, 0);
      break;
    }
    case 0x08: {                                                // VECT_FLASHERS: RA3 banco, RB3 bloco
      const base = cpu.getB(RA3) === 1 ? 0x800000 : 0x200000;
      const blocks = flashBlocks(sys.chipSize(base)), k = cpu.getB(RB3);
      if (k < blocks.length) sys.flashErase(base, blocks[k][0], blocks[k][1]);
      cpu.setB(RA3, 0);
      break;
    }
    case 0x09: case 0x0B: case 0x0D: cpu.setB(RA3, 0); break;   // ALARMSET, ALARMDOWNSET, FLASHPROTECT
    case 0x0E: break;                                           // VECT_GEMODESET
    case 0x10: cpu.setB(RA3, 0); break;                         // VECT_COMINIT
    case 0x13: cpu.setB(RA3, 0); break;                         // VECT_COMCREATEDATA (cabo não ligado)
    case 0x14: cpu.setB(RA3, 1); break;                         // VECT_COMGETDATA: buffer vazio
    case 0x15: sys.write8(0xB2, 0); break;                      // VECT_COMONRTS
    case 0x16: sys.write8(0xB2, 1); break;                      // VECT_COMOFFRTS
    case 0x17: case 0x18: cpu.setW(RA3, 0); break;              // COMSENDSTATUS / COMRECIVESTATUS
    case 0x19: {                                                // VECT_COMCREATEBUFDATA: consome os bytes
      cpu.setL(XHL3, cpu.getL(XHL3) + cpu.getB(RB3)); cpu.setB(RB3, 0);
      break;
    }
    default: break;
  }
  void RW3;
  return 'ret';
}
