#!/usr/bin/env bash
# Revalida o Índigo para PC inteiro contra o núcleo JavaScript (src/) e os testes públicos.
# uso: native/tools/validate.sh pasta_do_build pasta_das_roms [chave...|--mudou[=ref]]
#   Sem chave: a conferência inteira (para lançar uma versão). Os blocos rodam em paralelo (JOBS=n, padrão: todos
#   os núcleos), os mais demorados primeiro (tempos da última vez em pasta_do_build/.validate-tempos).
#   Antes de conferir, compila o build (SEM_BUILD=1 pula), para nunca conferir programas antigos.
#   Teste rápido durante o trabalho: só os blocos de algumas chaves (ex.: "dc nds"; --lista mostra as chaves), ou
#   --mudou, que escolhe pelos arquivos mudados desde ref (padrão origin/native-pc). Mudança fora das pastas de um
#   console (núcleo comum, app, build) roda tudo.
#   pasta_das_roms/gb-test-roms   ROMs de teste do Blargg (github.com/retrio/gb-test-roms)
#   pasta_das_roms/smsroms        ROMs de Master System / Game Gear (exemplos devkitSMS, homebrew, tests/sms)
#   pasta_das_roms/ngproms        ROMs de Neo Geo Pocket (homebrew, tests/ngp)
#   pasta_das_roms/gba            ROMs de GBA em qualquer subpasta (github.com/jsmolka/gba-tests,
#                                 github.com/alyosha-tas/gba-tests, github.com/PeterLemon/GBA)
#   pasta_das_roms/zexall.com, zexdoc.com
set -u
B=$(cd "$1" && pwd); R=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
J="$HERE/jsref.mjs"
TMPBASE=$(mktemp -d)
check() { # nome, arquivo C++, arquivo JS
  if cmp -s "$2" "$3"; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "  DIFERENTE: $1"; fi
}

# ---- blocos da conferência (cada um é independente; rodam em paralelo, ver o fim do arquivo)
bloco_1() {  # gb
  echo "== Game Boy / Game Boy Color: ROMs do Blargg x DMG/CGB x 1500 quadros"
  n0=$ok
  while IFS= read -r rom; do
    for m in dmg cgb; do
      "$B/indigo-run" "$rom" 1500 $m 37 > "$TMP/c" 2>&1; node "$J" "$rom" 1500 $m 37 > "$TMP/j" 2>&1
      check "$(basename "$rom") $m" "$TMP/c" "$TMP/j"
    done
  done < <(find "$R/gb-test-roms" -name "*.gb" | sort)
  echo "  iguais: $((ok - n0))"
}
BLOCOS+=("1 gb")
bloco_2() {  # sms
  echo "== Master System / Game Gear: ROMs x 3 padrões de botões x 1500 quadros"
  n0=$ok
  for rom in "$R"/smsroms/*; do
    for e in 0 37 11; do
      "$B/indigo-run" "$rom" 1500 auto $e > "$TMP/c" 2>&1; node "$J" "$rom" 1500 auto $e > "$TMP/j" 2>&1
      check "$(basename "$rom") $e" "$TMP/c" "$TMP/j"
    done
  done
  echo "  iguais: $((ok - n0))"
}
BLOCOS+=("2 sms")
bloco_3() {  # ngp
  echo "== Neo Geo Pocket: ROMs x 3 padrões de botões x 1500 quadros"
  n0=$ok
  for rom in "$R"/ngproms/*; do
    for e in 0 37 11; do
      "$B/indigo-run" "$rom" 1500 auto $e > "$TMP/c" 2>&1; node "$J" "$rom" 1500 auto $e > "$TMP/j" 2>&1
      check "$(basename "$rom") $e" "$TMP/c" "$TMP/j"
    done
  done
  echo "  iguais: $((ok - n0))"
}
BLOCOS+=("3 ngp")
# as ROMs de GBA (o bloco mais demorado) em 6 partes, que rodam ao mesmo tempo: a parte k pega as ROMs k, k+6, ...
gba_roms() {
  echo "== Game Boy Advance: ROMs x 3 padrões de botões x 1500 quadros (parte $(($1 + 1)) de 6)"
  local n0=$ok i=0
  while IFS= read -r -d '' rom; do
    i=$((i + 1)); [ $((i % 6)) -eq "$1" ] || continue
    for e in 0 37 11; do
      "$B/indigo-run" "$rom" 1500 gba $e > "$TMP/c" 2>&1; node "$J" "$rom" 1500 gba $e > "$TMP/j" 2>&1
      check "$(basename "$rom") $e" "$TMP/c" "$TMP/j"
    done
  done < <(find "$R/gba" -iname "*.gba" -print0 | sort -z)
  echo "  iguais: $((ok - n0))"
}
bloco_4() { gba_roms 0; }
BLOCOS+=("4 gba")
bloco_5() {  # gba
  echo "== GBA: testes do jsmolka (tela final igual à de um teste que passa)"
  pass=$(find "$R/gba" -path "*jsmolka*" -name "arm.gba" | head -1)
  if [ -z "$pass" ]; then
    echo "  FALTAM os testes do jsmolka em $R/gba"; bad=$((bad + 1))
  else
    ref=$("$B/indigo-run" "$pass" 200 gba 0 | sed -n 200p | cut -d' ' -f2)
    # sem a tela de referência (o programa caiu ou não imprimiu os 200 quadros), todos "passariam" com vazio = vazio
    if [ -z "$ref" ]; then echo "  o teste de referência (arm.gba) não rodou"; bad=$((bad + 1)); fi
    for t in arm thumb memory bios sram flash64 flash128 none; do
      rom=$(find "$R/gba" -path "*jsmolka*" -name "$t.gba" | head -1)
      got=$("$B/indigo-run" "$rom" 200 gba 0 | sed -n 200p | cut -d' ' -f2)
      if [ -n "$got" ] && [ "$got" = "$ref" ]; then echo "  $t: passou"; else echo "  $t: FALHOU"; bad=$((bad + 1)); fi
    done
  fi
}
BLOCOS+=("5 gba")
bloco_6() {  # gba
  echo "== GBA: sistema inteiro com ROMs aleatórias (código ARM/Thumb aleatório)"
  "$B/indigo-gbafuzz" 1000 1 > "$TMP/gc"; node "$HERE/gbafuzz.mjs" 1000 1 > "$TMP/gj"
  check "gbafuzz" "$TMP/gc" "$TMP/gj"; cmp -s "$TMP/gc" "$TMP/gj" && echo "  iguais (casos 1 a 1.000)"
}
BLOCOS+=("6 gba")
bloco_7() {  # backup
  echo "== Backup .indigo: identificação dos jogos e ida e volta com o código do site"
  roms=()
  while IFS= read -r -d '' f; do roms+=("$f"); done < <(find "$R/gba" "$R/gb-test-roms" "$R/smsroms" "$R/ngproms" -type f \( -iname "*.gba" -o -iname "*.gb" -o -iname "*.gbc" -o -iname "*.sms" -o -iname "*.gg" -o -iname "*.ngp" -o -iname "*.ngc" \) -print0 | sort -z)
  "$B/indigo-backuptest" ids "${roms[@]}" > "$TMP/ic"; node "$HERE/backuptest.mjs" ids "${roms[@]}" > "$TMP/ij"
  check "identidade das ROMs" "$TMP/ic" "$TMP/ij"; cmp -s "$TMP/ic" "$TMP/ij" && echo "  ${#roms[@]} ROMs com a mesma identidade do site"
  head -c 32768 /dev/urandom > "$TMP/s1"; head -c 8192 /dev/urandom > "$TMP/s2"
  node "$HERE/backuptest.mjs" make "$TMP/w.indigo" 1 "${roms[0]}=$TMP/s1" "${roms[1]}=$TMP/s2" "${roms[2]}"
  "$B/indigo-backuptest" copy "$TMP/w.indigo" "$TMP/c.indigo"
  node "$HERE/backuptest.mjs" restore "$TMP/w.indigo" | grep -v "^withRoms\|^resultado" > "$TMP/rw"
  node "$HERE/backuptest.mjs" restore "$TMP/c.indigo" | grep -v "^withRoms\|^resultado" > "$TMP/rc"
  check "backup site -> PC -> site" "$TMP/rw" "$TMP/rc"; cmp -s "$TMP/rw" "$TMP/rc" && echo "  backup do site lido e regravado pelo PC restaura igual no site"
}
BLOCOS+=("7 backup")
bloco_8() {  # savestate
  echo "== Save state determinístico"
  st_ok=0; st_bad=0
  gbast=()
  while IFS= read -r -d '' f; do gbast+=("$f"); done < <(find "$R/gba" \( -path "*jsmolka*save*" -o -name "3DEngine.gba" -o -name "BigBuckBunny.gba" \) -name "*.gba" -print0)
  for rom in "$R"/gb-test-roms/cpu_instrs/cpu_instrs.gb "$R"/gb-test-roms/cgb_sound/cgb_sound.gb "$R"/smsroms/* "$R"/ngproms/* "${gbast[@]}"; do
    if "$B/indigo-run" --statetest "$rom" | grep -q IGUAL; then st_ok=$((st_ok + 1)); else st_bad=$((st_bad + 1)); echo "  DIFERENTE: $(basename "$rom")"; fi
  done
  echo "  iguais: $st_ok, diferentes: $st_bad"; bad=$((bad + st_bad))
}
BLOCOS+=("8 savestate")
bloco_9() {  # nes
  echo "== NES: nestest, ROMs de teste do blargg e de outros autores (protocolo \$6000)"
  N="$R/nes-test-roms"
  if [ -d "$N" ]; then
    if "$B/indigo-nestest" log "$N/other/nestest.nes" "$N/other/nestest.log" > "$TMP/n"; then ok=$((ok + 1)); else bad=$((bad + 1)); fi
    echo "  $(cat "$TMP/n")"
    np=0; nf=0
    for d in instr_test-v5/rom_singles cpu_dummy_writes cpu_exec_space cpu_interrupts_v2/rom_singles cpu_reset \
             instr_misc/rom_singles instr_timing/rom_singles apu_test/rom_singles apu_reset ppu_vbl_nmi/rom_singles \
             ppu_open_bus ppu_read_buffer oam_read oam_stress mmc3_test_2/rom_singles; do
      for f in "$N/$d"/*.nes; do
        # 6-MMC3_alt testa outra revisão do chip (o Índigo segue a MMC3B, a mais comum)
        case "$f" in *6-MMC3_alt*) continue;; esac
        r=$("$B/indigo-nestest" rom "$f" 3600)
        case "$r" in PASSOU*) np=$((np + 1));; *) nf=$((nf + 1)); echo "  FALHOU: ${f#$N/}: $r";; esac
      done
    done
    echo "  passaram: $np, falharam: $nf"; ok=$((ok + np)); bad=$((bad + nf))
    for rom in "$N"/nes15-1.0.0/nes15-NTSC.nes "$N"/240pee/240pee.nes "$N"/mmc3_test_2/rom_singles/4-scanline_timing.nes; do
      if "$B/indigo-run" --statetest "$rom" | grep -q IGUAL; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "  estado DIFERENTE: $(basename "$rom")"; fi
    done
    echo "  save state determinístico em 3 ROMs de NES"
  fi
  if [ -d "$R/ss65x02/6502/v1" ]; then
    if "$B/indigo-nes6502test" "$R/ss65x02/6502/v1" > "$TMP/n"; then ok=$((ok + 1)); else bad=$((bad + 1)); fi
    echo "  $(tail -1 "$TMP/n")"
  fi
}
BLOCOS+=("9 nes")
bloco_10() {  # nes
  echo "== NES: mapeadores com chips próprios (MMC5, VRC6, VRC7, Namco 163, Sunsoft 5B): bancos, IRQ e som"
  if "$B/indigo-nesmappertest" > "$TMP/m"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep ERRO "$TMP/m"; fi
  echo "  $(tail -1 "$TMP/m")"
  if [ -d "$R/nes-test-roms/mmc5test" ]; then
    for rom in "$R"/nes-test-roms/exram/mmc5exram.nes "$R"/nes-test-roms/mmc5test/mmc5test.nes "$R"/nes-test-roms/mmc5test_v2/mmc5test.nes; do
      if "$B/indigo-run" --statetest "$rom" | grep -q IGUAL; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "  estado DIFERENTE: $rom"; fi
    done
    echo "  save state determinístico em 3 ROMs de teste do MMC5"
  fi
}
BLOCOS+=("10 nes")
bloco_11() {  # snes
  echo "== SNES: CPU 65816 e SPC700 (SingleStepTests), ROMs de teste do PeterLemon, save state"
  if [ -d "$R/ss/65816/v1" ]; then
    # 300 casos de cada um dos 512 arquivos (a bateria inteira, 5 milhões de casos, roda com o 4º argumento 0)
    if "$B/indigo-snes65816test" "$R/ss/65816/v1" - - 300 > "$TMP/s"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep "falhas" "$TMP/s" | head -5; fi
    echo "  $(tail -1 "$TMP/s")"
  fi
  if [ -d "$R/ss/spc700/v1" ]; then
    if "$B/indigo-spc700test" "$R/ss/spc700/v1" > "$TMP/s"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep "falhas" "$TMP/s" | head -5; fi
    echo "  $(tail -1 "$TMP/s")"
  fi
  P="$R/snes/peterlemon"
  if [ -d "$P" ]; then
    np=0; nf=0
    for rom in "$P"/CPUTest/*/*/*.sfc; do
      r=$("$B/indigo-snestest" texto "$rom" 1500)
      case "$r" in PASSOU*) np=$((np + 1));; *) nf=$((nf + 1)); echo "  FALHOU: ${rom#$P/}: $r";; esac
    done
    echo "  ROMs de teste de CPU e SPC700: passaram $np, falharam $nf"; ok=$((ok + np)); bad=$((bad + nf))
    if [ -d "$P/CHIP/GSU" ]; then
      # Super FX: uma ROM por instrução (PASS/FAIL na tela) e as demos de PLOT comparadas com as telas de referência
      np=0; nf=0
      for rom in "$P"/CHIP/GSU/GSUTest/*/*.sfc; do
        r=$("$B/indigo-snestest" texto "$rom" 600)
        case "$r" in PASSOU*) np=$((np + 1));; *) nf=$((nf + 1)); echo "  FALHOU: ${rom#$P/}: $r";; esac
      done
      echo "  Super FX, ROMs de teste por instrução: passaram $np, falharam $nf"; ok=$((ok + np)); bad=$((bad + nf))
      np=0; nf=0; nk=0
      for rom in "$P"/CHIP/GSU/*BPP/*/*/*.sfc; do
        r=$("$B/indigo-snestest" compara "$rom" 300 "${rom%.sfc}.png")
        case "$r" in
          IGUAL) np=$((np + 1));;
          # PlotPixel 256x192: a imagem de referência tem 2 linhas acesas (17 e 18) que a tabela de HDMA da própria
          # ROM apaga (nas demos PlotLine e FillPoly, com a mesma tabela, elas estão apagadas); diferença conhecida
          "DIFERENTE 512") case "$rom" in *256x192PlotPixel.sfc) nk=$((nk + 1));; *) nf=$((nf + 1)); echo "  TELA DIFERENTE: ${rom#$P/}: $r";; esac;;
          *) nf=$((nf + 1)); echo "  TELA DIFERENTE: ${rom#$P/}: $r";;
        esac
      done
      echo "  Super FX, demos de PLOT: $np telas iguais à referência, $nk com a diferença conhecida, $nf diferentes"
      ok=$((ok + np)); bad=$((bad + nf))
    fi
    for rom in "$P"/HelloWorld/HelloWorld.sfc "$P"/PPU/Mode7/RotZoom/RotZoom.sfc "$P"/SPC700/Twinkle/Twinkle.sfc "$P"/PPU/HDMA/WaveHDMA/WaveHDMA.sfc \
               "$P"/CHIP/GSU/4BPP/FillPoly/256x192/GSU4BPP256x192FillPoly.sfc "$P"/CHIP/GSU/GSUTest/LMULT/GSULMULT.sfc; do
      if "$B/indigo-run" --statetest "$rom" | grep -q IGUAL; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "  estado DIFERENTE: $(basename "$rom")"; fi
    done
    echo "  save state determinístico em 6 ROMs de SNES (2 com Super FX)"
  fi
}
BLOCOS+=("11 snes")
bloco_12() {  # snes
  echo "== SNES: chips DSP-1, SA-1, S-DD1 e SPC7110 (programas e cartuchos montados nos próprios testes; não há ROMs de teste públicas)"
  for t in dsp1test sa1test sdd1test spc7110test; do
    if "$B/indigo-$t" > "$TMP/c"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep FALHA "$TMP/c" | head -5; fi
    echo "  $(tail -1 "$TMP/c")"
  done
}
BLOCOS+=("12 snes")
bloco_13() {  # pm
  echo "== Pokémon Mini: CPU (exemplos da documentação e 300 mil vetores do PokeMini), ROMs de exemplo, EEPROM, save state"
  if "$B/indigo-t7_pm" "$R/pm" > "$TMP/pm"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falha" "$TMP/pm" | head -5; fi
  grep "RESULTADO" "$TMP/pm" | sed 's/^/  /'
}
BLOCOS+=("13 pm")
bloco_14() {  # sgb
  echo "== Super Game Boy: pacotes, paletas por área, máscara, transferências pela imagem, moldura, MLT_REQ, save state (ROM própria)"
  [ -f "$R/sgb/sgb_teste.gb" ] || "$HERE/t7_sgb.sh" "$R/sgb" > /dev/null
  if "$B/indigo-t7_sgb" "$R/sgb" > "$TMP/sgb"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/sgb" | head -5; fi
  grep "RESULTADO" "$TMP/sgb" | sed 's/^/  /'
}
BLOCOS+=("14 sgb")
bloco_15() {  # a2600
  echo "== Atari 2600: CPU (Klaus Dormann, modo decimal), telas de 81 ROMs iguais ao Stella, bancos, save state"
  if "$B/indigo-t7_a2600" "$R/a2600" > "$TMP/a26"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/a26" | head -5; fi
  grep "RESULTADO" "$TMP/a26" | sed 's/^/  /'
}
BLOCOS+=("15 a2600")
bloco_16() {  # msx
  echo "== MSX: espera M1 do Z80, PSG e SCC, mapeadores, telas de homebrew e de testes do VDP, save state"
  if "$B/indigo-t7_msx" "$R/msx" > "$TMP/msx"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/msx" | head -5; fi
  grep "RESULTADO" "$TMP/msx" | sed 's/^/  /'
}
BLOCOS+=("16 msx")
bloco_17() {  # tms
  echo "== ColecoVision e SG-1000: vídeo TMS9918A, som, 28 programas do CVBasic, save state"
  if "$B/indigo-t7_tms" "$R/tms" > "$TMP/tms"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/tms" | head -5; fi
  grep "RESULTADO" "$TMP/tms" | sed 's/^/  /'
}
BLOCOS+=("17 tms")
bloco_18() {  # ws
  echo "== WonderSwan: CPU (SingleStepTests do V20 e WSCPUTest), WSHWTest, telas, save state"
  for m in cpu roms hw state; do
    if "$B/indigo-t7_ws" "$R/ws" $m > "$TMP/ws"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/ws" | head -5; fi
    echo "  $m: $(grep "RESULTADO" "$TMP/ws")"
  done
}
BLOCOS+=("18 ws")
bloco_19() {  # a7800
  echo "== Atari 7800: MARIA (DMA por linha, DLI, WSYNC), som TIA/POKEY, 36 exemplos do 7800basic, SaveKey, save state"
  if "$B/indigo-t7_a7800" "$R/a7800" > "$TMP/a78"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/a78" | head -5; fi
  grep "RESULTADO" "$TMP/a78" | sed 's/^/  /'
}
BLOCOS+=("19 a7800")
bloco_20() {  # a5200
  echo "== Atari 5200: ANTIC ciclo a ciclo (400 telas contra a referência, mapa de ciclos, registradores no meio da linha, DLI, WSYNC), GTIA, POKEY, BIOS de teste e do Altirra, save state"
  if "$B/indigo-t7_a5200" "$R/a5200" > "$TMP/a52"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/a52" | head -5; fi
  grep "RESULTADO" "$TMP/a52" | sed 's/^/  /'
}
BLOCOS+=("20 a5200")
bloco_21() {  # vectrex
  echo "== Vectrex: 6809 (cputest, ciclos medidos no hardware, 15 milhões de instruções iguais ao vecx), feixe, som, save state"
  if "$B/indigo-t7_vectrex" "$R/vectrex" > "$TMP/vec"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/vec" | head -5; fi
  grep "RESULTADO" "$TMP/vec" | sed 's/^/  /'
}
BLOCOS+=("21 vectrex")
bloco_22() {  # intv
  echo "== Intellivision: CP1610 (instruções, flags, SDBD, ciclos), STIC (fundo, MOBs, colisões, janela de acesso), AY-3-8914, formatos, save state"
  if "$B/indigo-t7_intv" "$R" > "$TMP/intv"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/intv" | head -5; fi
  grep "RESULTADO" "$TMP/intv" | sed 's/^/  /'
}
BLOCOS+=("22 intv")
bloco_23() {  # jogadores
  echo "== Dois jogadores: o controle 2 chega à porta 2 do NES, do SNES e do Master System (os outros consoles, nos próprios testes)"
  if "$B/indigo-t7_jogadores" > "$TMP/jog"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/jog" | head -5; fi
  grep "RESULTADO" "$TMP/jog" | sed 's/^/  /'
}
BLOCOS+=("23 jogadores")
bloco_24() {  # app
  if [ -x "$B/indigo-t_app_uso" ]; then  # só existe com o app (SDL3) no Linux
    echo "== Uso no app: botão do disco (FDS), teclado numérico na tela (5200, Intellivision), abas da estante, Abrir com do Android, jogador 2"
    if "$B/indigo-t_app_uso" "$HERE/../android/app/src/main/AndroidManifest.xml" > "$TMP/appuso"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/appuso" | head -5; fi
    grep "RESULTADO" "$TMP/appuso" | sed 's/^/  /'
  fi
}
BLOCOS+=("24 app")
bloco_25() {  # lynx
  echo "== Atari Lynx: 65SC02 (SingleStepTests), sprites e matemática da Suzy, telas iguais ao Handy, EEPROM, save state"
  if "$B/indigo-t7_lynx" "$R/lynx" > "$TMP/lynx"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/lynx" | head -5; fi
  grep "RESULTADO" "$TMP/lynx" | sed 's/^/  /'
  if "$B/indigo-t7_lynxboot" "$R/lynx" > "$TMP/lynxb"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/lynxb" | head -5; fi
  grep -E "RESULTADO|pulado" "$TMP/lynxb" | sed 's/^/  /'
}
BLOCOS+=("25 lynx")
bloco_26() {  # vb
  echo "== Virtual Boy: V810 (instruções à mão e contra o PC), telas dos dois olhos iguais ao Mednafen, save state"
  if "$B/indigo-t7_vb" "$R/vb" > "$TMP/vb"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/vb" | head -5; fi
  grep "RESULTADO" "$TMP/vb" | sed 's/^/  /'
}
BLOCOS+=("26 vb")
bloco_27() {  # fds
  echo "== Famicom Disk System: BIOS e discos de teste próprios (.fds/.qd), CRC, gravação no disco, troca de lado, som de onda, save state"
  if "$B/indigo-t7_fds" "$R/fds" > "$TMP/fds"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/fds" | head -5; fi
  grep "RESULTADO" "$TMP/fds" | sed 's/^/  /'
}
BLOCOS+=("27 fds")
bloco_28() {  # duck
  echo "== Mega Duck: mesmo programa no Game Boy e no Duck (telas e som iguais), bancos MD0/MD1/MD2, save state"
  if "$B/indigo-t7_duck" "$R/duck" > "$TMP/duck"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/duck" | head -5; fi
  grep "RESULTADO" "$TMP/duck" | sed 's/^/  /'
}
BLOCOS+=("28 duck")
bloco_29() {  # svision
  echo "== Watara Supervision: bancos, LCD, timer, NMI, som (tons, ruído, DMA), ROM do cc65, save state"
  if "$B/indigo-t7_svision" "$R/svision" > "$TMP/svision"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/svision" | head -5; fi
  grep "RESULTADO" "$TMP/svision" | sed 's/^/  /'
}
BLOCOS+=("29 svision")
bloco_30() {  # pce
  echo "== PC Engine e SuperGrafx: HuC6280 (SingleStepTests), testes do HuC, 30 casos iguais ao Mesen 2, BRAM, save state"
  if "$B/indigo-t7_pce" "$R/pce" > "$TMP/pce"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/pce" | head -5; fi
  grep "RESULTADO" "$TMP/pce" | sed 's/^/  /'
}
BLOCOS+=("30 pce")
bloco_31() {  # md
  echo "== Mega Drive: 68000 (1 milhão de SingleStepTests), cartuchos, 28 ROMs do SGDK comparadas ao Genesis Plus GX, som, save state"
  if "$B/indigo-t7_md" "$R/md" > "$TMP/md"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/md" | head -5; fi
  grep "RESULTADO" "$TMP/md" | sed 's/^/  /'
}
BLOCOS+=("31 md")
bloco_32() {  # md32x
  echo "== 32X: SH-2 (200 mil casos contra o PicoDrive/MAME), periféricos do SH7604, 8 ROMs iguais ao PicoDrive, PWM, save state"
  if "$B/indigo-t7_32x" "$R/32x" > "$TMP/32x"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/32x" | head -5; fi
  grep "RESULTADO" "$TMP/32x" | sed 's/^/  /'
}
BLOCOS+=("32 md32x")
bloco_33() {  # cps
  echo "== Capcom CPS1/CPS2: decifração do CPS2 igual ao MAME, vídeo/som/EEPROM em pacotes montados no teste, save state"
  if "$B/indigo-t7_cps" "$R/cps" > "$TMP/cps"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/cps" | head -5; fi
  grep "RESULTADO" "$TMP/cps" | sed 's/^/  /'
}
BLOCOS+=("33 cps")
bloco_34() {  # neogeo
  echo "== Neo Geo: 25 programas do ngdevkit (MVS e AES, .zip e .neo) iguais ao Geolith, som YM2610, cartuchos e proteções, save state"
  if "$B/indigo-t7_neogeo" "$R/neogeo" > "$TMP/ng"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/ng" | head -5; fi
  grep "RESULTADO" "$TMP/ng" | sed 's/^/  /'
}
BLOCOS+=("34 neogeo")
bloco_35() {  # neogeocd
  echo "== Neo Geo CD: partida pelo CD (BIOS de teste), 17 CDs do ngdevkit iguais ao NeoCD, CD-DA, som, carga, save state"
  if "$B/indigo-t7_neogeocd" "$R/neogeocd" > "$TMP/ngcd"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/ngcd" | head -5; fi
  grep "RESULTADO" "$TMP/ngcd" | sed 's/^/  /'
}
BLOCOS+=("35 neogeocd")
bloco_36() {  # segacd
  echo "== Sega CD: BIOS de teste e disco sintético iguais ao Genesis Plus GX (quadros, som), ASIC e PCM contra o GPGX, save state"
  if "$B/indigo-t7_segacd" "$R/segacd" > "$TMP/scd"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/scd" | head -5; fi
  grep "RESULTADO" "$TMP/scd" | sed 's/^/  /'
}
BLOCOS+=("36 segacd")
bloco_37() {  # cdrom
  echo "== Leitor de CD: .cue com vários .bin, pregap, .iso, .bin sozinho, detecção do console"
  mkdir -p "$TMP/cd"
  if "$B/indigo-t7_cdrom" "$TMP/cd" > "$TMP/cdr"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/cdr" | head -5; fi
  grep "RESULTADO" "$TMP/cdr" | sed 's/^/  /'
}
BLOCOS+=("37 cdrom")
bloco_38() {  # chd
  echo "== Leitor de .chd: imagens convertidas pelo chdman (vários codecs) x .cue, setor a setor; .chd corrompidos"
  mkdir -p "$TMP/chd"
  if "$B/indigo-t7_chd" "$R/chd" "$TMP/chd" > "$TMP/chdr"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/chdr" | head -5; fi
  grep "RESULTADO" "$TMP/chdr" | sed 's/^/  /'
}
BLOCOS+=("38 chd")
bloco_39() {  # seguranca
  echo "== Segurança: backup com NaN, .zip com \"..\\\", nomes enormes no Neo Geo, .chd com mapa gigante, .cue fora da pasta, nomes vindos de outro app"
  mkdir -p "$TMP/seg"
  if "$B/indigo-t_seguranca" "$TMP/seg" > "$TMP/segr" 2>&1; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh\|error" "$TMP/segr" | head -5; fi
  grep "RESULTADO" "$TMP/segr" | sed 's/^/  /'
}
BLOCOS+=("39 seguranca")
bloco_40() {  # pcecd
  echo "== PC Engine CD: System Cards de teste (SCSI, CD-DA, ADPCM, Arcade Card) e scsitest/okitest iguais ao Mesen 2, som, BIOS, BRAM, save state"
  if "$B/indigo-t7_pcecd" "$R/pcecd" > "$TMP/pcecd"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/pcecd" | head -5; fi
  grep "RESULTADO" "$TMP/pcecd" | sed 's/^/  /'
}
BLOCOS+=("40 pcecd")
bloco_41() {  # pcfx
  echo "== PC-FX: BIOS de teste própria dá a partida no CD, CPU V810 igual ao PC, CD por DMA/PIO, interrupções, tela calculada ponto a ponto (2 VDCs + KING + HuC6261), vídeo comprimido RAINBOW, mouse, PSG/CD-DA/ADPCM, save state"
  if "$B/indigo-t7_pcfx" "$R/pcfx" > "$TMP/pcfx"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/pcfx" | head -5; fi
  grep "RESULTADO" "$TMP/pcfx" | sed 's/^/  /'
}
BLOCOS+=("41 pcfx")
bloco_42() {  # ps1
  echo "== PlayStation: CPU (PeterLemon), BIOS de teste, SIO0 (controle, vibração, cartão), save state; com a BIOS do usuário: menu, ps1-tests (psx.log e VRAM do console) e Beetle PSX"
  if "$B/indigo-t9_ps1" "$R/ps1" > "$TMP/ps1"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/ps1" | head -5; fi
  grep "RESULTADO\|pulados" "$TMP/ps1" | sed 's/^/  /'
}
BLOCOS+=("42 ps1")
bloco_43() {  # ps1
  echo "== PS1 GTE (COP2): 1150 vetores do ps1-tests (console), 2 milhões de casos iguais ao Mednafen, divisão UNR, save state, velocidade"
  if "$B/indigo-t9_gte" "$R/ps1gte" > "$TMP/gte"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/gte" | head -5; fi
  grep -E "iguais|ns por|RESULTADO" "$TMP/gte" | sed 's/^/  /'
}
BLOCOS+=("43 ps1")
bloco_44() {  # ps1
  echo "== PlayStation SPU: 40 cenários aleatórios e o registro de um jogo (ps1-tests) iguais ao Mednafen amostra a amostra, FIFO/DMA, save state"
  if "$B/indigo-t9_spu" "$R/ps1spu" > "$TMP/spu"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/spu" | head -5; fi
  grep -E "cenários:|playback:|Índigo:|RESULTADO" "$TMP/spu" | sed 's/^/  /'
}
BLOCOS+=("44 ps1")
bloco_45() {  # ps1
  echo "== PS1 CD-ROM e MDEC: MDEC x Beetle PSX (fluxos aleatórios) e x ps1-tests (console), roteiros de CD x Beetle, psx-spx, save state"
  if "$B/indigo-t9_cdmdec" "$R/ps1cd" > "$TMP/ps1cd"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh\|diferente" "$TMP/ps1cd" | head -5; fi
  grep "casos iguais\|interrupções comparadas\|RESULTADO" "$TMP/ps1cd" | sed 's/^/  /'
}
BLOCOS+=("45 ps1")
bloco_46() {  # ps1
  echo "== PlayStation inteiro: ps1-tests de GTE, SPU, CD-ROM, MDEC e DMA (psx.log e VRAM do console), disco de teste pela BIOS (GTE, som, CD-DA, leitura do CD, controle, save state) e o Beetle PSX"
  if "$B/indigo-t9_ps1int" "$R/ps1int" > "$TMP/ps1int"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/ps1int" | head -5; fi
  grep -E "RESULTADO|pulados|velocidade" "$TMP/ps1int" | sed 's/^/  /'
}
BLOCOS+=("46 ps1")
bloco_47() {  # tdo
  echo "== 3DO: CPU ARM60 (SingleStepTests), cels x imagens esperadas, matrizes, VDL, CLIO, DSP, CD sintético, save state, BIOS de teste e homebrew próprio"
  if "$B/indigo-t9b_3do" "$R/3do" > "$TMP/3do"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/3do" | head -5; fi
  grep -E "RESULTADO|velocidade|^  total:" "$TMP/3do" | sed 's/^/  /'
}
BLOCOS+=("47 tdo")
bloco_48() {  # saturn
  echo "== Saturn: SCU (interrupções, DMA, DSP), SMPC, VDP1, VDP2, SCSP e 68000, bloco de CD sintético, save state, BIOS de teste e homebrew próprio"
  if "$B/indigo-t7_saturn" "$R/saturn" > "$TMP/saturn"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/saturn" | head -5; fi
  grep -E "RESULTADO|homebrew:" "$TMP/saturn" | sed 's/^/  /'
}
BLOCOS+=("48 saturn")
bloco_49() {  # dc
  echo "== Dreamcast: CPU SH-4 igual ao PC, BIOS de teste pelo GD-ROM (.gdi/.iso/.chd), TA/PVR pixel a pixel, Maple e VMU, AICA (ARM7 e som), save state"
  if "$B/indigo-t7_dc" "$R/dc" > "$TMP/dc"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/dc" | head -5; fi
  grep -E "RESULTADO|velocidade|instruções/s" "$TMP/dc" | sed 's/^/  /'
}
BLOCOS+=("49 dc")
bloco_50() {  # gnw
  echo "== Game & Watch: CPUs SM510/SM5A/SM511 (programas próprios), tela pelo .svg, teclas, piezo e melodia, pacotes da lista do MAME, save state"
  mkdir -p "$R/gnw"
  if "$B/indigo-t7_gnw" "$R/gnw" > "$TMP/gnw"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/gnw" | head -5; fi
  grep "RESULTADO" "$TMP/gnw" | sed 's/^/  /'
}
BLOCOS+=("50 gnw")
bloco_51() {  # gamecom
  echo "== Game.com: SM8500 instrução a instrução (resultados calculados à mão), BIOS e cartucho de teste próprios (vídeo, DMA, timers, som, teclas, caneta), save state"
  if "$B/indigo-t7_gamecom" "$R/gamecom" > "$TMP/gamecom"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/gamecom" | head -5; fi
  grep "RESULTADO" "$TMP/gamecom" | sed 's/^/  /'
}
BLOCOS+=("51 gamecom")
bloco_52() {  # nds
  echo "== Nintendo DS: CPU ARM7 x SingleStepTests ARM7TDMI (ARM e Thumb), ARMv5TE do ARM9, homebrew próprio sem BIOS (SWI em alto nível, IPC, IRQ, DMA, cartucho, save por SPI, toque, RTC, som, 2D e 3D), save state, velocidade"
  if "$B/indigo-t7_nds" "$R/nds" > "$TMP/nds"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/nds" | head -5; fi
  grep -E "RESULTADO|velocidade|casos iguais" "$TMP/nds" | sed 's/^/  /'
}
BLOCOS+=("52 nds")
bloco_53() {  # jaguar
  echo "== Atari Jaguar: Object Processor, Blitter, GPU e DSP (cartuchos de teste próprios), interrupções, som I2S, controle, EEPROM, programas para a RAM, save state"
  if "$B/indigo-t7_jaguar" "$R/jaguar" > "$TMP/jag"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/jag" | head -5; fi
  grep -E "RESULTADO|conferidos" "$TMP/jag" | sed 's/^/  /'
}
BLOCOS+=("53 jaguar")
bloco_54() {  # n64
  echo "== Nintendo 64: CPU VR4300 (inteiros, TLB, exceções, FPU), RSP (vetorial, DMA), RDP (triângulos, z, textura, cópia, XBUS), VI, PIF (controle, EEPROM, Controller Pak), PI (SRAM, FlashRAM), AI, save state (cartuchos de teste próprios, partida HLE)"
  if "$B/indigo-t7_n64" "$R/n64" > "$TMP/n64"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/n64" | head -5; fi
  grep -E "RESULTADO|velocidade" "$TMP/n64" | sed 's/^/  /'
}
BLOCOS+=("54 n64")
bloco_55() {  # z80
  echo "== CPU Z80: ZEXDOC e ZEXALL"
  for z in zexdoc zexall; do
    "$B/indigo-zextest" "$R/$z.com" > "$TMP/z"; rc=$?
    nok=$(grep -c ' OK' "$TMP/z"); nerr=$(grep -c ERROR "$TMP/z")
    echo "  $z: $nok OK, $nerr ERROR"
    bad=$((bad + nerr))
    # os 67 grupos têm de terminar com OK: se o programa cair, travar ou faltar o $z.com, não há ERROR nenhum
    if [ "$rc" -ne 0 ] || [ "$nok" -ne 67 ]; then echo "  $z: o teste não chegou ao fim (código $rc, $nok de 67)"; bad=$((bad + 1)); fi
  done
}
BLOCOS+=("55 z80")
bloco_56() {  # ngp
  echo "== CPU TLCS-900H: 1 milhão de instruções aleatórias"
  "$B/indigo-tlcsfuzz" 1000000 3000001 > "$TMP/fc"; node "$HERE/tlcsfuzz.mjs" 1000000 3000001 > "$TMP/fj"
  check "tlcsfuzz" "$TMP/fc" "$TMP/fj"; cmp -s "$TMP/fc" "$TMP/fj" && echo "  iguais (casos 3.000.001 a 4.000.000)"
}
BLOCOS+=("56 ngp")

bloco_57() { gba_roms 1; }
BLOCOS+=("57 gba")
bloco_58() { gba_roms 2; }
BLOCOS+=("58 gba")
bloco_59() { gba_roms 3; }
BLOCOS+=("59 gba")
bloco_60() { gba_roms 4; }
BLOCOS+=("60 gba")
bloco_61() { gba_roms 5; }
BLOCOS+=("61 gba")

# ---- quais blocos rodar
# pastas de src/ (e testes tools/t7_*, t9_*) -> chaves dos blocos; a pasta que não está aqui roda tudo
declare -A PASTA=(
  [a2600]="a2600" [a5200]="a5200" [a7800]="a7800" [atari]="a2600 a5200 a7800 lynx" [cps]="cps" [dc]="dc"
  [fds]="fds" [gamecom]="gamecom" [gb]="gb sgb duck backup savestate" [gba]="gba backup savestate" [gnw]="gnw"
  [intv]="intv" [jaguar]="jaguar" [lynx]="lynx" [md]="md md32x segacd" [md32x]="md32x" [32x]="md32x" [msx]="msx" [n64]="n64"
  [nds]="nds" [neogeo]="neogeo neogeocd" [neogeocd]="neogeocd" [nes]="nes fds" [ngp]="ngp backup savestate"
  [pce]="pce pcecd" [pcecd]="pcecd" [pcfx]="pcfx" [pm]="pm" [ps1]="ps1" [saturn]="saturn" [segacd]="segacd"
  [sms]="sms tms backup savestate" [snes]="snes" [svision]="svision" [tdo]="tdo" [3do]="tdo" [tms]="tms msx"
  [v810]="vb pcfx" [vb]="vb" [vectrex]="vectrex" [ws]="ws" [sgb]="sgb" [duck]="duck" [cdrom]="cdrom chd"
  [chd]="chd" [jogadores]="jogadores" [pokemini]="pm" [gbafuzz]="gba" [nestest]="nes" [nesmappertest]="nes"
  [nes6502test]="nes" [snes65816test]="snes" [spc700test]="snes" [snestest]="snes" [dsp1test]="snes"
  [sa1test]="snes" [sdd1test]="snes" [spc7110test]="snes" [gte]="ps1" [spu]="ps1" [cdmdec]="ps1" [ps1int]="ps1"
  [lynxboot]="lynx" [zextest]="z80" [tlcsfuzz]="ngp"
)
# chaves de um arquivo mudado (vazio = não sei: roda tudo)
chaves_de() {
  local f=$1 p
  case "$f" in
    native/src/*/*) p=${f#native/src/}; p=${p%%/*} ;;
    native/tools/t7_*|native/tools/t9_*|native/tools/t9b_*|native/tools/*test.cpp|native/tools/*fuzz.cpp)
      p=${f##*/}; p=${p%.*}; p=${p#t7_}; p=${p#t9b_}; p=${p#t9_}; p=${p%%_*} ;;
    native/CONTEXTO.md|native/*.md|*.md|native/installer/*|native/android/*|web/*|dist/*|api/*) echo "-"; return ;;
    src/*|tools/*|native/tools/jsref.mjs) echo "gb sms ngp gba backup savestate z80"; return ;;
    *) return ;;
  esac
  [ "$p" = app ] && { echo "app jogadores"; return; }
  echo "${PASTA[$p]:-}"
}
ESCOLHA=()
TUDO=1
if [ $# -gt 2 ]; then
  TUDO=0
  for a in "${@:3}"; do
    case "$a" in
      --lista) for b in "${BLOCOS[@]}"; do echo "${b#* }"; done | sort -u | tr '\n' ' '; echo; exit 0 ;;
      --mudou|--mudou=*)
        ref=origin/native-pc; [ "$a" != --mudou ] && ref=${a#--mudou=}
        while IFS= read -r f; do
          [ -n "$f" ] || continue
          k=$(chaves_de "$f")
          if [ -z "$k" ]; then echo "mudou $f: confere tudo"; TUDO=1; break; fi
          [ "$k" = - ] || ESCOLHA+=($k)
        done < <(cd "$HERE/../.." && { git diff --name-only "$ref"...HEAD; git diff --name-only HEAD; git ls-files --others --exclude-standard; } | sort -u)
        ;;
      *) ESCOLHA+=("$a") ;;
    esac
  done
  if [ "$TUDO" = 0 ]; then  # chave digitada errada não pode virar "0 problemas"
    for x in "${ESCOLHA[@]}"; do
      achou=0
      for b in "${BLOCOS[@]}"; do [ "${b#* }" = "$x" ] && achou=1; done
      [ "$achou" = 1 ] || { echo "chave desconhecida: $x (veja --lista)"; rm -rf "$TMPBASE"; exit 1; }
    done
  fi
  [ "$TUDO" = 0 ] && ESCOLHA+=(jogadores)  # teste curto que passa por quase todos os consoles
  [ "$TUDO" = 0 ] && echo "conferindo só: $(printf '%s\n' "${ESCOLHA[@]}" | sort -u | tr '\n' ' ')"
fi
quer() { [ "$TUDO" = 1 ] && return 0; local x; for x in "${ESCOLHA[@]}"; do [ "$x" = "$1" ] && return 0; done; return 1; }

# ---- compila antes (nunca conferir programas antigos)
if [ "${SEM_BUILD:-0}" != 1 ] && [ -f "$B/CMakeCache.txt" ]; then
  echo "compilando $B ..."
  if ! cmake --build "$B" -j"$(nproc)" > "$TMPBASE/build.log" 2>&1; then
    tail -20 "$TMPBASE/build.log"; echo "RESULTADO: a compilação falhou"; rm -rf "$TMPBASE"; exit 1
  fi
fi

# ---- roda os blocos em paralelo, os mais demorados primeiro
JOBS=${JOBS:-$(nproc)}
TEMPOS="$B/.validate-tempos"
fila=()
for b in "${BLOCOS[@]}"; do
  n=${b%% *}; k=${b#* }
  quer "$k" || continue
  t=$(awk -v n="$n" '$1 == n { print $2 }' "$TEMPOS" 2>/dev/null); fila+=("${t:-99999} $n")
done
[ ${#fila[@]} -gt 0 ] || { echo "nenhum bloco com essas chaves (veja --lista)"; rm -rf "$TMPBASE"; exit 1; }
inicio=$(date +%s)
roda() {  # n: roda o bloco n sozinho, com a pasta temporária e as contas dele
  local n=$1 t0; t0=$(date +%s)
  TMP="$TMPBASE/b$n"; mkdir -p "$TMP"; ok=0; bad=0
  "bloco_$n" > "$TMPBASE/log$n" 2>&1
  echo "$ok $bad $(( $(date +%s) - t0 ))" > "$TMPBASE/conta$n"
  rm -rf "$TMP"
}
while read -r _ n; do
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
  roda "$n" &
done < <(printf '%s\n' "${fila[@]}" | sort -rn)
wait

# ---- junta tudo na ordem dos blocos
ok=0; bad=0; novos=()
for b in "${BLOCOS[@]}"; do
  n=${b%% *}
  [ -f "$TMPBASE/conta$n" ] || { quer "${b#* }" && { echo "  bloco $n (${b#* }) não terminou"; bad=$((bad + 1)); }; continue; }
  cat "$TMPBASE/log$n"
  read -r o x s < "$TMPBASE/conta$n"
  ok=$((ok + o)); bad=$((bad + x)); novos+=("$n $s")
done
# guarda os tempos (os blocos que não rodaram agora ficam com o tempo antigo)
{ printf '%s\n' "${novos[@]}"; awk 'NR==FNR { v[$1]; next } !($1 in v)' <(printf '%s\n' "${novos[@]}") "$TEMPOS" 2>/dev/null; } | sort -n > "$TEMPOS.novo" && mv "$TEMPOS.novo" "$TEMPOS"
rm -rf "$TMPBASE"
echo
echo "tempo: $(( ($(date +%s) - inicio) / 60 )) min com $JOBS ao mesmo tempo; mais demorados: $(sort -k2 -rn "$TEMPOS" | head -3 | while read -r n s; do printf '%s %smin, ' "$(printf '%s\n' "${BLOCOS[@]}" | awk -v n="$n" '$1 == n { print $2 }')" $((s / 60)); done)"
[ "$TUDO" = 1 ] || echo "(conferência parcial: para lançar uma versão, rode sem chaves)"
# piso da conferência completa: com material de teste faltando (pasta errada, ambiente pela metade) os blocos são
# pulados sem contar. Ao acrescentar testes, suba este número.
ESPERADO=1047
if [ "$TUDO" = 1 ] && [ "$ok" -lt "$ESPERADO" ]; then
  echo "FALTOU TESTE: só $ok comparações (o esperado é $ESPERADO ou mais): confira a pasta das ROMs de teste"
  bad=$((bad + 1))
fi
echo "RESULTADO: $ok comparações iguais, $bad problemas"
[ "$bad" -eq 0 ]
