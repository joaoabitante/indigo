#include "coleco.h"

#include <algorithm>
#include <stdexcept>

namespace indigo::tms {

namespace {
const char STATE_MAGIC[4] = {'I', 'C', 'V', '1'};
constexpr size_t MAX_ROM = 1 << 20;  // MegaCart: até 1 MB

bool endsWith(const std::string& s, const char* suf) {
  const std::string t(suf);
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}
}  // namespace

bool isColecoRom(const std::vector<uint8_t>& rom, const std::string& n) {
  (void)rom;
  return endsWith(n, ".col") || endsWith(n, ".cv");
}

Coleco::Coleco(const std::vector<uint8_t>& b, const std::vector<uint8_t>& data)
    : bios(0x2000, 0xFF), ram(0x400), sgmRam(0x8000) {
  if (b.size() < 0x2000)
    throw std::runtime_error("Abra o arquivo da BIOS do ColecoVision (coleco.rom, 8 KB) como um jogo");
  std::copy(b.begin(), b.begin() + 0x2000, bios.begin());
  if (data.empty() || data.size() > MAX_ROM) throw std::runtime_error("ROM de ColecoVision inválida.");
  mega = data.size() > 0x8000;
  if (mega) {
    // tamanho em potência de 2 (bancos de 16 KB); o banco fixo é o último
    size_t size = 0x4000;
    while (size < data.size()) size <<= 1;
    rom.assign(size, 0xFF);
    std::copy(data.begin(), data.end(), rom.begin() + (size - data.size()));
    banks = static_cast<int>(size >> 14);
  } else {
    rom.assign(0x8000, 0xFF);
    std::copy(data.begin(), data.end(), rom.begin());
    banks = 2;
  }
  reset();
}

void Coleco::reset() {
  resetBase();
  std::fill(ram.begin(), ram.end(), 0);
  std::fill(sgmRam.begin(), sgmRam.end(), 0);
  bank = 0;
  joyMode = true;
  sgmUpper = sgmLower = false;
  nmiLine = false;
}

// o sinal do vídeo vai ao NMI: a CPU só é interrompida na descida do sinal (borda)
void Coleco::updateIrq() {
  const bool l = vdp.irq();
  if (l && !nmiLine) cpu.nmiPending = true;
  nmiLine = l;
}

// ------------------------------------------------------------ memória
int Coleco::read(int a) {
  if (a < 0x2000) return sgmLower ? sgmRam[a] : bios[a];
  if (a < 0x8000) {
    if (sgmUpper) return sgmRam[a];
    return a >= 0x6000 ? ram[a & 0x3FF] : 0xFF;
  }
  if (!mega) return rom[a - 0x8000];
  if (a < 0xC000) return rom[(static_cast<size_t>(banks - 1) << 14) | (a & 0x3FFF)];
  const int v = rom[(static_cast<size_t>(bank) << 14) | (a & 0x3FFF)];
  if (a >= 0xFFC0) bank = a & (banks - 1) & 0x3F;
  return v;
}

void Coleco::write(int a, int v) {
  if (a < 0x2000) { if (sgmLower) sgmRam[a] = static_cast<uint8_t>(v); return; }
  if (a < 0x8000) {
    if (sgmUpper) sgmRam[a] = static_cast<uint8_t>(v);
    else if (a >= 0x6000) ram[a & 0x3FF] = static_cast<uint8_t>(v);
    return;
  }
  if (mega && a >= 0xFFC0) bank = a & (banks - 1) & 0x3F;
}

// ------------------------------------------------------------ portas
// controle: no modo joystick, bits 0–3 = cima, direita, baixo, esquerda e bit 6 = botão esquerdo; no modo
// teclado, bits 0–3 = código da tecla e bit 6 = botão direito (tudo ativo em 0)
int Coleco::pad(int which) const {
  if (which != 0) return 0x7F;
  const uint32_t k = keys;
  int v = 0x7F;
  if (joyMode) {
    if (k & KEY_UP) v &= ~0x01;
    if (k & KEY_RIGHT) v &= ~0x02;
    if (k & KEY_DOWN) v &= ~0x04;
    if (k & KEY_LEFT) v &= ~0x08;
    if (k & KEY_A) v &= ~0x40;
  } else {
    // códigos do teclado: 1=0D 2=07 3=0C 4=02 *=09 #=06 (nenhuma = 0F)
    int code = 0x0F;
    if (k & KEY_START) code = 0x0D;
    else if (k & KEY_SELECT) code = 0x07;
    else if (k & KEY_X) code = 0x0C;
    else if (k & KEY_Y) code = 0x02;
    else if (k & KEY_L) code = 0x09;
    else if (k & KEY_R) code = 0x06;
    v = (v & 0xF0) | code;
    if (k & KEY_B) v &= ~0x40;
  }
  return v;
}

int Coleco::in(int port) {
  const int p = port & 0xFF;
  switch (p & 0xE0) {
    case 0xA0:
      if (p & 1) { const int v = vdp.readStatus(); updateIrq(); return v; }
      return vdp.readData(now());
    case 0xE0: return pad((p >> 1) & 1);
    case 0x40:
      if (p == 0x52) return snd.ayRead();
      return 0xFF;
    default: return 0xFF;
  }
}

void Coleco::out(int port, int v) {
  const int p = port & 0xFF;
  switch (p & 0xE0) {
    case 0x80: joyMode = false; return;
    case 0xC0: joyMode = true; return;
    case 0xA0:
      if (p & 1) { vdp.writeControl(v, now()); updateIrq(); }
      else vdp.writeData(v, now());
      return;
    case 0xE0: snd.run(now()); snd.snWrite(v); return;
    case 0x40:
      if (p == 0x50) snd.ayAddress(v);
      else if (p == 0x51) { snd.run(now()); snd.ayWrite(v); }
      else if (p == 0x53) sgmUpper = v & 1;
      return;
    case 0x60:
      if (p == 0x7F) sgmLower = !(v & 2);
      return;
    default: return;
  }
}

// ------------------------------------------------------------ save state
void Coleco::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  if (io.reading() && !std::equal(magic, magic + 4, STATE_MAGIC))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  int nb = banks;
  io.sync(nb);
  if (io.reading() && nb != banks) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  syncBase(io);
  io.bytes(ram); io.bytes(sgmRam);
  io.sync(bank); io.sync(joyMode); io.sync(sgmUpper); io.sync(sgmLower); io.sync(nmiLine);
  if (io.reading()) bank &= banks - 1;
}

std::vector<uint8_t> Coleco::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void Coleco::loadState(const std::vector<uint8_t>& s) {
  const std::vector<uint8_t> backup = saveState();
  try {
    StateIO io(s);
    sync(io);
  } catch (...) {
    StateIO io(backup);
    sync(io);
    throw;
  }
}

// ------------------------------------------------------------ adaptador
ColecoCore::ColecoCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save)
    : m_(std::make_unique<Coleco>(firmware("coleco"), rom)) {
  (void)save;
}

}  // namespace indigo::tms
