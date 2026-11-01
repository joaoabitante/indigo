#!/bin/bash
# Baixa o material de teste do Vectrex para /root/indigo-env/roms/vectrex (fora do repositório):
#  - 6809Decoder (David Banks/hoglet67): tabela de ciclos do 6809 conferida em processadores de verdade
#    (src/em_6809.c), usada como régua dos ciclos de cada instrução e de cada pós-byte indexado;
#  - cputest.txt de W. Schwotzer (projeto flexemu): programa de validação do 6809 testado num EF6809P,
#    montado aqui com o asm80 (npm) depois de uma conversão de sintaxe;
#  - BIOS do Vectrex (System.bin, distribuída no repositório do vectrexy).
# uso: bash native/tools/t7_vectrex.sh [pasta]   (depois: indigo-t7_vectrex /root/indigo-env/roms/vectrex)
set -e
DEST=${1:-/root/indigo-env/roms/vectrex}
mkdir -p "$DEST"
cd "$DEST"
export GIT_TERMINAL_PROMPT=0

pega() { # pasta url commit arquivo...
  local dir=$1 url=$2 sha=$3; shift 3
  if [ ! -d "$dir/.git" ]; then
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
  fi
  git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$sha" || git -C "$dir" fetch -q --depth 1 --filter=blob:none origin HEAD
  git -C "$dir" checkout -q FETCH_HEAD -- "$@"
}

pega 6809Decoder https://github.com/hoglet67/6809Decoder 4f7e1846375024888ad7987bd390f434897953cf src/em_6809.c
pega flexemu https://github.com/aladur/flexemu f3af0305d1f326f6a9bad033368c3ec74b24709a src/tools/cputest.txt
pega vectrexy https://github.com/amaiorano/vectrexy b78d7d33c84fc91be26ca6e3b09bbfec6074b224 data/bios/System.bin
mkdir -p bios
cp vectrexy/data/bios/System.bin bios/System.bin

# montador asm80 (npm) e conversão da sintaxe Motorola clássica do cputest.txt
mkdir -p tools cputest
[ -x tools/node_modules/.bin/asm80 ] || (cd tools && npm install --silent --no-audit --no-fund asm80@1.11.14 >/dev/null)
# correção do asm80: ele codifica "[n,R]" com n pequeno no modo de 5 bits, que não tem indireção no 6809
# (o certo é o deslocamento de 8 bits); aqui o modo de 5 bits só vale sem colchetes
python3 -I - tools/node_modules/asm80/monolith.js <<'EOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
s2 = re.sub(r'(\w)<16&&\1>-17&&4!=(\w)\((\w)\)\?\(r\.lens\[(\w)\]=(\w)\(\3\)\|(\w)\|31&\1',
            lambda m: m.group(0).replace('4!=%s(%s)?' % (m.group(2), m.group(3)),
                                         '4!=%s(%s)&&!%s?' % (m.group(2), m.group(3), m.group(6))), s)
open(p, 'w').write(s2)
EOF
cat > tools/prep.py <<'EOF'
import sys, re
out, skip = [], False
for line in open(sys.argv[1], encoding='latin-1'):
    line = line.rstrip('\r\n')
    if line.startswith('*') or not line.strip():
        out.append(';' + line[1:] if line.startswith('*') else '')
        continue
    m = re.match(r'^(\S*)\s*(.*)$', line)
    label, rest = m.group(1), m.group(2)
    op = rest.split()[0].upper() if rest.split() else ''
    if op == 'IF':
        skip = True
    if skip:
        if op == 'ENDIF':
            skip = False
        continue
    if op == 'END':
        continue
    if label and not label.endswith(':') and op != 'EQU':
        label += ':'
    rest = rest.replace(',PCR', ',PC')
    rest = re.sub(r'(\s)[<>](\w+,PC)', r'\1\2', rest)
    fm = re.match(r"^FCC\s+'([^']*)',(\w+)\s*$", rest)
    if fm:
        out.append((label + ' ' if label else '       ') + 'FCC "%s"' % fm.group(1))
        out.append('       FCB ' + fm.group(2))
        continue
    out.append((label + ' ' if label else '       ') + rest)
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
EOF
python3 -I tools/prep.py flexemu/src/tools/cputest.txt cputest/ct.a09
node tools/node_modules/asm80/asm80.js -m M6809 -t hex cputest/ct.a09 >/dev/null
# régua: o vecx (emulador de referência do Vectrex, GPL; compilado só aqui, fora do repositório) roda a BIOS
# por 15 milhões de instruções e grava o estado da CPU a cada 1000 (PC A B X Y U S DP CC sem H). O teste roda
# o núcleo com as leituras/gravações de E/S no começo da instrução (como o vecx faz) e compara.
pega vecx https://github.com/jhawthorn/vecx 9b2894cacf73bdacaba32cb505289c658d981fd9 e6809.c e6809.h vecx.c vecx.h osint.h e8910.h
mkdir -p tools/vecx
cp vecx/*.c vecx/*.h tools/vecx/
sed -i 's/^static einline/einline/' tools/vecx/vecx.c tools/vecx/e6809.c
sed -i 's/^static unsigned reg_/unsigned reg_/' tools/vecx/e6809.c
cat > tools/vecx/ref.c <<'REF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vecx.h"
void e8910_write(int r, int v) { (void)r; (void)v; }
void osint_render(void) {}
extern unsigned reg_pc, reg_a, reg_b, reg_x, reg_y, reg_u, reg_s, reg_dp, reg_cc;
int main(int argc, char** argv) {
  if (argc < 3) return 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f || fread(rom, 1, 8192, f) != 8192) return 1;
  fclose(f);
  memset(cart, 0xFF, sizeof cart); /* sem cartucho: o barramento lê $FF */
  vecx_reset();
  alg_jch0 = alg_jch1 = alg_jch2 = alg_jch3 = 0x80; /* joysticks no centro */
  long n = atol(argv[2]);
  for (long i = 0; i < n; i++) {
    if (i % 1000 == 0)
      printf("%04X %02X %02X %04X %04X %04X %04X %02X %02X\n", reg_pc & 0xFFFF, reg_a & 0xFF, reg_b & 0xFF,
             reg_x & 0xFFFF, reg_y & 0xFFFF, reg_u & 0xFFFF, reg_s & 0xFFFF, reg_dp & 0xFF, reg_cc & 0xDF);
    vecx_emu(1);
  }
  return 0;
}
REF
gcc -O1 -w -o tools/vecx/ref tools/vecx/ref.c tools/vecx/e6809.c tools/vecx/vecx.c
tools/vecx/ref bios/System.bin 15000000 > vecx_trace.txt
ls -la cputest/ct.hex bios/System.bin 6809Decoder/src/em_6809.c vecx_trace.txt
