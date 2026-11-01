#include "tms9918.h"

#include <algorithm>
#include <cstring>

namespace indigo::tms {

// Paleta do TMS9918 (valores RGB usuais, os mesmos do MAME), em 0xAABBGGRR
#define TMS_RGB(r, g, b) (0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r))
const uint32_t TMS_PALETTE[16] = {
    TMS_RGB(0x00, 0x00, 0x00), TMS_RGB(0x00, 0x00, 0x00), TMS_RGB(0x21, 0xC8, 0x42), TMS_RGB(0x5E, 0xDC, 0x78),
    TMS_RGB(0x54, 0x55, 0xED), TMS_RGB(0x7D, 0x76, 0xFC), TMS_RGB(0xD4, 0x52, 0x4D), TMS_RGB(0x42, 0xEB, 0xF5),
    TMS_RGB(0xFC, 0x55, 0x54), TMS_RGB(0xFF, 0x79, 0x78), TMS_RGB(0xD4, 0xC1, 0x54), TMS_RGB(0xE6, 0xCE, 0x80),
    TMS_RGB(0x21, 0xB0, 0x3B), TMS_RGB(0xC9, 0x5B, 0xBA), TMS_RGB(0xCC, 0xCC, 0xCC), TMS_RGB(0xFF, 0xFF, 0xFF),
};
#undef TMS_RGB

namespace {
constexpr int LINE_TICKS = 342;  // pontos (5,37 MHz) por linha = 228 ciclos de CPU × 1,5

// Janelas de acesso da CPU numa linha, em pontos do TMS9918 (medidas no aparelho real; publicadas no
// openMSX em unidades 4× menores, aqui já divididas por 4).
struct SlotTables {
  // espera (em ciclos de CPU) de um pedido feito em cada ponto da linha até a janela que o atende
  uint8_t wait[4][LINE_TICKS];  // 0 = tela desligada/retraço, 1 = Graphics I/II, 2 = Multicolor, 3 = Text
  SlotTables() {
    int off[107], n = 0;
    for (int t = 4; t <= 204; t += 8) off[n++] = t / 4;
    for (int t = 220; t <= 1228; t += 16) off[n++] = t / 4;
    for (int t = 1236; t <= 1364; t += 8) off[n++] = t / 4;
    static const int gfx[] = {4, 12, 20, 28, 116, 124, 132, 140, 220, 348, 476, 604, 732, 860, 988, 1116, 1236, 1244, 1364};
    static const int multi[] = {4,   12,  20,  28,  116, 124, 132, 140, 220, 228, 260, 292, 324,  348,  356,  388,  420,
                                452, 476, 484, 516, 548, 580, 604, 612, 644, 676, 708, 732, 740,  772,  804,  836,  860,
                                868, 900, 932, 964, 988, 996, 1028, 1060, 1092, 1116, 1124, 1156, 1188, 1220, 1236, 1244, 1364};
    int text[91], m = 0;
    for (int t = 4; t <= 228; t += 8) text[m++] = t / 4;
    text[m++] = 244 / 4;
    for (int t = 268; t <= 1180; t += 24) text[m++] = t / 4;
    for (int t = 1196; t <= 1364; t += 8) text[m++] = t / 4;
    int g[19], mu[51];
    for (int i = 0; i < 19; i++) g[i] = gfx[i] / 4;
    for (int i = 0; i < 51; i++) mu[i] = multi[i] / 4;
    fill(0, off, n); fill(1, g, 19); fill(2, mu, 51); fill(3, text, m);
  }
  void fill(int k, const int* s, int n) {
    for (int p = 0; p < LINE_TICKS; p++) {
      // a janela precisa estar pelo menos 4 pontos depois do pedido (tempo de o chip registrá-lo)
      int best = 1 << 20;
      for (int i = 0; i < n; i++) {
        for (int rep = 0; rep < 2; rep++) {
          const int d = s[i] + rep * LINE_TICKS - p;
          if (d >= 4 && d < best) best = d;
        }
      }
      wait[k][p] = static_cast<uint8_t>((best * 2 + 2) / 3);  // pontos → ciclos de CPU (arredonda para cima)
    }
  }
};
const SlotTables SLOTS;
}  // namespace

void TMS9918::reset() {
  std::memset(vram, 0, sizeof vram);
  std::memset(regs, 0, sizeof regs);
  std::fill(fb, fb + W * H, TMS_PALETTE[1]);
  status = 0; addr = 0; latchByte = 0; latch = false; data = 0;
  pending = 0; pendingAt = 0; line = 0; lineCycle = 0;
}

// ------------------------------------------------------------ acesso à VRAM com as janelas da linha
int64_t TMS9918::accessDelay(int64_t now) const {
  int64_t rel = now - lineCycle;
  if (rel < 0) rel = 0;
  int64_t ln = line + rel / 228;
  rel %= 228;
  int k = 0;
  if (ln < H && (regs[1] & 0x40)) k = (regs[1] & 0x10) ? 3 : (regs[1] & 0x08) ? 2 : 1;
  return SLOTS.wait[k][(rel * 3) / 2];
}

void TMS9918::flush(int64_t now) {
  if (!pending || now < pendingAt) return;
  if (pending == 1) vram[addr] = static_cast<uint8_t>(data);
  else data = vram[addr];
  addr = (addr + 1) & 0x3FFF;
  pending = 0;
}

// pede um acesso; se o anterior ainda não foi feito, ele é substituído (o dado anterior se perde)
void TMS9918::schedule(int kind, int64_t now) {
  if (pending) { pending = kind; tooFast++; return; }
  pending = kind;
  pendingAt = now + accessDelay(now);
}

int TMS9918::readData(int64_t now) {
  flush(now);
  latch = false;
  const int v = data;
  schedule(2, now);
  return v;
}

void TMS9918::writeData(int v, int64_t now) {
  flush(now);
  latch = false;
  data = v & 0xFF;
  schedule(1, now);
}

int TMS9918::readStatus() {
  const int v = status;
  status &= 0x1F;
  latch = false;
  return v;
}

void TMS9918::writeControl(int v, int64_t now) {
  v &= 0xFF;
  if (!latch) {
    latch = true;
    latchByte = v;
    return;
  }
  latch = false;
  if (v & 0x80) {
    static const uint8_t MASK[8] = {0x03, 0xFF, 0x0F, 0xFF, 0x07, 0x7F, 0x07, 0xFF};
    regs[v & 7] = static_cast<uint8_t>(latchByte & MASK[v & 7]);
    return;
  }
  flush(now);
  addr = ((v & 0x3F) << 8) | latchByte;
  if (!(v & 0x40)) schedule(2, now);  // leitura: o chip já busca o primeiro byte
}

// ------------------------------------------------------------ linhas
void TMS9918::startLine(int ln, int64_t cycle) {
  flush(cycle);
  line = ln;
  lineCycle = cycle;
  if (ln < H) render(ln);
  else if (ln == H) status |= 0x80;
}

void TMS9918::render(int y) {
  uint8_t px[W];  // índices de cor; 0 = transparente (mostra a cor de fundo)
  const int r0 = regs[0], r1 = regs[1];
  const int back = regs[7] & 15;
  uint32_t* out = fb + y * W;
  if (!(r1 & 0x40)) {  // tela desligada: só a cor de fundo
    std::fill(out, out + W, TMS_PALETTE[back]);
    return;
  }
  const bool m1 = r1 & 0x10, m2 = r1 & 0x08, m3 = r0 & 0x02;
  const int nameBase = (regs[2] & 0x0F) << 10;
  const int row = y & 7, third = y >> 6;
  if (m1) {
    // modo texto: 40 colunas de 6 pontos com 8 pontos de borda dos dois lados; sem sprites
    const int fg = regs[7] >> 4, bg = regs[7] & 15;
    std::memset(px, 0, 8);
    std::memset(px + 248, 0, 8);
    const int nt = nameBase + (y >> 3) * 40;
    for (int col = 0; col < 40; col++) {
      uint8_t* p = px + 8 + col * 6;
      if (m2) {  // M1+M2 (não documentado): 4 pontos de frente e 2 de fundo em toda célula
        p[0] = p[1] = p[2] = p[3] = static_cast<uint8_t>(fg);
        p[4] = p[5] = static_cast<uint8_t>(bg);
        continue;
      }
      const int name = vram[(nt + col) & 0x3FFF];
      int pa;
      if (m3)  // M1+M3 (não documentado): texto com a tabela de padrões dividida em terços, como no Graphics II
        pa = ((regs[4] & 4) << 11) | ((((third << 8) | name) << 3 | row) & (((regs[4] & 3) << 11) | 0x7FF));
      else pa = ((regs[4] & 7) << 11) + (name << 3) + row;
      const int bits = vram[pa & 0x3FFF];
      for (int k = 0; k < 6; k++) p[k] = static_cast<uint8_t>((bits & (0x80 >> k)) ? fg : bg);
    }
  } else if (m2) {
    // multicolor: cada byte do padrão dá dois blocos de 4×4 pontos
    const int nt = nameBase + ((y >> 3) << 5);
    for (int col = 0; col < 32; col++) {
      const int name = vram[nt + col];
      int pa;
      if (m3) pa = ((regs[4] & 4) << 11) | ((((third << 8) | name) << 3) & (((regs[4] & 3) << 11) | 0x7FF));
      else pa = ((regs[4] & 7) << 11) + (name << 3);
      const int c = vram[(pa + ((y >> 2) & 7)) & 0x3FFF];
      uint8_t* p = px + col * 8;
      p[0] = p[1] = p[2] = p[3] = static_cast<uint8_t>(c >> 4);
      p[4] = p[5] = p[6] = p[7] = static_cast<uint8_t>(c & 15);
    }
  } else {
    // Graphics I (cores por grupo de 8 caracteres) e Graphics II (padrão e cor por linha, tela em terços;
    // os bits baixos dos registradores 3 e 4 funcionam como máscaras de endereço)
    const int nt = nameBase + ((y >> 3) << 5);
    for (int col = 0; col < 32; col++) {
      const int name = vram[nt + col];
      int bits, colors;
      if (m3) {
        const int idx = (((third << 8) | name) << 3) | row;
        bits = vram[((regs[4] & 4) << 11) | (idx & (((regs[4] & 3) << 11) | 0x7FF))];
        colors = vram[((regs[3] & 0x80) << 6) | (idx & (((regs[3] & 0x7F) << 6) | 0x3F))];
      } else {
        bits = vram[((regs[4] & 7) << 11) + (name << 3) + row];
        colors = vram[(regs[3] << 6) + (name >> 3)];
      }
      const uint8_t fg = static_cast<uint8_t>(colors >> 4), bg = static_cast<uint8_t>(colors & 15);
      uint8_t* p = px + col * 8;
      for (int k = 0; k < 8; k++) p[k] = (bits & (0x80 >> k)) ? fg : bg;
    }
  }
  if (!m1) spritesLine(y, px);
  for (int x = 0; x < W; x++) out[x] = TMS_PALETTE[px[x] ? px[x] : back];
}

void TMS9918::spritesLine(int y, uint8_t* px) {
  const int r1 = regs[1];
  const int size = (r1 & 2) ? 16 : 8, mag = r1 & 1, msize = size << mag;
  const int sat = (regs[5] & 0x7F) << 7, spg = (regs[6] & 7) << 11;
  int list[4], rows[4], n = 0, fifth = -1, i = 0;
  for (; i < 32; i++) {
    const int sy = vram[sat + i * 4];
    if (sy == 208) break;
    const int r = (y - 1 - sy) & 0xFF;  // o sprite aparece a partir da linha Y+1
    if (r >= msize) continue;
    if (n == 4) { fifth = i; break; }
    list[n] = i; rows[n] = r >> mag; n++;
  }
  // sinal e número do 5º sprite (só enquanto o sinal de quadro está apagado); sem 5º sprite, o campo
  // guarda o último sprite examinado
  if (fifth >= 0 && !(status & 0xC0)) status = 0x40 | (status & 0x20) | fifth;
  if (!(status & 0x40)) status = (status & 0xA0) | std::min(i, 31);

  uint8_t drawn[W + 32];  // 1 = algum sprite tem ponto aqui (colisão); 2 = já foi pintado por sprite visível
  std::memset(drawn, 0, sizeof drawn);
  for (int k = 0; k < n; k++) {  // do mais prioritário (menor número) para o menos
    const int s = sat + list[k] * 4;
    int x = vram[s + 1];
    const int name = vram[s + 2] & (size == 16 ? 0xFC : 0xFF);
    const int attr = vram[s + 3];
    if (attr & 0x80) x -= 32;
    const int color = attr & 15;
    const int pa = spg + (name << 3) + rows[k];
    const int pat = (vram[pa] << 8) | (size == 16 ? vram[(pa + 16) & 0x3FFF] : 0);
    for (int p = 0; p < msize; p++) {
      const int sx = x + p;
      if (sx < 0 || sx >= W) continue;
      if (!(pat & (0x8000 >> (p >> mag)))) continue;
      if (drawn[sx]) status |= 0x20;  // colisão: conta mesmo com cor 0, só dentro da tela
      if (color && drawn[sx] != 2) { px[sx] = static_cast<uint8_t>(color); drawn[sx] = 2; }
      else if (!drawn[sx]) drawn[sx] = 1;
    }
  }
}

// ------------------------------------------------------------ save state
void TMS9918::sync(StateIO& io) {
  io.raw(vram, sizeof vram);
  io.raw(regs, sizeof regs);
  for (int* p : {&status, &addr, &latchByte, &data, &pending, &line}) io.sync(*p);
  io.sync(latch);
  io.sync(pendingAt); io.sync(lineCycle);
  if (io.reading()) {
    static const uint8_t MASK[8] = {0x03, 0xFF, 0x0F, 0xFF, 0x07, 0x7F, 0x07, 0xFF};
    for (int k = 0; k < 8; k++) regs[k] &= MASK[k];
    status &= 0xFF; addr &= 0x3FFF; latchByte &= 0xFF; data &= 0xFF;
    clampTo(pending, 0, 2); clampTo(line, 0, 261);
    clampTo<int64_t>(lineCycle, 0, int64_t(1) << 60);
    if (pending) clampTo<int64_t>(pendingAt, lineCycle - 512, lineCycle + 512);  // sem acesso pendente, não é usado
  }
}

}  // namespace indigo::tms
