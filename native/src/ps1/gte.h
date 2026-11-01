// GTE (Geometry Transformation Engine) do PlayStation: o coprocessador 2 (COP2) do R3000A.
// Faz em ponto fixo a projeção de vértices, a iluminação e a interpolação de cores. Implementado a partir
// da documentação pública (psx-spx/nocash) e conferido contra o GTE do Mednafen num teste diferencial
// (tools/t9_gte.cpp) e contra os vetores do ps1-tests, que foram medidos num console de verdade.
//
// Registradores: 32 de dados (MFC2/MTC2/LWC2/SWC2) e 32 de controle (CFC2/CTC2). Os de 16 bits com sinal
// voltam com o sinal estendido na leitura (inclusive o H, que é sem sinal: defeito do hardware).
#pragma once
#include <cstdint>

#include "core/state.h"

namespace indigo::ps1 {

class Gte {
public:
  Gte() { reset(); }

  uint32_t readData(int r);           // MFC2/SWC2 (r = 0..31)
  void writeData(int r, uint32_t v);  // MTC2/LWC2
  uint32_t readCtrl(int r);           // CFC2
  void writeCtrl(int r, uint32_t v);  // CTC2
  int command(uint32_t op);           // COP2 imm25; devolve os ciclos aproximados do comando
  void reset();
  void save(StateIO& s);
  void load(StateIO& s);

private:
  // ---- registradores de dados
  int16_t v_[3][3];        // V0..V2 (X, Y, Z)
  uint8_t rgbc_[4];        // R, G, B, CODE
  uint16_t otz_;
  int16_t ir_[4];          // IR0..IR3
  int16_t sxy_[3][2];      // FIFO de coordenadas de tela SXY0..SXY2 (X, Y)
  uint16_t sz_[4];         // FIFO de Z SZ0..SZ3
  uint32_t rgbFifo_[3];    // FIFO de cores RGB0..RGB2
  uint32_t res1_;          // registrador 23 (sem função; guarda o que se escreve)
  int32_t mac_[4];         // MAC0..MAC3
  uint32_t lzcs_, lzcr_;
  // ---- registradores de controle
  int16_t rt_[9], llm_[9], lcm_[9];  // matrizes 3x3 em ordem de linha
  int32_t tr_[3], bk_[3], fc_[3];    // vetor de translação, cor de fundo e cor distante
  int32_t ofx_, ofy_;
  uint16_t h_;
  int16_t dqa_;
  int32_t dqb_;
  int16_t zsf3_, zsf4_;
  uint32_t flag_;

  void sync(StateIO& s);

  // passos de cálculo (gte.cpp)
  inline int64_t chk44(int i, int64_t x);
  inline int64_t chk32(int64_t x);
  inline int16_t satIR(int i, int32_t x, bool lm);
  inline uint8_t satC(int i, int32_t x);
  inline void mulMat(const int16_t* m, const int16_t* vec, const int32_t* t, int sf, bool lm);
  inline void pushColor();
  inline void depthCue(const int64_t* base, int sf, bool lm);
  inline uint32_t divide();
  inline void rtp(int i, int sf, bool lm, bool last);
  inline void normalColor(int i, int sf, bool lm, int kind);
  inline void colorStep(int sf, bool lm, int kind);
  void mvmva(uint32_t op, int sf, bool lm);
};

}  // namespace indigo::ps1
