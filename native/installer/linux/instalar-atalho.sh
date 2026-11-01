#!/bin/sh
# Cria o atalho do Índigo no menu de aplicativos (só para o usuário, sem pedir administrador).
# Uso: ./instalar-atalho.sh            (instala)
#      ./instalar-atalho.sh --remover  (tira o atalho; os saves continuam em ~/.local/share/indigo)
set -e
dir=$(cd "$(dirname "$0")" && pwd)
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
icons=${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps
if [ "$1" = "--remover" ]; then
  rm -f "$apps/net.abitante.indigo.desktop" "$icons/net.abitante.indigo.png"
  echo "Atalho removido."
  exit 0
fi
mkdir -p "$apps" "$icons"
cp "$dir/indigo.png" "$icons/net.abitante.indigo.png"
# o caminho do executável vai entre aspas (a pasta pode ter espaços); dentro das aspas, \ " ` e $ levam \
# e, por ser texto do .desktop, cada \ vira \\ (especificação do freedesktop)
EXE="$dir/indigo" awk '
  /^Exec=/ { p = ENVIRON["EXE"]; gsub(/[\\"`$]/, "\\\\\\\\&", p); print "Exec=\"" p "\" %f"; next }
  /^TryExec=/ { print "TryExec=" ENVIRON["EXE"]; next }
  { print }' "$dir/net.abitante.indigo.desktop" > "$apps/net.abitante.indigo.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$apps" || true
echo "Atalho criado no menu de aplicativos (Jogos > Índigo)."
