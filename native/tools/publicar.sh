#!/bin/bash
# Monta a cópia limpa do Índigo para o repositório público (github.com/joaoabitante/indigo) e faz o commit lá.
# Uso: native/tools/publicar.sh <clone do repositório público> [commit da native-pc] [commit da main]
#   (padrão: origin/native-pc e origin/main deste repositório privado)
# Fica de fora: o histórico, as branches de trabalho, downloads/ (APKs e zips), a chave do APK, os relatórios e os
# arquivos internos das sessões, o workflow do GitHub Actions deste repositório e as ROMs de teste sem licença clara.
# O autor do commit público é o e-mail anônimo do GitHub do João. Depois: git -C <clone> push origin main
set -euo pipefail
dest=$(realpath "$1"); app=${2:-origin/native-pc}; site=${3:-origin/main}
repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
[ -d "$dest/.git" ] || { echo "não é um clone git: $dest"; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# app nativo (native/) da native-pc e o site/emulador do navegador da main
git -C "$repo" archive "$app" native LICENSE | tar -x -C "$tmp"
git -C "$repo" archive "$site" src web tools api package.json package-lock.json vercel.json CHANGELOG.md .gitignore | tar -x -C "$tmp"
ver=$(head -1 "$tmp/native/VERSION")

# o que nunca vai para o público
rm -rf "$tmp/native/relatorios" "$tmp/native/android/indigo-dev.keystore" \
       "$tmp/native/EM-ANDAMENTO.md" "$tmp/native/NOTURNO.md" "$tmp/native/PASSAGEM-PC.md" \
       "$tmp/native/CONTEXTO.md"
for r in asteroids blueprint pengu tutankham; do rm -f "$tmp/native/tests/roms/ngproms/$r.ngp"; done
# Android: sem a chave do Índigo; quem compilar assina com a própria (ou a de depuração do Android)
python3 - "$tmp/native/android/app/build.gradle" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
s = re.sub(r"\n\s*signingConfigs \{.*?\n    \}\n", "\n", s, flags=re.S)
s = re.sub(r"\n\s*signingConfig signingConfigs\.indigo", "", s)
open(p, "w").write(s)
PY
python3 - "$tmp/native/README.md" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
s = re.sub(r"O APK é gerado pelo GitHub Actions.*?sem apagar saves\.", "Os APKs oficiais são publicados em https://indigo.nexus; para compilar o seu, assine com a sua própria chave.", s, flags=re.S)
open(p, "w").write(s)
PY
grep -rq "indigo-dev" "$tmp/native/android" "$tmp/native/README.md" && { echo "senha da chave ainda no build.gradle"; exit 1; }

cat > "$tmp/README.md" <<EOF
# Índigo Nexus

**O ponto de encontro de 50 anos de videogames.** Emulador grátis e de código aberto para **Windows**,
**Linux** e **Android** (o mesmo código em C++20 + SDL3), do Atari 2600 ao PlayStation, com um emulador que também roda no
navegador. Site e downloads: **https://indigo.nexus**

Versão $ver. Um projeto do [Autista Empreende](https://autistaempreende.com.br), criado pelo João Abitante com a
ajuda de inteligência artificial (Claude).

## Consoles

Game Boy, Game Boy Color, Super Game Boy, Game Boy Advance, NES, Famicom Disk System, SNES (com Super FX, SA-1,
DSP-1, S-DD1 e SPC7110), Master System, Game Gear, SG-1000, Mega Drive, 32X, Sega CD, PC Engine, SuperGrafx e PC
Engine CD, Neo Geo (MVS/AES) e Neo Geo CD, Capcom CPS1/CPS2, Atari 2600/7800, Lynx, WonderSwan/Color, Virtual Boy,
Neo Geo Pocket/Color, Pokémon Mini, MSX, ColecoVision, Vectrex, Mega Duck, Watara Supervision, Game & Watch e
PlayStation. Experimentais: Panasonic 3DO, Atari 5200, Intellivision, Tiger Game.com, PC-FX, Atari Jaguar, Sega
Saturn, Nintendo 64, Nintendo DS e Dreamcast.

## Jogos e BIOS

O Índigo **não traz jogos nem BIOS** de nenhum fabricante. Use cópias dos seus próprios cartuchos e discos. Os
consoles que precisam de BIOS (PlayStation, Sega CD, Neo Geo, Neo Geo CD, PC Engine CD, ColecoVision, 3DO, Famicom
Disk System, Atari 5200, Intellivision, Game.com, PC-FX, Saturn e Dreamcast) pedem o arquivo do seu console na primeira vez. O
Game & Watch usa o .zip do MAME do próprio usuário, que traz o desenho da tela.

## Compilar

- App (Linux, testes e build do Windows com MinGW): \`native/tools/setup-env.sh ~/indigo-env\` instala o que
  precisa, baixa as ROMs de teste públicas e compila. Detalhes em \`native/README.md\`.
- Conferência: \`native/tools/validate.sh <build> ~/indigo-env/roms\` compara milhares de quadros e sons contra as
  referências; cada versão só sai com 0 problemas.
- Android: \`native/android\` (Gradle). Assine com a sua própria chave.
- Site e emulador do navegador: \`npm install\` e \`node tools/build.js\` (gera \`dist/\`).

## Licença

GNU General Public License versão 3 (\`LICENSE\`). Código e dados de terceiros, todos compatíveis, estão em
\`native/TERCEIROS.md\`. Os nomes dos consoles servem só para indicar compatibilidade e são marcas dos seus donos;
o Índigo não tem ligação com nenhum fabricante.
EOF

cat > "$tmp/SECURITY.md" <<'EOF'
# Segurança

Achou uma falha de segurança no Índigo (app, site ou nuvem dos saves)? Não abra uma issue pública com os detalhes:
use **Security > Report a vulnerability** neste repositório, que só o mantenedor vê. Diga a versão, o sistema
(Windows, Linux, Android ou navegador) e como reproduzir.

Recebe correção só a versão mais nova (a de https://indigo.nexus). Cada versão passa pela conferência completa, por
testes com arquivos estragados de propósito (com AddressSanitizer e UBSan) e pelo teste de segurança `t_seguranca`.
EOF

# Dependabot: avisa de dependências com falha (npm do site, Gradle do Android) e abre a correção
mkdir -p "$tmp/.github"
cat > "$tmp/.github/dependabot.yml" <<'EOF'
version: 2
updates:
  - package-ecosystem: npm
    directory: /
    schedule: { interval: weekly }
    open-pull-requests-limit: 5
  - package-ecosystem: gradle
    directory: /native/android
    schedule: { interval: weekly }
    open-pull-requests-limit: 5
EOF

# build do Windows no GitHub Actions do público (é o build que a assinatura de código confere)
mkdir -p "$tmp/.github/workflows"
cp "$tmp/native/tools/ci/windows-publico.yml" "$tmp/.github/workflows/windows.yml"

# conferência final: nenhuma chave, senha ou e-mail particular pode sair
# (o e-mail particular vem do autor do commit privado, para não ficar escrito neste script, que também é público)
email=$(git -C "$repo" log -1 --format=%ae "$app")
if grep -rIlE --exclude=publicar.sh "BEGIN [A-Z ]*PRIVATE KEY|storePassword|keyPassword" "$tmp" || grep -rIlF "$email" "$tmp" \
   || find "$tmp" -type f \( -name '*.keystore' -o -name '*.jks' -o -name '*.pem' -o -name '*.p12' -o -name '.env*' \) | grep -q .; then
  echo "segredo ou e-mail particular encontrado na cópia pública; nada foi publicado"; exit 1
fi

# troca o conteúdo do clone pelo novo (mantém só o .git)
find "$dest" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
cp -a "$tmp/." "$dest/"
cd "$dest"
git add -A
if git diff --cached --quiet; then echo "nada mudou"; exit 0; fi
GIT_AUTHOR_NAME="João Abitante" GIT_AUTHOR_EMAIL="270025200+joaoabitante@users.noreply.github.com" \
GIT_COMMITTER_NAME="João Abitante" GIT_COMMITTER_EMAIL="270025200+joaoabitante@users.noreply.github.com" \
  git commit -q -m "Índigo $ver"
git log -1 --format='%h %s'
