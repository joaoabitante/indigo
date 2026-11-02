#!/bin/bash
# Prepara o material de teste do Intellivision em /root/indigo-env/roms/intv (ou na pasta do 2º argumento).
# Não baixa nada: a EXEC, a GROM e os jogos do Intellivision não são livres. O teste usa a EXEC e a GROM livres
# do Índigo (src/intv/freebios.cpp) e um cartucho de teste próprio, montado em C++ (src/intv/asm1610.h) pelo
# próprio indigo-t7_intv, que grava aqui as versões .rom (Intellicart) e .int para abrir no app ou no indigo-run.
# uso: bash native/tools/t7_intv.sh pasta_do_build [pasta]   (depois: indigo-t7_intv /root/indigo-env/roms)
set -e
B=$(cd "${1:?pasta do build}" && pwd)
DEST=${2:-/root/indigo-env/roms/intv}
"$B/indigo-t7_intv" --gerar "$DEST"
ls -la "$DEST"
