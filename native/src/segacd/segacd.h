// Sega CD / Mega-CD: a unidade de CD ligada ao Mega Drive pela porta de expansão.
//
// O Mega Drive (src/md) roda sem mudanças; o Sega CD entra como um "barramento externo" (md::MdExpansion) que
// responde por 000000-7FFFFF e A12000-A1202F do 68000 principal e anda junto com ele.
//
// Escrito a partir da documentação do aparelho: "Mega-CD Hardware Manual" e "Mega-CD Software Development
// Manual" da Sega (mapa de memória, registradores do gate array, Word RAM, ASIC de gráficos, protocolo do CDD),
// folha de dados do Sanyo LC8951 (CDC) e o comportamento descrito do Ricoh RF5C164 (PCM). Peças:
//   - SegaCD: o gate array (registradores, comunicação, interrupções, temporizador, cronômetro, Word RAM 2M/1M,
//     DMA do CDC, mapa de memória dos dois 68000) e o núcleo; segacd.cpp e gatearray.cpp;
//   - Lc8951: o controlador de dados do CD (registradores, buffer de 16 KB, contadores); lc8951.cpp;
//   - CdDrive: o drive (protocolo de 10 nibbles de comando/estado, cabeça de leitura a 75 setores/s, áudio);
//     cddrive.cpp;
//   - StampAsic: o circuito de rotação/escala (mapa de selos -> buffer de imagem em células); asic.cpp;
//   - Rf5c164: o chip PCM de 8 vozes, e a mistura do som do CD; pcm.cpp.
//
// Tempo: o hardware do CD conta em ciclos do cristal de 50 MHz ("SCD"; o sub-CPU de 12,5 MHz gasta 4 por ciclo).
// Uma linha do Mega Drive (3420 ciclos mestres de 53,693175 MHz em NTSC, 53,203424 MHz em PAL) vale
// 3420 * 50 / 53,69 = 3184 ciclos SCD em NTSC e 3214 em PAL (arredondado para baixo).
//
// A BIOS do aparelho é obrigatória (direitos autorais: vem do usuário, nunca do Índigo).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/core.h"
#include "../core/state.h"
#include "../md/m68k.h"
#include "../md/md.h"

namespace indigo::segacd {

// BIOS do Sega CD/Mega-CD: 128 KB com o cabeçalho "SEGA" do Mega Drive e o código da BIOS do CD
// ("BR ..." no número de série ou "BOOT ROM" no nome).
bool isSegaCdBios(const std::vector<uint8_t>& b);
// região de uma BIOS ou do disco (cabeçalho em 0x1F0): "us", "eu", "jp" ("" se não der para saber)
std::string segaCdBiosRegion(const std::vector<uint8_t>& bios);
std::string segaCdDiscRegion(cdrom::Disc& d);
// nome do firmware para guardar a BIOS: "segacd_us", "segacd_eu" ou "segacd_jp" (com a região) ou "segacd"
std::string segaCdFirmwareName(const std::vector<uint8_t>& bios);

// ------------------------------------------------------------ Ricoh RF5C164 (8 vozes, 64 KB de RAM de ondas)
class Rf5c164 {
public:
  void reset();
  // a = endereço do chip (0x0000-0x1FFF): 0x00-0x08 registradores da voz escolhida e de controle,
  // 0x10-0x1F contadores de endereço (leitura), 0x1000-0x1FFF janela de 4 KB no banco de ondas escolhido
  void write(uint32_t a, uint8_t v);
  uint8_t read(uint32_t a) const;
  void generate(int32_t& l, int32_t& r);  // uma amostra de saída (32552 Hz), ±32767
  uint32_t voicePosition(int i) const { return voice_[i & 7].pos; }  // contador de endereço 16.11
  uint8_t waveBank() const { return waveBank_; }
  std::vector<uint8_t> ram = std::vector<uint8_t>(0x10000);
  void sync(StateIO& io);

private:
  struct Voice {
    uint8_t env = 0, pan = 0xFF, startHi = 0;
    uint16_t step = 0, loop = 0;  // FD (passo 5.11) e LS (endereço de laço)
    uint32_t pos = 0;             // contador de endereço: 16 bits de endereço e 11 de fração
    uint32_t start() const { return static_cast<uint32_t>(startHi) << 19; }
  };
  Voice voice_[8];
  uint8_t selected_ = 0, waveBank_ = 0, keyOn_ = 0;  // keyOn_: bit 1 = voz ligada
  bool sounding_ = false;
};

// ------------------------------------------------------------ Sanyo LC8951 (CDC)
class Lc8951 {
public:
  static constexpr int BUFFER = 0x4000;
  enum class Effect { None, StartTransfer, StopTransfer, ChipReset };

  void reset();
  uint8_t address() const { return ar_; }
  void setAddress(uint8_t v) { ar_ = v & 0x0F; }
  Effect writeRegister(uint8_t v, int64_t now);
  uint8_t readRegister(int64_t now);
  // pino /INT (ativo): fontes habilitadas no IFCTRL e ativas no IFSTAT
  bool interruptActive(int64_t now);
  bool outputEnabled() const { return (ifstat_ & DTEN) == 0; }  // DTEN ativo: há uma transferência em curso
  // saída de dados para o gate array (o DBC conta bytes - 1; vira negativo no último)
  uint8_t outByte();
  uint16_t outWord();
  bool countDone() const { return static_cast<int16_t>(dbc_) < 0; }
  int bytesLeft() const { return static_cast<int>(dbc_) + 1; }
  void endTransfer(int64_t when);  // DTEI ativo, DTBSY/DTEN inativos
  // um setor chegou do drive: header (MSF + modo) e o setor inteiro (2352 bytes) se houver dados para gravar
  void sectorArrived(const uint8_t header[4], const uint8_t* raw, int64_t now);
  bool wantsSectorData() const { return (ctrl0_ & DECEN) && (ctrl0_ & WRRQ); }
  const std::vector<uint8_t>& buffer() const { return buf_; }
  void sync(StateIO& io);

private:
  // bits (ativos em 0 no IFSTAT, como no chip)
  static constexpr uint8_t DTEI = 0x40, DECI = 0x20, DTBSY = 0x08, DTEN = 0x02;
  static constexpr uint8_t DOUTEN = 0x02;
  static constexpr uint8_t DECEN = 0x80, AUTORQ = 0x10, WRRQ = 0x04;
  static constexpr uint8_t MODRQ = 0x08, FORMRQ = 0x04, SHDREN = 0x01;
  void expireDeci(int64_t now);
  uint8_t modeStatus() const;  // STAT2
  void put(uint32_t a, uint8_t v) { buf_[a & (BUFFER - 1)] = v; }

  std::vector<uint8_t> buf_ = std::vector<uint8_t>(BUFFER);
  uint8_t ar_ = 0, ifstat_ = 0xFF, ifctrl_ = 0, ctrl0_ = 0, ctrl1_ = 0;
  uint8_t header_[4] = {}, subheader_[4] = {}, stat0_ = 0, stat2_ = 0, stat3_ = 0x80;
  uint16_t dbc_ = 0, dac_ = 0, pt_ = 0, wa_ = 0;
  int64_t deciUntil_ = 0;  // DECI é um pulso: fica ativo até esta hora (ou até ler STAT3)
};

// ------------------------------------------------------------ drive (CDD)
class CdDrive {
public:
  void attach(cdrom::Disc* d);
  void reset();
  // o sub-CPU escreveu CS9 (envio): interpreta o comando e monta a resposta RS0-RS9
  void command();
  // um quadro do disco (1/75 s): avança a cabeça; diz se o CDC recebe um setor e qual
  struct Frame {
    bool toDecoder = false;
    uint8_t header[4] = {};
    int dataLba = -1;  // setor de dados lido nesta passagem (-1: nenhum)
  };
  Frame tick();
  void readAudio(int n, std::vector<int16_t>& out);  // n amostras estéreo de 44,1 kHz
  int tracks() const { return static_cast<int>(tracks_.size()); }
  int leadOut() const { return leadOut_; }

  uint8_t rs[10] = {}, cs[10] = {};
  bool muted = true;      // bit DM do registrador 36: sem som de CD-DA
  uint16_t fadeNow = 0x400, fadeTarget = 0x400;
  void sync(StateIO& io);

private:
  struct TrackInfo { int first, end; bool data; uint8_t mode; };
  bool dataTrack(int i) const { return i >= 0 && i < tracks() && tracks_[i].data; }
  int firstSector(int i) const { return i < 0 ? 0 : i >= tracks() ? leadOut_ : tracks_[i].first; }
  int endSector(int i) const { return i < 0 || i >= tracks() ? 0 : tracks_[i].end; }
  uint8_t idleState() const { return tracks() ? 0x09 : 0x0B; }
  void putTime(int sectors);
  void reportPosition(int kind);
  void finishReply();
  void cueAudio(int lba) { audioSample_ = static_cast<int64_t>(lba < 0 ? 0 : lba) * 588; }

  cdrom::Disc* disc_ = nullptr;
  std::vector<TrackInfo> tracks_;
  int leadOut_ = 0;
  uint8_t state_ = 0, queued_ = 0;   // estado do drive (RS0) e o que fazer quando a busca acabar
  int wait_ = 0, track_ = 0, head_ = 0, scanStep_ = 0;
  int64_t audioSample_ = 0;          // próxima amostra de CD-DA (LBA * 588 + amostra)
  uint8_t sector_[2352] = {};
  int sectorLba_ = -1;
};

// ------------------------------------------------------------ ASIC de rotação/escala
struct StampAsic {
  uint8_t config = 0;              // 58: bit 0 repetição, bit 1 selo 32x32, bit 2 mapa de 4096x4096 pontos
  bool busy = false;               // 58 bit 15
  uint16_t mapReg = 0, cellsReg = 0, imageReg = 0, offsetReg = 0, widthReg = 0, heightReg = 0, traceReg = 0;
  int64_t clk = 0;                 // início da próxima linha
  int64_t lineCycles = 1;
  uint32_t traceAddr = 0;          // próximo vetor de traço (bytes na Word RAM)
  uint32_t line = 0;               // linha atual do buffer de imagem
  void sync(StateIO& io);
};

class SegaCD : public md::MdExpansion {
public:
  SegaCD(md::MD& m, std::vector<uint8_t> bios, std::unique_ptr<cdrom::Disc> disc, bool ramCart);
  ~SegaCD() override;

private:
  struct SubBus;
  std::unique_ptr<SubBus> subBus_;
public:
  md::M68k sub;
  void reset();                      // ligar o aparelho (o MD é reiniciado à parte)
  void endFrame();                   // fim do quadro: o PCM e o CD-DA andam até o fim do quadro
  void produceAudio(size_t n);       // reamostra o som do CD do quadro em n amostras (as do Mega Drive)
  void setSampleRate(int rate) { rate_ = rate > 0 ? rate : 48000; }
  // som do CD (PCM + CD-DA) do último quadro, já na taxa de saída: n amostras somadas em l/r
  void mixAudio(float* l, float* r, size_t n);
  size_t audioQueued() const { return audL_.size() - audPos_; }
  void dropAudio(size_t n);

  // save: RAM de backup interna (8 KB) e, se houver, a do cartucho
  std::vector<uint8_t> exportSave();
  void importSave(const std::vector<uint8_t>& b);
  bool saveDirty() const { return dirty_; }
  void formatBackup();
  void sync(StateIO& io);

  // MdExpansion (68000 principal)
  uint8_t read8(uint32_t a) override;
  uint16_t read16(uint32_t a) override;
  void write8(uint32_t a, uint8_t v) override;
  void write16(uint32_t a, uint16_t v) override;
  uint16_t dmaRead(uint32_t a) override;
  void runTo(int64_t mclk) override;
  bool diskUnit() const override { return true; }

  struct SubWait {};  // acesso à Word RAM que é do principal: o sub-CPU espera (a instrução é refeita)

  const std::vector<uint8_t>& prgRam() const { return prg_; }
  const std::vector<uint8_t>& wordRam() const { return wram_; }
  const std::vector<uint8_t>& bram() const { return bram_; }
  const std::vector<uint8_t>& pcmRam() const { return pcm_.ram; }
  uint16_t reg(int a) { return subRegRead16(static_cast<uint32_t>(a) & 0x1FE); }  // registrador do sub (FF80xx)
  std::string discTitle;
  bool pal() const { return md_.pal; }

  // testes de unidade (indigo-t7_segacd): uma operação inteira do ASIC sobre a Word RAM dada (r = registradores
  // 58, 5A, 5C, 5E, 60, 62, 64, 66 e o modo de prioridade) e o chip PCM direto
  void testAsic(const uint8_t* w, const uint16_t* r);
  Rf5c164& testPcm() { return pcm_; }

private:
  // ---- barramento do sub-CPU
  uint8_t subRead8(uint32_t a);
  uint16_t subRead16(uint32_t a);
  void subWrite8(uint32_t a, uint8_t v);
  void subWrite16(uint32_t a, uint16_t v);
  struct SubBus : md::M68kBus {
    explicit SubBus(SegaCD* c) : s(c) {}
    SegaCD* s;
    uint8_t read8(uint32_t a) override { return s->subRead8(a); }
    uint16_t read16(uint32_t a) override { return s->subRead16(a); }
    void write8(uint32_t a, uint8_t v) override { s->subWrite8(a, v); }
    void write16(uint32_t a, uint16_t v) override { s->subWrite16(a, v); }
    int intAck(int level) override { s->acknowledge(level); return 24 + level; }
  };

  // ---- tempo e interrupções do sub-CPU (níveis 1 gráficos, 2 principal, 3 temporizador, 4 CDD, 5 CDC)
  int64_t now() const;               // hora do sub-CPU (ciclos SCD)
  int64_t mainNow() const;           // hora do 68000 principal em ciclos SCD
  void sync68k() { advance(mainNow()); }
  void advance(int64_t target);      // sub-CPU e periféricos até target
  void execute(int64_t stop);
  int64_t nextEvent(int64_t target) const;
  void request(int level);           // pedido de interrupção (só se o nível estiver habilitado na máscara)
  void refreshIpl();
  void acknowledge(int level);
  void cdcInterruptCheck(bool wasActive, int64_t t);

  // ---- registradores do gate array
  uint8_t mainRegRead8(uint32_t a);
  uint16_t mainRegRead16(uint32_t a);
  void mainRegWrite8(uint32_t a, uint8_t v);
  void mainRegWrite16(uint32_t a, uint16_t v);
  void mainResetBusreq(uint8_t v);   // A12001
  void mainMemoryMode(uint8_t v);    // A12003
  void mainInterrupt2();             // A12000 bit 8
  uint8_t subRegRead8(uint32_t a);
  uint16_t subRegRead16(uint32_t a);
  void subRegWrite8(uint32_t a, uint8_t v);
  void subRegWrite16(uint32_t a, uint16_t v);
  void subMemoryMode(uint8_t v);     // FF8003
  void peripheralReset();            // sub-CPU zera o bit RES0 (FF8001) ou o aparelho liga
  uint16_t fontWord(int k) const;
  uint16_t stopwatch(int64_t t) const;
  uint8_t mainModeByte() const;
  uint8_t subModeByte() const;
  uint8_t cdcModeByte() const { return static_cast<uint8_t>((edt_ << 7) | (dsr_ << 6) | dd_); }
  void setTimer(uint8_t v);
  void setMask(uint8_t v);
  void setCdcMode(uint8_t dd);
  void cdcWrite(uint8_t v);
  uint8_t cdcRead();

  // ---- Word RAM
  static uint32_t interleave(int half, uint32_t off) { return ((off >> 1) << 2) | (static_cast<uint32_t>(half) << 1) | (off & 1); }
  int subHalf() const { return ret_ ? 0 : 1; }   // 1M: metade (banco) do sub-CPU; o principal fica com a outra
  int mainHalf() const { return ret_ ? 1 : 0; }
  uint8_t wordRamMain(uint32_t a);
  void wordRamMainWrite(uint32_t a, uint8_t v);
  uint8_t blend(uint8_t old, uint8_t fresh) const;  // modos de prioridade de escrita (PM)

  // ---- DMA do CDC (gate array)
  enum Dest : uint8_t { TO_NONE = 0, TO_PCM = 1, TO_PRG = 2, TO_WRAM_A = 3, TO_WRAM_B = 4, TO_WRAM_2M = 5 };
  void beginTransfer();
  void runTransfer(int64_t t);
  void moveBytes(int n);
  int64_t transferDeadline() const;
  uint16_t hostRead(uint8_t who);    // registrador 08 lido pelo principal (2) ou pelo sub (3)
  void finishTransfer(int64_t when, bool dma);
  void driveTick();

  // ---- ASIC
  void asicStart();
  void asicRun(int64_t t);
  void asicLine();
  int64_t asicDeadline() const;

  // ---- som
  void pcmRun(int64_t t);
  void pcmWrite(uint32_t a, uint8_t v);
  uint8_t pcmRead(uint32_t a);

  md::MD& md_;
  std::vector<uint8_t> bios_, prg_, wram_, bram_, cart_;
  std::unique_ptr<cdrom::Disc> disc_;
  Lc8951 cdc_;
  CdDrive drive_;
  StampAsic asic_;
  Rf5c164 pcm_;

  // registradores (nomes do manual)
  uint8_t resetBusreq_ = 2;        // A12001: bit 0 SRES, bit 1 SBRQ (como lido de volta)
  bool ifl2_ = false;              // A12000 bit 8
  uint16_t hint_ = 0xFFFF;         // A12006
  uint8_t wp_ = 0, bank_ = 0, pm_ = 0;
  bool mode1M_ = false, ret_ = true, dmna_ = false;
  bool swapAsked_ = false;         // DMNA escrito pelo principal ainda não atendido
  uint8_t dd_ = 0, edt_ = 0, dsr_ = 0;
  uint16_t hostData_ = 0xFFFF, dmaAddr_ = 0xFFFF;
  uint8_t commMain_ = 0, commSub_ = 0;
  uint16_t commCmd_[8] = {}, commStatus_[8] = {};
  uint8_t led_ = 0, timerReload_ = 0, mask_ = 0, fontColor_ = 0;
  bool hock_ = false;
  uint16_t fontBits_ = 0;
  uint16_t subcode_[0x40] = {};

  uint8_t pending_ = 0;            // interrupções pendentes do sub-CPU (bit = nível)
  int ipl_ = 0;
  bool subHalt_ = true;            // /HALT do sub-CPU (SBRQ ou SRES)
  bool subReset_ = true;           // sub-CPU segurado em reset (lê os vetores ao soltar)
  bool subWaiting_ = false;        // sub-CPU esperando a Word RAM
  int64_t subClk_ = 0;             // hora do sub-CPU (SCD)
  int64_t timerAt_ = 0;            // próxima interrupção do temporizador (0 = parado)
  int64_t driveAt3_ = 0;           // próximo quadro do drive, em terços de ciclo SCD
  int64_t stopwatchZero_ = 0;      // hora em que o cronômetro marcou 0
  int64_t frameScd_ = 0;           // hora (SCD) do fim do último quadro de som
  Dest xfer_ = TO_NONE, xferHeld_ = TO_NONE;
  int64_t xferClk_ = 0;            // DMA do CDC: até onde já andou
  uint8_t cartProt_ = 1, cartId_ = 0;
  bool dirty_ = false;
  int scdPerLine_ = 3184;
  int64_t pcmClk_ = 0;
  std::vector<int16_t> pcmBuf_;    // amostras do PCM do quadro (L,R intercalados)
  int16_t pcmLast_[2] = {}, cddaLast_[2] = {};
  double cddaFrac_ = 0;
  float hpX_[2] = {}, hpY_[2] = {};  // passa-altas da saída
  int rate_ = 48000;
  std::vector<float> audL_, audR_;
  size_t audPos_ = 0;
};

class SegaCdCore : public Core {
public:
  // bios: 128 KB do aparelho (obrigatória); save: RAM de backup (8 KB, mais o cartucho se ramCart)
  SegaCdCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>& bios, const std::vector<uint8_t>* save,
             bool ramCart = false);
  ~SegaCdCore() override;
  md::MD& md() { return *md_; }
  SegaCD& cd() { return *cd_; }

  std::string system() const override { return "segacd"; }
  std::string title() const override { return cd_->discTitle; }
  void runFrame() override;
  void reset() override;
  void setKeys(uint32_t mask) override { md_->setKeys(mask); }
  void setKeys2(uint32_t mask) override { md_->setKeys2(mask); }
  int players() const override { return 2; }
  const uint32_t* framebuffer() const override { return md_->vdp.fb; }
  int width() const override { return md_->vdp.outW; }
  int height() const override { return md_->vdp.outH; }
  double fps() const override { return md_->fps(); }
  void setSampleRate(int rate) override { md_->setSampleRate(rate); cd_->setSampleRate(rate); }
  size_t audioAvailable() const override { return md_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override;
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return cd_->saveDirty(); }
  std::vector<uint8_t> exportSave() override { return cd_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { cd_->importSave(b); }
  std::vector<uint8_t> saveState() override;
  void loadState(const std::vector<uint8_t>& s) override;

private:
  void sync(StateIO& io);
  std::unique_ptr<md::MD> md_;
  std::unique_ptr<SegaCD> cd_;
};

// cria o núcleo com a BIOS da região do disco (firmware("segacd_us"/"segacd_eu"/"segacd_jp")), ou a de
// firmware("segacd"), ou a de outra região; sem nenhuma lança std::runtime_error com a mensagem para o usuário
std::unique_ptr<Core> createSegaCdCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save);

}  // namespace indigo::segacd
