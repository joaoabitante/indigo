#include "spu.h"

#include <algorithm>
#include <cstring>

namespace indigo::ps1 {

namespace {

// Tabela da interpolação gaussiana do hardware (512 entradas, psx-spx "SPU Gaussian Interpolation").
// Para a fração i (bits 4..11 do contador de fase), as 4 amostras mais recentes s[-3..0] usam os pesos
// g[0xFF-i], g[0x1FF-i], g[0x100+i] e g[i]; a soma dos 4 produtos é deslocada 15 bits.
constexpr int16_t GAUSS[512] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7,
    8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24,
    25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56,
    58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111,
    116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199,
    205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328,
    338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512,
    525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761,
    779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089,
    1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508,
    1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031,
    2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668,
    2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427,
    3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315,
    4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332,
    5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475,
    6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735,
    7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097,
    9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542,
    10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045,
    12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577,
    13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104,
    15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592,
    16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001,
    18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295,
    19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439,
    20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399,
    21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148,
    22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662,
    22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927,
    22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
};

// Pesos dos 5 filtros do ADPCM (s-1, s-2) em 1/64; os índices 5..15 valem 0 no hardware.
constexpr int32_t FILTRO_ADPCM[16][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

// FIR de 39 pontos da reverberação (conversão 44,1 kHz <-> 22,05 kHz): só os coeficientes ímpares são diferentes
// de zero, mais o central (0x4000) na redução de taxa. psx-spx "Reverb Buffer Resampling".
constexpr int32_t FIR_REVERB[20] = {-1,    2,     -10,  35,   -103, 266,  -616, 1332, -2960, 10246,
                                    10246, -2960, 1332, -616, 266,  -103, 35,   -10,  2,     -1};

inline int32_t sat16(int32_t v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }

// -v sem estourar: -(-32768) vira 32767
inline int32_t negSat(int32_t v) { return v == -32768 ? 32767 : -v; }

// Passo dos envelopes (ADSR) e das varreduras de volume. A taxa tem 7 bits: shift = taxa >> 2 e
// passo = 7 - (taxa & 3) (ou -8 + (taxa & 3) quando o sentido é invertido). Com shift < 11 o passo é ampliado
// (<< (11 - shift)); com shift > 11 o passo acontece a cada 2^(shift - 11) amostras, contado num acumulador
// de 15 bits que recebe 'div' por amostra. O modo exponencial multiplica o passo de descida pelo nível
// (passo * nivel / 0x8000) e, na subida, a partir de 0x6000 fica 4 vezes mais lento.
// 'parada' é a taxa em que o envelope congela (0x7F; 0x7C para decaimento e liberação, que têm 5 bits << 2).
struct Passo {
  int32_t inc;
  int32_t div;
};
inline Passo calculaPasso(uint32_t taxa, bool exponencial, bool descida, bool inverte, int16_t nivel, uint32_t parada) {
  Passo p;
  p.inc = 7 - int32_t(taxa & 3);
  if (inverte) p.inc = ~p.inc;
  p.div = 0x8000;
  if (taxa < 0x2C) p.inc = int32_t(uint32_t(p.inc) << ((0x2F - taxa) >> 2));
  if (taxa >= 0x30) p.div >>= (taxa - 0x2C) >> 2;
  if (exponencial) {
    if (descida) {
      p.inc = (int32_t(nivel) * p.inc) >> 15;
    } else if ((nivel & 0x7FFF) >= 0x6000) {
      // 4 vezes mais lento: divide o passo, o ritmo ou os dois, conforme o shift
      if (taxa < 0x28) {
        p.inc >>= 2;
      } else if (taxa >= 0x2C) {
        p.div >>= 2;
      } else {
        p.inc >>= 1;
        p.div >>= 1;
      }
    }
  }
  if (p.div == 0 && taxa < parada) p.div = 1;
  return p;
}

}  // namespace

Spu::Spu() { reset(); }

void Spu::reset() {
  for (auto& z : voz_) z = Voz{};
  principal_[0] = principal_[1] = Volume{};
  std::memset(ram_, 0, sizeof(ram_));
  std::memset(regs_, 0, sizeof(regs_));
  std::memset(aux_, 0, sizeof(aux_));
  status_ = 0;
  kon_ = koff_ = fm_ = ruidoVozes_ = reverbVozes_ = endx_ = 0;
  ctrl_ = 0;
  for (int i = 0; i < 2; i++) volReverb_[i] = volCd_[i] = volExt_[i] = 0;
  irqEnd_ = transfEnd_ = 0;
  irq_ = false;
  cwa_ = 0;
  ruidoDiv_ = ruidoCont_ = 0;
  lfsr_ = 0;
  rvbBase_ = rvbAtual_ = 0;
  std::memset(rvbDown_, 0, sizeof(rvbDown_));
  std::memset(rvbUp_, 0, sizeof(rvbUp_));
  rvbPos_ = 0;
  std::memset(fifo_, 0, sizeof(fifo_));
  fifoN_ = 0;
  std::memset(bloco_, 0, sizeof(bloco_));
  blocoN_ = 0;
  ocupado_ = 0;
  contador_ = CICLOS_POR_AMOSTRA;
  cdR_ = cdW_ = 0;
  outR_ = outW_ = 0;
}

// ---------------------------------------------------------------------------------------------------------
// IRQ por endereço: com SPUCNT bit 6 ligado, qualquer acesso à RAM de som (transferência, leitura das vozes,
// reverberação, captura) no endereço de 0x1A4 liga a IRQ9, que só volta a zero desligando o bit 6.

void Spu::assertIrq() {
  if (irq_) return;
  irq_ = true;
  if (onIrq) onIrq();
}

void Spu::verificaIrq(uint32_t a) {
  if ((ctrl_ & 0x40) && irqEnd_ == a) assertIrq();
}

// ---------------------------------------------------------------------------------------------------------
// Transferências para a RAM. A escrita manual passa pela FIFO de 32 meias palavras, que vai para a RAM quando
// o modo de transferência (SPUCNT bits 4-5) é 1 (escrita manual). O tipo em 0x1AC (bits 1-3) decide como os
// dados chegam à RAM (psx-spx "Sound RAM Data Transfer Control"): 2 = normal; 3, 4 e 5 repetem a primeira
// meia palavra de cada par/quarteto ou a última de cada grupo de 8; 0, 1, 6 e 7 (preenchimento) usam a última.

void Spu::transfereDireto(uint16_t v) {
  ramEscreve(transfEnd_, v);
  transfEnd_ = (transfEnd_ + 1) & RAM_MASK;
  verificaIrq(transfEnd_);
}

void Spu::descarregaBloco() {
  const int tipo = tipoTransf();
  const int n = blocoN_;
  blocoN_ = 0;
  for (int k = 0; k < n; k++) {
    uint16_t v;
    if (tipo == 3) v = bloco_[k & ~1];
    else if (tipo == 4) v = bloco_[k & ~3];
    else if (tipo == 2) v = bloco_[k];
    else v = bloco_[n - 1];
    transfereDireto(v);
  }
}

void Spu::transfere(uint16_t v) {
  if (tipoTransf() == 2) {
    if (blocoN_) descarregaBloco();
    transfereDireto(v);
    return;
  }
  bloco_[blocoN_++] = v;
  if (blocoN_ == 8) descarregaBloco();
}

void Spu::descarregaFifo() {
  const int n = fifoN_;
  fifoN_ = 0;
  for (int i = 0; i < n; i++) transfere(fifo_[i]);
  if (blocoN_) descarregaBloco();
  // a FIFO esvazia em cerca de 16 ciclos por meia palavra (SPUSTAT bit 10 ligado enquanto isso)
  ocupado_ += n * 16;
}

void Spu::dmaWrite(uint32_t word) {
  transfere(uint16_t(word));
  transfere(uint16_t(word >> 16));
}

uint32_t Spu::dmaRead() {
  uint32_t v = ramLe(transfEnd_);
  transfEnd_ = (transfEnd_ + 1) & RAM_MASK;
  v |= uint32_t(ramLe(transfEnd_)) << 16;
  transfEnd_ = (transfEnd_ + 1) & RAM_MASK;
  verificaIrq(transfEnd_);
  return v;
}

bool Spu::dmaRequest() const {
  const int m = modoTransf();
  return m == 2 || m == 3;
}

// ---------------------------------------------------------------------------------------------------------
// Registradores

uint32_t Spu::read(uint32_t offset, int size) {
  offset &= 0x3FF;
  if (size == 4) return read16(offset & 0x3FC) | uint32_t(read16((offset & 0x3FC) | 2)) << 16;
  const uint16_t v = read16(offset & 0x3FE);
  if (size == 1) return (offset & 1) ? (v >> 8) : (v & 0xFF);
  return v;
}

void Spu::write(uint32_t offset, uint32_t value, int size) {
  offset &= 0x3FF;
  if (size == 4) {
    write16(offset & 0x3FC, uint16_t(value));
    write16((offset & 0x3FC) | 2, uint16_t(value >> 16));
    return;
  }
  if (size == 1 && (offset & 1)) return;  // escrita de 8 bits em endereço ímpar não tem efeito
  write16(offset & 0x3FE, uint16_t(value));
}

uint16_t Spu::read16(uint32_t a) {
  if (a >= 0x200) {
    if (a < 0x260) return voz_[(a - 0x200) >> 2].vol[(a >> 1) & 1].atual;
    if (a < 0x280) return aux_[(a & 0x1F) >> 1];
    return 0xFFFF;
  }
  if (a < 0x180) {
    const Voz& z = voz_[a >> 4];
    switch (a & 0xF) {
      case 0x0C: return z.env.nivel;
      case 0x0E: return uint16_t(z.repeticao >> 2);
      default: break;
    }
    return regs_[a >> 1];
  }
  switch (a & 0x7F) {
    case 0x1C: return uint16_t(endx_);
    case 0x1E: return uint16_t(endx_ >> 16);
    case 0x28: {  // leitura direta da RAM no endereço de transferência
      const uint16_t v = ramLe(transfEnd_);
      transfEnd_ = (transfEnd_ + 1) & RAM_MASK;
      verificaIrq(transfEnd_);
      return v;
    }
    case 0x2A: return ctrl_;
    case 0x2E: {
      // bits 7-10 são do estado atual da transferência; os outros vêm da última amostra
      uint16_t s = status_ & ~0x0780;
      const int m = modoTransf();
      if (m >= 2) s |= 0x80;
      if (m == 2) s |= 0x100;
      if (m == 3) s |= 0x200;
      if (ocupado_ > 0) s |= 0x400;
      return s;
    }
    case 0x38: case 0x3A: return principal_[(a >> 1) & 1].atual;
    case 0x3C: return 0;
    default: break;
  }
  return regs_[a >> 1];
}

void Spu::write16(uint32_t a, uint16_t v) {
  if (a >= 0x200) {
    if (a < 0x260) voz_[(a - 0x200) >> 2].vol[(a >> 1) & 1].atual = v;
    else if (a < 0x280) aux_[(a & 0x1F) >> 1] = v;
    return;
  }
  if (a < 0x180) {
    Voz& z = voz_[a >> 4];
    switch (a & 0xF) {
      case 0x00: case 0x02: z.vol[(a >> 1) & 1].reg = v; break;
      case 0x04: z.pitch = v; break;
      case 0x06: z.inicio = (uint32_t(v) << 2) & RAM_MASK; break;
      case 0x08: z.adsr = (z.adsr & 0xFFFF0000u) | v; break;
      case 0x0A: z.adsr = (z.adsr & 0x0000FFFFu) | uint32_t(v) << 16; break;
      case 0x0C: z.env.nivel = v; break;
      case 0x0E:
        z.repeticao = (uint32_t(v) << 2) & RAM_MASK;
        z.ignoraLoopBloco = true;
        break;
    }
    regs_[a >> 1] = v;
    return;
  }
  // as máscaras de 24 vozes: a metade baixa substitui os bits 0-15, a alta os bits 16-23
  auto baixa = [v](uint32_t& m) { m = (m & 0xFFFF0000u) | v; };
  auto alta = [v](uint32_t& m) { m = (m & 0x0000FFFFu) | uint32_t(v & 0xFF) << 16; };
  switch (a & 0x7F) {
    case 0x00: case 0x02: principal_[(a >> 1) & 1].reg = v; break;
    case 0x04: volReverb_[0] = int16_t(v); break;
    case 0x06: volReverb_[1] = int16_t(v); break;
    case 0x08: baixa(kon_); break;
    case 0x0A: alta(kon_); break;
    case 0x0C: baixa(koff_); break;
    case 0x0E: alta(koff_); break;
    case 0x10: baixa(fm_); break;
    case 0x12: alta(fm_); break;
    case 0x14: baixa(ruidoVozes_); break;
    case 0x16: alta(ruidoVozes_); break;
    case 0x18: baixa(reverbVozes_); break;
    case 0x1A: alta(reverbVozes_); break;
    case 0x1C: endx_ = (endx_ & 0xFFFF0000u) | v; break;
    case 0x1E: endx_ = (endx_ & 0x0000FFFFu) | uint32_t(v) << 16; break;
    case 0x22:
      rvbBase_ = (uint32_t(v) << 2) & RAM_MASK;
      rvbAtual_ = rvbBase_;
      break;
    case 0x24:
      irqEnd_ = (uint32_t(v) << 2) & RAM_MASK;
      verificaIrq(transfEnd_);
      break;
    case 0x26:
      transfEnd_ = (uint32_t(v) << 2) & RAM_MASK;
      verificaIrq(transfEnd_);
      break;
    case 0x28:  // FIFO de dados
      if (modoTransf() == 1) {
        transfere(v);
        ocupado_ += 16;
      } else if (fifoN_ < 32) {
        fifo_[fifoN_++] = v;
      }
      break;
    case 0x2A: {
      const int antes = modoTransf();
      ctrl_ = v;
      if (!(v & 0x40)) irq_ = false;
      verificaIrq(transfEnd_);
      if (modoTransf() == 1 && fifoN_) descarregaFifo();
      if (modoTransf() != antes && blocoN_) descarregaBloco();
      break;
    }
    case 0x2C:
      if (blocoN_) descarregaBloco();  // muda o tipo: o grupo pendente vai com o tipo antigo
      break;
    case 0x2E: status_ = v; break;  // somente leitura; o valor dura até a próxima amostra
    case 0x30: volCd_[0] = int16_t(v); break;
    case 0x32: volCd_[1] = int16_t(v); break;
    case 0x34: volExt_[0] = int16_t(v); break;
    case 0x36: volExt_[1] = int16_t(v); break;
    case 0x38: case 0x3A: principal_[(a >> 1) & 1].atual = v; break;
    default: break;
  }
  regs_[a >> 1] = v;
}

// ---------------------------------------------------------------------------------------------------------
// Entrada do CD e saída

void Spu::pushCdAudio(int16_t l, int16_t r) {
  if (((cdW_ - cdR_) & 0xFFFFFFFFu) >= CD_CAP) cdR_++;  // cheia: descarta a mais antiga
  cd_[cdW_ & (CD_CAP - 1)][0] = l;
  cd_[cdW_ & (CD_CAP - 1)][1] = r;
  cdW_++;
}

size_t Spu::take(int16_t* out, size_t maxPairs) {
  size_t n = 0;
  while (n < maxPairs && outR_ != outW_) {
    out[n * 2] = out_[outR_][0];
    out[n * 2 + 1] = out_[outR_][1];
    outR_ = (outR_ + 1) & (OUT_CAP - 1);
    n++;
  }
  return n;
}

void Spu::run(int cycles) {
  if (cycles <= 0) return;
  ocupado_ = ocupado_ > cycles ? ocupado_ - cycles : 0;
  contador_ -= cycles;
  while (contador_ <= 0) {
    contador_ += CICLOS_POR_AMOSTRA;
    amostra();
  }
}

// ---------------------------------------------------------------------------------------------------------
// Vozes

// Decodifica 4 amostras quando restam menos de 11 no anel. Os blocos de 16 bytes têm cabeçalho (shift, filtro,
// flags) e 28 amostras de 4 bits. Flags: bit 0 = fim (salta para o endereço de repetição e liga ENDX),
// bit 1 = repete (sem ele, o fim do bloco silencia a voz), bit 2 = início da repetição.
void Spu::decodifica(Voz& z, int n) {
  if (z.disp >= 11) {
    if (ctrl_ & 0x40) {
      const uint32_t t = (z.atual - 1) & RAM_MASK;
      if (irqEnd_ == t || irqEnd_ == (t & ~7u)) assertIrq();
    }
    return;
  }
  if ((z.atual & 7) == 0 && (z.flags & 1)) {
    // fim do bloco anterior: efeitos atrasados até começar o próximo
    z.atual = z.repeticao & ~7u;
    endx_ |= 1u << n;
    if (!(z.flags & 2) && !(ruidoVozes_ & (1u << n))) {
      z.env.fase = LIBERACAO;
      z.env.nivel = 0;
    }
  }
  if (ctrl_ & 0x40) {
    const uint32_t t = z.atual & RAM_MASK;
    if (irqEnd_ == t || irqEnd_ == (t & ~7u)) assertIrq();
  }
  if ((z.atual & 7) == 0) {
    const uint16_t cab = ram_[z.atual];
    z.shift = cab & 0xF;
    z.filtro = (cab >> 4) & 0xF;
    z.flags = uint8_t(cab >> 8);
    if ((z.flags & 4) && !z.ignoraLoopBloco) z.repeticao = z.atual;
    z.atual = (z.atual + 1) & RAM_MASK;
  }
  uint32_t dados = ram_[z.atual];
  int shift = z.shift;
  if (shift > 12) {  // valores reservados: o hardware fica só com o bit de sinal de cada amostra
    shift = 8;
    dados &= 0x8888;
  }
  const int32_t p1 = FILTRO_ADPCM[z.filtro][0], p2 = FILTRO_ADPCM[z.filtro][1];
  int16_t* d = &z.dec[z.posEsc];
  for (int i = 0; i < 4; i++) {
    int32_t s = int32_t(int16_t(uint16_t((dados >> (i * 4)) << 12))) >> shift;
    s += (z.h1 * p1) >> 6;
    s += (z.h2 * p2) >> 6;
    s = sat16(s);
    d[i] = int16_t(s);
    z.h2 = z.h1;
    z.h1 = int16_t(s);
  }
  z.posEsc = (z.posEsc + 4) & 0x1F;
  z.disp += 4;
  z.atual = (z.atual + 1) & RAM_MASK;
}

void Spu::envelope(Voz& z) {
  Envelope& e = z.env;
  if (e.fase == ATAQUE && e.nivel == 0x7FFF) e.fase = DECAIMENTO;
  const uint32_t r = z.adsr;
  const int16_t nivel = int16_t(e.nivel);
  Passo p;
  uint16_t limite;
  switch (e.fase) {
    case ATAQUE:
      p = calculaPasso((r >> 8) & 0x7F, r & 0x8000, false, false, nivel, 0x7F);
      limite = 0x7FFF;
      break;
    case DECAIMENTO:
      p = calculaPasso(((r >> 4) & 0xF) << 2, true, true, true, nivel, 0x7C);
      limite = 0;
      break;
    case SUSTENTACAO: {
      const bool desce = r & 0x40000000u;
      p = calculaPasso((r >> 22) & 0x7F, r & 0x80000000u, desce, desce, nivel, 0x7F);
      limite = desce ? 0 : 0x7FFF;
      break;
    }
    default:
      p = calculaPasso(((r >> 16) & 0x1F) << 2, r & 0x200000u, true, true, nivel, 0x7C);
      limite = 0;
      break;
  }
  e.div += uint32_t(p.div);
  if (!(e.div & 0x8000)) return;
  e.div = 0;
  const uint16_t antes = e.nivel;
  e.nivel = uint16_t(e.nivel + p.inc);
  if (e.fase == ATAQUE) {
    if ((antes ^ e.nivel) & e.nivel & 0x8000) e.nivel = limite;  // passou de 0x7FFF
  } else if (e.nivel & 0x8000) {
    e.nivel = limite;
  }
  if (e.fase == DECAIMENTO && e.nivel < uint32_t((r & 0xF) + 1) << 11) e.fase = SUSTENTACAO;
}

// Volume com varredura (bit 15 do registrador): bit 14 exponencial, bit 13 descida, bit 12 fase negativa,
// bits 0-6 a taxa. Na fase negativa o volume anda do lado negativo (o sentido do passo se inverte).
void Spu::varredura(Volume& v) {
  const uint16_t c = v.reg;
  if (!(c & 0x8000)) {
    v.atual = uint16_t((c & 0x7FFF) << 1);
    return;
  }
  const bool expo = c & 0x4000, desce = c & 0x2000, neg = c & 0x1000;
  const bool inverte = (desce != neg) || (desce && expo);
  const uint16_t xorNivel = (neg && !(desce && expo)) ? 0xFFFF : 0;
  const uint16_t xorTeste = neg ? 0xFFFF : 0;
  const Passo p = calculaPasso(c & 0x7F, expo, desce, inverte, int16_t(v.atual ^ xorNivel), 0x7F);
  if (desce && !(neg && expo) && ((v.atual & 0x8000) == (neg ? 0 : 0x8000) || v.atual == 0)) {
    v.atual = 0;  // a descida chegou (ou passou) do zero
    return;
  }
  v.div += uint32_t(p.div);
  if (!(v.div & 0x8000)) return;
  v.div = 0;
  if (!desce && (v.atual ^ xorTeste) == 0x7FFF) return;  // já no máximo
  const uint16_t antes = v.atual;
  v.atual = uint16_t(v.atual + p.inc);
  if (!desce && ((v.atual ^ antes) & 0x8000) && ((v.atual ^ xorTeste) & 0x8000)) v.atual = 0x7FFF ^ xorTeste;
}

void Spu::ruido() {
  const uint32_t f = (ctrl_ >> 8) & 0x3F;
  uint32_t passoDiv = 2u << (f >> 2), passoCont = 4 + (f & 3);
  if (f >= 0x3C) {
    passoDiv = 0x8000;
    passoCont = 8;
  }
  ruidoDiv_ += passoDiv;
  if (!(ruidoDiv_ & 0x8000)) return;
  ruidoDiv_ = 0;
  ruidoCont_ += passoCont;
  if (!(ruidoCont_ & 8)) return;
  ruidoCont_ &= 7;
  const uint32_t b = ((lfsr_ >> 15) ^ (lfsr_ >> 12) ^ (lfsr_ >> 11) ^ (lfsr_ >> 10) ^ 1) & 1;
  lfsr_ = uint16_t((lfsr_ << 1) | b);
}

// ---------------------------------------------------------------------------------------------------------
// Reverberação (psx-spx "SPU Reverb Formula"). Roda a 22,05 kHz, um canal por amostra de 44,1 kHz: o
// esquerdo nas posições ímpares e o direito nas pares, os dois com o mesmo ponteiro do buffer, que anda depois
// do direito. Os 32 registradores ficam em 0x1C0..0x1FF; os endereços são relativos ao ponteiro atual e dão a
// volta dentro da área de trabalho (de 0x1A2 até o fim da RAM).

uint32_t Spu::rvbEndereco(uint32_t rel) const {
  uint32_t a = rvbAtual_ + (rel & RAM_MASK);
  if (a & 0x40000) a += rvbBase_;
  return a & RAM_MASK;
}

int16_t Spu::rvbLe(uint16_t rel, int32_t extra) {
  return int16_t(ramLe(rvbEndereco((uint32_t(rel) << 2) + uint32_t(extra))));
}

void Spu::rvbEscreve(uint16_t rel, int16_t v) {
  if (ctrl_ & 0x80) ramEscreve(rvbEndereco(uint32_t(rel) << 2), uint16_t(v));
}

void Spu::reverbCanal(int lr, int32_t entrada) {
  const uint16_t* R = &regs_[0xE0];
  auto vol = [R](int i) { return int32_t(int16_t(R[i])); };
  const int32_t vIIR = vol(2), vWALL = vol(7), vAPF1 = vol(8), vAPF2 = vol(9), vIN = vol(30 + lr);
  // filtro de "paredes" (mesmo lado e lado oposto) com o passa-baixas vIIR
  const int32_t inA = sat16((((rvbLe(R[16 + lr], 0) * vWALL) >> 14) + ((entrada * vIN) >> 14)) >> 1);
  const int32_t inB = sat16((((rvbLe(R[24 + (lr ^ 1)], 0) * vWALL) >> 14) + ((entrada * vIN) >> 14)) >> 1);
  auto iir = [vIIR](int32_t in, int32_t anterior) {
    int32_t resto;  // anterior * (1 - vIIR), com o caso vIIR = -32768
    if (vIIR == -32768) resto = anterior == -32768 ? 0 : anterior * -65536;
    else resto = anterior * (32768 - vIIR);
    return sat16((((in * vIIR) >> 14) + (resto >> 14)) >> 1);
  };
  const int32_t iirA = iir(inA, rvbLe(R[10 + lr], -1));
  const int32_t iirB = iir(inB, rvbLe(R[18 + lr], -1));
  rvbEscreve(R[10 + lr], int16_t(iirA));
  rvbEscreve(R[18 + lr], int16_t(iirB));
  // filtros pente
  int32_t acc = (rvbLe(R[12 + lr], 0) * vol(3)) >> 14;
  acc += (rvbLe(R[14 + lr], 0) * vol(4)) >> 14;
  acc += (rvbLe(R[20 + lr], 0) * vol(5)) >> 14;
  acc += (rvbLe(R[22 + lr], 0) * vol(6)) >> 14;
  // dois passa-tudo em série
  const int32_t fbA = rvbLe(uint16_t(R[26 + lr] - R[0]), 0);
  const int32_t fbB = rvbLe(uint16_t(R[28 + lr] - R[1]), 0);
  const int32_t mda = sat16((acc + ((fbA * negSat(vAPF1)) >> 14)) >> 1);
  const int32_t mdb = sat16(fbA + ((((mda * vAPF1) >> 14) + ((fbB * negSat(vAPF2)) >> 14)) >> 1));
  const int32_t ivb = sat16(fbB + ((mdb * vAPF2) >> 15));
  rvbEscreve(R[26 + lr], int16_t(mda));
  rvbEscreve(R[28 + lr], int16_t(mdb));
  const uint32_t p = rvbPos_ >> 1;
  rvbUp_[lr][p] = rvbUp_[lr][p | 0x20] = int16_t(ivb);
}

void Spu::reverb(const int32_t in[2], int32_t out[2]) {
  for (int lr = 0; lr < 2; lr++) rvbDown_[lr][rvbPos_] = rvbDown_[lr][rvbPos_ | 0x40] = int16_t(in[lr]);
  const int lr = (rvbPos_ & 1) ? 0 : 1;
  {
    // redução para 22,05 kHz: FIR de 39 pontos terminando na amostra atual
    const int16_t* s = &rvbDown_[lr][(rvbPos_ - 38) & 0x3F];
    int32_t acc = 0x4000 * s[19];
    for (int i = 0; i < 20; i++) acc += FIR_REVERB[i] * s[i * 2];
    reverbCanal(lr, sat16(acc >> 15));
  }
  if (lr == 1) {
    rvbAtual_ = (rvbAtual_ + 1) & RAM_MASK;
    if (rvbAtual_ == 0) rvbAtual_ = rvbBase_;
  }
  // volta para 44,1 kHz: o canal calculado nesta amostra passa pelo FIR, o outro repete a amostra central
  for (int c = 0; c < 2; c++) {
    const int16_t* s = &rvbUp_[c][((rvbPos_ >> 1) - 19) & 0x1F];
    if (c == lr) {
      int32_t acc = 0;
      for (int i = 0; i < 20; i++) acc += FIR_REVERB[i] * s[i];
      out[c] = sat16(acc >> 14);
    } else {
      out[c] = s[9];
    }
  }
  rvbPos_ = (rvbPos_ + 1) & 0x3F;
}

// ---------------------------------------------------------------------------------------------------------
// Uma amostra estéreo (44,1 kHz)

void Spu::amostra() {
  status_ = uint16_t((ctrl_ & 0x3F) | (irq_ ? 0x40 : 0) | ((cwa_ & 0x100) ? 0x800 : 0));
  int32_t soma[2] = {0, 0}, somaRvb[2] = {0, 0};
  const uint32_t fm = fm_ & ~1u;  // a voz 0 não tem modulação

  for (int n = 0; n < 24; n++) {
    Voz& z = voz_[n];
    const uint32_t bit = 1u << n;
    z.saida = 0;
    if (z.atraso) z.ignoraLoopBloco = false;
    if (z.disp < 11 || (ctrl_ & 0x40)) decodifica(z, n);  // com 11 ou mais só há o teste da IRQ

    int32_t s;
    if (ruidoVozes_ & bit) {
      s = int16_t(lfsr_);
    } else if (z.env.nivel == 0) {
      s = 0;  // o envelope zera a amostra de qualquer jeito
    } else {
      const int i = (z.fase >> 4) & 0xFF;
      const int p = z.posLei;
      s = (z.dec[p & 0x1F] * GAUSS[0xFF - i] + z.dec[(p + 1) & 0x1F] * GAUSS[0x1FF - i] +
           z.dec[(p + 2) & 0x1F] * GAUSS[0x100 + i] + z.dec[(p + 3) & 0x1F] * GAUSS[i]) >> 15;
    }
    s = (s * int16_t(z.env.nivel)) >> 15;
    z.saida = s;
    if (n == 1 || n == 3) ramEscreve(0x400 | uint32_t(n >> 1) * 0x200 | cwa_, uint16_t(s));

    const int32_t l = (s * int16_t(z.vol[0].atual)) >> 15;
    const int32_t r = (s * int16_t(z.vol[1].atual)) >> 15;
    soma[0] += l;
    soma[1] += r;
    if (reverbVozes_ & bit) {
      somaRvb[0] += l;
      somaRvb[1] += r;
    }
    for (Volume& v : z.vol) {
      if (v.reg & 0x8000) varredura(v);
      else v.atual = uint16_t((v.reg & 0x7FFF) << 1);
    }

    if (!z.atraso) {
      envelope(z);
      uint32_t inc = z.pitch;
      if (fm & bit) inc = uint32_t(int32_t(z.pitch) + ((int32_t(int16_t(z.pitch)) * voz_[n - 1].saida) >> 15));
      if (inc > 0x3FFF) inc = 0x3FFF;
      const uint32_t f = z.fase + inc;
      z.fase = f & 0xFFF;
      z.disp = uint8_t(z.disp - (f >> 12));
      z.posLei = uint8_t((z.posLei + (f >> 12)) & 0x1F);
    } else {
      z.atraso--;
    }

    if ((koff_ & bit) && z.env.fase != LIBERACAO && z.atraso < 3) {
      z.env.div = 0;
      z.env.fase = LIBERACAO;
    }
    if (kon_ & bit) {
      z.env.nivel = 0;
      z.env.div = 0;
      z.env.fase = ATAQUE;
      z.flags = 0;
      z.posEsc = z.posLei = z.disp = 0;
      z.atraso = 4;
      endx_ &= ~bit;
      z.h1 = z.h2 = 0;
      z.fase = 0;
      z.atual = z.inicio & ~7u;
      z.ignoraLoopBloco = false;
    }
    if (!(ctrl_ & 0x8000)) {  // SPU desligado
      z.env.fase = LIBERACAO;
      z.env.nivel = 0;
    }
  }
  kon_ = koff_ = 0;

  if (!(ctrl_ & 0x4000)) {  // mudo (não afeta o CD)
    soma[0] = soma[1] = somaRvb[0] = somaRvb[1] = 0;
  }

  // CD: sempre gravado no buffer de captura; entra na mistura com SPUCNT bit 0 e na reverberação com o bit 2
  int32_t cd[2] = {0, 0};
  if (cdR_ != cdW_) {
    cd[0] = cd_[cdR_ & (CD_CAP - 1)][0];
    cd[1] = cd_[cdR_ & (CD_CAP - 1)][1];
    cdR_++;
  }
  ramEscreve(cwa_, uint16_t(cd[0]));
  ramEscreve(cwa_ | 0x200, uint16_t(cd[1]));
  if (ctrl_ & 1) {
    for (int c = 0; c < 2; c++) {
      const int32_t v = (cd[c] * volCd_[c]) >> 15;
      soma[c] += v;
      if (ctrl_ & 4) somaRvb[c] += v;
    }
  }
  cwa_ = (cwa_ + 1) & 0x1FF;

  ruido();

  somaRvb[0] = sat16(somaRvb[0]);
  somaRvb[1] = sat16(somaRvb[1]);
  int32_t molhado[2];
  reverb(somaRvb, molhado);

  int16_t* o = out_[outW_];
  for (int c = 0; c < 2; c++) {
    const int32_t m = sat16(soma[c] + ((molhado[c] * volReverb_[c]) >> 15));
    o[c] = int16_t(sat16((m * int16_t(principal_[c].atual)) >> 15));
  }
  outW_ = (outW_ + 1) & (OUT_CAP - 1);
  if (outW_ == outR_) outR_ = (outR_ + 1) & (OUT_CAP - 1);  // ninguém leu: perde a mais antiga

  varredura(principal_[0]);
  varredura(principal_[1]);
}

// ---------------------------------------------------------------------------------------------------------

void Spu::syncCampos(StateIO& io) {
  for (auto& z : voz_) {
    io.raw(z.dec, sizeof(z.dec));
    io.sync(z.h1); io.sync(z.h2);
    io.sync(z.atraso); io.sync(z.posEsc); io.sync(z.posLei); io.sync(z.disp);
    io.sync(z.ignoraLoopBloco);
    io.sync(z.shift); io.sync(z.filtro); io.sync(z.flags);
    for (auto& v : z.vol) { io.sync(v.reg); io.sync(v.atual); io.sync(v.div); }
    io.sync(z.pitch); io.sync(z.fase);
    io.sync(z.inicio); io.sync(z.atual); io.sync(z.repeticao);
    io.sync(z.adsr); io.sync(z.saida);
    io.sync(z.env.nivel); io.sync(z.env.div); io.sync(z.env.fase);
  }
  for (auto& v : principal_) { io.sync(v.reg); io.sync(v.atual); io.sync(v.div); }
  io.raw(ram_, sizeof(ram_));
  io.raw(regs_, sizeof(regs_));
  io.raw(aux_, sizeof(aux_));
  io.sync(status_);
  io.sync(kon_); io.sync(koff_); io.sync(fm_); io.sync(ruidoVozes_); io.sync(reverbVozes_); io.sync(endx_);
  io.sync(ctrl_);
  io.raw(volReverb_, sizeof(volReverb_)); io.raw(volCd_, sizeof(volCd_)); io.raw(volExt_, sizeof(volExt_));
  io.sync(irqEnd_); io.sync(transfEnd_); io.sync(irq_); io.sync(cwa_);
  io.sync(ruidoDiv_); io.sync(ruidoCont_); io.sync(lfsr_);
  io.sync(rvbBase_); io.sync(rvbAtual_);
  io.raw(rvbDown_, sizeof(rvbDown_)); io.raw(rvbUp_, sizeof(rvbUp_));
  io.sync(rvbPos_);
  io.raw(fifo_, sizeof(fifo_)); io.sync(fifoN_);
  io.raw(bloco_, sizeof(bloco_)); io.sync(blocoN_);
  io.sync(ocupado_); io.sync(contador_);
  // fila do CD: só o que ainda não foi tocado
  uint32_t n = (cdW_ - cdR_) & 0xFFFFFFFFu;
  if (n > CD_CAP) n = CD_CAP;
  io.sync(n);
  if (io.reading()) {
    if (n > CD_CAP) n = CD_CAP;
    cdR_ = 0;
    cdW_ = n;
    for (uint32_t i = 0; i < n; i++) io.raw(cd_[i], sizeof(cd_[i]));
  } else {
    for (uint32_t i = 0; i < n; i++) io.raw(cd_[(cdR_ + i) & (CD_CAP - 1)], sizeof(cd_[0]));
  }
}

// um estado danificado não pode levar a índices fora dos vetores nem a laços sem fim
void Spu::saneia() {
  for (auto& z : voz_) {
    z.posEsc &= 0x1F; z.posLei &= 0x1F;
    z.shift &= 0xF; z.filtro &= 0xF;
    z.fase &= 0xFFF;
    z.inicio &= RAM_MASK; z.atual &= RAM_MASK; z.repeticao &= RAM_MASK;
    if (z.env.fase > LIBERACAO) z.env.fase = LIBERACAO;
    clampTo(z.saida, -32768, 32767);
  }
  for (int c = 0; c < 2; c++) {
    clampTo(volReverb_[c], -32768, 32767);
    clampTo(volCd_[c], -32768, 32767);
    clampTo(volExt_[c], -32768, 32767);
  }
  irqEnd_ &= RAM_MASK; transfEnd_ &= RAM_MASK; cwa_ &= 0x1FF;
  rvbBase_ &= RAM_MASK; rvbAtual_ &= RAM_MASK; rvbPos_ &= 0x3F;
  if (fifoN_ > 32) fifoN_ = 32;
  if (blocoN_ > 7) blocoN_ = 7;
  clampTo(ocupado_, 0, 1 << 20);
  clampTo(contador_, 1, CICLOS_POR_AMOSTRA);
  outR_ = outW_ = 0;
}

void Spu::sync(StateIO& io) {
  try {
    syncCampos(io);
  } catch (...) {
    if (io.reading()) saneia();  // estado truncado: o que já foi lido fica dentro dos limites
    throw;
  }
  if (io.reading()) saneia();
}

}  // namespace indigo::ps1
