#include "vdp.h"

#include <algorithm>
#include <cstring>

namespace indigo::sms {

VDP::VDP(bool isGG) : gg(isGG), w(isGG ? 160 : 256), h(isGG ? 144 : 192), vram(0x4000), cram(isGG ? 64 : 32) {
  reset();
}

void VDP::reset() {
  std::fill(vram.begin(), vram.end(), 0);
  std::fill(cram.begin(), cram.end(), 0);
  std::fill(regs, regs + 16, 0);
  // valores deixados pela BIOS
  const uint8_t init[] = {0x36, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00, 0x00, 0x00, 0xFF};
  std::copy(std::begin(init), std::end(init), regs);
  addr = 0; code = 0; latch = false; first = 0; buffer = 0; ggLatch = 0;
  status = 0; lineIrq = false;
  lineCounter = 0xFF; vscrollLatch = 0;
  vcount = 0; hcount = 0;
  std::fill(rgb, rgb + 32, 0xFF000000u);
  std::fill(fb, fb + 256 * 192, 0xFF000000u);
}

int VDP::readData() {
  latch = false;
  const int v = buffer;
  buffer = vram[addr];
  addr = (addr + 1) & 0x3FFF;
  return v;
}

void VDP::writeData(int v) {
  latch = false;
  if (code == 3) {
    if (gg) {
      if (!(addr & 1)) ggLatch = v;
      else {
        const int i = addr & 0x3E;
        cram[i] = static_cast<uint8_t>(ggLatch); cram[i + 1] = static_cast<uint8_t>(v & 0x0F);
        color(i >> 1);
      }
    } else { const int i = addr & 0x1F; cram[i] = static_cast<uint8_t>(v & 0x3F); color(i); }
  } else vram[addr] = static_cast<uint8_t>(v);
  buffer = v;
  addr = (addr + 1) & 0x3FFF;
}

int VDP::readStatus() {
  const int v = status | 0x1F;
  status = 0; lineIrq = false; latch = false;
  return v;
}

void VDP::writeControl(int v) {
  if (!latch) { first = v; addr = (addr & 0x3F00) | v; latch = true; return; }
  latch = false;
  code = v >> 6;
  addr = ((v & 0x3F) << 8) | first;
  if (code == 0) { buffer = vram[addr]; addr = (addr + 1) & 0x3FFF; }
  else if (code == 2) regs[v & 0x0F] = static_cast<uint8_t>(first);
}

void VDP::color(int i) {
  uint32_t r, g, b;
  if (gg) {
    const int wd = cram[i * 2] | (cram[i * 2 + 1] << 8);
    r = (wd & 0xF) * 17; g = ((wd >> 4) & 0xF) * 17; b = ((wd >> 8) & 0xF) * 17;
  } else {
    const int c = cram[i];
    r = (c & 3) * 85; g = ((c >> 2) & 3) * 85; b = ((c >> 4) & 3) * 85;
  }
  rgb[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
}

// início da linha: contador de linha (gera a interrupção de linha) e rolagem vertical
void VDP::lineStart(int ln) {
  vcount = ln;
  if (ln == 0) vscrollLatch = regs[9];
  if (ln <= 192) {
    if (lineCounter == 0) { lineCounter = regs[10]; lineIrq = true; }
    else lineCounter = (lineCounter - 1) & 0xFF;
  } else lineCounter = regs[10];
  if (ln == 193) status |= 0x80;  // fim da imagem: interrupção de quadro
}

void VDP::renderLine(int y) {
  const int r0 = regs[0], r1 = regs[1];
  const uint32_t back = rgb[16 + (regs[7] & 15)];
  if (!(r1 & 0x40)) { fillLine(back); store(y); return; }
  if (!(r0 & 0x04)) { fillLine(0xFF000000u); store(y); return; }  // modos TMS antigos: não suportados
  bgLine(y);
  sprites(y);
  if (r0 & 0x20) for (int x = 0; x < 8; x++) line[x] = back;
  store(y);
}

void VDP::store(int y) {
  if (gg) {
    if (y < 24 || y >= 168) return;
    std::memcpy(fb + (y - 24) * 160, line + 48, 160 * sizeof(uint32_t));
  } else std::memcpy(fb + y * 256, line, 256 * sizeof(uint32_t));
}

void VDP::bgLine(int y) {
  const int r0 = regs[0];
  const int nameBase = (regs[2] & 0x0E) << 10;
  const int hs = (r0 & 0x40) && y < 16 ? 0 : regs[8];
  const int vyScroll = (y + vscrollLatch) % 224;
  int lastKey = -1, p0 = 0, p1 = 0, p2 = 0, p3 = 0, pal = 0, prio = 0, hflip = 0;
  for (int x = 0; x < 256; x++) {
    // as 8 colunas da direita podem ignorar a rolagem vertical (placar fixo)
    const int vy = (r0 & 0x80) && x >= 192 ? y : vyScroll;
    const int bx = (x - hs) & 255;
    const int key = ((vy >> 3) << 5) | (bx >> 3);
    if (key != lastKey) {
      lastKey = key;
      const int ent = nameBase + (key << 1);
      const int wd = vram[ent] | (vram[ent + 1] << 8);
      hflip = wd & 0x200; pal = (wd & 0x800) ? 16 : 0; prio = wd & 0x1000;
      int ty = vy & 7; if (wd & 0x400) ty = 7 - ty;
      const int a = ((wd & 0x1FF) << 5) + (ty << 2);
      p0 = vram[a]; p1 = vram[a + 1]; p2 = vram[a + 2]; p3 = vram[a + 3];
    }
    const int px = bx & 7, bit = hflip ? px : 7 - px;
    const int c = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
    line[x] = rgb[pal + c];
    bgPri[x] = prio && c ? 1 : 0;
  }
}

void VDP::sprites(int y) {
  const int r0 = regs[0], r1 = regs[1];
  const int sat = (regs[5] & 0x7E) << 7;
  const int big = r1 & 2, zoom = r1 & 1;
  const int hgt = (big ? 16 : 8) << (zoom ? 1 : 0);
  const int patBase = (regs[6] & 4) ? 256 : 0;
  const int shift = (r0 & 8) ? 8 : 0;
  std::memset(sprCol, 0, sizeof sprCol);
  int list[8], n = 0;
  for (int i = 0; i < 64; i++) {
    const int sy = vram[sat + i];
    if (sy == 0xD0) break;
    int top = sy + 1; if (top > 240) top -= 256;
    if (y >= top && y < top + hgt) {
      if (n == 8) { status |= 0x40; break; }
      list[n++] = i;
    }
  }
  // o primeiro da lista tem prioridade: desenha de trás para frente
  for (int k = n - 1; k >= 0; k--) {
    const int i = list[k];
    int top = vram[sat + i] + 1; if (top > 240) top -= 256;
    const int sx = vram[sat + 0x80 + i * 2] - shift;
    int tile = vram[sat + 0x81 + i * 2] + patBase;
    if (big) tile &= ~1;
    int ty = (y - top) >> (zoom ? 1 : 0);
    if (ty >= 8) { tile += 1; ty -= 8; }
    const int a = ((tile & 0x1FF) << 5) + (ty << 2);
    const int p0 = vram[a], p1 = vram[a + 1], p2 = vram[a + 2], p3 = vram[a + 3];
    const int wpx = zoom ? 16 : 8;
    for (int px = 0; px < wpx; px++) {
      const int x = sx + px;
      if (x < 0 || x > 255) continue;
      const int bit = 7 - (zoom ? px >> 1 : px);
      const int c = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
      if (!c) continue;
      if (sprCol[x]) status |= 0x20;
      sprCol[x] = 1;
      if (!bgPri[x]) line[x] = rgb[16 + c];
    }
  }
}

void VDP::sync(StateIO& io) {
  io.bytes(vram); io.bytes(cram);
  io.raw(regs, sizeof regs);
  for (int* p : {&addr, &code, &first, &buffer, &ggLatch, &status, &lineCounter, &vscrollLatch, &hcount}) io.sync(*p);
  io.sync(latch); io.sync(lineIrq);
  if (io.reading()) {
    addr &= 0x3FFF; code &= 3;
    for (int* p : {&first, &buffer, &ggLatch, &status, &lineCounter, &vscrollLatch, &hcount}) *p &= 0xFF;
    for (int i = 0; i < 32; i++) color(i);
  }
}

}  // namespace indigo::sms
