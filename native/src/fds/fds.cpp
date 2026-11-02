// Famicom Disk System: imagem de disco, adaptador RAM (drive, temporizador e som) e o núcleo.
// A temporização do drive, o CRC dos blocos e o som seguem a wiki do NESdev e o Mesen 2 (GPL-3.0; ver
// native/TERCEIROS.md): um byte a cada 150 ciclos de CPU, 50000 ciclos de espera quando a cabeça volta ao começo,
// intervalo de 28300 bits antes do primeiro bloco e de 976 bits depois de cada bloco.
#include "fds.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace indigo::fds {

namespace {
constexpr size_t FDS_SIDE = 65500, QD_SIDE = 65536;
constexpr size_t GAP_START = 28300 / 8, GAP_BLOCK = 976 / 8;
constexpr uint32_t BYTE_DELAY = 149, REWIND_CYCLES = 50000;  // um byte a cada 150 ciclos (149 de espera)
constexpr int32_t INSERT_CYCLES = 1100000;  // ~0,6 s com o drive vazio quando o jogador troca o lado
const char HVC[] = "*NINTENDO-HVC*";

bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
bool hasHeader(const std::vector<uint8_t>& d) {
  return d.size() >= 16 && d[0] == 'F' && d[1] == 'D' && d[2] == 'S' && d[3] == 0x1A;
}
bool diskInfoAt(const std::vector<uint8_t>& d, size_t o) {
  return d.size() >= o + 15 && d[o] == 1 && std::memcmp(&d[o + 1], HVC, 14) == 0;
}
// .qd: pelo nome, ou um disco sem cabeçalho com tamanho múltiplo de 64 KB (e não de 65500)
bool qdFormat(const std::vector<uint8_t>& d, const std::string& n) {
  if (endsWith(n, ".qd")) return true;
  if (hasHeader(d) || endsWith(n, ".fds")) return false;
  return !d.empty() && d.size() % QD_SIDE == 0 && d.size() % FDS_SIDE != 0;
}
}  // namespace

bool isFdsDisk(const std::vector<uint8_t>& d, const std::string& n) {
  if (hasHeader(d)) return d[4] > 0 && diskInfoAt(d, 16);
  // sem cabeçalho: o bloco 1 (informações do disco) logo no começo é assinatura forte
  return diskInfoAt(d, 0);
}

bool isFdsBios(const std::vector<uint8_t>& d, const std::string& n) {
  if (d.size() != 0x2000) return false;
  if (n.find("disksys") != std::string::npos) return true;
  auto vec = [&](size_t o) { return d[o] | (d[o + 1] << 8); };
  if (vec(0x1FFA) < 0xE000 || vec(0x1FFC) < 0xE000 || vec(0x1FFE) < 0xE000) return false;
  for (size_t k = 0; k + 14 <= d.size(); k++)
    if (std::memcmp(&d[k], HVC, 14) == 0) return true;
  return false;
}

uint16_t blockCrc(const uint8_t* b, size_t n) {
  uint16_t c = 0;
  auto upd = [&](uint8_t v) {
    c ^= v;
    for (int k = 0; k < 8; k++) c = (c & 1) ? static_cast<uint16_t>((c >> 1) ^ 0x8408) : static_cast<uint16_t>(c >> 1);
  };
  upd(0x80);
  for (size_t k = 0; k < n; k++) upd(b[k]);
  return c;
}

bool rawSides(const std::vector<uint8_t>& file, bool qd, std::vector<std::vector<uint8_t>>& out, std::string& erro) {
  out.clear();
  const size_t cap = qd ? QD_SIDE : FDS_SIDE;
  size_t off = 0, count;
  if (hasHeader(file)) {
    off = 16;
    count = file[4];
    // cabeçalho que promete mais lados do que o arquivo tem: fica com os que existem
    count = std::min(count, (file.size() - 16 + cap - 1) / cap);
  } else {
    count = (file.size() + cap - 1) / cap;
  }
  if (count == 0 || count > 32) { erro = "Imagem de disco do Famicom Disk System inválida."; return false; }
  for (size_t s = 0; s < count; s++) {
    std::vector<uint8_t> in(cap, 0);
    const size_t from = off + s * cap;
    if (from < file.size()) std::memcpy(in.data(), &file[from], std::min(cap, file.size() - from));
    std::vector<uint8_t> raw(GAP_START, 0);
    uint32_t fileSize = 0;
    for (size_t j = 0; j < cap;) {
      size_t len;
      switch (in[j]) {
        case 1: len = 56; break;
        case 2: len = 2; break;
        case 3: len = 16; if (j + 15 < cap) fileSize = in[j + 13] | (in[j + 14] << 8); break;
        case 4: len = 1 + fileSize; break;
        default: len = 0; break;
      }
      if (len == 0) {
        // fim dos blocos (ou dados fora do padrão): o resto vai para o disco como está
        bool vazio = true;
        for (size_t k = j; k < cap && vazio; k++) vazio = in[k] == 0;
        if (!vazio) { raw.push_back(0x80); raw.insert(raw.end(), in.begin() + static_cast<long>(j), in.end()); }
        break;
      }
      const size_t total = len + (qd ? 2 : 0);
      if (j + total > cap) break;
      raw.push_back(0x80);
      raw.insert(raw.end(), in.begin() + static_cast<long>(j), in.begin() + static_cast<long>(j + total));
      if (!qd) {
        const uint16_t c = blockCrc(&in[j], len);
        raw.push_back(static_cast<uint8_t>(c));
        raw.push_back(static_cast<uint8_t>(c >> 8));
      }
      raw.insert(raw.end(), GAP_BLOCK, 0);
      j += total;
    }
    // o disco de verdade tem espaço de sobra depois do último bloco
    raw.resize(std::max(raw.size() + GAP_BLOCK * 8, cap + GAP_START), 0);
    out.push_back(std::move(raw));
  }
  return true;
}

// ================================================================ adaptador
FdsMapper::FdsMapper(nes::Cart& c, std::vector<std::vector<uint8_t>> lados, bool qd)
    : Mapper(c), sides_(std::move(lados)), qd_(qd) {
  orig_ = sides_;
  customCpu = true;
}

void FdsMapper::reset() {
  irqReload_ = irqCounter_ = 0;
  irqOn_ = irqRepeat_ = timerIrq_ = diskIrq_ = false;
  diskRegs_ = soundRegs_ = true;
  writeData_ = readData_ = ext_ = 0;
  ctrl_ = 0;
  motor_ = resetXfer_ = readMode_ = crcCtl_ = ready_ = diskIrqOn_ = false;
  endOfHead_ = true; scanning_ = gapEnded_ = prevCrc_ = transfer_ = badCrc_ = false;
  pos_ = delay_ = 0;
  crc_ = 0;
  vol_ = Env{}; mod_ = Env{};
  master_ = 0xE8;
  std::memset(wave_, 0, sizeof wave_);
  std::memset(modTable_, 0, sizeof modTable_);
  wavePos_ = modPos_ = masterVol_ = 0;
  modCounter_ = 0;
  waveAcc_ = modAcc_ = 0;
  modOut_ = 0;
  waveWrite_ = envHalt_ = waveHalt_ = false;
  modHalt_ = true;
  level_ = 0;
  audioOut_ = lowpass_ = 0;
  irq = false;
  update();
}

void FdsMapper::update() {
  chr8(0);
  mirror((ctrl_ & 8) ? HORIZONTAL : VERTICAL);
}

void FdsMapper::setIrq() { irq = timerIrq_ || diskIrq_; }

void FdsMapper::trocar(int lado) {
  disk_ = -1;
  if (lado >= 0 && lado < sides()) { pending_ = lado; insertDelay_ = INSERT_CYCLES; }
  else { pending_ = -1; insertDelay_ = 0; }
}

// ---------------------------------------------------------------- CPU
uint8_t FdsMapper::cpuRead(uint16_t a, uint8_t ob) {
  if (a >= 0xE000) return cart.prg[a & 0x1FFF];
  if (a >= 0x6000) return cart.prgRam[a - 0x6000];
  if (a >= 0x4040 && a < 0x4100) return soundRegs_ ? audioRead(a, ob) : ob;
  if (!diskRegs_ || a < 0x4030 || a > 0x4033) return ob;
  const bool inserted = disk_ >= 0;
  switch (a) {
    case 0x4030: {
      uint8_t v = static_cast<uint8_t>(ob & 0x24);
      if (timerIrq_) v |= 0x01;
      if (transfer_) v |= 0x02;
      if (ctrl_ & 8) v |= 0x08;
      if (badCrc_) v |= 0x10;
      transfer_ = false;
      timerIrq_ = diskIrq_ = false;
      setIrq();
      return v;
    }
    case 0x4031:
      transfer_ = false;
      diskIrq_ = false;
      setIrq();
      return readData_;
    case 0x4032: {
      uint8_t v = static_cast<uint8_t>(ob & 0xF8);
      if (!inserted) v |= 0x05;                // sem disco (e, sem disco, sem proteção contra gravação a medir)
      if (!inserted || !scanning_) v |= 0x02;  // drive não está pronto
      return v;
    }
    default:  // $4033: bit 7 = bateria boa; os outros espelham o que foi gravado em $4026
      return static_cast<uint8_t>((ext_ & 0x7F) | 0x80);
  }
}

uint8_t FdsMapper::cpuPeek(uint16_t a) {
  if (a >= 0xE000) return cart.prg[a & 0x1FFF];
  if (a >= 0x6000) return cart.prgRam[a - 0x6000];
  return 0;
}

void FdsMapper::writeReg(uint16_t a, uint8_t v) {
  if (a >= 0xE000) return;  // BIOS
  if (a >= 0x6000) { cart.prgRam[a - 0x6000] = v; return; }
  if (a >= 0x4040) { if (soundRegs_ && a <= 0x408A) audioWrite(a, v); return; }
  if (!diskRegs_ && a >= 0x4024 && a <= 0x4026) return;
  switch (a) {
    case 0x4020: irqReload_ = static_cast<uint16_t>((irqReload_ & 0xFF00) | v); break;
    case 0x4021: irqReload_ = static_cast<uint16_t>((irqReload_ & 0x00FF) | (v << 8)); break;
    case 0x4022:
      irqRepeat_ = v & 1;
      irqOn_ = (v & 2) && diskRegs_;
      if (irqOn_) irqCounter_ = irqReload_;
      else { timerIrq_ = false; setIrq(); }
      break;
    case 0x4023:
      diskRegs_ = v & 1;
      soundRegs_ = v & 2;
      if (!diskRegs_) { irqOn_ = false; timerIrq_ = diskIrq_ = false; setIrq(); }
      break;
    case 0x4024:
      writeData_ = v;
      transfer_ = false;
      diskIrq_ = false;
      setIrq();
      break;
    case 0x4025:
      ctrl_ = v;
      motor_ = v & 0x01;
      resetXfer_ = v & 0x02;
      readMode_ = v & 0x04;
      crcCtl_ = v & 0x10;
      ready_ = v & 0x40;
      diskIrqOn_ = v & 0x80;
      diskIrq_ = false;
      setIrq();
      update();
      break;
    case 0x4026: ext_ = v; break;
    default: break;
  }
}

// ---------------------------------------------------------------- drive
void FdsMapper::crc(uint8_t v) {
  crc_ ^= v;
  for (int k = 0; k < 8; k++) crc_ = (crc_ & 1) ? static_cast<uint16_t>((crc_ >> 1) ^ 0x8408) : static_cast<uint16_t>(crc_ >> 1);
}

void FdsMapper::clockDisk() {
  if (insertDelay_ > 0 && --insertDelay_ == 0) { disk_ = pending_; pending_ = -1; }
  if (disk_ < 0 || !motor_) { endOfHead_ = true; scanning_ = false; return; }
  if (resetXfer_ && !scanning_) return;
  if (endOfHead_) { delay_ = REWIND_CYCLES; endOfHead_ = false; pos_ = 0; gapEnded_ = false; return; }
  if (delay_ > 0) { delay_--; return; }
  scanning_ = true;
  std::vector<uint8_t>& side = sides_[static_cast<size_t>(disk_)];
  if (pos_ >= side.size()) pos_ = 0;  // defesa: nunca ler fora do lado
  bool needIrq = diskIrqOn_;
  if (readMode_) {
    const uint8_t d = side[pos_];
    if (!prevCrc_) crc(d);
    if (!ready_) { gapEnded_ = false; crc_ = 0; badCrc_ = false; }
    else if (d && !gapEnded_) { gapEnded_ = true; needIrq = false; }  // a marca $80 fecha o intervalo
    if (gapEnded_) {
      transfer_ = true;
      readData_ = d;
      if (needIrq) { diskIrq_ = true; setIrq(); }
    }
    if (!prevCrc_ && crcCtl_) badCrc_ = crc_ != 0;
  } else {
    uint8_t d = 0;
    if (!crcCtl_) {
      transfer_ = true;
      d = writeData_;
      if (needIrq) { diskIrq_ = true; setIrq(); }
    }
    if (!ready_) { d = 0; crc_ = 0; }
    if (!crcCtl_) crc(d);
    else { d = static_cast<uint8_t>(crc_); crc_ >>= 8; }
    if (side[pos_] != d) { side[pos_] = d; dirty = true; }
    gapEnded_ = false;
    badCrc_ = false;
  }
  prevCrc_ = crcCtl_;
  if (++pos_ >= side.size()) {
    // fim do disco: o motor para (e o Kosodate Gokko espera um IRQ aqui)
    motor_ = false;
    pos_ = 0;
    if (diskIrqOn_) { diskIrq_ = true; setIrq(); }
  } else {
    delay_ = BYTE_DELAY;
  }
}

void FdsMapper::cpuCycle() {
  if (irqOn_) {
    if (irqCounter_ == 0) {
      timerIrq_ = true;
      setIrq();
      irqCounter_ = irqReload_;
      if (!irqRepeat_) irqOn_ = false;
    } else {
      irqCounter_--;
    }
  }
  clockDisk();
  clockAudio();
}

// ---------------------------------------------------------------- som
void FdsMapper::writeEnv(Env& e, uint8_t v) {
  e.speed = v & 0x3F;
  e.up = v & 0x40;
  e.off = v & 0x80;
  e.timer = 8u * (e.speed + 1u) * master_;
  if (e.off) e.gain = e.speed;
}

bool FdsMapper::tickEnv(Env& e) {
  if (e.off || master_ == 0) return false;
  if (e.timer == 0 || --e.timer == 0) {
    e.timer = 8u * (e.speed + 1u) * master_;
    if (e.up && e.gain < 32) e.gain++;
    else if (!e.up && e.gain > 0) e.gain--;
    return true;
  }
  return false;
}

// desvio da frequência da onda pela modulação (fórmula da wiki do NESdev)
int FdsMapper::modOutput() const {
  int t = modCounter_ * mod_.gain;
  int rem = t & 0xF;
  t >>= 4;
  if (rem > 0 && (t & 0x80) == 0) t += modCounter_ < 0 ? -1 : 2;
  if (t >= 192) t -= 256;
  else if (t < -64) t += 256;
  t = vol_.freq * t;
  rem = t & 0x3F;
  t >>= 6;
  if (rem >= 32) t += 1;
  return t;
}

void FdsMapper::updateLevel() {
  if (waveWrite_) return;  // com a tabela aberta para gravação, a saída fica parada
  static const int MV[4] = {36, 24, 17, 14};  // volume mestre 2/2, 2/3, 2/4, 2/5
  level_ = wave_[wavePos_] * std::min<int>(vol_.gain, 32) * MV[masterVol_] / 1152;
}

void FdsMapper::clockAudio() {
  if (!waveHalt_ && !envHalt_) {
    tickEnv(vol_);
    if (tickEnv(mod_)) modOut_ = modOutput();
  }
  if (!modHalt_ && mod_.freq > 0) {
    const uint16_t before = modAcc_;
    modAcc_ = static_cast<uint16_t>(modAcc_ + mod_.freq);
    if (modAcc_ < before) {
      static const int LUT[8] = {0, 1, 2, 4, 99, -4, -2, -1};
      const int d = LUT[modTable_[modPos_] & 7];
      int c = d == 99 ? 0 : modCounter_ + d;
      if (c >= 64) c -= 128; else if (c < -64) c += 128;
      modCounter_ = static_cast<int8_t>(c);
      modPos_ = (modPos_ + 1) & 0x3F;
      modOut_ = modOutput();
    }
  }
  updateLevel();
  const int step = vol_.freq + modOut_;
  if (!waveHalt_ && step > 0) {
    const uint32_t s = waveAcc_ + static_cast<uint32_t>(step);
    waveAcc_ = static_cast<uint16_t>(s);
    if (s > 0xFFFF) wavePos_ = (wavePos_ + 1) & 0x3F;
  }
  // nível 63 = ~2,4 vezes uma quadrada do console no máximo; filtro passa-baixa de ~2 kHz da saída do adaptador
  const float target = static_cast<float>(level_) * (0.357f / 63.0f);
  lowpass_ += (target - lowpass_) * 0.00700f;
  audioOut_ = lowpass_;
}

uint8_t FdsMapper::audioRead(uint16_t a, uint8_t ob) {
  if (a <= 0x407F) return static_cast<uint8_t>((ob & 0xC0) | (waveWrite_ ? wave_[a & 0x3F] : wave_[wavePos_]));
  if (a == 0x4090) return static_cast<uint8_t>((ob & 0xC0) | vol_.gain);
  if (a == 0x4092) return static_cast<uint8_t>((ob & 0xC0) | mod_.gain);
  return ob;
}

void FdsMapper::audioWrite(uint16_t a, uint8_t v) {
  if (a <= 0x407F) { if (waveWrite_) wave_[a & 0x3F] = v & 0x3F; return; }
  switch (a) {
    case 0x4080: writeEnv(vol_, v); break;
    case 0x4082: vol_.freq = static_cast<uint16_t>((vol_.freq & 0x0F00) | v); break;
    case 0x4083:
      vol_.freq = static_cast<uint16_t>((vol_.freq & 0xFF) | ((v & 0x0F) << 8));
      envHalt_ = v & 0x40;
      waveHalt_ = v & 0x80;
      if (waveHalt_) { wavePos_ = 0; waveAcc_ = 0; }
      if (envHalt_) { vol_.timer = 8u * (vol_.speed + 1u) * master_; mod_.timer = 8u * (mod_.speed + 1u) * master_; }
      break;
    case 0x4084: writeEnv(mod_, v); break;
    case 0x4085: { int c = v & 0x7F; if (c >= 64) c -= 128; modCounter_ = static_cast<int8_t>(c); break; }
    case 0x4086: mod_.freq = static_cast<uint16_t>((mod_.freq & 0x0F00) | v); break;
    case 0x4087:
      mod_.freq = static_cast<uint16_t>((mod_.freq & 0xFF) | ((v & 0x0F) << 8));
      modHalt_ = v & 0x80;
      if (modHalt_) modAcc_ = 0;
      break;
    case 0x4088:
      if (modHalt_) {  // a tabela só aceita gravação com a modulação parada; cada valor ocupa duas posições
        modTable_[modPos_ & 0x3F] = v & 7;
        modTable_[(modPos_ + 1) & 0x3F] = v & 7;
        modPos_ = (modPos_ + 2) & 0x3F;
      }
      break;
    case 0x4089: masterVol_ = v & 3; waveWrite_ = v & 0x80; break;
    case 0x408A: master_ = v; break;
    default: break;
  }
  if (a == 0x4080 || a == 0x4082 || a == 0x4083 || a == 0x4084 || a == 0x4085) modOut_ = modOutput();
}

// ---------------------------------------------------------------- save (diferenças) e estado
// formato: "FDSD", 1 byte de versão (2), 1 byte com o número de lados, 4 bytes com a assinatura (FNV-1a) do
// disco original (a versão 1 não tinha); depois trechos (lado, posição de 4 bytes,
// tamanho de 2 bytes, bytes); lado 0xFF encerra
std::vector<uint8_t> FdsMapper::diff() const {
  std::vector<uint8_t> o = {'F', 'D', 'S', 'D', 2, static_cast<uint8_t>(sides_.size())};
  const uint32_t h = origHash();
  for (int i = 0; i < 4; i++) o.push_back(static_cast<uint8_t>(h >> (8 * i)));
  for (size_t s = 0; s < sides_.size(); s++) {
    const auto& a = sides_[s];
    const auto& b = orig_[s];
    size_t k = 0;
    while (k < a.size()) {
      if (a[k] == b[k]) { k++; continue; }
      // junta diferenças separadas por até 8 bytes iguais
      size_t ultimo = k, end = k + 1;
      while (end < a.size() && end - k < 0xFFFF && end - ultimo <= 8) {
        if (a[end] != b[end]) ultimo = end;
        end++;
      }
      end = ultimo + 1;
      const size_t n = end - k;
      o.push_back(static_cast<uint8_t>(s));
      for (int i = 0; i < 4; i++) o.push_back(static_cast<uint8_t>(k >> (8 * i)));
      o.push_back(static_cast<uint8_t>(n)); o.push_back(static_cast<uint8_t>(n >> 8));
      o.insert(o.end(), a.begin() + static_cast<long>(k), a.begin() + static_cast<long>(end));
      k = end;
    }
  }
  o.push_back(0xFF);
  return o;
}

uint32_t FdsMapper::origHash() const {
  uint32_t h = 2166136261u;
  for (const auto& l : orig_)
    for (uint8_t b : l) h = (h ^ b) * 16777619u;
  return h;
}

bool FdsMapper::applyDiff(const std::vector<uint8_t>& d) {
  if (d.size() < 7 || std::memcmp(d.data(), "FDSD", 4) != 0 || (d[4] != 1 && d[4] != 2) || d[5] != sides_.size())
    return false;
  size_t p = 6;
  if (d[4] == 2) {  // confere se o save é deste disco
    if (d.size() < 11) return false;
    const uint32_t h = d[6] | (d[7] << 8) | (d[8] << 16) | (uint32_t(d[9]) << 24);
    if (h != origHash()) return false;
    p = 10;
  }
  std::vector<std::vector<uint8_t>> novo = orig_;
  while (p < d.size() && d[p] != 0xFF) {
    if (p + 7 > d.size()) return false;
    const size_t s = d[p];
    const size_t at = d[p + 1] | (d[p + 2] << 8) | (d[p + 3] << 16) | (size_t(d[p + 4]) << 24);
    const size_t n = d[p + 5] | (d[p + 6] << 8);
    p += 7;
    if (s >= novo.size() || at > novo[s].size() || n > novo[s].size() - at || n > d.size() - p) return false;
    std::memcpy(&novo[s][at], &d[p], n);
    p += n;
  }
  if (p >= d.size()) return false;  // sem o marcador de fim: save danificado
  sides_ = std::move(novo);
  return true;
}

void FdsMapper::syncRegs(StateIO& io) {
  io.sync(irqReload_); io.sync(irqCounter_); io.sync(irqOn_); io.sync(irqRepeat_); io.sync(timerIrq_); io.sync(diskIrq_);
  io.sync(diskRegs_); io.sync(soundRegs_);
  io.sync(disk_); io.sync(pending_); io.sync(insertDelay_); io.sync(pos_); io.sync(delay_); io.sync(crc_);
  io.sync(writeData_); io.sync(readData_); io.sync(ctrl_); io.sync(ext_);
  io.sync(motor_); io.sync(resetXfer_); io.sync(readMode_); io.sync(crcCtl_); io.sync(ready_); io.sync(diskIrqOn_);
  io.sync(endOfHead_); io.sync(scanning_); io.sync(gapEnded_); io.sync(prevCrc_); io.sync(transfer_); io.sync(badCrc_);
  for (Env* e : {&vol_, &mod_}) { io.sync(e->speed); io.sync(e->gain); io.sync(e->off); io.sync(e->up); io.sync(e->freq); io.sync(e->timer); }
  io.sync(master_); io.raw(wave_, sizeof wave_); io.raw(modTable_, sizeof modTable_);
  io.sync(wavePos_); io.sync(modPos_); io.sync(masterVol_); io.sync(modCounter_); io.sync(waveAcc_); io.sync(modAcc_); io.sync(modOut_);
  io.sync(waveWrite_); io.sync(envHalt_); io.sync(waveHalt_); io.sync(modHalt_); io.sync(level_); io.sync(audioOut_); io.sync(lowpass_);
  // o disco vai como diferenças em relação ao original
  std::vector<uint8_t> d;
  if (!io.reading()) d = diff();
  uint32_t n = static_cast<uint32_t>(d.size());
  io.sync(n);
  if (io.reading()) {
    if (n > 64u * 1024 * 1024) throw std::runtime_error("Estado salvo danificado.");
    d.resize(n);
  }
  if (n) io.raw(d.data(), n);
  if (io.reading()) {
    if (!applyDiff(d)) throw std::runtime_error("Estado salvo incompatível com este disco.");
    dirty = true;
    const int ns = sides();
    clampTo(disk_, -1, ns - 1);
    clampTo(pending_, -1, ns - 1);
    // sem disco no drive o cabeçote está sempre no começo (como em clockDisk)
    if (disk_ < 0) { pos_ = 0; endOfHead_ = true; scanning_ = false; }
    else if (pos_ >= sides_[static_cast<size_t>(disk_)].size()) pos_ = 0;
    clampTo(delay_, 0u, REWIND_CYCLES);
    clampTo(insertDelay_, 0, INSERT_CYCLES);
    wavePos_ &= 0x3F; modPos_ &= 0x3F; masterVol_ &= 3;
    for (auto& w : wave_) w &= 0x3F;
    for (auto& m : modTable_) m &= 7;
    clampTo(level_, 0, 63);
    clampTo<int8_t>(modCounter_, -64, 63);
    for (float* f : {&audioOut_, &lowpass_})
      if (!(*f == *f) || *f > 1.0f || *f < -1.0f) *f = 0;
    setIrq();  // a linha de IRQ do mapeador vem dos dois pedidos
  }
}

// ================================================================ núcleo
FdsCore::FdsCore(const std::vector<uint8_t>& disco, const std::string& nome, const std::vector<uint8_t>* save,
                 const std::vector<uint8_t>* bios) {
  std::string n = nome;
  for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const std::vector<uint8_t>& b = bios ? *bios : firmware("fds");
  if (b.size() != 0x2000)
    throw std::runtime_error("o Famicom Disk System precisa da BIOS do console (disksys.rom, 8 KB). "
                             "Abra o arquivo da BIOS uma vez no Índigo.");
  const bool qd = qdFormat(disco, n);
  std::vector<std::vector<uint8_t>> lados;
  std::string erro;
  if (!rawSides(disco, qd, lados, erro)) throw std::runtime_error(erro);
  nes::Cart c;
  c.prg = b;
  c.chr.assign(0x2000, 0);
  c.chrRam = true;
  c.prgRam.assign(0x8000, 0);
  c.mapper = 20;  // número do FDS na lista do iNES (só informativo)
  m_ = std::make_unique<nes::NES>(std::move(c), [&](nes::Cart& cart) {
    auto p = std::make_unique<FdsMapper>(cart, std::move(lados), qd);
    map_ = p.get();
    return p;
  });
  if (save) importSave(*save);
}

FdsCore::~FdsCore() = default;

bool FdsCore::saveDirty() const { return map_->dirty; }

std::vector<uint8_t> FdsCore::exportSave() {
  map_->dirty = false;
  return map_->diff();
}

void FdsCore::importSave(const std::vector<uint8_t>& b) {
  if (b.empty()) return;
  map_->applyDiff(b);  // save de outro disco ou danificado: fica o disco original
  map_->dirty = false;
}

std::vector<uint8_t> FdsCore::saveState() {
  std::vector<uint8_t> s = {'F', 'D', 'S', '1'};
  const std::vector<uint8_t> n = m_->saveState();
  s.insert(s.end(), n.begin(), n.end());
  return s;
}

void FdsCore::loadState(const std::vector<uint8_t>& s) {
  if (s.size() < 4 || std::memcmp(s.data(), "FDS1", 4) != 0) throw std::runtime_error("Este estado não é do Famicom Disk System.");
  m_->loadState(std::vector<uint8_t>(s.begin() + 4, s.end()));
}

std::string FdsCore::note() const {
  const int n = map_->sides();
  return n > 1 ? " · disco com " + std::to_string(n) + " lados: troque o lado pelo menu" : "";
}

int FdsCore::diskSides() const { return map_->sides(); }
int FdsCore::diskSide() const { return map_->pendingSide() >= 0 ? map_->pendingSide() : map_->side(); }
void FdsCore::setDiskSide(int lado) { map_->trocar(lado); }

}  // namespace indigo::fds
