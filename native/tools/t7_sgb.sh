#!/bin/bash
# Prepara o teste do Super Game Boy em /root/indigo-env/roms/sgb (ou na pasta do 1º argumento):
#  - sgb_teste.gb: ROM gerada por t7_sgb_rom.py (código próprio do Índigo, sem material de terceiros) que manda
#    pacotes SGB pelo joypad e faz as transferências pela imagem (PAL_TRN, ATTR_TRN, CHR_TRN, PCT_TRN).
# Depois: indigo-t7_sgb <pasta>
set -e
DEST="${1:-/root/indigo-env/roms/sgb}"
mkdir -p "$DEST"
python3 -I "$(dirname "$0")/t7_sgb_rom.py" "$DEST/sgb_teste.gb"
