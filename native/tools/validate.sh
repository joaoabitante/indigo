#!/usr/bin/env bash
# Revalida o Índigo para PC inteiro contra o núcleo JavaScript (src/) e os testes públicos.
# uso: native/tools/validate.sh pasta_do_build pasta_das_roms
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
TMP=$(mktemp -d)
ok=0; bad=0
check() { # nome, arquivo C++, arquivo JS
  if cmp -s "$2" "$3"; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "  DIFERENTE: $1"; fi
}

echo "== Game Boy / Game Boy Color: ROMs do Blargg x DMG/CGB x 1500 quadros"
n0=$ok
while IFS= read -r rom; do
  for m in dmg cgb; do
    "$B/indigo-run" "$rom" 1500 $m 37 > "$TMP/c" 2>&1; node "$J" "$rom" 1500 $m 37 > "$TMP/j" 2>&1
    check "$(basename "$rom") $m" "$TMP/c" "$TMP/j"
  done
done < <(find "$R/gb-test-roms" -name "*.gb" | sort)
echo "  iguais: $((ok - n0))"

echo "== Master System / Game Gear: ROMs x 3 padrões de botões x 1500 quadros"
n0=$ok
for rom in "$R"/smsroms/*; do
  for e in 0 37 11; do
    "$B/indigo-run" "$rom" 1500 auto $e > "$TMP/c" 2>&1; node "$J" "$rom" 1500 auto $e > "$TMP/j" 2>&1
    check "$(basename "$rom") $e" "$TMP/c" "$TMP/j"
  done
done
echo "  iguais: $((ok - n0))"

echo "== Neo Geo Pocket: ROMs x 3 padrões de botões x 1500 quadros"
n0=$ok
for rom in "$R"/ngproms/*; do
  for e in 0 37 11; do
    "$B/indigo-run" "$rom" 1500 auto $e > "$TMP/c" 2>&1; node "$J" "$rom" 1500 auto $e > "$TMP/j" 2>&1
    check "$(basename "$rom") $e" "$TMP/c" "$TMP/j"
  done
done
echo "  iguais: $((ok - n0))"

echo "== Game Boy Advance: ROMs x 3 padrões de botões x 1500 quadros"
n0=$ok
while IFS= read -r -d '' rom; do
  for e in 0 37 11; do
    "$B/indigo-run" "$rom" 1500 gba $e > "$TMP/c" 2>&1; node "$J" "$rom" 1500 gba $e > "$TMP/j" 2>&1
    check "$(basename "$rom") $e" "$TMP/c" "$TMP/j"
  done
done < <(find "$R/gba" -iname "*.gba" -print0 | sort -z)
echo "  iguais: $((ok - n0))"

echo "== GBA: testes do jsmolka (tela final igual à de um teste que passa)"
pass=$(find "$R/gba" -path "*jsmolka*" -name "arm.gba" | head -1)
if [ -n "$pass" ]; then
  ref=$("$B/indigo-run" "$pass" 200 gba 0 | sed -n 200p | cut -d' ' -f2)
  for t in arm thumb memory bios sram flash64 flash128 none; do
    rom=$(find "$R/gba" -path "*jsmolka*" -name "$t.gba" | head -1)
    got=$("$B/indigo-run" "$rom" 200 gba 0 | sed -n 200p | cut -d' ' -f2)
    if [ "$got" = "$ref" ]; then echo "  $t: passou"; else echo "  $t: FALHOU"; bad=$((bad + 1)); fi
  done
fi

echo "== GBA: sistema inteiro com ROMs aleatórias (código ARM/Thumb aleatório)"
"$B/indigo-gbafuzz" 1000 1 > "$TMP/gc"; node "$HERE/gbafuzz.mjs" 1000 1 > "$TMP/gj"
check "gbafuzz" "$TMP/gc" "$TMP/gj"; cmp -s "$TMP/gc" "$TMP/gj" && echo "  iguais (casos 1 a 1.000)"

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

echo "== Save state determinístico"
st_ok=0; st_bad=0
gbast=()
while IFS= read -r -d '' f; do gbast+=("$f"); done < <(find "$R/gba" \( -path "*jsmolka*save*" -o -name "3DEngine.gba" -o -name "BigBuckBunny.gba" \) -name "*.gba" -print0)
for rom in "$R"/gb-test-roms/cpu_instrs/cpu_instrs.gb "$R"/gb-test-roms/cgb_sound/cgb_sound.gb "$R"/smsroms/* "$R"/ngproms/* "${gbast[@]}"; do
  if "$B/indigo-run" --statetest "$rom" | grep -q IGUAL; then st_ok=$((st_ok + 1)); else st_bad=$((st_bad + 1)); echo "  DIFERENTE: $(basename "$rom")"; fi
done
echo "  iguais: $st_ok, diferentes: $st_bad"; bad=$((bad + st_bad))

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

echo "== NES: mapeadores com chips próprios (MMC5, VRC6, VRC7, Namco 163, Sunsoft 5B): bancos, IRQ e som"
if "$B/indigo-nesmappertest" > "$TMP/m"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep ERRO "$TMP/m"; fi
echo "  $(tail -1 "$TMP/m")"
if [ -d "$R/nes-test-roms/mmc5test" ]; then
  for rom in "$R"/nes-test-roms/exram/mmc5exram.nes "$R"/nes-test-roms/mmc5test/mmc5test.nes "$R"/nes-test-roms/mmc5test_v2/mmc5test.nes; do
    if "$B/indigo-run" --statetest "$rom" | grep -q IGUAL; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "  estado DIFERENTE: $rom"; fi
  done
  echo "  save state determinístico em 3 ROMs de teste do MMC5"
fi

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

echo "== SNES: chips DSP-1, SA-1, S-DD1 e SPC7110 (programas e cartuchos montados nos próprios testes; não há ROMs de teste públicas)"
for t in dsp1test sa1test sdd1test spc7110test; do
  if "$B/indigo-$t" > "$TMP/c"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep FALHA "$TMP/c" | head -5; fi
  echo "  $(tail -1 "$TMP/c")"
done

echo "== Pokémon Mini: CPU (exemplos da documentação e 300 mil vetores do PokeMini), ROMs de exemplo, EEPROM, save state"
if "$B/indigo-t7_pm" "$R/pm" > "$TMP/pm"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falha" "$TMP/pm" | head -5; fi
grep "RESULTADO" "$TMP/pm" | sed 's/^/  /'

echo "== Atari 2600: CPU (Klaus Dormann, modo decimal), telas de 81 ROMs iguais ao Stella, bancos, save state"
if "$B/indigo-t7_a2600" "$R/a2600" > "$TMP/a26"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/a26" | head -5; fi
grep "RESULTADO" "$TMP/a26" | sed 's/^/  /'

echo "== MSX: espera M1 do Z80, PSG e SCC, mapeadores, telas de homebrew e de testes do VDP, save state"
if "$B/indigo-t7_msx" "$R/msx" > "$TMP/msx"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/msx" | head -5; fi
grep "RESULTADO" "$TMP/msx" | sed 's/^/  /'

echo "== ColecoVision e SG-1000: vídeo TMS9918A, som, 28 programas do CVBasic, save state"
if "$B/indigo-t7_tms" "$R/tms" > "$TMP/tms"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/tms" | head -5; fi
grep "RESULTADO" "$TMP/tms" | sed 's/^/  /'

echo "== WonderSwan: CPU (SingleStepTests do V20 e WSCPUTest), WSHWTest, telas, save state"
for m in cpu roms hw state; do
  if "$B/indigo-t7_ws" "$R/ws" $m > "$TMP/ws"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/ws" | head -5; fi
  echo "  $m: $(grep "RESULTADO" "$TMP/ws")"
done

echo "== Atari 7800: MARIA (DMA por linha, DLI, WSYNC), som TIA/POKEY, 36 exemplos do 7800basic, SaveKey, save state"
if "$B/indigo-t7_a7800" "$R/a7800" > "$TMP/a78"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/a78" | head -5; fi
grep "RESULTADO" "$TMP/a78" | sed 's/^/  /'

echo "== Vectrex: 6809 (cputest, ciclos medidos no hardware, 15 milhões de instruções iguais ao vecx), feixe, som, save state"
if "$B/indigo-t7_vectrex" "$R/vectrex" > "$TMP/vec"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/vec" | head -5; fi
grep "RESULTADO" "$TMP/vec" | sed 's/^/  /'

echo "== Atari Lynx: 65SC02 (SingleStepTests), sprites e matemática da Suzy, telas iguais ao Handy, EEPROM, save state"
if "$B/indigo-t7_lynx" "$R/lynx" > "$TMP/lynx"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/lynx" | head -5; fi
grep "RESULTADO" "$TMP/lynx" | sed 's/^/  /'
if "$B/indigo-t7_lynxboot" "$R/lynx" > "$TMP/lynxb"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/lynxb" | head -5; fi
grep -E "RESULTADO|pulado" "$TMP/lynxb" | sed 's/^/  /'

echo "== Virtual Boy: V810 (instruções à mão e contra o PC), telas dos dois olhos iguais ao Mednafen, save state"
if "$B/indigo-t7_vb" "$R/vb" > "$TMP/vb"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/vb" | head -5; fi
grep "RESULTADO" "$TMP/vb" | sed 's/^/  /'

echo "== PC Engine e SuperGrafx: HuC6280 (SingleStepTests), testes do HuC, 30 casos iguais ao Mesen 2, BRAM, save state"
if "$B/indigo-t7_pce" "$R/pce" > "$TMP/pce"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/pce" | head -5; fi
grep "RESULTADO" "$TMP/pce" | sed 's/^/  /'

echo "== Mega Drive: 68000 (1 milhão de SingleStepTests), cartuchos, 28 ROMs do SGDK comparadas ao Genesis Plus GX, som, save state"
if "$B/indigo-t7_md" "$R/md" > "$TMP/md"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/md" | head -5; fi
grep "RESULTADO" "$TMP/md" | sed 's/^/  /'

echo "== 32X: SH-2 (200 mil casos contra o PicoDrive/MAME), periféricos do SH7604, 8 ROMs iguais ao PicoDrive, PWM, save state"
if "$B/indigo-t7_32x" "$R/32x" > "$TMP/32x"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/32x" | head -5; fi
grep "RESULTADO" "$TMP/32x" | sed 's/^/  /'

echo "== Capcom CPS1/CPS2: decifração do CPS2 igual ao MAME, vídeo/som/EEPROM em pacotes montados no teste, save state"
if "$B/indigo-t7_cps" "$R/cps" > "$TMP/cps"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/cps" | head -5; fi
grep "RESULTADO" "$TMP/cps" | sed 's/^/  /'

echo "== Neo Geo: 25 programas do ngdevkit (MVS e AES, .zip e .neo) iguais ao Geolith, som YM2610, cartuchos e proteções, save state"
if "$B/indigo-t7_neogeo" "$R/neogeo" > "$TMP/ng"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/ng" | head -5; fi
grep "RESULTADO" "$TMP/ng" | sed 's/^/  /'

echo "== Neo Geo CD: partida pelo CD (BIOS de teste), 17 CDs do ngdevkit iguais ao NeoCD, CD-DA, som, carga, save state"
if "$B/indigo-t7_neogeocd" "$R/neogeocd" > "$TMP/ngcd"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/ngcd" | head -5; fi
grep "RESULTADO" "$TMP/ngcd" | sed 's/^/  /'

echo "== Sega CD: BIOS de teste e disco sintético iguais ao Genesis Plus GX (quadros, som), ASIC e PCM contra o GPGX, save state"
if "$B/indigo-t7_segacd" "$R/segacd" > "$TMP/scd"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/scd" | head -5; fi
grep "RESULTADO" "$TMP/scd" | sed 's/^/  /'

echo "== Leitor de CD: .cue com vários .bin, pregap, .iso, .bin sozinho, detecção do console"
mkdir -p "$TMP/cd"
if "$B/indigo-t7_cdrom" "$TMP/cd" > "$TMP/cdr"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/cdr" | head -5; fi
grep "RESULTADO" "$TMP/cdr" | sed 's/^/  /'

echo "== Leitor de .chd: imagens convertidas pelo chdman (vários codecs) x .cue, setor a setor; .chd corrompidos"
mkdir -p "$TMP/chd"
if "$B/indigo-t7_chd" "$R/chd" "$TMP/chd" > "$TMP/chdr"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/chdr" | head -5; fi
grep "RESULTADO" "$TMP/chdr" | sed 's/^/  /'

echo "== PC Engine CD: System Cards de teste (SCSI, CD-DA, ADPCM, Arcade Card) e scsitest/okitest iguais ao Mesen 2, som, BIOS, BRAM, save state"
if "$B/indigo-t7_pcecd" "$R/pcecd" > "$TMP/pcecd"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/pcecd" | head -5; fi
grep "RESULTADO" "$TMP/pcecd" | sed 's/^/  /'

echo "== PlayStation: CPU (PeterLemon), BIOS de teste, SIO0 (controle, vibração, cartão), save state; com a BIOS do usuário: menu, ps1-tests (psx.log e VRAM do console) e Beetle PSX"
if "$B/indigo-t9_ps1" "$R/ps1" > "$TMP/ps1"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/ps1" | head -5; fi
grep "RESULTADO\|pulados" "$TMP/ps1" | sed 's/^/  /'
echo "== PS1 GTE (COP2): 1150 vetores do ps1-tests (console), 2 milhões de casos iguais ao Mednafen, divisão UNR, save state, velocidade"
if "$B/indigo-t9_gte" "$R/ps1gte" > "$TMP/gte"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/gte" | head -5; fi
grep -E "iguais|ns por|RESULTADO" "$TMP/gte" | sed 's/^/  /'
echo "== PlayStation SPU: 40 cenários aleatórios e o registro de um jogo (ps1-tests) iguais ao Mednafen amostra a amostra, FIFO/DMA, save state"
if "$B/indigo-t9_spu" "$R/ps1spu" > "$TMP/spu"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/spu" | head -5; fi
grep -E "cenários:|playback:|Índigo:|RESULTADO" "$TMP/spu" | sed 's/^/  /'
echo "== PS1 CD-ROM e MDEC: MDEC x Beetle PSX (fluxos aleatórios) e x ps1-tests (console), roteiros de CD x Beetle, psx-spx, save state"
if "$B/indigo-t9_cdmdec" "$R/ps1cd" > "$TMP/ps1cd"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh\|diferente" "$TMP/ps1cd" | head -5; fi
grep "casos iguais\|interrupções comparadas\|RESULTADO" "$TMP/ps1cd" | sed 's/^/  /'
echo "== PlayStation inteiro: ps1-tests de GTE, SPU, CD-ROM, MDEC e DMA (psx.log e VRAM do console), disco de teste pela BIOS (GTE, som, CD-DA, leitura do CD, controle, save state) e o Beetle PSX"
if "$B/indigo-t9_ps1int" "$R/ps1int" > "$TMP/ps1int"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/ps1int" | head -5; fi
grep -E "RESULTADO|pulados|velocidade" "$TMP/ps1int" | sed 's/^/  /'
echo "== 3DO: CPU ARM60 (SingleStepTests), cels x imagens esperadas, matrizes, VDL, CLIO, DSP, CD sintético, save state, BIOS de teste e homebrew próprio"
if "$B/indigo-t9b_3do" "$R/3do" > "$TMP/3do"; then ok=$((ok + 1)); else bad=$((bad + 1)); grep -i "falh" "$TMP/3do" | head -5; fi
grep -E "RESULTADO|velocidade|^  total:" "$TMP/3do" | sed 's/^/  /'

echo "== CPU Z80: ZEXDOC e ZEXALL"
for z in zexdoc zexall; do
  "$B/indigo-zextest" "$R/$z.com" > "$TMP/z"
  echo "  $z: $(grep -c ' OK' "$TMP/z") OK, $(grep -c ERROR "$TMP/z") ERROR"
  bad=$((bad + $(grep -c ERROR "$TMP/z")))
done

echo "== CPU TLCS-900H: 1 milhão de instruções aleatórias"
"$B/indigo-tlcsfuzz" 1000000 3000001 > "$TMP/fc"; node "$HERE/tlcsfuzz.mjs" 1000000 3000001 > "$TMP/fj"
check "tlcsfuzz" "$TMP/fc" "$TMP/fj"; cmp -s "$TMP/fc" "$TMP/fj" && echo "  iguais (casos 3.000.001 a 4.000.000)"

rm -rf "$TMP"
echo
echo "RESULTADO: $ok comparações iguais, $bad problemas"
[ "$bad" -eq 0 ]
