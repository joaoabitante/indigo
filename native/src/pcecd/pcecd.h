// PC Engine CD-ROM² / Super CD-ROM² / Arcade CD-ROM²: o PC Engine (src/pce) com o System Card no lugar do
// HuCard e a interface do CD (cdunit.h) ligada pelo ponto de extensão.
//
// BIOS: o System Card (1.0/2.0/2.1 = CD-ROM², 3.0 = Super CD-ROM², e o 3.0 também serve ao Arcade Card) é do
// usuário, entra por setFirmware("pcecd", bytes) e nunca vem com o Índigo. Sem ele o núcleo não é criado.
// Reconhecido pelo tamanho (256 KB; 512 KB nos dumps com espelho; mais 512 bytes de cabeçalho de copiadora) e
// pelo texto "PC Engine CD-ROM SYSTEM" na ROM (também nos dumps americanos com os bits invertidos). O modelo
// sai do CRC32 dos System Cards conhecidos; um System Card desconhecido roda como Arcade CD-ROM² (tudo ligado).
//
// Save: a BRAM de 2 KB (banco $F7). Imagem e teclas como no PC Engine (pce.h).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/core.h"
#include "../pce/pce.h"
#include "cdunit.h"

namespace indigo::pcecd {

// diz se os bytes são um System Card (o app instala quando o usuário abre o arquivo)
bool isPceCdBios(const std::vector<uint8_t>& bytes);
// os 256 KB do System Card (sem cabeçalho, sem espelho, bits na ordem certa); vazio se não for um
std::vector<uint8_t> normalizeBios(const std::vector<uint8_t>& bytes);
// modelo pelo System Card (CRC32 dos conhecidos); bytes já normalizados
Model modelForBios(const std::vector<uint8_t>& bios256k);

class PceCdCore : public Core {
public:
  // bios nulo = firmware("pcecd"); modelo automático pela BIOS. Lança std::runtime_error sem BIOS válida.
  PceCdCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save,
            const std::vector<uint8_t>* bios = nullptr, const Model* model = nullptr);
  ~PceCdCore() override;

  pce::Pce& pce() { return *pce_; }
  CdUnit& cd() { return *cd_; }
  cdrom::Disc& disc() { return *disc_; }

  std::string system() const override { return "pcecd"; }
  std::string title() const override { return disc_->name(); }
  void runFrame() override;
  void reset() override;
  void setKeys(uint32_t mask) override { pce_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return pce_->video(); }
  int width() const override { return pce_->width(); }
  int height() const override { return pce_->height(); }
  double fps() const override { return pce_->fps(); }
  void setSampleRate(int rate) override { pce_->setSampleRate(rate); cd_->setSampleRate(rate); }
  size_t audioAvailable() const override;
  void pullAudio(float* l, float* r, size_t n) override;
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return pce_->saveDirty(); }
  std::vector<uint8_t> exportSave() override { return pce_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { pce_->importSave(b); }
  std::string note() const override;
  std::vector<uint8_t> saveState() override;
  void loadState(const std::vector<uint8_t>& s) override;

private:
  std::unique_ptr<cdrom::Disc> disc_;
  std::unique_ptr<pce::Pce> pce_;
  std::unique_ptr<CdUnit> cd_;
  std::vector<float> tmpL_, tmpR_;
  void sync(StateIO& io);
};

// abre a imagem (.cue/.iso/.chd) e cria o núcleo com o System Card de setFirmware("pcecd").
// nullptr com a mensagem em err (imagem que não abre, ou falta a BIOS).
std::unique_ptr<PceCdCore> createPceCdCore(const std::string& discPath, const std::vector<uint8_t>* save,
                                           std::string* err = nullptr);

}  // namespace indigo::pcecd
