// Máquina NES: mapa de memória da CPU, relógio mestre (cada ciclo de CPU = 12 do mestre, dividido em começo e
// fim como no chip, com o PPU andando 3 pontos), DMA de sprites ($4014) e do DMC, controles e save states.
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "nes.h"

namespace indigo::nes {

NES::NES(Cart c) : cart(std::move(c)), ppu(*this), apu(*this) {
  mapper = Mapper::create(cart);
  if (!mapper) throw std::runtime_error("Este jogo de NES usa o mapeador " + std::to_string(cart.mapper) + ", que o Índigo ainda não tem.");
  mapper->nes = this;
  mapper->ciram = ppu.vram();
  cpu.bus = this;
  reset(true);
}

void NES::reset(bool power) {
  if (power) {
    std::memset(ram, 0, sizeof ram);
    master = ppuMaster = 0;
    cycles = 0;
    ppuDots = 0;
    openBus = 0;
    cpu.powerOn();
    mapper->reset();
  }
  oamDma = false;
  strobe = false;
  shift[0] = shift[1] = 0;
  ppu.reset(power);
  apu.reset(power);
  cpu.reset();
}

void NES::setKeys(uint32_t m) {
  // A, B, Select, Start, Cima, Baixo, Esquerda, Direita
  uint8_t b = 0;
  if (m & KEY_A) b |= 0x01;
  if (m & KEY_B) b |= 0x02;
  if (m & KEY_SELECT) b |= 0x04;
  if (m & KEY_START) b |= 0x08;
  if (m & KEY_UP) b |= 0x10;
  if (m & KEY_DOWN) b |= 0x20;
  if (m & KEY_LEFT) b |= 0x40;
  if (m & KEY_RIGHT) b |= 0x80;
  // cima+baixo ou esquerda+direita juntos travam alguns jogos (o controle de verdade não deixa)
  if ((b & 0x30) == 0x30) b &= ~0x30;
  if ((b & 0xC0) == 0xC0) b &= ~0xC0;
  pad[0] = b;
}

void NES::runFrame() {
  ppu.frameDone = false;
  // limite de segurança: um quadro tem ~29781 ciclos
  for (int guard = 0; !ppu.frameDone && guard < 200000; guard++) cpu.step();
}

// ---------------------------------------------------------------- relógio
void NES::runPpu(uint64_t to) {
  while (ppuMaster + 4 <= to) {
    ppu.step();
    ppuMaster += 4;
    ppuDots++;
  }
}

void NES::startCycle(bool forRead) {
  master += forRead ? 5 : 7;
  cycles++;
  runPpu(master - 1);
  mapper->cpuCycle();
}

void NES::endCycle(bool forRead) {
  master += forRead ? 7 : 5;
  runPpu(master - 1);
  // a CPU vê a linha de IRQ do APU como estava antes do passo deste ciclo
  cpu.endCycle(ppu.nmiLine(), apu.irqLine() || mapper->irq);
  apu.clock();
}

// ---------------------------------------------------------------- DMA (a CPU para nos ciclos de leitura)
void NES::dma(uint16_t readAddr) {
  const bool sprites = oamDma;
  oamDma = false;
  // leituras repetidas do controle em ciclos seguidos contam como uma só (o sinal de leitura não sobe entre elas)
  const bool joypad = readAddr == 0x4016 || readAddr == 0x4017;
  bool first = true;
  auto dummy = [&] {
    startCycle(true);
    if (!joypad || first) memRead(readAddr);
    first = false;
    endCycle(true);
  };
  auto dmcFetch = [&] {
    startCycle(true);
    const uint8_t v = memRead(apu.dmcAddress());
    endCycle(true);
    apu.dmcFetched(v);
  };
  dummy();  // ciclo de parada
  if (!sprites) {
    if (apu.dmcWantsByte()) {
      dummy();
      if (!(cycles & 1)) dummy();  // espera um ciclo de leitura ("get")
      dmcFetch();
    }
    return;
  }
  if (cycles & 1) dummy();  // alinhamento
  for (int i = 0; i < 256; i++) {
    startCycle(true);
    const uint8_t v = memRead(static_cast<uint16_t>((oamPage << 8) | i));
    endCycle(true);
    startCycle(false);
    ppu.writeReg(4, v);
    endCycle(false);
    if (apu.dmcWantsByte()) { dummy(); dmcFetch(); }
  }
}

uint8_t NES::read(uint16_t a) {
  if (oamDma || apu.dmcWantsByte()) dma(a);
  startCycle(true);
  const uint8_t v = memRead(a);
  endCycle(true);
  return v;
}

void NES::write(uint16_t a, uint8_t v) {
  startCycle(false);
  memWrite(a, v);
  endCycle(false);
}

// ---------------------------------------------------------------- mapa de memória
uint8_t NES::memRead(uint16_t a) {
  uint8_t v;
  if (a < 0x2000) {
    v = ram[a & 0x7FF];
  } else if (a < 0x4000) {
    v = ppu.readReg(a & 7);
    if (ppu.suppressNmi) { cpu.needNmi = false; ppu.suppressNmi = false; }
  } else if (a == 0x4015) {
    return static_cast<uint8_t>(apu.readStatus() | (openBus & 0x20));  // não muda o barramento aberto
  } else if (a == 0x4016 || a == 0x4017) {
    const int k = a & 1;
    if (strobe) shift[k] = pad[k];
    v = static_cast<uint8_t>((shift[k] & 1) | (openBus & 0xE0));
    if (!strobe) shift[k] = static_cast<uint8_t>((shift[k] >> 1) | 0x80);
  } else if (a >= 0x4020 && mapper->customCpu) {
    v = mapper->cpuRead(a, openBus);
  } else if (a < 0x6000) {
    v = openBus;
  } else if (a < 0x8000) {
    if (mapper->lowRom >= 0) v = cart.prg[mapper->lowRom + (a & 0x1FFF)];  // ROM em $6000 (FME-7)
    else v = mapper->ramEnabled ? cart.prgRam[a & 0x1FFF] : openBus;
  } else {
    v = mapper->prgRead(a);
  }
  openBus = v;
  return v;
}

uint8_t NES::peek(uint16_t a) {
  if (a < 0x2000) return ram[a & 0x7FF];
  if (a >= 0x6000 && mapper->customCpu) return mapper->cpuPeek(a);
  if (a >= 0x6000 && a < 0x8000) return cart.prgRam[a & 0x1FFF];
  if (a >= 0x8000) return mapper->prgRead(a);
  return 0;
}

void NES::memWrite(uint16_t a, uint8_t v) {
  openBus = v;
  if (a < 0x2000) { ram[a & 0x7FF] = v; return; }
  if (a < 0x4000) { ppu.writeReg(a & 7, v); mapper->ppuRegWrite(a & 7, v); return; }
  if (a < 0x4020) {
    if (a == 0x4014) { oamPage = v; oamDma = true; }
    else if (a == 0x4016) {
      strobe = v & 1;
      if (strobe) { shift[0] = pad[0]; shift[1] = pad[1]; }
    } else if (a <= 0x4017) apu.write(a, v);
    return;
  }
  if (mapper->customCpu) { mapper->writeReg(a, v); return; }
  if (a >= 0x6000 && a < 0x8000 && mapper->ramWritable && mapper->lowRom < 0) {
    uint8_t& r = cart.prgRam[a & 0x1FFF];
    if (r != v) { r = v; if (cart.battery) dirty = true; }
  }
  mapper->writeReg(a, v);
}

// ---------------------------------------------------------------- estados
void NES::sync(StateIO& io) {
  cpu.sync(io);
  ppu.sync(io);
  apu.sync(io);
  mapper->syncRegs(io);
  io.raw(ram, sizeof ram);
  io.bytes(cart.prgRam);
  if (cart.chrRam) io.bytes(cart.chr);
  io.sync(master); io.sync(ppuMaster); io.sync(cycles); io.sync(ppuDots);
  io.sync(openBus); io.sync(oamDma); io.sync(oamPage);
  io.raw(pad, sizeof pad); io.raw(shift, sizeof shift); io.sync(strobe);
  if (io.reading()) {
    // o PPU anda junto com a CPU: no máximo um ciclo de diferença
    if (ppuMaster > master || master - ppuMaster > 24) ppuMaster = master - (master % 4);
    mapper->update();
  }
}

std::vector<uint8_t> NES::saveState() {
  StateIO io;
  uint32_t magic = 0x3153454E;  // "NES1"
  io.sync(magic);
  sync(io);
  return io.data();
}

void NES::loadState(const std::vector<uint8_t>& s) {
  StateIO io(s);
  uint32_t magic = 0;
  io.sync(magic);
  if (magic != 0x3153454E) throw std::runtime_error("Este estado não é de NES.");
  // carrega numa cópia: se o arquivo estiver danificado, o jogo continua como estava
  const std::vector<uint8_t> backup = saveState();
  try {
    sync(io);
  } catch (...) {
    StateIO restore(backup);
    restore.sync(magic);
    sync(restore);
    throw;
  }
}

// ---------------------------------------------------------------- núcleo
namespace {
Cart parseOrThrow(const std::vector<uint8_t>& rom) {
  Cart c;
  std::string err;
  if (!parseRom(rom, c, err)) throw std::runtime_error(err);
  return c;
}
}  // namespace

NESCore::NESCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save)
    : m_(std::make_unique<NES>(parseOrThrow(rom))) {
  if (save) importSave(*save);
}

void NESCore::importSave(const std::vector<uint8_t>& b) {
  if (b.empty()) return;
  auto& r = m_->cart.prgRam;
  std::memcpy(r.data(), b.data(), std::min(r.size(), b.size()));
  m_->dirty = false;
}

}  // namespace indigo::nes
