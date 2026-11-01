#include "ppu.h"

#include <algorithm>
#include <cstring>

#include "gb.h"

namespace indigo::gb {

namespace {
// tempos calibrados pelos testes do Mooneye (intr_2_*, hblank_ly_scx_timing)
constexpr int OAM_LEN = 84, M3_BASE = 169, SPR_COST = 6;
enum { S_OAM = 0, S_DRAW = 1, S_HBLANK = 2, S_LINE_END = 3, S_VLINE_END = 4, S_LY153 = 5, S_LINE_END_FRAME = 6, S_HBLANK_IRQ = 7 };
const uint32_t GREY[4] = {0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000};
}  // namespace

PPU::PPU(GB& g) : vram(0x4000), oam(0xA0), gb(g) {
  std::copy(GREY, GREY + 4, dmgBg);
  std::copy(GREY, GREY + 4, dmgObj0);
  std::copy(GREY, GREY + 4, dmgObj1);
  std::memset(lineIdx, 0, sizeof lineIdx);
  std::memset(linePri, 0, sizeof linePri);
  reset();
}

void PPU::reset() {
  lcdc = 0x91; stat = 0x80; scy = 0; scx = 0; ly = 0; lyc = 0;
  bgp = 0xFC; obp0 = 0xFF; obp1 = 0xFF; wy = 0; wx = 0;
  vbk = 0; bcps = 0; ocps = 0; opri = 0;
  mode = 2; dot = 0; nextAt = 80; stage = S_DRAW; line = 0;
  statLine = false; winLine = 0; wyHit = false; hblankIrq = false;
  frameDone = false; skipFrame = false;
  std::fill(vram.begin(), vram.end(), 0);
  std::fill(oam.begin(), oam.end(), 0);
  for (int k = 0; k < 64; k++) { bgPal[k] = 0xFF; objPal[k] = 0; }
  for (int k = 0; k < 32; k++) { bgRGB[k] = rgb(0x7FFF); objRGB[k] = rgb(0); }
  std::fill(fb, fb + 160 * 144, 0xFFFFFFFFu);
}

uint32_t PPU::rgb(int c) {
  const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  return 0xFF000000u | (((b << 3) | (b >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((r << 3) | (r >> 2));
}

void PPU::tick(int n) {
  if (!(lcdc & 0x80)) return;
  dot += n;
  while (dot >= nextAt) step();
}

void PPU::step() {
  switch (stage) {
    case S_OAM:
      setMode(2);
      nextAt = OAM_LEN; stage = S_DRAW;
      break;
    case S_DRAW:
      setMode(3);
      if (ly == wy) wyHit = true;
      nextAt = OAM_LEN + mode3Length() - 1;
      stage = S_HBLANK_IRQ;
      break;
    case S_HBLANK_IRQ:
      // a interrupção de HBlank sai 1 ponto antes de o STAT mostrar o modo 0
      hblankIrq = true; updateStat(); hblankIrq = false;
      nextAt++; stage = S_HBLANK;
      break;
    case S_HBLANK:
      if (!skipFrame) renderLine(ly);
      setMode(0);
      gb.hblankDma();
      nextAt = 456; stage = S_LINE_END;
      break;
    case S_LINE_END:
      dot -= 456;
      ly++;
      if (ly == 144) {
        setMode(1);
        gb.requestIrq(0x01);
        frameDone = true;
        skipFrame = false;
        nextAt = 456; stage = S_VLINE_END;
      } else { nextAt = 0; stage = S_OAM; }
      updateStat();
      break;
    case S_VLINE_END:
      dot -= 456;
      ly++;
      if (ly == 153) { nextAt = 4; stage = S_LY153; }
      else nextAt = 456;
      updateStat();
      break;
    case S_LY153:
      // na linha 153 o LY já volta a 0 depois de poucos pontos
      ly = 0;
      updateStat();
      nextAt = 456; stage = S_LINE_END_FRAME;
      break;
    case S_LINE_END_FRAME:
      dot -= 456;
      ly = 0; winLine = 0; wyHit = false;
      nextAt = 0; stage = S_OAM;
      updateStat();
      break;
  }
}

int PPU::mode3Length() {
  // 172 pontos + descarte do SCX fino + custo dos sprites (6 cada, mais o atraso de alinhamento com o
  // tile do fundo para o primeiro sprite de cada coluna de 8 pixels; regra da Pan Docs)
  int len = M3_BASE + (scx & 7);
  if (lcdc & 2) {
    const int h = (lcdc & 4) ? 16 : 8;
    int xs[10], n = 0;
    for (int k = 0; k < 40 && n < 10; k++) {
      const int y = oam[k * 4] - 16;
      if (ly >= y && ly < y + h) xs[n++] = oam[k * 4 + 1];
    }
    std::sort(xs, xs + n);
    int seen[10], ns = 0;
    for (int i = 0; i < n; i++) {
      const int x = xs[i];
      if (x >= 168) continue;
      len += SPR_COST;
      const int key = x == 0 ? -1 : (x + scx) >> 3;
      if (std::find(seen, seen + ns, key) != seen + ns) continue;
      seen[ns++] = key;
      len += x == 0 ? 5 : std::max(0, 5 - ((x + scx) & 7));
    }
  }
  return len;
}

// linha de interrupção de STAT: só a borda de subida gera pedido (bloqueio do hardware)
void PPU::updateStat() {
  const int s = stat;
  const bool lyMatch = ly == lyc;
  const bool on = (lcdc & 0x80) != 0;
  const bool l = on && (
    ((s & 0x40) && lyMatch) ||
    ((s & 0x08) && (mode == 0 || hblankIrq)) ||
    ((s & 0x20) && (mode == 2 || (mode == 1 && ly == 144 && dot < 4))) ||
    ((s & 0x10) && mode == 1));
  if (l && !statLine) gb.requestIrq(0x02);
  statLine = l;
}

int PPU::readStat() const {
  const bool on = (lcdc & 0x80) != 0;
  return 0x80 | (stat & 0x78) | (on && ly == lyc ? 4 : 0) | (on ? mode : 0);
}
void PPU::writeStat(int v) { stat = (stat & 0x87) | (v & 0x78); updateStat(); }
void PPU::writeLyc(int v) { lyc = v; updateStat(); }

void PPU::writeLcdc(int v) {
  const int was = lcdc & 0x80;
  lcdc = v;
  if (was && !(v & 0x80)) {
    // tela desligada: LY zera e o modo vai a 0; a imagem fica branca
    ly = 0; dot = 0; mode = 0; statLine = false;
    winLine = 0; wyHit = false;
    std::fill(fb, fb + 160 * 144, gb.cgb ? 0xFFFFFFFFu : dmgBg[0]);
    frameDone = true;
  } else if (!was && (v & 0x80)) {
    // ao religar, a primeira linha começa sem a busca de OAM e o primeiro quadro não aparece
    ly = 0; dot = 0; mode = 0;
    nextAt = 80; stage = S_DRAW;
    skipFrame = true;
    updateStat();
  }
}

void PPU::writeBcpd(int v) {
  const int i = bcps & 0x3F;
  bgPal[i] = static_cast<uint8_t>(v);
  const int p = i >> 1;
  bgRGB[p] = rgb(bgPal[p * 2] | (bgPal[p * 2 + 1] << 8));
  if (bcps & 0x80) bcps = 0x80 | ((i + 1) & 0x3F);
}
void PPU::writeOcpd(int v) {
  const int i = ocps & 0x3F;
  objPal[i] = static_cast<uint8_t>(v);
  const int p = i >> 1;
  objRGB[p] = rgb(objPal[p * 2] | (objPal[p * 2 + 1] << 8));
  if (ocps & 0x80) ocps = 0x80 | ((i + 1) & 0x3F);
}

void PPU::renderLine(int ly_) {
  if (ly_ < 0 || ly_ >= 144) return;  // estado danificado (linha de VBlank no estágio de HBlank): nada a desenhar
  const bool cgb = gb.cgb;
  const int row = ly_ * 160;
  const bool bgOn = cgb || (lcdc & 1);
  const bool winOn = (lcdc & 0x20) && wyHit && wx <= 166 && bgOn;
  const int winX = wx - 7;
  const int bgEnd = winOn ? std::max(0, winX) : 160;

  if (bgOn) {
    const int mapBase = (lcdc & 0x08) ? 0x1C00 : 0x1800;
    const int y = (ly_ + scy) & 255;
    drawTiles(row, 0, bgEnd, mapBase, y, scx);
  } else {
    const uint32_t c = dmgBg[0];
    for (int x = 0; x < 160; x++) { fb[row + x] = c; lineIdx[x] = 0; linePri[x] = 0; }
  }
  if (winOn) {
    const int mapBase = (lcdc & 0x40) ? 0x1C00 : 0x1800;
    drawTiles(row, bgEnd, 160, mapBase, winLine, -winX);
    winLine++;
  }
  if (lcdc & 2) drawSprites(ly_, row);
}

// desenha um trecho [x0, x1) da linha com o mapa de tiles indicado; sx = deslocamento horizontal
void PPU::drawTiles(int row, int x0, int x1, int mapBase, int y, int sx) {
  if (x1 <= x0) return;
  const bool cgb = gb.cgb;
  const bool unsignedTiles = (lcdc & 0x10) != 0;
  const int mapRow = mapBase + ((y >> 3) << 5);
  int x = x0;
  while (x < x1) {
    const int px = (x + sx) & 255;
    const int col = px >> 3;
    const int tile = vram[mapRow + col];
    const int attr = cgb ? vram[0x2000 + mapRow + col] : 0;
    int ty = y & 7;
    if (attr & 0x40) ty = 7 - ty;
    const int tileAddr = (unsignedTiles ? tile << 4 : 0x1000 + (static_cast<int8_t>(tile) * 16)) + (ty << 1) + ((attr & 0x08) ? 0x2000 : 0);
    const int lo = vram[tileAddr], hi = vram[tileAddr + 1];
    const bool flipX = (attr & 0x20) != 0;
    const int palBase = (attr & 7) << 2;
    int fx = px & 7;
    for (; fx < 8 && x < x1; fx++, x++) {
      const int bit = flipX ? fx : 7 - fx;
      const int ci = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
      lineIdx[x] = static_cast<uint8_t>(ci);
      linePri[x] = static_cast<uint8_t>(attr & 0x80);
      fb[row + x] = cgb ? bgRGB[palBase + ci] : dmgBg[(bgp >> (ci << 1)) & 3];
    }
  }
}

void PPU::drawSprites(int ly_, int row) {
  const bool cgb = gb.cgb;
  const int h = (lcdc & 4) ? 16 : 8;
  // até 10 sprites por linha, na ordem da OAM
  int n = 0;
  for (int k = 0; k < 40 && n < 10; k++) {
    const int y = oam[k * 4] - 16;
    if (ly_ >= y && ly_ < y + h) { objN[n] = static_cast<uint8_t>(k); objX[n] = oam[k * 4 + 1]; n++; }
  }
  // ordem de prioridade: no DMG, menor X vence (empate: menor índice); no CGB, menor índice
  int order[10];
  for (int k = 0; k < n; k++) order[k] = k;
  const bool byX = !cgb || (opri & 1);
  if (byX) std::stable_sort(order, order + n, [this](int a, int b) {
    if (objX[a] != objX[b]) return objX[a] < objX[b];
    return objN[a] < objN[b];
  });
  // desenha do menos para o mais prioritário
  const bool masterPri = !cgb || (lcdc & 1);
  for (int o = n - 1; o >= 0; o--) {
    const int k = objN[order[o]] * 4;
    const int sy = oam[k] - 16, sx = oam[k + 1] - 8, attr = oam[k + 3];
    int tile = oam[k + 2];
    if (h == 16) tile &= 0xFE;
    int ty = ly_ - sy;
    if (attr & 0x40) ty = h - 1 - ty;
    const int addr = (tile << 4) + (ty << 1) + (cgb && (attr & 0x08) ? 0x2000 : 0);
    const int lo = vram[addr], hi = vram[addr + 1];
    for (int fx = 0; fx < 8; fx++) {
      const int x = sx + fx;
      if (x < 0 || x >= 160) continue;
      const int bit = (attr & 0x20) ? fx : 7 - fx;
      const int ci = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
      if (!ci) continue;
      if (masterPri && lineIdx[x] && ((attr & 0x80) || (cgb && linePri[x]))) continue;
      if (cgb) fb[row + x] = objRGB[((attr & 7) << 2) + ci];
      else {
        const int pal = (attr & 0x10) ? obp1 : obp0;
        fb[row + x] = ((attr & 0x10) ? dmgObj1 : dmgObj0)[(pal >> (ci << 1)) & 3];
      }
    }
  }
}

void PPU::sync(StateIO& io) {
  io.bytes(vram); io.bytes(oam);
  io.raw(bgPal, sizeof bgPal); io.raw(objPal, sizeof objPal);
  for (int* p : {&lcdc, &stat, &scy, &scx, &ly, &lyc, &bgp, &obp0, &obp1, &wy, &wx,
                 &vbk, &bcps, &ocps, &opri, &mode, &dot, &nextAt, &stage, &winLine})
    io.sync(*p);
  io.sync(statLine); io.sync(wyHit);
  if (io.reading()) {
    for (int* p : {&lcdc, &stat, &scy, &scx, &lyc, &bgp, &obp0, &obp1, &wy, &wx}) *p &= 0xFF;
    vbk &= 1; bcps &= 0xBF; ocps &= 0xBF; opri &= 1; mode &= 3; stage &= 7;
    clampTo(ly, 0, 153); clampTo(dot, 0, 1024); clampTo(nextAt, 0, 1024); clampTo(winLine, 0, 255);
    for (int p = 0; p < 32; p++) {
      bgRGB[p] = rgb(bgPal[p * 2] | (bgPal[p * 2 + 1] << 8));
      objRGB[p] = rgb(objPal[p * 2] | (objPal[p * 2 + 1] << 8));
    }
  }
}

}  // namespace indigo::gb
