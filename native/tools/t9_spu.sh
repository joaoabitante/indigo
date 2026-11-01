#!/bin/bash
# Prepara os testes do SPU do PlayStation em /root/indigo-env/roms/ps1spu (ou na pasta dada):
#  1. régua: o SPU do Mednafen (Beetle PSX, libretro; GPL, usado só como régua, fora do repositório) compilado
#     sozinho numa biblioteca (libspuref.so) com um arnês mínimo em C: registradores, DMA, CD, IRQ e a RAM;
#     a única mudança é tirar a atenuação de 75% que o Beetle aplica na saída para dar folga ao reamostrador;
#  2. ps1-tests (JaCzekanski): o registro de escritas no SPU de um jogo (spu/playback/spu-dump.h), tocado nos dois
#     SPUs pelo indigo-t9_spu, e os testes spu/* (que precisam do console inteiro; ficam para a integração).
# Precisa de git e gcc.
set -e
D=${1:-/root/indigo-env/roms/ps1spu}
mkdir -p "$D"
cd "$D"

pega() {  # pega <pasta> <url> <commit> [caminhos do sparse-checkout...]
  local p=$1 u=$2 c=$3
  shift 3
  if [ ! -d "$p/.git" ]; then
    git init -q "$p"
    if [ $# -gt 0 ]; then
      git -C "$p" config core.sparseCheckout true
      printf '%s\n' "$@" > "$p/.git/info/sparse-checkout"
    fi
    git -C "$p" fetch -q --depth 1 --filter=blob:none "$u" "$c"
    git -C "$p" -c advice.detachedHead=false checkout -q FETCH_HEAD
  fi
}
pega beetle https://github.com/libretro/beetle-psx-libretro 53246dc2d00f3c00cc56baa1061902da7a6651e4 \
  mednafen/psx/spu.c mednafen/psx/spu.h
pega ps1-tests https://github.com/JaCzekanski/ps1-tests f727802fead11f1daa7549285548392ef87749cb spu/

mkdir -p ref/mednafen/psx ref/inc
cp beetle/mednafen/psx/spu.h ref/mednafen/psx/spu.h
# saída sem a atenuação de 75% (o Índigo entrega a amostra cheia)
sed 's/IntermediateBuffer\[IntermediateBufferPos\]\[lr\] = (out \* 3 + 2) >> 2;/IntermediateBuffer[IntermediateBufferPos][lr] = out;/' \
  beetle/mednafen/psx/spu.c > ref/mednafen/psx/spu.c
grep -q 'IntermediateBuffer\[IntermediateBufferPos\]\[lr\] = out;' ref/mednafen/psx/spu.c

cat > ref/mednafen/mednafen-types.h <<'FIM'
/* tipos e macros mínimos do Mednafen para compilar o SPU sozinho */
#ifndef __MDFN_TYPES_H
#define __MDFN_TYPES_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define INLINE inline __attribute__((always_inline))
#define MDFN_UNLIKELY(n) __builtin_expect((n) != 0, 0)
#define MDFN_LIKELY(n) __builtin_expect((n) != 0, 1)
#endif
FIM
cat > ref/mednafen/state.h <<'FIM'
/* save state do Mednafen: o arnês não usa */
#ifndef __MDFN_STATE_H
#define __MDFN_STATE_H
#include <stdint.h>
typedef struct { int dummy; } StateMem;
typedef struct { void* v; uint32_t size; uint32_t flags; const char* name; } SFORMAT;
#define SFVAR(x) { &(x), sizeof(x), 0, #x }
#define SFARRAY16(x, n) { (x), (n) * 2, 0, #x }
#define SFARRAY32(x, n) { (x), (n) * 4, 0, #x }
#define SFEND { 0, 0, 0, 0 }
static inline int MDFNSS_StateAction(StateMem* sm, int load, int data_only, SFORMAT* sf, const char* name) {
  (void)sm; (void)load; (void)data_only; (void)sf; (void)name; return 1;
}
#endif
FIM
echo '/* vazio */' > ref/mednafen/state_helpers.h
echo '/* vazio */' > ref/inc/libretro.h
cat > ref/mednafen/psx/irq.h <<'FIM'
#ifndef REF_IRQ_H
#define REF_IRQ_H
#include <stdbool.h>
enum { IRQ_SPU = 9 };
void IRQ_Assert(int which, bool asserted);
#endif
FIM
cat > ref/mednafen/psx/cdc.h <<'FIM'
#ifndef REF_CDC_H
#define REF_CDC_H
#include <stdint.h>
void CDC_GetCDAudioSample(int32_t samples[2]);
#endif
FIM
cat > ref/glue.c <<'FIM'
/* Arnês do SPU do Mednafen para o teste diferencial do Índigo (indigo-t9_spu, por dlopen). */
#include "mednafen/psx/spu.c"

uint8_t spu_samples = 1;
bool psx_spu_silent_voice_opt = false; /* a otimização do Beetle pula vozes mudas e muda ENDX/endereços */

static int irq_linha, irq_subidas;
void IRQ_Assert(int which, bool asserted) {
  (void)which;
  if (asserted && !irq_linha) irq_subidas++;
  irq_linha = asserted;
}

/* fila do CD com as mesmas regras do Índigo: cheia descarta a mais antiga, vazia dá zero */
#define CD_CAP 8192u
static int16_t cdq[CD_CAP][2];
static uint32_t cdr, cdw;
void CDC_GetCDAudioSample(int32_t s[2]) {
  if (cdr != cdw) { s[0] = cdq[cdr % CD_CAP][0]; s[1] = cdq[cdr % CD_CAP][1]; cdr++; }
  else s[0] = s[1] = 0;
}

void ref_power(void) {
  SPU_Init(); SPU_Power();
  irq_linha = irq_subidas = 0; cdr = cdw = 0; IntermediateBufferPos = 0;
}
void ref_write(uint32_t a, uint16_t v) { SPU_Write(0, a, v); }
uint16_t ref_read(uint32_t a) { return SPU_Read(0, a); }
void ref_run(int32_t ciclos) { SPU_UpdateFromCDC(ciclos); }
uint32_t ref_take(int16_t* out, uint32_t max) {
  uint32_t n = IntermediateBufferPos < max ? IntermediateBufferPos : max, i;
  for (i = 0; i < n; i++) { out[i * 2] = IntermediateBuffer[i][0]; out[i * 2 + 1] = IntermediateBuffer[i][1]; }
  IntermediateBufferPos = 0;
  return n;
}
void ref_dma_write(uint32_t v) { SPU_WriteDMA(v); }
uint32_t ref_dma_read(void) { return SPU_ReadDMA(); }
void ref_push_cd(int16_t l, int16_t r) {
  if (cdw - cdr >= CD_CAP) cdr++;
  cdq[cdw % CD_CAP][0] = l; cdq[cdw % CD_CAP][1] = r; cdw++;
}
int ref_irq_line(void) { return irq_linha; }
int ref_irq_edges(void) { return irq_subidas; }
uint16_t ref_ram(uint32_t a) { return SPURAM[a & 0x3FFFF]; }
FIM
gcc -O2 -fPIC -shared -std=gnu99 -w -Iref -Iref/inc ref/glue.c -o libspuref.so
echo "testes do SPU do PlayStation prontos em $D"
