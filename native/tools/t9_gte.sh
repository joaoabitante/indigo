#!/bin/bash
# Prepara o teste do GTE do PlayStation (COP2) em /root/indigo-env/roms/ps1gte (ou na pasta dada):
#  1. régua: o gte.cpp do Mednafen 0.9.37 (como veio no beetle-psx-libretro, commit e0c0e8de, antes do
#     PGXP), compilado aqui fora do repositório numa biblioteca (libgteref.so) com uma casca mínima.
#     É GPL: só serve de régua no teste diferencial, nenhum código dele entra no Índigo;
#  2. testes públicos de GTE do ps1-tests (JaCzekanski): são executáveis do console inteiro (.exe),
#     ficam baixados para a integração (9.4 com CPU/barramento/GPU).
# Depois: indigo-t9_gte /root/indigo-env/roms/ps1gte [casos]
# Precisa de git e g++.
set -e
D=${1:-/root/indigo-env/roms/ps1gte}
mkdir -p "$D"
cd "$D"

BEETLE=e0c0e8dee50e553622444966392bd6913dbae54e   # "(0.9.37-unstable) Backport over changes to gte.cpp"
if [ ! -f mdfn/gte.cpp ]; then
  rm -rf beetle mdfn
  git init -q beetle
  git -C beetle remote add origin https://github.com/libretro/beetle-psx-libretro
  git -C beetle sparse-checkout set --no-cone '/mednafen/psx/gte.cpp'
  git -C beetle fetch -q --depth 1 --filter=blob:none origin "$BEETLE"
  git -C beetle checkout -q FETCH_HEAD
  mkdir -p mdfn
  cp beetle/mednafen/psx/gte.cpp mdfn/gte.cpp
fi

# casca: tipos do Mednafen, clamp() e funções C exportadas; o GTE_StateAction (que depende do resto do
# Mednafen) é tirado com o sed
mkdir -p ref/psx
sed '/^int GTE_StateAction/,/^}/d' mdfn/gte.cpp > ref/psx/gte.cpp
cat > ref/clamp.h <<'FIM'
template <typename T, typename U, typename V>
static inline void clamp(T* v, U lo, V hi) { if (*v < lo) *v = lo; else if (*v > hi) *v = hi; }
FIM
cat > ref/ref.cpp <<'FIM'
#include <stdint.h>
#include <string.h>
typedef int8_t int8; typedef int16_t int16; typedef int32_t int32; typedef int64_t int64;
typedef uint8_t uint8; typedef uint16_t uint16; typedef uint32_t uint32; typedef uint64_t uint64;
#define INLINE inline
#define TRUE 1
#define FALSE 0
#define MDFN_NOWARN_UNUSED __attribute__((unused))
#define PSXDEV_GTE_TESTING 1
#include "psx/gte.cpp"
extern "C" {
void ref_init(void) { GTE_Init(); GTE_Power(); }
void ref_power(void) { GTE_Power(); }
int ref_cmd(uint32_t op) { return GTE_Instruction(op); }
void ref_wd(unsigned r, uint32_t v) { GTE_WriteDR(r, v); }
void ref_wc(unsigned r, uint32_t v) { GTE_WriteCR(r, v); }
uint32_t ref_rd(unsigned r) { return GTE_ReadDR(r); }
uint32_t ref_rc(unsigned r) { return GTE_ReadCR(r); }
}
FIM
g++ -O2 -fPIC -shared -w -o libgteref.so ref/ref.cpp

# testes públicos (precisam do console inteiro; ficam para a integração)
PS1T=f727802fead11f1daa7549285548392ef87749cb
if [ ! -d ps1-tests/.git ] || [ "$(git -C ps1-tests rev-parse HEAD)" != "$PS1T" ]; then
  rm -rf ps1-tests
  git init -q ps1-tests
  git -C ps1-tests remote add origin https://github.com/JaCzekanski/ps1-tests
  git -C ps1-tests sparse-checkout set --no-cone '/gte/' '/common/'
  git -C ps1-tests fetch -q --depth 1 --filter=blob:none origin "$PS1T"
  git -C ps1-tests checkout -q FETCH_HEAD
fi
echo "régua do GTE em $D/libgteref.so; ps1-tests/gte em $D/ps1-tests/gte"
