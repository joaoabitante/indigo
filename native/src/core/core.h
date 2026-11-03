// Interface comum a todos os núcleos do Índigo nativo (espelha os comandos de web/emu-core.js:
// load, run, rate, palette, stateSave/stateLoad, saveGet/saveImport).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace indigo {

// mesma numeração de teclas da versão web (KEY em src/gba.js e src/gb/gb.js)
enum Key : uint32_t {
  KEY_A = 1u << 0, KEY_B = 1u << 1, KEY_SELECT = 1u << 2, KEY_START = 1u << 3,
  KEY_RIGHT = 1u << 4, KEY_LEFT = 1u << 5, KEY_UP = 1u << 6, KEY_DOWN = 1u << 7,
  KEY_R = 1u << 8, KEY_L = 1u << 9,
  KEY_X = 1u << 10, KEY_Y = 1u << 11,  // só no SNES
  // teclado numérico do controle (Atari 5200): vem do teclado numérico do PC; os outros núcleos ignoram
  KEY_KP0 = 1u << 16,  // KEY_KP0 + n = tecla n (0–9)
  KEY_KPSTAR = 1u << 26, KEY_KPHASH = 1u << 27,
};

class Core {
public:
  virtual ~Core() = default;

  // sistema: "gb", "gbc", "gba", "gg", "sms", "ngp", "ngpc", "nes", "fds"...
  virtual std::string system() const = 0;
  virtual std::string title() const = 0;

  virtual void runFrame() = 0;
  virtual void reset() = 0;
  virtual void setKeys(uint32_t mask) = 0;   // bits de Key, 1 = apertado

  // vídeo: pixels 0xAABBGGRR (bytes R, G, B, A na memória), como o Uint32Array da versão web
  virtual const uint32_t* framebuffer() const = 0;
  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual double fps() const = 0;

  // som estéreo em float, na taxa pedida
  virtual void setSampleRate(int rate) = 0;
  virtual size_t audioAvailable() const = 0;
  virtual void pullAudio(float* left, float* right, size_t n) = 0;

  // save do cartucho (bateria)
  virtual bool hasSave() const = 0;
  virtual bool saveDirty() const = 0;
  // mudança ainda não gravada, mesmo que saveDirty() a esteja segurando para gravar de tempos em tempos
  // (vale ao fechar, trocar de jogo ou perder o foco)
  virtual bool savePendente() const { return saveDirty(); }
  virtual std::vector<uint8_t> exportSave() = 0;
  virtual void importSave(const std::vector<uint8_t>& bytes) = 0;

  // paleta do Game Boy clássico (4 cores 0xAABBGGRR cada); obj0/obj1 nulos = mesmas cores do fundo
  virtual void setDmgPalette(const uint32_t* bg, const uint32_t* obj0, const uint32_t* obj1) {}

  // caneta/toque na tela do jogo, em pixels da imagem do núcleo (só consoles com tela de toque, ex.: Game.com)
  virtual void setPointer(int x, int y, bool down) {}
  // mouse relativo (consoles com mouse, ex.: PC-FX): movimento em pixels da imagem e botões (bit 0 = esquerdo,
  // bit 1 = direito)
  virtual void setMouse(int dx, int dy, uint32_t buttons) {}

  // observação para mostrar ao abrir o jogo (por exemplo, como roda um chip do cartucho); vazio = nada
  virtual std::string note() const { return ""; }

  // discos com lados (Famicom Disk System): quantos lados há (0 = o console não tem disco), qual está no drive
  // (-1 = nenhum) e a troca: setDiskSide ejeta o disco e põe o lado pedido um instante depois (-1 = só ejeta)
  virtual int diskSides() const { return 0; }
  virtual int diskSide() const { return -1; }
  virtual void setDiskSide(int side) {}

  // save state
  virtual std::vector<uint8_t> saveState() = 0;
  virtual void loadState(const std::vector<uint8_t>& state) = 0;  // lança std::runtime_error se incompatível
  // chamado pelo app depois de carregar um estado: relógios de cartucho voltam para a hora do aparelho
  virtual void followHostClock() {}
};

// escolhe o núcleo pelo conteúdo/extensão da ROM; devolve nullptr se não reconhecer.
// gbaBios: BIOS original do GBA (opcional, 16 KB); sem ela o núcleo usa a BIOS própria (HLE)
std::unique_ptr<Core> createCore(const std::vector<uint8_t>& rom, const std::string& fileName,
                                 const std::vector<uint8_t>* save, const std::vector<uint8_t>* gbaBios = nullptr);

// jogos de arcade no formato do MAME (.zip com várias ROMs): o app entrega o .zip inteiro ao createCore em vez
// de extrair um arquivo. nomeArquivo = nome do .zip (ex.: "sf2.zip").
bool isArcadeSet(const std::vector<uint8_t>& zip, const std::string& nomeArquivo);

// arquivos de chips que o usuário fornece, como a BIOS do GBA ("dsp1" = programa do DSP-1 do SNES, 8 KB).
// Valem para os núcleos criados depois; vazio = o núcleo usa a imitação própria.
void setFirmware(const std::string& name, std::vector<uint8_t> data);
const std::vector<uint8_t>& firmware(const std::string& name);

}  // namespace indigo
