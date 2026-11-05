// PlayStation (PS1). Núcleo central da etapa 9: CPU R3000A, memória e barramento, DMA, timers, interrupções,
// controles/cartões de memória (SIO0) e GPU por software. GTE, SPU, CD-ROM e MDEC entram por perifericos.h.
//
// BIOS: a do usuário (512 KB, com "Sony Computer Entertainment" no texto), por setFirmware("ps1", bytes). Nunca
// vem com o Índigo; sem ela o núcleo do disco não é criado. Programas PS-X EXE (.exe) podem ser carregados
// depois da BIOS chegar ao menu (side-load, como fazem os emuladores com programas de teste) ou, só para os
// testes, direto na RAM sem BIOS nenhuma (com um núcleo mínimo que trata as exceções).
//
// Teclas: A = Círculo, B = Xis, X = Triângulo, Y = Quadrado, L/R = L1/R1, Start/Select e o direcional. L2, R2,
// L3, R3, os direcionais analógicos e o botão ANALOG entram pelos métodos próprios abaixo; a vibração sai por
// rumbleSmall()/rumbleLarge(). Save: o cartão de memória da porta 1 (128 KB, formato .mcr/.mcd).
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/core.h"
#include "../core/state.h"

namespace indigo::ps1 {

class System;

// diz se os bytes são uma BIOS do PS1 (o app instala quando o usuário abre o arquivo)
bool isPs1Bios(const std::vector<uint8_t>& bytes);
// programa PS-X EXE
bool isPsxExe(const std::vector<uint8_t>& bytes);

class Ps1Core : public Core {
public:
  // disco (.cue/.bin/.chd/.iso); bios nulo = firmware("ps1"). Lança std::runtime_error sem BIOS válida.
  Ps1Core(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save, const std::vector<uint8_t>* bios = nullptr);
  // programa PS-X EXE: com BIOS (side-load depois do menu) ou direto sem BIOS (testes)
  enum class ExeBoot { Bios, Direct };
  Ps1Core(const std::vector<uint8_t>& exe, const std::string& name, const std::vector<uint8_t>* save,
          ExeBoot boot = ExeBoot::Bios, const std::vector<uint8_t>* bios = nullptr);
  ~Ps1Core() override;

  std::string system() const override { return "ps1"; }
  std::string title() const override { return title_; }
  void runFrame() override;
  void reset() override;
  void setKeys(uint32_t mask) override;
  void setKeys2(uint32_t mask) override;  // controle digital na porta 2
  int players() const override { return 2; }
  const uint32_t* framebuffer() const override;
  int width() const override;
  int height() const override;
  double fps() const override;
  void setSampleRate(int rate) override { rate_ = rate > 0 ? rate : 44100; }
  size_t audioAvailable() const override;
  void pullAudio(float* left, float* right, size_t n) override;
  bool hasSave() const override { return true; }
  bool saveDirty() const override;
  std::vector<uint8_t> exportSave() override;
  void importSave(const std::vector<uint8_t>& bytes) override;
  std::string note() const override;
  std::vector<uint8_t> saveState() override;
  void loadState(const std::vector<uint8_t>& state) override;

  // controle do PS1 além das teclas comuns
  void setExtraButtons(bool l2, bool r2, bool l3, bool r3);
  void setAnalog(uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);  // 0..255, centro 128
  void pressAnalogButton();                                         // liga/desliga o modo analógico
  bool analogMode() const;
  uint8_t rumbleSmall() const;                                      // 0 ou 255
  uint8_t rumbleLarge() const;                                      // 0..255

  System& sys() { return *sys_; }
  const std::string& tty() const;   // texto que o programa escreveu pela BIOS (putchar), para os testes
  cdrom::Disc* disc() { return disc_.get(); }
  // troca o disco (nullptr = sem disco) e abre/fecha a tampa do leitor
  void insertDisc(std::unique_ptr<cdrom::Disc> disc);
  void setLidOpen(bool open);
  bool lidOpen() const;
  // resolução interna da GPU: 1 (a do console), 2 ou 4. A imagem (framebuffer, width, height) sai ampliada; o
  // estado salvo e tudo o que o programa vê ficam iguais em qualquer escala. Pode trocar no meio do jogo.
  void setResolution(int scale);
  int resolution() const;

private:
  std::unique_ptr<System> sys_;
  std::unique_ptr<cdrom::Disc> disc_;
  std::string title_;
  std::vector<uint8_t> exe_;
  ExeBoot boot_ = ExeBoot::Bios;
  uint32_t keys_ = 0;
  bool l2_ = false, r2_ = false, l3_ = false, r3_ = false;
  int rate_ = 44100;
  std::deque<float> audioL_, audioR_;
  double audioPos_ = 0;
  void start();
  void updatePad();
  void sync(StateIO& io);
};

// abre a imagem e cria o núcleo com a BIOS de setFirmware("ps1"); nullptr com a mensagem em err
std::unique_ptr<Ps1Core> createPs1Core(const std::string& discPath, const std::vector<uint8_t>* save, std::string* err = nullptr);

}  // namespace indigo::ps1
