#include "gnw.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "../core/state.h"
#include "../core/zip.h"

namespace indigo::gnw {

namespace {

const Jogo TABELA[] = {
#include "tabela.inc"
};

std::string minusculo(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool termina(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

constexpr size_t SVG_MAX = 32u * 1024 * 1024;

// o que interessa dentro do .zip
struct Pacote {
  const Jogo* jogo = nullptr;     // jogo da tabela (nulo = desconhecido)
  std::string prog, melodia;      // nomes dos arquivos
  std::vector<std::string> svgs;  // desenhos das telas, na ordem
};

bool analisar(const std::vector<uint8_t>& zip, Pacote& p) {
  std::vector<zip::Entry> es;
  if (!zip::isZip(zip) || !zip::listEntries(zip, es)) return false;
  std::vector<std::string> svgs;
  for (const auto& e : es)
    if (termina(minusculo(e.name), ".svg") && e.size > 0 && e.size <= SVG_MAX) svgs.push_back(e.name);
  if (svgs.empty()) return false;
  std::sort(svgs.begin(), svgs.end());
  auto acha = [&](const char* nome) -> const zip::Entry* {
    if (!nome) return nullptr;
    const std::string n = minusculo(nome);
    for (const auto& e : es)
      if (minusculo(e.name) == n) return &e;
    return nullptr;
  };
  // jogo conhecido: pelo nome do programa (o CRC igual desempata entre versões)
  const Jogo* melhor = nullptr;
  for (const Jogo& j : TABELA) {
    const zip::Entry* e = acha(j.prog);
    if (!e) continue;
    if (!melhor || e->crc == j.progCrc) melhor = &j;
    if (e->crc == j.progCrc) break;
  }
  if (melhor) {
    p.jogo = melhor;
    p.prog = acha(melhor->prog)->name;
    if (const zip::Entry* m = acha(melhor->melodia)) p.melodia = m->name;
    for (const char* s : melhor->svg)
      if (const zip::Entry* e = acha(s)) p.svgs.push_back(e->name);
    if (p.svgs.empty()) p.svgs = svgs;  // desenho com outro nome
    const size_t telas = melhor->svg[1] ? 2 : 1;
    if (p.svgs.size() > telas) p.svgs.resize(telas);
    return true;
  }
  // desconhecido: critério estrito, para não roubar .zip de outro console que traga arte .svg
  if (svgs.size() > 2) return false;
  for (const auto& e : es) {
    const std::string n = minusculo(e.name);
    if (!n.empty() && n.back() == '/') continue;  // pasta
    if (termina(n, ".svg")) continue;
    if (termina(n, ".melody")) {
      if (!p.melodia.empty() || e.size > 0x100) return false;
      p.melodia = e.name;
      continue;
    }
    const size_t barra = n.find_last_of('/');
    const std::string base = barra == std::string::npos ? n : n.substr(barra + 1);
    const bool semExt = base.find('.') == std::string::npos;
    if (!p.prog.empty() || !(termina(n, ".program") || semExt)) return false;  // arquivo estranho: não é G&W
    if (e.size != 0x740 && e.size != 0x1000 && !(termina(n, ".program") && e.size > 0 && e.size <= 0x1000)) return false;
    p.prog = e.name;
  }
  if (p.prog.empty()) return false;
  p.svgs = svgs;
  return true;
}

// ligação de teclas de um pacote desconhecido (ver gnw.h)
Jogo jogoPadrao(Cpu cpu) {
  Jogo j{};
  j.nome = "";
  j.titulo = "";
  j.pai = "";
  j.cpu = cpu;
  j.rMask = cpu == Cpu::SM5A ? -1 : 2;
  j.layout = 1;
  j.linhas = 3;
  j.fixa = -1;
  j.presentes = 0x7;
  j.atrasoLiga = 8;
  j.atrasoDura = 17;
  const uint32_t t[12] = {KEY_SELECT, KEY_R, KEY_START, KEY_L, KEY_RIGHT, KEY_UP, KEY_LEFT, KEY_DOWN, KEY_A, KEY_B, KEY_X, KEY_Y};
  for (int i = 0; i < 12; i++) j.entradas[i] = {static_cast<uint8_t>(i / 4), static_cast<uint8_t>(1 << (i % 4)), t[i], 0};
  return j;
}

uint32_t hashNome(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) h = (h ^ c) * 16777619u;
  return h;
}

}  // namespace

const std::vector<Jogo>& jogos() {
  static const std::vector<Jogo> v(std::begin(TABELA), std::end(TABELA));
  return v;
}

bool isGnwSet(const std::vector<uint8_t>& zip) {
  Pacote p;
  return analisar(zip, p);
}

GnwCore::GnwCore(const std::vector<uint8_t>& zip, const std::string& nomeArquivo, const std::vector<uint8_t>* save)
    : cpu_(Cpu::SM510, this) {
  Pacote p;
  if (!analisar(zip, p)) throw std::runtime_error("Este .zip não é um Game & Watch (falta o desenho .svg da tela ou o programa da CPU).");
  std::vector<uint8_t> prog, mel;
  if (!zip::extractEntry(zip, p.prog, prog) || prog.empty() || prog.size() > 0x1000)
    throw std::runtime_error("Game & Watch: o programa \"" + p.prog + "\" dentro do .zip está danificado.");
  if (!p.melodia.empty() && (!zip::extractEntry(zip, p.melodia, mel) || mel.size() > 0x100))
    throw std::runtime_error("Game & Watch: a melodia \"" + p.melodia + "\" dentro do .zip está danificada.");
  if (p.jogo) {
    cfg_ = *p.jogo;
    conhecido_ = true;
    nome_ = cfg_.nome;
    const std::string t = cfg_.titulo;
    titulo_ = t.rfind("Micro Vs.", 0) == 0 ? t : "Game & Watch: " + t;
    if ((cfg_.cpu == Cpu::SM511 || cfg_.cpu == Cpu::SM512) && mel.empty())
      throw std::runtime_error("Game & Watch: falta a melodia \"" + std::string(cfg_.melodia ? cfg_.melodia : "*.melody") + "\" no .zip.");
  } else {
    Cpu c = Cpu::SM510;
    if (prog.size() <= 0x740) c = Cpu::SM5A;
    else if (!mel.empty()) c = Cpu::SM511;
    cfg_ = jogoPadrao(c);
    std::string n = nomeArquivo;
    const size_t barra = n.find_last_of("/\\");
    if (barra != std::string::npos) n = n.substr(barra + 1);
    if (termina(minusculo(n), ".zip")) n.resize(n.size() - 4);
    nome_ = n;
    titulo_ = n.empty() ? "Game & Watch" : n;
  }
  // desconhecido: o nome do arquivo pode mudar, então o save e os estados se ligam ao conteúdo do programa
  idEstado_ = conhecido_ ? hashNome(nome_) : hashNome(std::string(prog.begin(), prog.end()) + std::string(mel.begin(), mel.end()));
  atrasoLiga_ = cfg_.atrasoLiga;
  atrasoDura_ = cfg_.atrasoDura;

  std::vector<std::vector<uint8_t>> svgs;
  for (const auto& s : p.svgs) {
    std::vector<uint8_t> d;
    if (!zip::extractEntry(zip, s, d) || d.empty() || d.size() > SVG_MAX)
      throw std::runtime_error("Game & Watch: o desenho da tela \"" + s + "\" está danificado.");
    svgs.push_back(std::move(d));
  }
  montarTela(svgs);

  cpu_.setTipo(cfg_.cpu);
  cpu_.setRom(prog, mel);
  cpu_.setRMask(cfg_.rMask);
  setKeys(0);
  cpu_.powerOn();
  if (save && !save->empty()) importSave(*save);
  for (int i = 0; i < 128; i++) ramSalva_[i] = cpu_.ram(i);
}

bool GnwCore::savePendente() const {
  for (int i = 0; i < 128; i++)
    if (cpu_.ram(i) != ramSalva_[i]) return true;
  return false;
}

std::vector<uint8_t> GnwCore::exportSave() {
  quadrosDesdeSave_ = 0;
  for (int i = 0; i < 128; i++) ramSalva_[i] = cpu_.ram(i);
  return saveState();
}

void GnwCore::importSave(const std::vector<uint8_t>& bytes) {
  // save de outra versão ou de outro jogo: o aparelho só começa do zero
  try { loadState(bytes); } catch (const std::exception&) {}
}

void GnwCore::montarTela(const std::vector<std::vector<uint8_t>>& svgs) {
  std::vector<float> ws, hs;
  for (const auto& s : svgs) {
    float w, h;
    std::string erro;
    if (!medirSvg(s, w, h, erro)) throw std::runtime_error("Game & Watch: " + erro + ".");
    ws.push_back(w);
    hs.push_back(h);
  }
  // disposição das telas (em unidades do desenho)
  const bool lado = cfg_.layout == 2 && svgs.size() > 1;
  float tw = 0, th = 0;
  for (size_t i = 0; i < svgs.size(); i++) {
    if (lado) { tw += ws[i]; th = std::max(th, hs[i]); }
    else { tw = std::max(tw, ws[i]); th += hs[i]; }
  }
  const float esc = std::min(800.0f / tw, 600.0f / th);
  W_ = std::max(16, static_cast<int>(std::ceil(tw * esc)));
  H_ = std::max(16, static_cast<int>(std::ceil(th * esc)));
  fundo_.assign(static_cast<size_t>(W_) * H_, 0xFF000000u);
  float pos = 0;
  for (size_t i = 0; i < svgs.size(); i++) {
    int ox, oy;
    if (lado) { ox = static_cast<int>(pos * esc); oy = static_cast<int>((th - hs[i]) * esc / 2); pos += ws[i]; }
    else { ox = static_cast<int>((tw - ws[i]) * esc / 2); oy = static_cast<int>(pos * esc); pos += hs[i]; }
    ox = std::clamp(ox, 0, W_ - 1);
    oy = std::clamp(oy, 0, H_ - 1);
    rasterizarSvg(svgs[i], esc, ox, oy, W_, H_, cfg_.cpu == Cpu::SM5A, fundo_, segs_);
  }
  fb_ = fundo_;
}

void GnwCore::setKeys(uint32_t mask) {
  teclas_ = mask;
  for (int p = 0; p < 11; p++) portas_[p] = cfg_.base[p];
  for (const Entrada& e : cfg_.entradas) {
    if (!e.tecla || e.porta > 10) continue;
    if (mask & e.tecla) {
      if (e.ativoBaixo) portas_[e.porta] &= ~e.mascara;
      else portas_[e.porta] |= e.mascara;
    }
  }
}

uint8_t GnwCore::readK() {
  uint8_t v = 0;
  for (int i = 0; i < cfg_.linhas && i < 9; i++)
    if (mux_ >> i & 1) v |= portas_[i];
  if (cfg_.fixa >= 0 && cfg_.fixa < 9) v |= portas_[cfg_.fixa];
  return v & 0xF;
}

void GnwCore::writeR(uint8_t v) {
  if (cfg_.cpu == Cpu::SM5A) {
    piezo_ = v & 1;  // R1 = piezo; R2-R4 = linhas de teclas
    mux_ = v >> 1;
  } else {
    piezo_ = (v >> cfg_.rBit) & 1;
  }
}

void GnwCore::tickLcd() {
  cpu_.lcdOutputs(lcd_);
  const int lim = atrasoLiga_ + atrasoDura_;
  for (int zx = 0; zx < 32; zx++)
    for (int y = 0; y < 16; y++) {
      uint8_t& d = decay_[y][zx];
      if (lcd_[zx] >> y & 1) { if (d < lim) d++; }
      else if (d > 0) d--;
    }
}

void GnwCore::tickSom() {
  somaNivel_ += piezo_;
  nNivel_++;
  fase_ += static_cast<uint32_t>(rate_);
  if (fase_ >= 32768) {
    fase_ -= 32768;
    const float x = nNivel_ ? static_cast<float>(somaNivel_) / nNivel_ : 0.0f;
    somaNivel_ = nNivel_ = 0;
    hpY_ = x - hpX_ + 0.995f * hpY_;  // tira o nível contínuo do piezo
    hpX_ = x;
    if (audio_.size() < static_cast<size_t>(rate_)) audio_.push_back(hpY_ * 0.25f);
  }
}

void GnwCore::runFrame() {
  if (quadrosDesdeSave_ < 0xFFFFFFF) quadrosDesdeSave_++;
  fracTick_ += 32768;
  const int ticks = static_cast<int>(fracTick_ / 60);
  fracTick_ %= 60;
  cpu_.run(ticks, [this] {
    tickSom();
    if (++lcdTick_ >= 32) {
      lcdTick_ = 0;
      tickLcd();
    }
  });
  std::copy(fundo_.begin(), fundo_.end(), fb_.begin());
  for (const Segmento& s : segs_)
    if (decay_[s.y][s.zx] >= atrasoLiga_) desenharSegmento(s, fb_.data(), W_);
}

void GnwCore::pullAudio(float* left, float* right, size_t n) {
  const size_t k = std::min(n, audio_.size());
  for (size_t i = 0; i < k; i++) left[i] = right[i] = audio_[i];
  for (size_t i = k; i < n; i++) left[i] = right[i] = 0;
  audio_.erase(audio_.begin(), audio_.begin() + static_cast<std::ptrdiff_t>(k));
}

std::string GnwCore::note() const {
  std::string n = conhecido_ ? "" : "Game & Watch desconhecido: CPU pelo tamanho do programa e teclas na ligação padrão. ";
  if (cfg_.linhas > 6) n += "2 jogadores: jogador 2 em X/B/Y/L (direcional) e R (botão); Game B no Select.";
  else n += "Game A = Start, Game B = R, Time = Select, Alarm = L.";
  return n;
}

std::vector<uint8_t> GnwCore::saveState() {
  StateIO s;
  uint32_t magic = 0x31574E47, h = idEstado_;  // "GNW1"
  s.sync(magic);
  s.sync(h);
  cpu_.sync(s);
  s.sync(mux_); s.sync(piezo_);
  for (auto& v : lcd_) s.sync(v);
  s.raw(decay_, sizeof decay_);
  s.sync(fracTick_); s.sync(lcdTick_);
  s.sync(fase_); s.sync(somaNivel_); s.sync(nNivel_); s.sync(hpX_); s.sync(hpY_);
  return s.data();
}

void GnwCore::loadState(const std::vector<uint8_t>& st) {
  {
    StateIO t(st);
    uint32_t magic = 0, h = 0;
    t.sync(magic);
    t.sync(h);
    if (magic != 0x31574E47 || h != idEstado_) throw std::runtime_error("Estado salvo de outro jogo.");
  }
  // estado truncado no meio: volta ao que era antes de lançar o erro
  const std::vector<uint8_t> antes = saveState();
  try {
    lerEstado(st);
  } catch (...) {
    lerEstado(antes);
    throw;
  }
}

void GnwCore::lerEstado(const std::vector<uint8_t>& st) {
  StateIO s(st);
  uint32_t magic = 0, h = 0;
  s.sync(magic);
  s.sync(h);
  cpu_.sync(s);
  s.sync(mux_); s.sync(piezo_);
  for (auto& v : lcd_) s.sync(v);
  s.raw(decay_, sizeof decay_);
  s.sync(fracTick_); s.sync(lcdTick_);
  s.sync(fase_); s.sync(somaNivel_); s.sync(nNivel_); s.sync(hpX_); s.sync(hpY_);
  // limites para estado danificado
  piezo_ &= 1;
  fracTick_ %= 60;
  clampTo(lcdTick_, 0, 31);
  fase_ %= 32768;
  if (nNivel_ > 4096) somaNivel_ = nNivel_ = 0;
  if (!std::isfinite(hpX_)) hpX_ = 0;
  if (!std::isfinite(hpY_)) hpY_ = 0;
  const int lim = atrasoLiga_ + atrasoDura_;
  for (auto& l : decay_)
    for (auto& d : l) if (d > lim) d = static_cast<uint8_t>(lim);
  setKeys(teclas_);
}

}  // namespace indigo::gnw
