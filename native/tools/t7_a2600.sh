#!/usr/bin/env bash
# Atari 2600: baixa as ROMs de teste e gera as telas de referência com o Stella (fora do repositório).
#   - 6502.ts (aux/2600): ROMs de teste do TIA (posicionamento, RESx/NUSIZ no meio da linha, HMOVE, estrelas...)
#     e o teste funcional do 6502 de Klaus Dormann;
#   - Gopher2600: teste do modo decimal do 6502 (Bruce Clark, versão de Klaus Dormann);
#   - 8bitworkshop: exemplos de programação do 2600 (montados aqui com o dasm);
#   - cartuchos sintéticos gerados aqui, um por esquema de bancos (F8, F6, F4, SC, FA, F0, UA, FE, E0, E7, 3F, DPC);
#   - Stella (commit fixo): compilado como núcleo libretro com RAM/CPU sem sorteio, mais um pequeno programa que
#     roda cada ROM por 300 quadros com uma sequência fixa de botões e grava o hash de cada tela (160 x linhas,
#     cores da paleta padrão do Stella). O t7_a2600 compara cada quadro do Índigo com esses hashes.
# Uso: t7_a2600.sh [pasta]   (padrão /root/indigo-env/roms/a2600)
set -e
D="${1:-/root/indigo-env/roms/a2600}"
mkdir -p "$D"
cd "$D"
clone() { # pasta url commit: busca só esse commit; se não der, o mais recente
  [ -n "$(git -C "$1" rev-parse --verify -q HEAD 2>/dev/null)" ] && return 0
  rm -rf "$1"; git init -q "$1"; git -C "$1" remote add origin "$2"
  if ! git -C "$1" fetch -q --depth 1 origin "$3" 2>/dev/null; then git -C "$1" fetch -q --depth 1 origin HEAD; fi
  git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
}
clone 6502ts https://github.com/6502ts/6502.ts 9c59f7d01e316a290480dd432a41cbab4e0238a9
clone gopher https://github.com/JetSetIlly/Gopher2600 HEAD
clone 8bw https://github.com/sehugg/8bitworkshop HEAD
clone dasm https://github.com/dasm-assembler/dasm HEAD
clone stella https://github.com/stella-emu/stella 2c3d7023e9136f75c789803b44f119d8749dc104

# dasm e os exemplos do 8bitworkshop
[ -x dasm/bin/dasm ] || make -C dasm -j4 >/dev/null 2>&1
mkdir -p 8bw-bin
for f in 8bw/presets/vcs/examples/*.a; do
  n=$(basename "$f" .a | tr -c 'A-Za-z0-9_\n' '_')
  [ -f "8bw-bin/$n.bin" ] && continue
  dasm/bin/dasm "$f" -f3 -I8bw/presets/vcs -Idasm/machines/atari2600 -o"8bw-bin/$n.bin" >/dev/null 2>&1 || rm -f "8bw-bin/$n.bin"
done

# Stella como núcleo libretro, sem sorteio de RAM/registradores/temporizador (para a referência ser repetível)
L=stella/src/os/libretro
if ! grep -q 'plr.ramrandom' $L/StellaLIBRETRO.cxx; then
  sed -i 's|  settings.setValue("speed", 1.0);|  settings.setValue("speed", 1.0);\n  settings.setValue("plr.ramrandom", false); settings.setValue("plr.cpurandom", ""); settings.setValue("plr.tv.jitter", false); settings.setValue("plr.tiadriven", false);|' $L/StellaLIBRETRO.cxx
fi
# o temporizador do RIOT também começa sorteado no Stella; aqui começa em 0, como no Índigo
sed -i 's/  myTimer = mySystem->randGenerator().next() \& 0xffU;/  myTimer = 0;/' stella/src/emucore/M6532.cxx
grep -q '  myTimer = 0;' stella/src/emucore/M6532.cxx || { echo "não consegui fixar o temporizador do Stella"; exit 1; }
# e os botões mudam no começo do quadro (o Stella espalha as mudanças pelo meio do quadro; o Índigo recebe as
# teclas uma vez por quadro, antes de rodá-lo)
sed -i 's|t.pos = myCyclesLastWindow \* (++k) / (pending + 1);|t.pos = 0; ++k;|' stella/src/emucore/Event.hxx
grep -q 't.pos = 0; ++k;' stella/src/emucore/Event.hxx || { echo "não consegui fixar a entrada do Stella"; exit 1; }
[ -f $L/stella_libretro.so ] || make -C $L -j"$(nproc)" >/dev/null 2>&1

mkdir -p ref-tools ref
cat > ref-tools/front.cpp <<'EOF'
// roda um núcleo libretro (uso: front nucleo.so rom quadros script [quadro saida.ppm]) e imprime "quadro LxA hash" de cada tela (FNV-1a 64 dos bytes B,G,R de cada ponto)
#include <dlfcn.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "libretro.h"
static std::vector<std::array<int, 3>> script;
static int frameNo = 0;
static uint32_t curMask = 0;
static int dumpFrame = -1;
static const char* dumpPath = nullptr;
static bool env(unsigned cmd, void* data) {
  if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return *(int*)data == RETRO_PIXEL_FORMAT_XRGB8888;
  if (cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY || cmd == RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY) {
    *(const char**)data = "/tmp";
    return true;
  }
  return false;
}
static void video(const void* d, unsigned w, unsigned h, size_t pitch) {
  if (!d) return;
  uint64_t f = 1469598103934665603ull;
  for (unsigned y = 0; y < h; y++)
    for (unsigned x = 0; x < w; x++) {
      uint32_t c = ((const uint32_t*)((const uint8_t*)d + y * pitch))[x] & 0xFFFFFF;
      for (int k = 0; k < 3; k++) { f ^= (c >> (8 * k)) & 0xFF; f *= 1099511628211ull; }
    }
  printf("%d %ux%u %016llx\n", frameNo, w, h, (unsigned long long)f);
  if (frameNo == dumpFrame && dumpPath) {  // opcional: grava a tela em PPM (para comparar a olho)
    FILE* o = fopen(dumpPath, "wb");
    fprintf(o, "P6 %u %u 255\n", w, h);
    for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++) {
        uint32_t c = ((const uint32_t*)((const uint8_t*)d + y * pitch))[x];
        uint8_t b[3] = {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
        fwrite(b, 1, 3, o);
      }
    fclose(o);
  }
}
static void audio(int16_t, int16_t) {}
static size_t audioBatch(const int16_t*, size_t n) { return n; }
static void poll() {}
static int16_t state(unsigned port, unsigned dev, unsigned, unsigned id) {
  if (port != 0 || dev != RETRO_DEVICE_JOYPAD) return 0;
  switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_B: return (curMask & 1) != 0;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return (curMask & 4) != 0;
    case RETRO_DEVICE_ID_JOYPAD_START: return (curMask & 8) != 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT: return (curMask & 16) != 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT: return (curMask & 32) != 0;
    case RETRO_DEVICE_ID_JOYPAD_UP: return (curMask & 64) != 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN: return (curMask & 128) != 0;
  }
  return 0;
}
int main(int argc, char** argv) {
  if (argc < 5) return 2;
  void* h = dlopen(argv[1], RTLD_NOW);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
#define S(n) auto n = (decltype(&::n))dlsym(h, #n)
  S(retro_set_environment); S(retro_set_video_refresh); S(retro_set_audio_sample); S(retro_set_audio_sample_batch);
  S(retro_set_input_poll); S(retro_set_input_state); S(retro_init); S(retro_load_game); S(retro_run);
  int n = atoi(argv[3]);
  if (argc > 6) { dumpFrame = atoi(argv[5]); dumpPath = argv[6]; }
  FILE* sf = fopen(argv[4], "r");
  int a, b, c;
  while (sf && fscanf(sf, "%d %d %d", &a, &b, &c) == 3) script.push_back({a, b, c});
  if (sf) fclose(sf);
  retro_set_environment(env); retro_set_video_refresh(video); retro_set_audio_sample(audio);
  retro_set_audio_sample_batch(audioBatch); retro_set_input_poll(poll); retro_set_input_state(state);
  retro_init();
  FILE* rf = fopen(argv[2], "rb");
  if (!rf) return 2;
  std::vector<uint8_t> rom;
  while ((c = fgetc(rf)) != EOF) rom.push_back(static_cast<uint8_t>(c));
  fclose(rf);
  retro_game_info gi{};
  gi.path = argv[2]; gi.data = rom.data(); gi.size = rom.size();
  if (!retro_load_game(&gi)) return 3;
  for (frameNo = 0; frameNo < n; frameNo++) {
    curMask = 0;
    for (auto& s : script) if (frameNo >= s[0] && frameNo <= s[1]) curMask |= s[2];
    retro_run();
  }
  return 0;
}
EOF
g++ -O2 -std=c++17 -I$L -o ref-tools/front ref-tools/front.cpp -ldl
# cartuchos sintéticos, um por esquema de bancos (o t7_a2600 confere a RAM; o Stella, a tela)
cat > ref-tools/synth.py <<'SYNTH_EOF'
#!/usr/bin/env python3
# Gera cartuchos sintéticos de teste dos esquemas de bancos do Atari 2600 (um por esquema).
# Cada cartucho troca de banco pelos pontos de acesso do esquema, lê um byte que identifica o banco (ou testa
# a RAM do cartucho), guarda os resultados na RAM do console ($80...) e mostra cada resultado como uma faixa de
# cor na tela. Grava <pasta>/<nome>.bin e <pasta>/<nome>.exp (os valores esperados na RAM, em hexadecimal).
import os
import sys


class Asm:
    def __init__(self, org):
        self.b = bytearray()
        self.org = org
        self.labels = {}
        self.fix = []

    def pc(self):
        return self.org + len(self.b)

    def label(self, n):
        self.labels[n] = self.pc()

    def emit(self, *bs):
        self.b += bytes(bs)

    def abs_(self, op, a):
        self.emit(op, a & 0xFF, (a >> 8) & 0xFF)

    def lda_i(self, v): self.emit(0xA9, v)
    def lda(self, a): self.abs_(0xAD, a)
    def sta(self, a): self.abs_(0x8D, a)
    def sta_z(self, a): self.emit(0x85, a)
    def lda_z(self, a): self.emit(0xA5, a)

    def bne(self, n):
        self.emit(0xD0, 0)
        self.fix.append((len(self.b) - 1, n))

    def bpl(self, n):
        self.emit(0x10, 0)
        self.fix.append((len(self.b) - 1, n))

    def jmp(self, n):
        self.emit(0x4C, 0, 0)
        self.fix.append((len(self.b) - 2, ('abs', n)))

    def done(self):
        for pos, n in self.fix:
            if isinstance(n, tuple):
                a = self.labels[n[1]]
                self.b[pos] = a & 0xFF
                self.b[pos + 1] = a >> 8
            else:
                d = self.labels[n] - (self.org + pos + 1)
                assert -128 <= d <= 127
                self.b[pos] = d & 0xFF
        return bytes(self.b)


def common(ops):
    """código comum (em $FC00): inicia, roda ops(asm) e mostra $80-$97 como faixas de cor"""
    a = Asm(0xFC00)
    a.emit(0x78, 0xD8, 0xA2, 0xFF, 0x9A)          # SEI CLD LDX #$FF TXS
    a.emit(0xA9, 0x00, 0xA2, 0x7F)                # LDA #0 LDX #$7F
    a.label('clr'); a.emit(0x95, 0x80, 0xCA); a.bpl('clr')   # STA $80,X DEX BPL
    ops(a)
    a.label('frame')
    # o TIA é acessado pelos espelhos $40-$7F (no 3F, escrever em $00-$3F troca o banco)
    a.lda_i(2); a.sta_z(0x41); a.sta_z(0x40)
    a.sta_z(0x42); a.sta_z(0x42); a.sta_z(0x42)
    a.lda_i(0); a.sta_z(0x40)
    a.emit(0xA2, 36); a.label('vb'); a.sta_z(0x42); a.emit(0xCA); a.bne('vb')
    a.sta_z(0x42); a.lda_i(0); a.sta_z(0x41)
    a.emit(0xA0, 0)                               # LDY #0
    a.label('row'); a.emit(0xB9, 0x80, 0x00); a.sta_z(0x49)   # LDA $0080,Y STA COLUBK
    a.emit(0xA2, 8); a.label('ln'); a.sta_z(0x42); a.emit(0xCA); a.bne('ln')
    a.emit(0xC8, 0xC0, 24); a.bne('row')          # INY CPY #24 BNE
    a.lda_i(0); a.sta_z(0x49); a.lda_i(2); a.sta_z(0x41)
    a.emit(0xA2, 30); a.label('ov'); a.sta_z(0x42); a.emit(0xCA); a.bne('ov')
    a.jmp('frame')
    code = a.done()
    assert len(code) < 0x1F0
    return code


def ident(n):  # byte que identifica o banco n (cores bem diferentes)
    return (0x16 + n * 0x22) & 0xFE


def not_sc(img):  # garante que o detector não veja Superchip (metades iguais no começo de cada 4 KB)
    for p in range(0, len(img), 4096):
        img[p + 1] ^= 0xFF
    return img


def put_vec(img, off):  # vetores de reset/IRQ em off (= endereço $1FFA da janela)
    img[off:off + 6] = bytes([0x00, 0xFC, 0x00, 0xFC, 0x00, 0xFC])


def bank4k(nbanks, hot, sc=False, extra=None, size=None):
    """esquemas de 4 KB (F8/F6/F4/FA/F0/UA/DPC): mesmo código em todos os bancos"""
    exp = []

    def ops(a):
        k = 0
        if extra:
            k = extra(a, exp)
        for b in hot:  # (endereço do ponto de acesso, banco esperado)
            if b[0] is not None:
                a.lda(b[0])
            a.lda(0x1400); a.sta_z(0x80 + k); exp.append(ident(b[1])); k += 1
        if sc:
            ram = 0x1000 if sc == 128 else 0x1000
            rd = 0x1080 if sc == 128 else 0x1100
            for i, v in enumerate([0x5A, 0xA4, 0x3C]):
                a.lda_i(v); a.sta(ram + 7 + i * 9); a.lda(rd + 7 + i * 9); a.sta_z(0x80 + k); exp.append(v); k += 1
    code = common(ops)
    img = bytearray()
    for n in range(nbanks):
        b = bytearray([ident(n)] * 4096)
        if sc:
            b[0:256] = bytes(256)          # área da RAM: duas metades iguais (assinatura do Superchip)
        else:
            b[0:128] = bytes([n + 1] * 128)
            b[128:256] = bytes([0xEE - n] * 128)
        b[0xC00:0xC00 + len(code)] = code
        put_vec(b, 0xFFA)
        img += b
    return img, exp


def fe():
    exp = []

    def ops(a):
        a.lda(0x1400); a.sta_z(0x80); exp.append(ident(0))
        a.abs_(0x20, 0xDE00)                       # JSR $DE00 (banco 1)
        a.lda(0x1400); a.sta_z(0x82); exp.append(ident(0))
        exp.insert(1, ident(1))
    code = common(ops)
    img = bytearray()
    for n in range(2):
        b = bytearray([ident(n)] * 4096)
        b[0xC00:0xC00 + len(code)] = code
        put_vec(b, 0xFFA)
        img += b
    r = Asm(0xDE00); r.lda(0x1400); r.sta_z(0x81); r.emit(0x60)
    img[4096 + 0xE00:4096 + 0xE00 + len(r.b)] = r.done()
    img[0x800:0x805] = bytes([0x20, 0x00, 0xD0, 0xC6, 0xC5])   # assinatura do FE (Decathlon)
    return not_sc(img), exp


def e0():
    exp = []

    def ops(a):
        k = 0
        for s, b in [(0, 4), (1, 5), (2, 6)]:     # bancos iniciais
            a.lda(0x1000 + s * 0x400 + 0x200); a.sta_z(0x80 + k); exp.append(ident(b)); k += 1
        for s, b in [(0, 2), (1, 0), (2, 3), (0, 6), (2, 1), (0, 0)]:
            a.lda(0x1FE0 + s * 8 + b)
            a.lda(0x1000 + s * 0x400 + 0x200); a.sta_z(0x80 + k); exp.append(ident(b)); k += 1
    code = common(ops)
    img = bytearray()
    for n in range(8):
        img += bytearray([ident(n)] * 1024)
    img[7 * 1024:7 * 1024 + len(code)] = code
    put_vec(img, 7 * 1024 + 0x3FA)
    return not_sc(img), exp


def e7():
    exp = []

    def ops(a):
        k = 0
        for b in [0, 3, 6, 1]:
            a.lda(0x1FE0 + b); a.lda(0x1200); a.sta_z(0x80 + k); exp.append(ident(b)); k += 1
        a.lda(0x1FE7)                               # RAM de 1 KB em $1000
        for i, v in enumerate([0x42, 0x99]):
            a.lda_i(v); a.sta(0x1000 + 0x123 + i * 0x100); a.lda(0x1400 + 0x123 + i * 0x100)
            a.sta_z(0x80 + k); exp.append(v); k += 1
        for rb, v in [(0, 0x31), (2, 0x77), (3, 0xC4)]:   # bancos de 256 bytes em $1800/$1900
            a.lda(0x1FE8 + rb); a.lda_i(v); a.sta(0x1810 + rb); a.lda(0x1910 + rb); a.sta_z(0x80 + k)
            exp.append(v); k += 1
        a.lda(0x1FE8); a.lda(0x1912); a.sta_z(0x80 + k); exp.append(0); k += 1
        a.lda(0x1FEA); a.lda(0x1912); a.sta_z(0x80 + k); exp.append(0x77); k += 1
        a.lda(0x1FE2); a.lda(0x1A00); a.sta_z(0x80 + k); exp.append(ident(7)); k += 1   # $1A00: banco fixo
    code = common(ops)
    img = bytearray()
    for n in range(8):
        img += bytearray([ident(n)] * 2048)
    img[7 * 2048 + 0x400:7 * 2048 + 0x400 + len(code)] = code
    put_vec(img, 7 * 2048 + 0x7FA)
    return not_sc(img), exp


def f3f():
    exp = []

    def ops(a):
        k = 0
        a.lda(0x1200); a.sta_z(0x80 + k); exp.append(ident(0)); k += 1
        for b in [2, 1, 3, 0, 2]:
            a.lda_i(b); a.sta_z(0x3F); a.lda(0x1200); a.sta_z(0x80 + k); exp.append(ident(b)); k += 1
    code = common(ops)
    img = bytearray()
    for n in range(4):
        img += bytearray([ident(n)] * 2048)
    img[3 * 2048 + 0x400:3 * 2048 + 0x400 + len(code)] = code
    put_vec(img, 3 * 2048 + 0x7FA)
    return not_sc(img), exp


def dpc():
    exp = []

    def ops(a):
        k = 0
        a.lda(0x1400); a.sta_z(0x80 + k); exp.append(ident(1)); k += 1
        a.lda(0x1FF8); a.lda(0x1400); a.sta_z(0x80 + k); exp.append(ident(0)); k += 1
        # busca 0: contador = $7FF, lê 3 bytes da área de gráficos (de trás para frente)
        a.lda_i(0xFF); a.sta(0x1050); a.lda_i(0x07); a.sta(0x1058)
        for i in range(3):
            a.lda(0x1008); a.sta_z(0x80 + k); exp.append((i * 37 + 5) & 0xFF); k += 1
        # busca 1 com máscara: topo $FD, base $FA; o contador começa em $FF
        a.lda_i(0xFD); a.sta(0x1041); a.lda_i(0xFA); a.sta(0x1049)
        a.lda_i(0xFF); a.sta(0x1051); a.lda_i(0x07); a.sta(0x1059)
        disp = lambda c: ((2047 - c) * 37 + 5) & 0xFF
        cnt = 0x7FF
        flag = 0
        for i in range(6):
            lo = cnt & 0xFF
            if lo == 0xFD: flag = 0xFF
            elif lo == 0xFA: flag = 0
            a.lda(0x1011); a.sta_z(0x80 + k); exp.append(disp(cnt) & flag); k += 1
            cnt -= 1
        # gerador aleatório: zera e lê
        a.sta(0x1070); a.lda(0x1000); a.sta_z(0x80 + k); k += 1
        r = 1
        f = [1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1]
        # cada acesso ao cartucho em $1000-$107F/$1FC0-$1FFF ou escrita gira o gerador; aqui só a própria leitura
        bit = f[((r >> 3) & 7) | (8 if r & 0x80 else 0)]
        r = ((r << 1) | bit) & 0xFF
        exp.append(r)
    code = common(ops)
    img = bytearray()
    for n in range(2):
        b = bytearray([ident(n)] * 4096)
        b[0:128] = bytes([n + 1] * 128)
        b[128:256] = bytes([0xEE - n] * 128)
        b[0xC00:0xC00 + len(code)] = code
        put_vec(b, 0xFFA)
        img += b
    img += bytes(((i * 37 + 5) & 0xFF) for i in range(2048))
    return img, exp


def ua():
    return bank4k(2, [(None, 0), (0x0240, 1), (0x0220, 0), (0x0240, 1)])


def f0():
    return bank4k(16, [(None, 15), (0x1FF0, 0), (0x1FF0, 1), (0x1FF0, 2)])


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    carts = {
        'F8': lambda: bank4k(2, [(None, 1), (0x1FF8, 0), (0x1FF9, 1), (0x1FF8, 0)]),
        'F8SC': lambda: bank4k(2, [(None, 1), (0x1FF8, 0), (0x1FF9, 1)], sc=128),
        'F6': lambda: bank4k(4, [(None, 0), (0x1FF8, 2), (0x1FF7, 1), (0x1FF9, 3), (0x1FF6, 0)]),
        'F6SC': lambda: bank4k(4, [(None, 0), (0x1FF9, 3), (0x1FF7, 1)], sc=128),
        'F4': lambda: bank4k(8, [(None, 0), (0x1FFB, 7), (0x1FF7, 3), (0x1FF5, 1), (0x1FFA, 6)]),
        'F4SC': lambda: bank4k(8, [(None, 0), (0x1FFB, 7), (0x1FF6, 2)], sc=128),
        'FA': lambda: bank4k(3, [(None, 2), (0x1FF8, 0), (0x1FF9, 1), (0x1FFA, 2)], sc=256),
        'F0': f0,
        'UA': ua,
        'FE': fe,
        'E0': e0,
        'E7': e7,
        '3F': f3f,
        'DPC': dpc,
    }
    for name, fn in carts.items():
        img, exp = fn()
        if name == 'UA':
            img[0x900:0x903] = bytes([0x8D, 0x40, 0x02])   # assinatura do UA (STA $240)
        with open(os.path.join(out, 'bank_' + name + '.bin'), 'wb') as f:
            f.write(img)
        with open(os.path.join(out, 'bank_' + name + '.exp'), 'w') as f:
            f.write(' '.join('%02x' % v for v in exp) + '\n')


main()
SYNTH_EOF
python3 -I ref-tools/synth.py synth

# botões (bits do Índigo): Reset, depois direita+botão, esquerda, cima, baixo, Select
printf '60 64 8\n90 140 17\n150 200 32\n210 240 64\n250 280 128\n285 290 4\n' > ref/script.txt

# lista de ROMs: "nome caminho"
: > ref/list.txt
for f in $(find 6502ts/aux/2600 -name '*.bin' | sort) $(ls 8bw-bin/*.bin | sort) $(ls synth/*.bin | sort); do
  n=$(echo "$f" | tr '/ ()?' '_____')
  echo "$n $f" >> ref/list.txt
done
while read -r n f; do
  f=$(echo "$f" | tr '?' ' ')
  [ -s "ref/$n.txt" ] && continue
  ./ref-tools/front $L/stella_libretro.so "$f" 300 ref/script.txt 2>/dev/null | grep -E '^[0-9]+ [0-9]+x[0-9]+ ' > "ref/$n.txt" || true
done < ref/list.txt
echo "a2600: $(wc -l < ref/list.txt) ROMs com telas de referência em $D/ref"
