#!/bin/bash
# Monta os pacotes do PC: versão portátil (zip) e instalador (NSIS).
# Uso: tools/pacote-pc.sh <pasta do build Windows> <pasta de saída>
set -e
build=$(realpath "$1"); out=$(realpath -m "$2")
cd "$(dirname "$0")/.."
ver=$(head -1 VERSION)
mkdir -p "$out"; tmp=$(mktemp -d)
sed "s/@VERSAO@/$ver/g" installer/LEIA-ME.txt > "$tmp/LEIA-ME.txt"
mkdir "$tmp/Indigo-PC-$ver"; cp "$build/indigo.exe" "$tmp/LEIA-ME.txt" "$tmp/Indigo-PC-$ver/"
[ -f ../LICENSE ] && cp ../LICENSE "$tmp/Indigo-PC-$ver/LICENCA.txt"; cp TERCEIROS.md "$tmp/Indigo-PC-$ver/TERCEIROS.txt"
(cd "$tmp" && rm -f "$out/Indigo-PC-$ver.zip" && zip -qr "$out/Indigo-PC-$ver.zip" "Indigo-PC-$ver")
(cd installer && makensis -V2 -DVERSAO="$ver" -DEXE="$build/indigo.exe" -DLEIAME="$tmp/LEIA-ME.txt" \
   -DLICENCA="$(realpath ../../LICENSE)" -DTERCEIROS="$(realpath ../TERCEIROS.md)" \
   -DSAIDA="$out/Indigo-$ver-instalador.exe" indigo.nsi)
rm -rf "$tmp"
ls -la "$out/Indigo-PC-$ver.zip" "$out/Indigo-$ver-instalador.exe"
