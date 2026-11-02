#!/bin/bash
# Prepara o material de teste do Game.com em /root/indigo-env/roms/gamecom (ou na pasta dada).
# Não existe BIOS livre nem suíte de teste pública do Game.com: a BIOS de teste (ROM interna e ROM do sistema)
# e o cartucho de teste são escritos pelo próprio indigo-t7_gamecom (montador mínimo de SM8500 no teste).
# Os arquivos gravados aqui servem para abrir no indigo-run/no app; o teste em si não precisa deles.
#   uso: t7_gamecom.sh [pasta] [pasta do build]
set -e
D=${1:-/root/indigo-env/roms/gamecom}
B=${2:-$(dirname "$0")/../build}
mkdir -p "$D"
"$B/indigo-t7_gamecom" gerar "$D"
ls -la "$D"
