// Programa de teste do ARM7 (Índigo, núcleo do Nintendo DS): atende pedidos do ARM9 pela FIFO do IPC (eco,
// tela de toque e firmware pela SPI, relógio, som, tabela de volume da BIOS).
#include "t7_nds_hw.h"

u32 swi_voltable(u32 i);
void swi_vblank(void);

#define IRQ_FLAGS7 R32(0x0380FFF8)
#define IRQ_HANDLER7 R32(0x0380FFFC)

void irq7(void) {
  const u32 f = IE & IF;
  IF = f;
  IRQ_FLAGS7 |= f;
}

static u8 spiXfer(u16 dev, u8 v, int hold) {
  while (SPICNT & 0x80) {}
  SPICNT = 0x8000 | dev | (hold ? 0x800 : 0);
  SPIDATA = v;
  while (SPICNT & 0x80) {}
  return (u8)SPIDATA;
}

static u32 touch(int chan) {
  spiXfer(0x200, 0x80 | (chan << 4), 1);
  u32 hi = spiXfer(0x200, 0, 1);
  u32 lo = spiXfer(0x200, 0, 0);
  return ((hi & 0x7F) << 5) | (lo >> 3);
}

static void rtcByteOut(u8 b) {
  for (int i = 7; i >= 0; i--) {
    const u16 bit = (b >> i) & 1;
    RTCCNT = 0x54 | bit;        // CS=1, SCK=0, saída
    RTCCNT = 0x56 | bit;        // SCK=1
  }
}
static u8 rtcByteIn(void) {
  u8 v = 0;
  for (int i = 0; i < 8; i++) {
    RTCCNT = 0x44;              // CS=1, SCK=0, entrada
    RTCCNT = 0x46;
    v |= (RTCCNT & 1) << i;
  }
  return v;
}

static const s16 onda[32] = {
  8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000,
  -8000, -8000, -8000, -8000, -8000, -8000, -8000, -8000, -8000, -8000, -8000, -8000, -8000, -8000, -8000, -8000};

int main(void) {
  IRQ_HANDLER7 = (u32)irq7;
  IPCFIFOCNT = 0x8008 | 0x4000;
  IPCSYNC = 7 << 8;
  RES7[0] = 0x4E445337;
  for (;;) {
    while (IPCFIFOCNT & 0x100) {}
    const u32 m = IPCFIFORECV;
    u32 r = 0;
    switch (m >> 8) {
      case 1: r = (m & 0xFF) * 2 + 1; break;
      case 2: {
        const u32 pen = (EXTKEYIN & 0x40) ? 0 : 1;
        r = (pen << 31) | (touch(5) << 16) | touch(1);
        break;
      }
      case 3: {  // data e hora (comando 0x65, enviado do bit mais alto para o mais baixo)
        RTCCNT = 0x50; RTCCNT = 0x54;
        rtcByteOut(0x65);
        u8 d[7];
        for (int i = 0; i < 7; i++) d[i] = rtcByteIn();
        RTCCNT = 0x50;
        r = (d[0] << 24) | (d[1] << 16) | (d[2] << 8) | (d[4] & 0x3F);
        break;
      }
      case 4: {  // som: canal 0, PCM16 em laço, volume máximo, centro
        SOUNDCNT = 0x807F;
        SCHAN_SAD(0) = (u32)onda;
        SCHAN_TMR(0) = 0x10000 - 0x200;
        SCHAN_PNT(0) = 0;
        SCHAN_LEN(0) = sizeof onda / 4;
        SCHAN_CNT(0) = 0x7F | (64 << 16) | (1u << 27) | (1u << 29) | 0x80000000u;
        r = 1;
        break;
      }
      case 5: {  // firmware: primeira letra do nome nos dados de usuário (0x3FE06)
        spiXfer(0x100, 0x03, 1); spiXfer(0x100, 0x03, 1); spiXfer(0x100, 0xFE, 1); spiXfer(0x100, 0x06, 1);
        r = spiXfer(0x100, 0, 1);
        r |= spiXfer(0x100, 0, 0) << 8;
        break;
      }
      case 6: r = swi_voltable(723) | (swi_voltable(0) << 8) | (swi_voltable(483) << 16); break;
      case 7: IPCSYNC = (7 << 8) | 0x2000; r = 1; break;  // IRQ de sincronismo no ARM9
      default: r = 0xEEEEEEEE; break;
    }
    IPCFIFOSEND = r;
  }
}
