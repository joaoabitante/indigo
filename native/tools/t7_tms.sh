#!/bin/sh
# Baixa e monta os programas de teste do ColecoVision / SG-1000 em /root/indigo-env/roms/tms (ou na pasta
# do 1º argumento). Usa o CVBasic e o Gasm80 de Oscar Toledo G. (licença BSD de 2 cláusulas), que geram
# cartuchos de ColecoVision (32 KB, MegaCart e Super Game Module) e de SG-1000 a partir dos exemplos que
# vêm com o compilador. Depois: indigo-t7_tms /root/indigo-env/roms/tms
set -e
DEST="${1:-/root/indigo-env/roms/tms}"
CVB_COMMIT=fd70d3d33f24951605de9405fc0630855e79c819
GASM_COMMIT=7d08ec419157a9afbdc759ea29b67d9b486e2cb2
mkdir -p "$DEST/src" "$DEST/coleco" "$DEST/sg1000"
cd "$DEST/src"
for r in CVBasic:$CVB_COMMIT gasm80:$GASM_COMMIT; do
  name=${r%%:*}; commit=${r#*:}
  if [ ! -d "$name/.git" ]; then
    git init -q "$name"
    git -C "$name" remote add origin "https://github.com/nanochess/$name.git"
  fi
  git -C "$name" fetch -q --depth 1 origin "$commit"
  git -C "$name" checkout -q FETCH_HEAD
done
make -s -C CVBasic >/dev/null 2>&1
make -s -C gasm80 >/dev/null 2>&1
CVB="$DEST/src/CVBasic/cvbasic"
GASM="$DEST/src/gasm80/gasm80"
EX="$DEST/src/CVBasic/examples"

# exemplos montados para os dois consoles
for b in demo happy_face space_attack viboritas portrait controller face_joystick music oscar plot vramcopy test1 test2; do
  (cd "$EX" && "$CVB" "$b.bas" "$DEST/src/t7tms_$b.asm" "$DEST/src/CVBasic" >/dev/null 2>&1 && "$GASM" "$DEST/src/t7tms_$b.asm" -o "$DEST/coleco/$b.rom" >/dev/null 2>&1)
  (cd "$EX" && "$CVB" --sg1000 "$b.bas" "$DEST/src/t7tms_$b.asm" "$DEST/src/CVBasic" >/dev/null 2>&1 && "$GASM" "$DEST/src/t7tms_$b.asm" -o "$DEST/sg1000/$b.rom" >/dev/null 2>&1)
done
# MegaCart (128 KB) e mapeador Sega no SG-1000
(cd "$EX" && "$CVB" bank.bas "$DEST"/src/t7tms_bank.asm "$DEST/src/CVBasic" >/dev/null 2>&1 && "$GASM" "$DEST"/src/t7tms_bank.asm -o "$DEST/coleco/bank.rom" >/dev/null 2>&1)
(cd "$EX" && "$CVB" --sg1000 bank.bas "$DEST"/src/t7tms_bank.asm "$DEST/src/CVBasic" >/dev/null 2>&1 && "$GASM" "$DEST"/src/t7tms_bank.asm -o "$DEST/sg1000/bank.rom" >/dev/null 2>&1)
# Super Game Module: RAM extra + AY-3-8910
for b in music viboritas; do
  (cd "$EX" && "$CVB" --sgm "$b.bas" "$DEST"/src/t7tms_sgm.asm "$DEST/src/CVBasic" >/dev/null 2>&1 && "$GASM" "$DEST"/src/t7tms_sgm.asm -o "$DEST/coleco/sgm_$b.rom" >/dev/null 2>&1)
done
rm -f "$DEST"/src/t7tms_*.asm

# fonte do CVBasic (caracteres 20h–7Fh) para a BIOS de teste: o CVBasic de ColecoVision copia a fonte da
# BIOS original; o teste usa uma BIOS mínima própria (só desvia para o cartucho) com esta fonte
awk '/^font_bitmaps:/{f=1;next} f&&/^[ \t]*db/{print;next} f{exit}' "$DEST/src/CVBasic/cvbasic_prologue.asm" |
  sed 's/;.*//; s/db//; s/\$//g; s/,/ /g' | tr -s ' \t' '\n' | grep -v '^$' |
  while read -r h; do printf "\\$(printf '%03o' "0x$h")"; done > "$DEST/font.bin"
ls -l "$DEST/coleco" "$DEST/sg1000" "$DEST/font.bin"
