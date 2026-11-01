#!/bin/sh
# Prepara os testes do Atari 7800 em /root/indigo-env/roms/a7800 (usado por indigo-t7_a7800).
# Não há ROMs de teste do 7800 prontas no GitHub; então o script baixa o 7800basic (código aberto, licença
# livre) e o montador dasm, compila os dois e monta os exemplos do 7800basic (samples/*.bas), que viram .a78
# de verdade: cartuchos planos, SuperGame com e sem RAM extra, POKEY em $450, modos 160A/160B/320A,
# bancos ("banksets"), etc. Precisa de gcc, make e flex (apt-get install flex).
set -e
DEST=${1:-/root/indigo-env/roms/a7800}
SRC=${SRC7800:-/root/indigo-env/src7800}
mkdir -p "$DEST" "$SRC"
[ -d "$SRC/dasm" ] || git clone --depth 1 https://github.com/dasm-assembler/dasm "$SRC/dasm"
[ -d "$SRC/7800basic" ] || git clone --depth 1 https://github.com/7800-devtools/7800basic "$SRC/7800basic"
make -C "$SRC/dasm" >/dev/null
make -C "$SRC/7800basic" LEX=flex all >/dev/null 2>&1
B="$SRC/7800basic"
export bas7800dir="$B"
export PATH="$B:$SRC/dasm/bin:$PATH"
for d in "$B"/samples/*/; do
  n=$(basename "$d")
  [ -f "$d/$n.bas" ] || continue
  [ -f "$DEST/$n.a78" ] && continue
  (
    cd "$d"
    7800preprocess <"$n.bas" >"$n.bas.pre"
    7800basic -i "$B" -b "$n.bas" -p "$n.bas.pre" >/dev/null 2>&1 || exit 0
    7800postprocess -i "$B" >"$n.bas.asm"
    if [ -r banksetrom.asm ]; then
      dasm "$B/includes/banksetskeleton.asm" -I"$B/includes" -f3 -p20 -s"banksetrom.symbol.txt" -o"banksetrom.bin" >/dev/null 2>&1
      banksetsymbols >/dev/null 2>&1
    fi
    dasm "$n.bas.asm" -I"$B/includes" -f3 -p20 -s"$n.bas.symbol.txt" -o"$n.bas.bin" >/dev/null 2>&1 || exit 0
    7800sign -w "$n.bas.bin" >/dev/null 2>&1
    if [ -r banksetrom.asm ]; then cat banksetrom.bin >>"$n.bas.bin"; rm -f banksetrom.bin banksetrom.asm; fi
    7800header -o -f a78info.cfg "$n.bas.bin" >/dev/null 2>&1
    [ -f "$n.bas.a78" ] && cp "$n.bas.a78" "$DEST/$n.a78"
  ) || true
done
ls "$DEST" | wc -l
