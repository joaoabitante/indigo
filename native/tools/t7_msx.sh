#!/bin/sh
# Baixa os arquivos de teste do núcleo MSX para /root/indigo-env/roms/msx (ou para a pasta dada).
# Tudo homebrew livre, em versões fixas (commit) para as telas conferidas não mudarem:
#   MSXgl (aoineko-fr, CC BY-SA): ROMs prontas de exemplos (VDP, sprites, comandos do V9938, SCC, PSG…)
#     e de cada tipo de cartucho (simples 8–64 KB, Konami, Konami SCC, ASCII 8/16)
#   Stevedore e Pyramid Warp EX (theNestruo), MSXlib (exemplos e o minijogo Run23)
# Depois: indigo-t7_msx /root/indigo-env/roms/msx
set -e
DEST=${1:-/root/indigo-env/roms/msx}
mkdir -p "$DEST/src"
cd "$DEST/src"

pega() {  # pega <dono/repositório> <commit> [pastas do checkout esparso...]
  repo=$1; sha=$2; shift 2
  dir=$(basename "$repo")
  if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$sha" ]; then return; fi
  rm -rf "$dir"
  git init -q "$dir"
  git -C "$dir" remote add origin "https://github.com/$repo.git"
  if [ $# -gt 0 ]; then
    git -C "$dir" sparse-checkout set --no-cone "$@"
  fi
  git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$sha"
  git -C "$dir" checkout -q FETCH_HEAD
}

pega aoineko-fr/MSXgl 9f72749315dc0a1ece31280ab86dcb8559d78796 \
  '/projects/samples/emul/rom/*.rom' '/projects/targets/emul/rom/*.rom'
pega theNestruo/msx-stevedore ddd7a76948df67c0fa13503c51580e4f1dad918c '/dist/main/*.rom'
pega theNestruo/msx-pyramidwarpex 368ed78d7f7b193a2eecd1942f00ca377177d281 '/rom/*.rom'
pega theNestruo/msx-msxlib e632adb2af85c2c61a52fa3dfa5bb4fff3295f7b '/games/**/*.rom'

cd "$DEST"
ln -sfn src/MSXgl/projects/samples/emul/rom gl
ln -sfn src/MSXgl/projects/targets/emul/rom gt
ln -sfn "src/msx-stevedore/dist/main/Stevedore (2020) (theNestruo & Wonder) [v1.2].rom" stevedore.rom
ln -sfn src/msx-pyramidwarpex/rom/PyramidWarp.enhanced.rom pyramid.rom
ln -sfn src/msx-msxlib/games/minigames/run23/run23.rom run23.rom
ln -sfn src/msx-msxlib/games/examples/02snake/snake.rom snake.rom
echo "testes do MSX em $DEST"
