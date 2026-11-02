// STIC (AY-3-8900) do Intellivision: fundo de 20×12 cartões de 8×8 (modo pilha de cores ou frente/fundo,
// quadrados coloridos), 8 MOBs (sprites) com tamanho, espelho, prioridade e colisões, borda, atraso
// horizontal/vertical e extensão da borda. Cartões vêm da GROM (2 KB, do console) ou da GRAM (64 cartões).
// A imagem é feita uma vez por quadro, no começo do retraço vertical, como no jzIntv.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::intv {

class Stic {
public:
  // tela de saída em meias linhas: 176 pontos × 224 meias linhas (fundo de 160×96 com borda em volta)
  static constexpr int OUT_W = 176, OUT_H = 224;

  void reset();
  uint16_t readReg(int a);            // já com as máscaras do chip; ler $21 liga o modo pilha de cores
  void writeReg(int a, uint16_t v);   // escrever $21 liga o modo frente/fundo; $20 liga a tela neste quadro
  void render(const uint16_t* btab);  // desenha (índices de cor 0..15 em out) e calcula as colisões
  void blank();                       // tela desligada: quadro preto

  uint16_t reg[0x40] = {};
  uint8_t gmem[0x1000] = {};  // GROM em $000-$7FF e GRAM em $800-$9FF (endereços do STIC)
  uint8_t out[OUT_W * OUT_H] = {};
  bool fgbg = false;          // modo em uso neste quadro
  bool fgbgNext = false;      // modo pedido pela CPU (vale a partir do próximo quadro)
  bool enableWrite = false;   // escreveram em $20 neste retraço

  void sync(StateIO& io);

private:
  void drawBackground(const uint16_t* btab);
  void drawMobs();
  void collisions();

  // fundo 160×96 (cor e bit de primeiro plano) e a cor de fundo do último cartão de cada linha
  uint8_t bgCol_[96][160] = {};
  uint8_t bgFg_[96][160] = {};
  uint8_t lastBg_[12] = {};
  // cada MOB: até 128 meias linhas × 16 pontos (bit 15 = ponto da esquerda), já com tamanho e espelho
  uint16_t mobBmp_[8][16] = {};
  int mobRows_[8] = {};       // linhas do cartão (8 ou 16)
};

}  // namespace indigo::intv
