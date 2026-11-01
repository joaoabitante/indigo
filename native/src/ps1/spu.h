// SPU do PlayStation 1 (Sony CXD2922/CXD2925): 24 vozes ADPCM com envelope ADSR, volume com varredura,
// interpolação gaussiana de 4 pontos, modulação de pitch (FM), ruído, 512 KB de RAM de som, IRQ por endereço,
// buffers de captura (CD e vozes 1/3), entrada do áudio do CD e reverberação a 22,05 kHz.
//
// Interface com o resto do console (convenção dos periféricos do PS1 no Índigo):
//   read/write  registradores em 0x1F801C00..0x1F801FFF, com offset relativo a 0x1F801C00 (tamanho 1, 2 ou 4);
//   run         avança em ciclos da CPU (33,8688 MHz); sai uma amostra estéreo a cada 768 ciclos (44,1 kHz);
//   onIrq       chamado na subida da IRQ9 (o controlador de interrupções guarda o pedido);
//   dmaRead/dmaWrite   canal 4 de DMA, uma palavra de 32 bits por chamada;
//   pushCdAudio pares estéreo do CD (CD-DA e XA já convertidos para 44,1 kHz), uma amostra consumida por saída;
//   take        retira as amostras produzidas (pares L,R intercalados).
//
// Referências de comportamento: documentação psx-spx (nocash) e o SPU do Mednafen/Beetle PSX, usado como régua
// em teste diferencial (tools/t9_spu.cpp), amostra por amostra.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

#include "../core/state.h"

namespace indigo::ps1 {

class Spu {
public:
  static constexpr int CICLOS_POR_AMOSTRA = 768;  // 33.868.800 / 44.100

  Spu();

  std::function<void()> onIrq;  // subida da IRQ9

  void reset();
  uint32_t read(uint32_t offset, int size);
  void write(uint32_t offset, uint32_t value, int size);
  void run(int cycles);

  // DMA (canal 4)
  uint32_t dmaRead();
  void dmaWrite(uint32_t word);
  bool dmaRequest() const;  // o SPU pede DMA (modo de transferência 2 = escrita ou 3 = leitura)

  // áudio do CD (44,1 kHz) com o volume do CD (0x1B0/0x1B2) e a reverberação do CD (SPUCNT bit 2)
  void pushCdAudio(int16_t l, int16_t r);

  // saída: pares estéreo intercalados (L, R); devolve quantos pares foram copiados
  size_t available() const { return (outW_ - outR_) & (OUT_CAP - 1); }
  size_t take(int16_t* out, size_t maxPairs);

  bool irqLine() const { return irq_; }

  void save(StateIO& io) { sync(io); }
  void load(StateIO& io) { sync(io); }
  void sync(StateIO& io);

  // para testes e depuração: RAM de som em meias palavras (256 K)
  const uint16_t* ram() const { return ram_; }

private:
  static constexpr uint32_t RAM_MASK = 0x3FFFF;  // endereços em meias palavras
  static constexpr size_t OUT_CAP = 32768;        // pares na fila de saída (potência de 2)
  static constexpr size_t CD_CAP = 8192;          // pares na fila do CD (potência de 2)

  enum : uint8_t { ATAQUE = 0, DECAIMENTO = 1, SUSTENTACAO = 2, LIBERACAO = 3 };

  // volume com varredura (das vozes e o principal)
  struct Volume {
    uint16_t reg = 0;    // registrador escrito (bit 15 = varredura)
    uint16_t atual = 0;  // volume atual (lido em 0x200.. e 0x1B8)
    uint32_t div = 0;    // acumulador do passo
  };

  struct Envelope {
    uint16_t nivel = 0;  // nível atual (0x1xC)
    uint32_t div = 0;
    uint8_t fase = ATAQUE;
  };

  struct Voz {
    int16_t dec[32] = {};  // amostras decodificadas (anel)
    int16_t h1 = 0, h2 = 0;  // as duas últimas amostras (filtro do ADPCM)
    uint8_t atraso = 0;      // amostras de espera depois do KON
    uint8_t posEsc = 0, posLei = 0;
    uint8_t disp = 0;        // amostras decodificadas ainda não consumidas
    bool ignoraLoopBloco = false;  // o programa escreveu o endereço de repetição: o flag do bloco não o muda
    uint8_t shift = 0, filtro = 0, flags = 0;  // cabeçalho do bloco atual
    Volume vol[2];
    uint16_t pitch = 0;
    uint32_t fase = 0;       // fração de 12 bits da posição
    uint32_t inicio = 0, atual = 0, repeticao = 0;
    uint32_t adsr = 0;       // os dois registradores do ADSR (0x1x8 baixo, 0x1xA alto)
    int32_t saida = 0;       // amostra depois do envelope (fonte da modulação da voz seguinte)
    Envelope env;
  };

  Voz voz_[24];
  Volume principal_[2];
  uint16_t ram_[0x40000];
  uint16_t regs_[0x100];  // 0x000..0x1FF: o que foi escrito (lido de volta nos registradores sem efeito)
  uint16_t aux_[0x10];    // 0x260..0x27F
  uint16_t status_ = 0;   // SPUSTAT (bits 0-6 e 11 atualizados a cada amostra)

  uint32_t kon_ = 0, koff_ = 0, fm_ = 0, ruidoVozes_ = 0, reverbVozes_ = 0, endx_ = 0;
  uint16_t ctrl_ = 0;
  int32_t volReverb_[2] = {}, volCd_[2] = {}, volExt_[2] = {};
  uint32_t irqEnd_ = 0, transfEnd_ = 0;
  bool irq_ = false;
  uint32_t cwa_ = 0;  // posição nos buffers de captura (0..511)

  // ruído
  uint32_t ruidoDiv_ = 0, ruidoCont_ = 0;
  uint16_t lfsr_ = 0;

  // reverberação
  uint32_t rvbBase_ = 0, rvbAtual_ = 0;
  int16_t rvbDown_[2][128] = {};  // entrada a 44,1 kHz (anel de 64, espelhado)
  int16_t rvbUp_[2][64] = {};     // saída a 22,05 kHz (anel de 32, espelhado)
  uint32_t rvbPos_ = 0;

  // transferência para a RAM (FIFO de 32 meias palavras e os tipos de 0x1AC)
  uint16_t fifo_[32] = {};
  uint8_t fifoN_ = 0;
  uint16_t bloco_[8] = {};  // tipos de transferência que repetem: grupo de 8 meias palavras
  uint8_t blocoN_ = 0;
  int32_t ocupado_ = 0;     // ciclos restantes com SPUSTAT bit 10 (transferência manual em andamento)

  int32_t contador_ = CICLOS_POR_AMOSTRA;

  // fila do CD
  int16_t cd_[CD_CAP][2];
  uint32_t cdR_ = 0, cdW_ = 0;

  // fila de saída (não vai para o save state)
  int16_t out_[OUT_CAP][2];
  size_t outR_ = 0, outW_ = 0;

  void syncCampos(StateIO& io);
  void saneia();
  uint16_t read16(uint32_t a);
  void write16(uint32_t a, uint16_t v);
  void amostra();
  void decodifica(Voz& z, int n);
  void envelope(Voz& z);
  static void varredura(Volume& v);
  void ruido();
  void reverb(const int32_t in[2], int32_t out[2]);
  void reverbCanal(int lr, int32_t entrada);
  int16_t rvbLe(uint16_t rel, int32_t extra);
  void rvbEscreve(uint16_t rel, int16_t v);
  uint32_t rvbEndereco(uint32_t rel) const;

  void verificaIrq(uint32_t a);
  void assertIrq();
  void ramEscreve(uint32_t a, uint16_t v) { verificaIrq(a); ram_[a] = v; }
  uint16_t ramLe(uint32_t a) { verificaIrq(a); return ram_[a]; }
  void transfere(uint16_t v);
  void transfereDireto(uint16_t v);
  void descarregaBloco();
  void descarregaFifo();
  int tipoTransf() const { return (regs_[0xD6] >> 1) & 7; }
  int modoTransf() const { return (ctrl_ >> 4) & 3; }
};

}  // namespace indigo::ps1
