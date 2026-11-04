// Leitor de imagens de CD comum aos consoles de CD (Sega CD, PC Engine CD, Neo Geo CD, PS1, 3DO e Saturn).
// Formatos: .cue + .bin (um ou vários .bin), .iso (só dados, 2048 bytes por setor) e .chd (MAME, versão 5).
// A imagem é lida do arquivo sob demanda (não carrega o CD inteiro na memória).
//
// Endereços: LBA absoluto a partir do início do disco, sem os 2 segundos iniciais (LBA 0 = MSF 00:02:00),
// como no CD de verdade. Pregap de faixa (INDEX 00 do .cue, ou PREGAP) e POSTGAP contam no LBA; setores de
// pregap/postgap que não existem no arquivo são lidos como zero. O POSTGAP conta no tamanho (length) da faixa.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace indigo::cdrom {

constexpr int RAW_SECTOR = 2352;   // setor bruto (sincronismo + cabeçalho + dados + correção, ou áudio)
constexpr int DATA_SECTOR = 2048;  // dados do usuário em Mode 1 / Mode 2 Form 1

enum class TrackType { Audio, Mode1, Mode2 };

struct Track {
  int number = 1;             // 1..99
  TrackType type = TrackType::Mode1;
  uint32_t start = 0;         // LBA do INDEX 01 (onde a faixa começa de fato)
  uint32_t pregap = 0;        // setores de pregap antes de start (INDEX 00 até INDEX 01)
  uint32_t length = 0;        // setores de start até o fim da faixa
};

class Disc {
public:
  virtual ~Disc() = default;
  const std::vector<Track>& tracks() const { return tracks_; }
  uint32_t totalSectors() const { return total_; }  // LBA do lead-out
  // índice (0..) da faixa que contém o LBA (contando o pregap como da faixa), ou -1
  int trackAt(uint32_t lba) const;
  // setor bruto de 2352 bytes. Faixa de dados guardada em 2048 (.iso) é remontada com sincronismo e cabeçalho
  // (Mode 1, sem calcular a correção). Fora do disco: false e zeros.
  virtual bool readRaw(uint32_t lba, uint8_t* out2352) = 0;
  // os 2048 bytes de dados do usuário (Mode 1, ou Mode 2 Form 1). false fora do disco ou em faixa de áudio.
  bool readData(uint32_t lba, uint8_t* out2048);
  // nome para mostrar (nome do arquivo sem extensão)
  const std::string& name() const { return name_; }

protected:
  std::vector<Track> tracks_;
  uint32_t total_ = 0;
  std::string name_;
};

// abre .cue, .iso, .gdi (Dreamcast), .bin sozinho (vira uma faixa Mode 1 / 2352) ou .chd; nullptr com a mensagem em err.
std::unique_ptr<Disc> open(const std::string& path, std::string* err = nullptr);

// diz se o caminho parece uma imagem de CD pela extensão (.cue, .chd, .iso, .gdi)
bool isDiscPath(const std::string& path);

// LBA <-> minutos/segundos/quadros (com os 150 setores iniciais), em binário (não BCD)
inline void lbaToMsf(uint32_t lba, uint8_t& m, uint8_t& s, uint8_t& f) {
  const uint32_t a = lba + 150;
  m = uint8_t(a / (60 * 75)); s = uint8_t(a / 75 % 60); f = uint8_t(a % 75);
}
inline uint32_t msfToLba(int m, int s, int f) { return uint32_t((m * 60 + s) * 75 + f - 150); }

// Qual console é o disco (olha o primeiro setor de dados):
//   "pcfx"    -> "PC-FX:Hu_CD-ROM" no início do primeiro setor de dados (PC-FX)
//   "segacd"  -> "SEGADISCSYSTEM" ou "SEGABOOTDISC" no início do LBA 0 (Sega CD / Mega-CD)
//   "pcecd"   -> "PC Engine CD-ROM SYSTEM" no setor 1 da primeira faixa de dados (PC Engine CD)
//   "neogeocd"-> ISO 9660 com IPL.TXT na raiz (Neo Geo CD)
//   "ps1"     -> ISO 9660 com SYSTEM.CNF ou PSX.EXE na raiz, ou "Licensed by ... Sony Computer Entertainment" no
//                setor 4 (PlayStation)
//   "3do"     -> rótulo de volume do sistema de arquivos Opera no setor 0 (tipo 1, 5x 0x5A, versão 1, "CD-ROM")
//   "saturn"  -> "SEGA SEGASATURN " no início do LBA 0 (Sega Saturn)
//   "dc"      -> "SEGA SEGAKATANA " no início de uma faixa de dados (Dreamcast; .gdi, .cue ou .chd)
//   ""        -> não reconhecido
std::string detectSystem(Disc& d);

}  // namespace indigo::cdrom
