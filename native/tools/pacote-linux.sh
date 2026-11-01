#!/bin/bash
# Monta o pacote do Linux (PC x86_64 e Steam Deck): Indigo-Linux-<versão>-x86_64.tar.gz com o executável, o SDL3
# em lib/ (o executável procura em $ORIGIN/lib), licenças, LEIA-ME, atalho .desktop e ícone.
# Uso: tools/pacote-linux.sh <pasta do build Linux> <pasta de saída>
#   o build é o do app com o SDL3 compartilhado: cmake -S native -B <build> -DCMAKE_PREFIX_PATH=<sdl3-linux>
# Com o appimagetool no PATH, também gera Indigo-Linux-<versão>-x86_64.AppImage.
set -euo pipefail
build=$(realpath "$1"); out=$(realpath -m "$2")
cd "$(dirname "$0")/.."
ver=$(head -1 VERSION)
nome="Indigo-Linux-$ver-x86_64"
[ -x "$build/indigo" ] || { echo "falta $build/indigo (compile o app primeiro)"; exit 1; }
# o SDL3 que o build usou (SDL3_DIR = <prefixo>/lib/cmake/SDL3)
sdl_dir=$(sed -n 's/^SDL3_DIR:[A-Z]*=//p' "$build/CMakeCache.txt")
sdl_lib=$(realpath "$sdl_dir/../../libSDL3.so.0")
[ -f "$sdl_lib" ] || { echo "não achei o libSDL3.so.0 em $sdl_dir/../.."; exit 1; }
sdl_lic=$(realpath -m "$sdl_dir/../../../share/licenses/SDL3/LICENSE.txt")

mkdir -p "$out"; tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
d="$tmp/$nome"; mkdir -p "$d/lib"
# o install troca o RUNPATH do build pelo do pacote ($ORIGIN/lib) e tira os símbolos de depuração
cmake --install "$build" --prefix "$d" --strip >/dev/null
cp "$sdl_lib" "$d/lib/libSDL3.so.0"; strip --strip-unneeded "$d/lib/libSDL3.so.0"
[ -f "$sdl_lic" ] && cp "$sdl_lic" "$d/lib/LICENCA-SDL3.txt"
sed "s/@VERSAO@/$ver/g" installer/linux/LEIA-ME.txt > "$d/LEIA-ME.txt"
[ -f ../LICENSE ] && cp ../LICENSE "$d/LICENCA.txt"; cp TERCEIROS.md "$d/TERCEIROS.txt"
cp installer/linux/net.abitante.indigo.desktop installer/linux/instalar-atalho.sh res/indigo.png "$d/"
chmod 755 "$d/indigo" "$d/instalar-atalho.sh"; chmod 644 "$d/lib/libSDL3.so.0"

# conferências: RUNPATH do pacote e versão da glibc exigida
readelf -d "$d/indigo" | grep -q 'RUNPATH.*\[\$ORIGIN/lib\]' || { echo "RUNPATH errado no executável"; exit 1; }
glibc=$(objdump -T "$d/indigo" "$d/lib/libSDL3.so.0" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1)
echo "glibc exigida: ${glibc#GLIBC_}"

# arquivos com dono neutro e data fixa (o mesmo conteúdo dá o mesmo .tar.gz)
data=$(git log -1 --format=%cI 2>/dev/null || date -u +%FT%TZ)
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="$data" -C "$tmp" -cf - "$nome" | gzip -9n > "$out/$nome.tar.gz"
ls -la "$out/$nome.tar.gz"

# AppImage (opcional): só se o appimagetool estiver instalado
if command -v appimagetool >/dev/null 2>&1; then
  a="$tmp/AppDir"; mkdir -p "$a/usr/bin"
  cp -a "$d/indigo" "$d/lib" "$a/usr/bin/"
  cp installer/linux/net.abitante.indigo.desktop "$a/"; cp res/indigo.png "$a/net.abitante.indigo.png"
  printf '#!/bin/sh\nexec "$(dirname "$(readlink -f "$0")")/usr/bin/indigo" "$@"\n' > "$a/AppRun"; chmod 755 "$a/AppRun"
  ARCH=x86_64 appimagetool --no-appstream "$a" "$out/$nome.AppImage" >/dev/null && ls -la "$out/$nome.AppImage"
fi
