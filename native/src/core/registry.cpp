// Escolha do núcleo pela ROM (mesma ordem de detectSystem em web/main.js).
#include <algorithm>
#include <cctype>
#include <map>

#include "../a2600/a2600.h"
#include "../a5200/a5200.h"
#include "../a7800/a7800.h"
#include "../cps/cps.h"
#include "../dc/dc.h"
#include "../fds/fds.h"
#include "../gamecom/gamecom.h"
#include "../gb/gb.h"
#include "../gnw/gnw.h"
#include "../lynx/lynx.h"
#include "../md/md.h"
#include "../md32x/mars.h"
#include "../msx/msx.h"
#include "../n64/n64.h"
#include "../gba/gba.h"
#include "../intv/intv.h"
#include "../jaguar/jaguar.h"
#include "../nes/nes.h"
#include "../neogeo/neogeo.h"
#include "../ngp/ngp.h"
#include "../nds/nds.h"
#include "../pce/pce.h"
#include "../pm/pm.h"
#include "../ps1/ps1.h"
#include "../saturn/saturn.h"
#include "../segacd/segacd.h"
#include "../sms/sms.h"
#include "../snes/snes.h"
#include "../svision/svision.h"
#include "../tdo/tdo.h"
#include "../tms/coleco.h"
#include "../vectrex/vectrex.h"
#include "../tms/sg1000.h"
#include "../vb/vb.h"
#include "../ws/ws.h"
#include "core.h"
#include "zip.h"

namespace indigo {

namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}
bool endsWith(const std::string& s, const char* suf) {
  const std::string x(suf);
  return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}
bool hasGbLogo(const std::vector<uint8_t>& b) {
  return b.size() >= 0x150 && b[0x104] == 0xCE && b[0x105] == 0xED && b[0x106] == 0x66 && b[0x107] == 0x66;
}
}  // namespace

std::unique_ptr<Core> createCore(const std::vector<uint8_t>& rom, const std::string& fileName,
                                 const std::vector<uint8_t>* save, const std::vector<uint8_t>* gbaBios) {
  const std::string n = lower(fileName);
  // BIOS (dc_boot.bin, 2 MB, pelo nome) e flash (128 KB com "KATANA_FLASH") do Dreamcast: não são jogos; o app as
  // guarda com setFirmware("dc_boot"/"dc_flash"). Os jogos de Dreamcast vêm em disco (.gdi, .chd, .cue/.iso).
  if (dc::isDcBoot(rom, n) || dc::isDcFlash(rom, n)) return nullptr;
  // arcade: o .zip inteiro vai ao núcleo (Capcom CPS1/CPS2)
  if (zip::isZip(rom) && cps::isCpsSet(rom, n)) return std::make_unique<cps::CpsCore>(rom, n, save);
  // Neo Geo: .zip do MAME (inteiro) ou .neo; o neogeo.zip da BIOS não é jogo
  if (zip::isZip(rom) && neogeo::isNeoGeoBiosZip(rom, n)) return nullptr;
  if (neogeo::isNeoGeoRom(rom, n)) return std::make_unique<neogeo::NeoGeoCore>(rom, n, save);
  // Game & Watch: .zip do MAME com o desenho .svg da tela e o programa da CPU Sharp
  if (zip::isZip(rom) && gnw::isGnwSet(rom)) return std::make_unique<gnw::GnwCore>(rom, fileName, save);
  // Tiger Game.com: as ROMs do console não são jogo (o app as guarda com setFirmware); cartucho pelo
  // "TigerDMGC" do cabeçalho (em 0 ou $40000) ou .tgc
  if (!gamecom::gamecomFirmwareKind(rom, n).empty()) return nullptr;
  if (gamecom::isGamecomRom(rom, n)) return std::make_unique<gamecom::GameComCore>(rom, save);
  // Nintendo 64: .z64/.v64/.n64 pelos 4 primeiros bytes do cabeçalho (80 37 12 40 em qualquer ordem de bytes)
  if (n64::isN64Rom(rom, n)) return std::make_unique<n64::N64Core>(rom, fileName, save);
  if (nes::isNesRom(rom)) return std::make_unique<nes::NESCore>(rom, save);  // mapeador sem suporte: lança erro com o número
  // Famicom Disk System: a BIOS (disksys.rom, do usuário) não é jogo; o app a guarda com setFirmware("fds").
  // Discos .fds/.qd: cabeçalho fwNES ou o bloco 1 ("*NINTENDO-HVC*") logo no começo; sem a BIOS, lança o erro
  if (fds::isFdsBios(rom, n)) return nullptr;
  if (fds::isFdsDisk(rom, n)) return std::make_unique<fds::FdsCore>(rom, n, save);
  if (snes::isSnesRom(rom, n)) return std::make_unique<snes::SNESCore>(rom, save);  // chip especial sem suporte: lança erro
  if (ngp::isNgpRom(rom)) return std::make_unique<ngp::NGPCore>(rom, save);
  // PlayStation: a BIOS (512 KB) não é jogo (o app a guarda com setFirmware("ps1")); um programa PS-X EXE roda
  // depois da BIOS chegar ao menu, e sem a BIOS o núcleo não é criado (lança o erro com a explicação)
  if (ps1::isPs1Bios(rom)) return nullptr;
  if (ps1::isPsxExe(rom)) return std::make_unique<ps1::Ps1Core>(rom, fileName, save);
  // Atari Jaguar: .j64, .jag/.abs (programas para a RAM) e .rom só com a marca 0x04040404 do cartucho em 0x400
  if (jaguar::isJaguarRom(rom, n)) return std::make_unique<jaguar::JaguarCore>(rom, fileName, save);
  // BIOS do Saturn (512 KB, vetores do SH-2 e "SEGASATURN"): não é jogo; o app a guarda com setFirmware("saturn").
  // Os jogos de Saturn vêm em CD.
  if (saturn::isSaturnBios(rom)) return nullptr;
  // BIOS do Sega CD (tem o cabeçalho "SEGA" de um cartucho): não é jogo; o app a guarda com setFirmware
  if (segacd::isSegaCdBios(rom)) return nullptr;
  // 32X: .32x ou "MARS CHECK MODE"/"SEGA 32X" no cabeçalho (antes do Mega Drive: também tem "SEGA" em 0x100)
  if (md32x::isMd32xRom(rom, n)) return std::make_unique<md32x::Md32xCore>(rom, save);
  // Mega Duck: só pela extensão (.duck, .md0, .md1, .md2), já que os cartuchos não têm cabeçalho
  if (const int dk = gb::duckKind(rom, n)) return std::make_unique<gb::GBCore>(rom, save, gb::Mode::Dmg, dk);
  // Mega Drive: .md, .gen, .smd, ou "SEGA" em 0x100 (antes do Master System e do .bin do Atari 2600)
  if (md::isMdRom(rom, n)) return std::make_unique<md::MDCore>(rom, save);
  if (pm::isPMRom(rom, n)) return std::make_unique<pm::PMCore>(rom, save);
  // ColecoVision (.col, .cv; precisa da BIOS do usuário) e SG-1000/SC-3000 (.sg, .sc: vídeo TMS9918 próprio,
  // antes do Master System, que não desenha os modos TMS)
  if (pce::isPceRom(rom, n)) return pce::createPceCore(rom, n, save);  // PC Engine (.pce) e SuperGrafx (.sgx)
  if (sv::isSvRom(rom, n)) return std::make_unique<sv::SupervisionCore>(rom, save);  // Watara Supervision: .sv
  if (vb::isVbRom(rom, n)) return std::make_unique<vb::VBCore>(rom, save);  // Virtual Boy: .vb, .vboy
  if (lynx::isLynxRom(rom, n)) return std::make_unique<lynx::LynxCore>(rom, save);  // .lnx (LYNX), .lyx, .o (BS93)
  if (vectrex::isVectrexRom(rom, n)) return std::make_unique<vectrex::VectrexCore>(rom, save);  // "g GCE", .vec, .gam
  if (a7800::isA7800Rom(rom, n)) return std::make_unique<a7800::A7800Core>(rom, save);  // cabeçalho ATARI7800 ou .a78
  if (ws::isWsRom(rom, n)) return std::make_unique<ws::WSCore>(rom, save);  // WonderSwan: .ws, .wsc, .pc2
  if (tms::isColecoRom(rom, n)) return std::make_unique<tms::ColecoCore>(rom, save);
  if (tms::isSg1000Rom(rom, n)) return std::make_unique<tms::SG1000Core>(rom, save);
  // Intellivision: a EXEC e a GROM (do usuário) não são jogos; o app as guarda com setFirmware("intv_exec"/"intv_grom").
  // Cartuchos: .int/.itv, .rom do Intellicart (cabeçalho e tamanho conferidos; antes do MSX, que também usa .rom)
  // e .bin de 10 bits (antes do .bin do Atari 2600)
  if (intv::intvBiosKind(rom, n)) return nullptr;
  if (intv::isIntvRom(rom, n)) return std::make_unique<intv::IntvCore>(rom, n);
  // MSX: .mx1/.mx2, ou .rom com o cabeçalho "AB" dos cartuchos
  if (msx::isMsxRom(rom, n)) return std::make_unique<msx::MSXCore>(rom, save, msx::msxMachineFor(rom, n));
  // Nintendo DS: CRC do logotipo (0xCF56 em 0x15C) ou .nds com cabeçalho coerente. As BIOS do DS (opcionais,
  // reconhecidas pelo CRC32) não são jogo: o app as guarda com setFirmware("nds_bios9"/"nds_bios7")
  if (!nds::ndsBiosKind(rom).empty()) return nullptr;
  if (nds::isNdsRom(rom, n)) return std::make_unique<nds::NDSCore>(rom, save);
  // Atari 5200: .a52, .car de 5200, ou .bin pequeno (até 40 KB) com o vetor de partida de 5200 no fim (antes do
  // teste fraco de .bin do GBA; os .bin de Game Boy, Master System e GBA têm assinatura e ficam de fora).
  // ATENÇÃO ao juntar consoles novos que usam .bin (Intellivision, Supervision...): o teste do 5200 é
  // heurístico, então assinaturas fortes desses consoles devem vir antes desta linha.
  if (a5200::isA5200Rom(rom, n) && !hasGbLogo(rom) && sms::smsKind(rom, "").empty())
    return std::make_unique<a5200::A5200Core>(rom, n, save);
  const bool gbaLogo = rom.size() >= 0xC0 && rom[4] == 0x24 && rom[5] == 0xFF && rom[6] == 0xAE && rom[7] == 0x51;
  const bool gbaExt = endsWith(n, ".gba") || endsWith(n, ".agb");
  if (gbaLogo || (rom.size() > 0xB2 && rom[0xB2] == 0x96 && (gbaExt || endsWith(n, ".bin"))))
    return std::make_unique<gba::GBACore>(rom, save, gbaBios);
  if (hasGbLogo(rom)) return std::make_unique<gb::GBCore>(rom, save);
  const std::string sk = sms::smsKind(rom, n);
  if (!sk.empty()) return std::make_unique<sms::SMSCore>(rom, sk == "gg", save);
  if (gbaExt) return std::make_unique<gba::GBACore>(rom, save, gbaBios);
  if (rom.size() >= 0x150 && (endsWith(n, ".gbc") || endsWith(n, ".cgb") || endsWith(n, ".gb") || endsWith(n, ".sgb")))
    return std::make_unique<gb::GBCore>(rom, save);
  // Atari 2600 por último: .a26, ou .bin com tamanho e vetor de reset de cartucho de 2600
  if (a2600::isA2600Rom(rom, n)) return std::make_unique<a2600::A2600Core>(rom, save);
  // 3DO: a BIOS (do usuário) não é jogo; o app a guarda com setFirmware("3do"). Os jogos vêm em CD.
  if (tdo::isTdoBios(rom)) return nullptr;
  return nullptr;
}

bool isArcadeSet(const std::vector<uint8_t>& z, const std::string& fileName) {
  if (!zip::isZip(z)) return false;
  const std::string n = lower(fileName);
  return cps::isCpsSet(z, n) || neogeo::isNeoGeoSet(z, n) || neogeo::isNeoGeoBiosZip(z, n) || gnw::isGnwSet(z);
}

namespace {
std::map<std::string, std::vector<uint8_t>>& firmwares() {
  static std::map<std::string, std::vector<uint8_t>> m;
  return m;
}
}  // namespace

void setFirmware(const std::string& name, std::vector<uint8_t> data) { firmwares()[name] = std::move(data); }

const std::vector<uint8_t>& firmware(const std::string& name) {
  static const std::vector<uint8_t> none;
  auto it = firmwares().find(name);
  return it == firmwares().end() ? none : it->second;
}

}  // namespace indigo
