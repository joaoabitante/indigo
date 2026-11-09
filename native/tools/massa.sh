#!/usr/bin/env bash
# Teste em massa com jogos caseiros (homebrew) de licença livre publicados no GitHub.
# uso: native/tools/massa.sh pasta_do_build pasta_dos_jogos [console...] [--quadros=N] [--so-baixar]
#   Baixa os jogos de native/tools/massa.lista (só os que faltam), confere o SHA-256 de cada um e roda todos com
#   indigo-massa, vários ao mesmo tempo (JOBS=n, padrão: todos os núcleos). A tela final de cada jogo fica em
#   pasta_dos_jogos/telas/ para dar uma olhada. Nenhum jogo vai para o repositório.
# A lista (separada por TAB): console, nome, url, arquivo dentro do .zip (ou -), sha256 da ROM, licença,
# onde a licença está escrita, repositório, tipo (jogo ou teste) e, se precisar, o nome com que o arquivo é
# guardado (a extensão decide o console, como no app). Uma linha com console "+" é um arquivo que
# acompanha o item de cima (o .bin de um .cue, por exemplo) e fica na mesma pasta, com o próprio nome.
# Só entra jogo ou teste original com licença livre: nada comercial, nada de personagens de outras empresas,
# nenhuma BIOS. Os consoles que pedem BIOS usam a do usuário, de BIOS_DIR (padrão /root/indigo-env/bios-joao),
# que nunca sai da máquina; sem ela, o item é pulado.
set -u
B=$(cd "$1" && pwd); mkdir -p "$2"; P=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
LISTA="$HERE/massa.lista"
BIOS_DIR=${BIOS_DIR:-/root/indigo-env/bios-joao}
# BIOS de cada console (nome do firmware = arquivo dentro de BIOS_DIR)
declare -A BIOS=(
  [ps1]="ps1=scph1001.bin" [neogeo]="neogeo=neogeo.zip" [neogeocd]="neogeocd=neocd.bin" [3do]="3do=panafz10.bin"
  [dc]="dc_boot=dc/dc_boot.bin;dc_flash=dc/dc_flash.bin" [saturn]="saturn=saturn_bios.bin" [pcecd]="pcecd=syscard.pce"
  [pcfx]="pcfx=pcfx.rom" [segacd]="segacd_us=segacd_us.bin" [coleco]="coleco=coleco.rom" [vectrex]="vectrex=vectrex.rom"
  [a5200]="a5200=5200.rom" [intv]="intv_exec=intv_exec.bin;intv_grom=intv_grom.bin" [fds]="fds=disksys.rom"
  [gamecom]="gamecom_int=gamecom_int.bin;gamecom_ext=gamecom_ext.bin"
)
# monta o INDIGO_FW do console (vazio = não precisa; "-" = falta arquivo)
fw_de() {
  local spec=${BIOS[$1]:-} out="" item
  [ -n "$spec" ] || { echo ""; return; }
  IFS=';' read -ra itens <<< "$spec"
  for item in "${itens[@]}"; do
    [ -f "$BIOS_DIR/${item#*=}" ] || { echo "-"; return; }
    out+="${item%%=*}=$BIOS_DIR/${item#*=};"
  done
  echo "$out"
}
QUADROS=3600
SO_BAIXAR=0
FILTRO=()
for a in "${@:3}"; do
  case "$a" in --quadros=*) QUADROS=${a#--quadros=} ;; --so-baixar) SO_BAIXAR=1 ;; *) FILTRO+=("$a") ;; esac
done
quer() { [ ${#FILTRO[@]} -eq 0 ] && return 0; local x; for x in "${FILTRO[@]}"; do [ "$x" = "$1" ] && return 0; done; return 1; }
[ "$SO_BAIXAR" = 1 ] || [ -x "$B/indigo-massa" ] || { echo "falta $B/indigo-massa (cmake --build $B --target indigo-massa)"; exit 2; }

# ---- baixa e confere
mkdir -p "$P/telas"
fila="$P/.fila"; : > "$fila"; : > "$P/.pulados"
baixados=0; ruins=0
pulados=0
baixa() {  # url dentro sha destino
  local tmp; tmp=$(mktemp -d)
  if curl -fsSL --retry 3 -o "$tmp/baixado" "$1"; then
    if [ "$2" = - ]; then mv "$tmp/baixado" "$tmp/rom"; else unzip -p "$tmp/baixado" "$2" > "$tmp/rom" 2>/dev/null; fi
    if [ "$(sha256sum "$tmp/rom" | cut -d' ' -f1)" = "$3" ]; then mv "$tmp/rom" "$4"; rm -rf "$tmp"; return 0; fi
    echo "SHA-256 diferente (o arquivo mudou no GitHub?): $4"
  else
    echo "não baixou: $1"
  fi
  rm -rf "$tmp"; return 1
}
dir=""; usa=0
while IFS=$'\t' read -r con nome url dentro sha lic _ _ tipo local; do
  case "$con" in ''|'#'*) continue ;; esac
  arqnome=$(basename "${dentro/#-/$url}"); arqnome=${arqnome//%20/ }; arqnome=${arqnome//%21/!}
  [ -n "${local:-}" ] && arqnome=$local  # 10ª coluna: nome do arquivo aqui (ex.: .a52 para um cartucho sem cabeçalho)
  if [ "$con" = + ]; then  # acompanha o item de cima
    [ "$usa" = 1 ] || continue
    if [ ! -f "$dir/$arqnome" ]; then baixa "$url" "$dentro" "$sha" "$dir/$arqnome" || ruins=$((ruins + 1)); fi
    continue
  fi
  usa=0
  quer "$con" || continue
  usa=1
  dir="$P/$con/${sha:0:12}"; mkdir -p "$dir"
  arq="$dir/$arqnome"
  if [ ! -f "$arq" ]; then
    if baixa "$url" "$dentro" "$sha" "$arq"; then baixados=$((baixados + 1)); else ruins=$((ruins + 1)); fi
  fi
  [ -f "$arq" ] || continue
  fw=$(fw_de "$con")
  if [ "$fw" = - ]; then pulados=$((pulados + 1)); echo "PULADO $con $arqnome: falta a BIOS do console em $BIOS_DIR" >> "$P/.pulados"; continue; fi
  printf '%s\t%s\t%s\t%s\n' "$con" "$arq" "$fw" "${tipo:-jogo}" >> "$fila"
done < "$LISTA"
total=$(wc -l < "$fila")
echo "jogos: $total na fila ($baixados baixados agora, $ruins com problema no download, $pulados pulados por falta de BIOS)"
[ "$SO_BAIXAR" = 1 ] && { [ "$ruins" -eq 0 ]; exit; }

# ---- roda, vários ao mesmo tempo (uma ROM por processo: se o programa cair, cai só aquele jogo)
roda() {
  local con=$1 arq=$2 fw=$3 tipo=$4 out rc tela
  tela="$P/telas/$con-$(basename "$(dirname "$arq")")-$(basename "$arq").ppm"
  out=$(MASSA_TESTE=$([ "$tipo" = teste ] && echo 1 || echo 0) INDIGO_FW="$fw" timeout 900 "$B/indigo-massa" "$con" "$arq" "$QUADROS" "$tela" 2>&1); rc=$?
  case $rc in
    0|1) echo "$out" | tail -1 ;;
    124) echo "FALHA $con $(basename "$arq"): passou de 15 minutos (travou?)" ;;
    *) echo "FALHA $con $(basename "$arq"): o programa caiu (código $rc) $(echo "$out" | tail -1)" ;;
  esac
}
export -f roda; export B P QUADROS
inicio=$(date +%s)
tr '\t' '\n' < "$fila" | xargs -d '\n' -n 4 -P "${JOBS:-$(nproc)}" bash -c 'roda "$1" "$2" "$3" "$4"' _ | sort > "$P/resultado.txt"
cat "$P/.pulados" >> "$P/resultado.txt"
cat "$P/resultado.txt"
ok=$(grep -c '^OK ' "$P/resultado.txt"); av=$(grep -c '^AVISO ' "$P/resultado.txt"); fa=$(grep -c '^FALHA ' "$P/resultado.txt")
echo
echo "tempo: $(( ($(date +%s) - inicio) / 60 )) min"
echo "RESULTADO massa: $total jogos, $ok OK, $av com aviso, $fa falhas, $ruins downloads com problema, $pulados pulados (falta BIOS)"
[ "$fa" -eq 0 ] && [ "$ruins" -eq 0 ]
