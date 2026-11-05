// Cartucho do DS (leitura por comandos depois da partida direta), save por SPI (EEPROM, FLASH, FRAM) com o tipo
// descoberto pelo jeito que o jogo conversa com o chip, SPI do ARM7 (energia, firmware gerado, tela de toque) e o
// relógio (RTC). Protocolos conforme o GBATEK.
#include <algorithm>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "nds.h"

namespace indigo::nds {

uint16_t crc16(uint16_t crc, const uint8_t* p, size_t n);

// ================================================================== cartucho
void Cart::load(const std::vector<uint8_t>& data) {
  rom = data;
  uint32_t sz = 0x20000;
  while (sz < rom.size() && sz < 0x80000000u) sz <<= 1;
  romMask = sz - 1;
  const uint32_t mb = std::max<uint32_t>(sz >> 20, 1);
  chipId = 0xC2u | ((mb - 1) << 8);
  save.clear();
  addrBytes = 0;
  dirty = false;
  // cartuchos com infravermelho, todos com FLASH de 512 KB: HeartGold/SoulSilver (IPK/IPG), Black/White (IRB/IRA)
  // e Black 2/White 2 (IRE/IRD). A letra final é a região.
  ir = false;
  if (rom.size() > 0x0F) {
    static const char* IR_GAMES[] = {"IPK", "IPG", "IRA", "IRB", "IRD", "IRE"};
    for (const char* g : IR_GAMES) if (std::memcmp(&rom[0x0C], g, 3) == 0) ir = true;
  }
  irPos = irCmd = 0;
  if (ir) resizeFor(3);
}

void Cart::resizeFor(int ab) {
  addrBytes = ab;
  size_t want = ab == 1 ? 512 : ab == 2 ? 0x10000 : 0x80000;
  if (save.size() < want) save.resize(want, 0xFF);
}

void Cart::setSave(const std::vector<uint8_t>& data) {
  if (data.empty()) return;
  save = data;
  if (save.size() <= 512) { save.resize(512, 0xFF); addrBytes = 1; }
  else if (save.size() <= 0x10000) addrBytes = 2;
  else addrBytes = 3;
  if (ir && save.size() < 0x80000) { save.resize(0x80000, 0xFF); addrBytes = 3; }
  // tamanho potência de 2 (para o espelhamento dos endereços)
  size_t p = 512;
  while (p < save.size()) p <<= 1;
  save.resize(p, 0xFF);
  dirty = false;
}

void Cart::writeRomctrl(uint32_t v) {
  romctrl = (romctrl & 0x00800000u) | (v & ~0x00800000u);
  if (!(v & 0x80000000u)) return;
  const int bs = (v >> 24) & 7;
  const uint32_t size = bs == 0 ? 0 : bs == 7 ? 4 : (0x100u << bs);
  xferPos = 0;
  xferLeft = size;
  switch (cmd[0]) {
    case 0xB7: {
      uint32_t a = (uint32_t(cmd[1]) << 24) | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
      a &= romMask;
      if (a < 0x8000) a = 0x8000 + (a & 0x1FF);  // a área segura não é lida depois da partida
      xferAddr = a;
      xferKind = 1;
      break;
    }
    case 0xB8: case 0x90: xferKind = 2; break;
    case 0x00: xferKind = 3; xferAddr = 0; break;
    default: xferKind = 4; break;
  }
  if (size == 0) {
    romctrl &= ~0x80800000u;
    s.cartXferDone();
    return;
  }
  romctrl |= 0x00800000u;
  s.cartDataReady();
}

uint32_t Cart::readData() {
  if (!xferLeft) return dataLatch;
  uint32_t w = 0xFFFFFFFFu;
  switch (xferKind) {
    case 1: {
      w = 0;
      for (int k = 0; k < 4; k++) {
        // a leitura anda dentro do bloco de 4 KB
        const uint32_t a = (xferAddr & ~0xFFFu) | ((xferAddr + xferPos + k) & 0xFFF);
        const uint32_t m = a & romMask;
        w |= uint32_t(m < rom.size() ? rom[m] : 0xFF) << (8 * k);
      }
      break;
    }
    case 2: w = chipId; break;
    case 3: {
      w = 0;
      for (int k = 0; k < 4; k++) {
        const uint32_t a = (xferPos + k) & 0xFFF;
        w |= uint32_t(a < rom.size() ? rom[a] : 0xFF) << (8 * k);
      }
      break;
    }
    default: break;
  }
  xferPos += 4;
  xferLeft -= 4;
  dataLatch = w;
  if (!xferLeft) {
    romctrl &= ~0x80800000u;
    s.cartXferDone();
  }
  return w;
}

void Cart::writeAuxSpiCnt(uint16_t v) {
  auxspicnt = v & 0xE043;
}

void Cart::writeAuxSpiData(uint8_t v) {
  if (!(auxspicnt & 0x2000)) { spiOut = 0xFF; return; }
  const bool hold = auxspicnt & 0x40;
  if (ir) {
    // chip de IR: o primeiro byte é o comando dele; 0x00 liga a FLASH até o fim da conversa, 0x08 é a identificação
    const int pos = irPos++;
    if (pos == 0) { irCmd = v; spiOut = 0xFF; }
    else if (irCmd == 0x00) spiOut = spiByte(v);
    else spiOut = irCmd == 0x08 ? 0xAA : 0x00;
    if (!hold) {
      irPos = 0;
      if (irCmd == 0x00 && pos > 0) { spiPos = 0; spiCmd = 0; }
    }
    return;
  }
  spiOut = spiByte(v);
  if (!hold) {
    // fim da conversa: o chip solta a seleção
    if (addrBytes == 0) finishProbe();
    spiPos = 0;
    spiCmd = 0;
  }
}

// primeira conversa de leitura/escrita com o chip ainda desconhecido: o número de bytes de endereço é o que deixa
// o resto (os dados) com tamanho potência de 2
void Cart::finishProbe() {
  if (probe.size() < 2) { probe.clear(); return; }
  const int c = probe[0];
  const size_t L = probe.size() - 1;
  if (c == 0x03 || c == 0x0B || c == 0x02 || c == 0x0A) {
    auto pow2 = [](size_t x) { return x && !(x & (x - 1)); };
    int ab = 0;
    if (c == 0x0B && L > 4 && pow2(L - 4)) ab = 3;           // leitura rápida da FLASH (byte extra)
    else if (L > 2 && pow2(L - 2)) ab = 2;
    else if (L > 3 && pow2(L - 3)) ab = 3;
    else if (L > 1 && pow2(L - 1)) ab = 1;
    else if (L > 3) ab = 2;
    if (ab) {
      resizeFor(ab);
      // refaz a conversa com o tipo conhecido (uma primeira escrita não se perde)
      const std::vector<uint8_t> p = std::move(probe);
      probe.clear();
      spiPos = 0;
      for (uint8_t b : p) spiByte(b);
      spiPos = 0;
      spiCmd = 0;
    }
  }
  probe.clear();
}

uint8_t Cart::spiByte(uint8_t v) {
  if (addrBytes == 0) {
    // tipo desconhecido: guarda a conversa e responde como chip apagado
    if (probe.size() < 0x100000) probe.push_back(v);
    if (spiPos == 0) { spiCmd = v; spiPos = 1; if (v == 0x06) status |= 2; if (v == 0x04) status &= ~2; return 0xFF; }
    spiPos++;
    if (spiCmd == 0x05) return status;
    return 0xFF;
  }
  const uint32_t mask = static_cast<uint32_t>(save.size() - 1);
  if (spiPos == 0) {
    spiCmd = v;
    spiPos = 1;
    spiAddr = 0;
    switch (v) {
      case 0x06: status |= 2; break;
      case 0x04: status &= ~2; break;
      default: break;
    }
    return 0xFF;
  }
  const int ab = addrBytes;
  switch (spiCmd) {
    case 0x05: spiPos++; return status;
    case 0x01: spiPos++; status = (status & 3) | (v & 0x0C); return 0xFF;
    case 0x9F: {  // identificação (FLASH)
      static const uint8_t ID[3] = {0x20, 0x40, 0x13};
      const int k = spiPos - 1;
      spiPos++;
      return (ab == 3 && k < 3) ? ID[k] : 0xFF;
    }
    case 0x03: case 0x0B: {
      const bool fast = spiCmd == 0x0B && ab == 3;
      if (spiPos <= ab) {
        spiAddr = (spiAddr << 8) | v;
        if (ab == 1 && spiCmd == 0x0B) spiAddr |= 0x100;
        spiPos++;
        return 0xFF;
      }
      if (fast && spiPos == ab + 1) { spiPos++; return 0xFF; }
      spiPos++;
      return save[spiAddr++ & mask];
    }
    case 0x02: case 0x0A: {
      if (spiPos <= ab) {
        spiAddr = (spiAddr << 8) | v;
        if (ab == 1 && spiCmd == 0x0A) spiAddr |= 0x100;
        spiPos++;
        return 0xFF;
      }
      spiPos++;
      if (status & 2) {
        save[spiAddr & mask] = v;
        spiAddr++;
        dirty = true;
      }
      return 0xFF;
    }
    case 0xD8: case 0xDB: {  // apagar setor (64 KB) / página (256 bytes) da FLASH
      if (spiPos <= ab) {
        spiAddr = (spiAddr << 8) | v;
        spiPos++;
        if (spiPos > ab && (status & 2)) {
          const uint32_t blk = spiCmd == 0xD8 ? 0x10000 : 0x100;
          const uint32_t start = spiAddr & ~(blk - 1) & mask;
          for (uint32_t k = 0; k < blk && start + k < save.size(); k++) save[start + k] = 0xFF;
          dirty = true;
        }
      }
      return 0xFF;
    }
    default: spiPos++; return 0xFF;
  }
}

void Cart::sync(StateIO& io) {
  io.sync(auxspicnt); io.sync(romctrl); io.raw(cmd, sizeof cmd);
  io.sync(xferAddr); io.sync(xferLeft); io.sync(xferPos); io.sync(xferKind); io.sync(dataLatch);
  uint32_t n = static_cast<uint32_t>(save.size());
  io.sync(n);
  if (io.reading()) {
    if (n > 0x800000 || (n & (n - 1))) throw std::runtime_error("Estado salvo inválido (save do cartucho).");
    save.resize(n);
  }
  if (n) io.raw(save.data(), n);
  io.sync(addrBytes); io.sync(spiOut); io.sync(csHold); io.sync(spiCmd); io.sync(spiPos); io.sync(spiAddr); io.sync(status);
  if (io.reading()) {
    if (addrBytes < 0 || addrBytes > 3 || (addrBytes && save.empty())) { addrBytes = 0; }
    if (xferLeft > 0x4000 || (xferLeft & 3)) xferLeft = 0;  // a transferência anda de 4 em 4 bytes
    probe.clear();
    irPos = irCmd = 0;
  }
}

// ================================================================== SPI do ARM7
void Spi::buildFirmware() {
  firmware.assign(0x40000, 0xFF);
  uint8_t* f = firmware.data();
  std::memset(f, 0, 0x200);
  f[0x1D] = 0xFF;                 // console: DS
  f[0x20] = 0xC0; f[0x21] = 0x7F; // dados de usuário em 0x7FC0 * 8 = 0x3FE00
  // configuração de rede: MAC fictício
  const uint8_t mac[6] = {0x00, 0x09, 0xBF, 0x12, 0x34, 0x56};
  std::memcpy(f + 0x36, mac, 6);
  f[0x2C] = 0x38; f[0x2D] = 0x01;
  const uint16_t wcrc = crc16(0, f + 0x2C, 0x138);
  f[0x2A] = uint8_t(wcrc); f[0x2B] = uint8_t(wcrc >> 8);
  for (int copy = 0; copy < 2; copy++) {
    uint8_t* u = f + 0x3FE00 + copy * 0x100;
    std::memset(u, 0, 0x100);
    u[0x00] = 5;
    u[0x02] = 11;       // cor favorita
    u[0x03] = 1; u[0x04] = 1;  // aniversário
    const char* name = "Indigo";
    int len = 0;
    for (; name[len]; len++) { u[0x06 + len * 2] = uint8_t(name[len]); u[0x07 + len * 2] = 0; }
    u[0x1A] = uint8_t(len);
    // calibração do toque: valor do ADC = pixel * 16
    auto p16 = [&](int o, int v) { u[o] = uint8_t(v); u[o + 1] = uint8_t(v >> 8); };
    p16(0x58, 0x20 * 16); p16(0x5A, 0x20 * 16); u[0x5C] = 0x20; u[0x5D] = 0x20;
    p16(0x5E, 0xE0 * 16); p16(0x60, 0xA0 * 16); u[0x62] = 0xE0; u[0x63] = 0xA0;
    p16(0x64, 0x0001 | (3 << 4));  // inglês, luz de fundo máxima
    u[0x66] = 26;                    // ano
    // 0x68: ajuste do relógio (0); 0x70: contador de atualização (a cópia 0 é a mais nova); 0x72: CRC de 0x00..0x6F.
    // Com o CRC no lugar errado o SDK descartava as duas cópias e zerava a calibração: o toque caía fora dos botões.
    p16(0x70, copy == 0 ? 1 : 0);
    const uint16_t crc = crc16(0xFFFF, u, 0x70);
    p16(0x72, crc);
  }
}

void Spi::writeCnt(uint16_t v) {
  const bool wasHold = cnt & 0x800;
  cnt = v & 0xCF03;
  (void)wasHold;
}

void Spi::writeData(uint8_t v) {
  if (!(cnt & 0x8000)) return;
  const int dev = (cnt >> 8) & 3;
  uint8_t out = 0;
  switch (dev) {
    case 0: {  // gerenciador de energia
      if (pmPos == 0) { pmIdx = v & 7; pmRead = v & 0x80; pmPos = 1; out = 0; }
      else {
        if (pmRead) out = pmReg[pmIdx];
        else pmReg[pmIdx] = v;
        pmPos++;
      }
      break;
    }
    case 1: {  // firmware (FLASH de 256 KB)
      if (fwPos == 0) {
        fwCmd = v; fwPos = 1; fwAddr = 0;
        if (v == 0x06) fwStatus |= 2;
        if (v == 0x04) fwStatus &= ~2;
        out = 0xFF;
      } else {
        switch (fwCmd) {
          case 0x03: case 0x0B:
            if (fwPos <= 3) { fwAddr = (fwAddr << 8) | v; out = 0xFF; }
            else if (fwCmd == 0x0B && fwPos == 4) out = 0xFF;
            else { out = firmware[fwAddr & 0x3FFFF]; fwAddr++; }
            break;
          case 0x05: out = fwStatus; break;
          case 0x9F: { static const uint8_t ID[3] = {0x20, 0x40, 0x12}; out = fwPos <= 3 ? ID[fwPos - 1] : 0xFF; break; }
          case 0x0A: case 0x02:
            if (fwPos <= 3) fwAddr = (fwAddr << 8) | v;
            else if (fwStatus & 2) { firmware[fwAddr & 0x3FFFF] = v; fwAddr++; }
            out = 0xFF;
            break;
          default: out = 0xFF; break;
        }
        fwPos++;
      }
      break;
    }
    case 2: {  // tela de toque (TSC2046): a resposta sai nos dois bytes seguintes ao comando
      if (tscPos == 1) out = uint8_t((tscVal >> 5) & 0x7F);
      else if (tscPos == 2) out = uint8_t((tscVal << 3) & 0xFF);
      else out = 0;
      tscPos++;
      if (v & 0x80) {
        tscCmd = v;
        tscPos = 1;
        const int chan = (v >> 4) & 7;
        switch (chan) {
          case 1: tscVal = s.touchDown ? uint16_t(s.touchY * 16 + 8) : 0xFFF; break;  // Y
          case 5: tscVal = s.touchDown ? uint16_t(s.touchX * 16 + 8) : 0; break;      // X
          case 3: tscVal = s.touchDown ? 0x200 : 0; break;                            // Z1
          case 4: tscVal = s.touchDown ? 0xE00 : 0xFFF; break;                        // Z2
          case 0: tscVal = 0x340; break;                                               // temperatura
          case 7: tscVal = 0x380; break;
          case 2: tscVal = 0x800; break;                                               // bateria
          default: tscVal = 0x800; break;                                              // microfone: silêncio
        }
        if (v & 0x08) tscVal &= 0xFF0;  // modo de 8 bits
      }
      break;
    }
    default: out = 0; break;
  }
  data = out;
  // seleção solta depois deste byte: fim da conversa
  if (!(cnt & 0x800)) { fwPos = 0; pmPos = 0; tscPos = tscPos > 2 ? 0 : tscPos; }
  if (cnt & 0x4000) s.irq7(1u << 23);
}

void Spi::sync(StateIO& io) {
  io.sync(cnt); io.sync(data); io.bytes(firmware);
  io.sync(fwCmd); io.sync(fwPos); io.sync(fwAddr); io.sync(fwStatus);
  io.sync(tscCmd); io.sync(tscVal); io.sync(tscPos);
  io.raw(pmReg, sizeof pmReg); io.sync(pmPos); io.sync(pmIdx); io.sync(pmRead);
  if (io.reading()) { pmIdx &= 7; }
}

// ================================================================== RTC
namespace {
uint8_t bcd(int v) { return uint8_t(((v / 10) << 4) | (v % 10)); }
uint8_t rev8(uint8_t b) {
  b = uint8_t((b & 0xF0) >> 4 | (b & 0x0F) << 4);
  b = uint8_t((b & 0xCC) >> 2 | (b & 0x33) << 2);
  return uint8_t((b & 0xAA) >> 1 | (b & 0x55) << 1);
}
}  // namespace

void Rtc::fillTime(bool date) {
  std::time_t t = std::time(nullptr) + static_cast<std::time_t>(offsetSec);
  std::tm tmv{};
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  int i = 0;
  if (date) {
    data[i++] = bcd(tmv.tm_year % 100);
    data[i++] = bcd(tmv.tm_mon + 1);
    data[i++] = bcd(tmv.tm_mday);
    data[i++] = bcd(tmv.tm_wday);
  }
  data[i++] = uint8_t(bcd(tmv.tm_hour) | (tmv.tm_hour >= 12 ? 0x40 : 0));
  data[i++] = bcd(tmv.tm_min);
  data[i++] = bcd(tmv.tm_sec);
  len = i;
}

void Rtc::command() {
  uint8_t g = rev8(cmd);  // ordem do GBATEK: bits 0-3 código fixo 0110, 4-6 comando, 7 leitura
  if ((g & 0xF) != 6) g = cmd;
  const int c = (g >> 4) & 7;
  const bool rd = g & 0x80;
  std::memset(data, 0, sizeof data);
  len = 1;
  switch (c) {
    case 0: data[0] = stat1; break;
    case 4: data[0] = stat2; break;
    case 2: fillTime(true); break;
    case 6: fillTime(false); break;
    case 1: case 5: len = 3; break;
    default: len = 1; break;
  }
  if (!rd) std::memset(data, 0, sizeof data);
}

void Rtc::write(uint16_t v) {
  const bool csOld = pins & 4, sckOld = pins & 2;
  pins = uint8_t(v);
  const bool cs = v & 4, sck = v & 2;
  if (!cs) { haveCmd = false; bits = 0; byteIdx = 0; cur = 0; return; }
  if (!csOld) { haveCmd = false; bits = 0; byteIdx = 0; cur = 0; }
  uint8_t g = rev8(cmd);
  if ((g & 0xF) != 6) g = cmd;
  const bool rd = g & 0x80;
  // na leitura o chip põe o bit na saída na borda de descida: o SDK lê com o SCK ainda baixo, antes de subir.
  // Antes o bit só saía na subida e o jogo recebia sempre o anterior (hora errada no HeartGold).
  if (sckOld && !sck) {
    if (haveCmd && rd) outBit_ = byteIdx < 8 ? ((data[byteIdx] >> bits) & 1) : 0;
    return;
  }
  if (sckOld || !sck) return;  // borda de subida do relógio
  if (!haveCmd) {
    cur = uint8_t((cur << 1) | (v & 1));
    if (++bits == 8) { cmd = cur; haveCmd = true; command(); bits = 0; byteIdx = 0; cur = 0; }
    return;
  }
  if (rd) {
    outBit_ = byteIdx < 8 ? ((data[byteIdx] >> bits) & 1) : 0;
  } else {
    if (byteIdx < 8) data[byteIdx] = uint8_t(data[byteIdx] | ((v & 1) << bits));
  }
  if (++bits == 8) {
    bits = 0;
    if (!rd) {
      const int c = (g >> 4) & 7;
      if (c == 0 && byteIdx == 0) {
        const uint8_t b = data[0];
        if (b & 1) { stat1 = 0x02; stat2 = 0; offsetSec = 0; }
        stat1 = uint8_t((stat1 & ~0x0E) | (b & 0x0E));
      } else if (c == 4 && byteIdx == 0) stat2 = data[0];
    }
    byteIdx++;
  }
}

uint16_t Rtc::read() const {
  // com o pino de dados como entrada (bit 4 = 0), o bit 0 traz o dado do relógio
  if (!(pins & 0x10)) return uint16_t((pins & 0xFE) | (outBit_ ? 1 : 0));
  return pins;
}

void Rtc::sync(StateIO& io) {
  io.sync(pins); io.sync(bits); io.sync(byteIdx); io.sync(cur); io.sync(cmd); io.sync(haveCmd);
  io.raw(data, sizeof data); io.sync(len); io.sync(stat1); io.sync(stat2); io.sync(offsetSec); io.sync(outBit_);
  if (io.reading()) { bits &= 7; if (byteIdx < 0 || byteIdx > 8) byteIdx = 8; if (len < 0 || len > 8) len = 1; }
}

}  // namespace indigo::nds
