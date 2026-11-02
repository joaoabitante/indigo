#!/bin/bash
# Prepara /root/indigo-env/roms/duck para o indigo-t7_duck. As ROMs de teste do Mega Duck (demonstração com
# vídeo e som, cartuchos MD0/MD1/MD2) são montadas pelo próprio teste, que as grava nesta pasta para abrir no
# app; nenhuma ROM de terceiros é usada.
set -e
mkdir -p /root/indigo-env/roms/duck
