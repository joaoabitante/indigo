// Game & Watch (Nintendo, 1980-1991) e Micro Vs. System: portáteis de LCD com CPU Sharp de 4 bits (sm510.h).
//
// Formato: o .zip do MAME do próprio usuário (gnw_*.zip), com o programa da CPU (ex.: "dk-52", "mb-108.program"),
// a ROM de melodia quando existe ("*.melody") e o desenho da tela ("gnw_*.svg"; dois nos de duas telas). O pacote
// é reconhecido pelo nome do arquivo do programa (lista em tabela.inc, gerada da lista do MAME); um pacote
// desconhecido roda com a ligação de teclas padrão abaixo se tiver só desenhos .svg (um ou dois), no máximo uma
// melodia e um único programa sem extensão (como nos .zip do MAME) ou *.program, de 0x740 bytes (SM5A) ou 4 KB
// (SM510; SM511 se tiver *.melody). Assim um .zip de outro console com arte .svg dentro não é tomado por G&W. O Índigo não traz nenhum jogo nem desenho de tela.
//
// Teclas (jogos conhecidos): direcional = direcional/botões de direção; A = botão de ação ("Jump" etc.);
// START = Game A; R = Game B; SELECT = Time; L = Alarm. Jogos de 4 botões (Flagman, Judge...) usam o
// direcional e A/B/X/Y. Micro Vs. System (2 jogadores): jogador 2 em X (cima), B (baixo), Y (esquerda),
// L (direita) e R (botão); Game B no SELECT. ACL (zera o aparelho) = reset().
// Ligação padrão de pacote desconhecido: linha 1 (S1 / R2) = Time, Game B, Game A, Alarm nos bits K1-K4;
// linha 2 = direita, cima, esquerda, baixo; linha 3 = A, B, X, Y.
//
// Tela: o .svg é rasterizado uma vez (tela.h); os segmentos acendem com o atraso do cristal líquido como no MAME
// (evita o pisca-pisca do SM5A, que varre o LCD por registradores de deslocamento). Som: o piezo (saída R).
// Save: o aparelho não tem bateria de save, fica sempre ligado; o save do Índigo é o estado inteiro da máquina
// (recorde, relógio e alarme continuam de onde pararam).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "sm510.h"
#include "tela.h"

namespace indigo::gnw {

// .zip com desenho de tela .svg e programa de CPU Sharp
bool isGnwSet(const std::vector<uint8_t>& zip);

struct Entrada {
  uint8_t porta;  // 0-8 = linha IN.n; 9 = BA; 10 = B
  uint8_t mascara;
  uint32_t tecla;  // bits de Key
  uint8_t ativoBaixo;
};

struct Jogo {
  const char* nome;
  const char* titulo;
  const char* pai;
  Cpu cpu;
  int8_t rMask;
  uint8_t rBit;     // bit da saída R ligado ao piezo
  uint8_t layout;   // 0 = uma tela, 1 = duas uma sobre a outra, 2 = lado a lado
  const char* prog;
  uint32_t progCrc;
  const char* melodia;
  uint32_t melodiaCrc;
  const char* svg[2];
  uint8_t linhas;   // linhas de teclas multiplexadas (S no SM51x, R2-R4 no SM5A)
  int8_t fixa;      // linha sempre lida (-1 = nenhuma)
  uint8_t base[11]; // valor das portas sem nada apertado
  uint16_t presentes;
  uint8_t atrasoLiga, atrasoDura;  // atraso do cristal líquido (em ms)
  Entrada entradas[16];
};

// lista dos jogos conhecidos (tabela.inc)
const std::vector<Jogo>& jogos();

class GnwCore : public Core, private SmIo {
public:
  GnwCore(const std::vector<uint8_t>& zip, const std::string& nomeArquivo, const std::vector<uint8_t>* save = nullptr);

  std::string system() const override { return "gnw"; }
  std::string title() const override { return titulo_; }
  void runFrame() override;
  void reset() override { cpu_.reset(); }
  void setKeys(uint32_t mask) override;
  const uint32_t* framebuffer() const override { return fb_.data(); }
  int width() const override { return W_; }
  int height() const override { return H_; }
  double fps() const override { return 60.0; }
  void setSampleRate(int rate) override { rate_ = rate > 0 ? rate : 48000; }
  size_t audioAvailable() const override { return audio_.size(); }
  void pullAudio(float* left, float* right, size_t n) override;
  // "save": o aparelho de verdade fica sempre ligado (recorde e relógio vivem na RAM), então o save é o estado
  // inteiro da máquina, gravado no máximo uma vez por minuto de jogo quando a RAM mudou (e sempre ao fechar)
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return quadrosDesdeSave_ >= 3600 && savePendente(); }
  bool savePendente() const override;
  std::vector<uint8_t> exportSave() override;
  void importSave(const std::vector<uint8_t>& bytes) override;
  std::string note() const override;
  std::vector<uint8_t> saveState() override;
  void loadState(const std::vector<uint8_t>& state) override;

  // testes
  Sm5xx& cpu() { return cpu_; }
  bool segmentoAceso(int zx, int y) const { return decay_[y & 31][zx & 31] >= atrasoLiga_; }
  const std::string& nomeJogo() const { return nome_; }
  int segmentosDesenhados() const { return static_cast<int>(segs_.size()); }

private:
  Sm5xx cpu_;
  Jogo cfg_{};
  std::string nome_, titulo_;
  bool conhecido_ = false;
  uint32_t idEstado_ = 0;  // identifica o jogo no save e nos estados (nome na lista; conteúdo do programa se desconhecido)
  int W_ = 0, H_ = 0;
  std::vector<uint32_t> fundo_, fb_;
  std::vector<Segmento> segs_;
  uint8_t atrasoLiga_ = 8, atrasoDura_ = 17;

  // estado do aparelho
  uint16_t mux_ = 0;
  uint8_t piezo_ = 0;
  uint32_t teclas_ = 0;
  uint8_t portas_[11] = {};
  uint16_t lcd_[32] = {};
  uint8_t decay_[32][32] = {};
  uint32_t fracTick_ = 0;  // resto de ciclos de 32768 Hz por quadro (em 60 avos)
  int lcdTick_ = 0;

  // save
  uint32_t quadrosDesdeSave_ = 0;
  uint8_t ramSalva_[128] = {};

  // som
  int rate_ = 48000;
  uint32_t fase_ = 0;
  uint32_t somaNivel_ = 0, nNivel_ = 0;
  float hpX_ = 0, hpY_ = 0;
  std::vector<float> audio_;

  uint8_t readK() override;
  int readBA() override { return (cfg_.presentes >> 9 & 1) ? (portas_[9] & 1) : 1; }
  int readB() override { return (cfg_.presentes >> 10 & 1) ? (portas_[10] & 1) : 1; }
  void writeS(uint8_t v) override { if (cfg_.cpu != Cpu::SM5A) mux_ = v; }
  void writeR(uint8_t v) override;
  void tickLcd();
  void tickSom();
  void montarTela(const std::vector<std::vector<uint8_t>>& svgs);
  void lerEstado(const std::vector<uint8_t>& st);
};

}  // namespace indigo::gnw
