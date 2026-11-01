// Vídeo TMS9918A (Texas Instruments), usado no ColecoVision e no SG-1000/SC-3000 (e no MSX1).
// Modos Graphics I, Graphics II (com as máscaras dos registradores 3 e 4), Multicolor, Text e os modos
// não documentados que misturam os bits M1/M2/M3; 32 sprites 8×8 ou 16×16 com ampliação, 4 por linha,
// sinal e número do 5º sprite, colisão (inclusive de sprite de cor 0), "early clock" (32 pontos para a
// esquerda), interrupção de fim de quadro e a paleta fixa de 15 cores.
//
// Temporização do acesso à VRAM: o processador só consegue ler ou gravar a VRAM nas "janelas" que o chip
// deixa livres em cada linha (posições medidas no TMS9918 real pela equipe do openMSX). Um acesso fica
// pendente até a próxima janela; se outro acesso chega antes disso, o anterior se perde (o dado é
// trocado), exatamente como no aparelho. Com a tela ligada, nos modos gráficos, a janela mais distante
// fica a 36 pontos (24 ciclos de CPU); com a tela desligada ou no retraço vertical elas são frequentes.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::tms {

class TMS9918 {
public:
  TMS9918() { reset(); }
  void reset();

  // portas: "now" é o relógio absoluto da CPU (ciclos) no momento do acesso
  int readData(int64_t now);
  void writeData(int v, int64_t now);
  int readStatus();
  void writeControl(int v, int64_t now);
  bool irq() const { return (status & 0x80) && (regs[1] & 0x20); }

  // início da linha "line" (0–261), cujo primeiro ciclo de CPU é "cycle": desenha as linhas visíveis e
  // liga o sinal de fim de quadro na linha 192
  void startLine(int line, int64_t cycle);

  void sync(StateIO& io);
  // depois de carregar um estado: os relógios do vídeo ficam perto do relógio da CPU
  void clampClock(int64_t cpuCycles) {
    clampTo<int64_t>(lineCycle, cpuCycles - 512, cpuCycles + 512);
    if (pending) clampTo<int64_t>(pendingAt, lineCycle - 512, lineCycle + 512);  // sem acesso pendente, não é usado
  }

  static constexpr int W = 256, H = 192;
  uint32_t fb[W * H];
  uint8_t vram[0x4000];
  uint8_t regs[8];
  uint32_t tooFast = 0;  // acessos perdidos por serem rápidos demais (para os testes; não vai no estado)

private:
  void render(int y);
  void spritesLine(int y, uint8_t* px);
  void flush(int64_t now);
  void schedule(int kind, int64_t now);
  int64_t accessDelay(int64_t now) const;

  int status = 0;
  int addr = 0;      // ponteiro de 14 bits
  int latchByte = 0;
  bool latch = false;
  int data = 0;      // registrador de dados (leitura antecipada / gravação), comum aos dois sentidos
  int pending = 0;   // 0 = nada, 1 = gravação, 2 = leitura antecipada
  int64_t pendingAt = 0;
  int line = 0;
  int64_t lineCycle = 0;
};

// paleta fixa do TMS9918 (0xAABBGGRR); a cor 0 (transparente) aparece como preto
extern const uint32_t TMS_PALETTE[16];

}  // namespace indigo::tms
