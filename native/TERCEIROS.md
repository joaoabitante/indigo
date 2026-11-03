# Código e dados de terceiros no Índigo

O Índigo é software livre sob a **GNU General Public License versão 3** (arquivo `LICENSE` na raiz do
repositório). Copyright © 2026 João Carlos Bueno Abitante (projeto do Autista Empreende).

O Índigo **não traz jogos nem BIOS de nenhum fabricante**. Os consoles que precisam de BIOS (PlayStation, Sega CD,
Neo Geo, Neo Geo CD, PC Engine CD, ColecoVision, 3DO, Famicom Disk System, Game.com, Atari 5200, Intellivision, PC-FX e
Saturn) usam o arquivo do console do próprio usuário. Os Game & Watch
abrem o pacote .zip do próprio usuário, que traz o programa e o desenho da tela (.svg); nenhum desenho vem junto.

Partes de terceiros incluídas, todas com licença compatível com a GPL-3.0:

| O quê | Onde | Licença |
|---|---|---|
| C-BIOS 0.29a (BIOS livre do MSX) | `native/src/msx/cbios.*` | tipo BSD (aviso completo mantido no arquivo) |
| FreeBIOS 1.3 do Pokémon Mini (Team Pokeme) | `native/src/pm/freebios.cpp` | domínio público |
| Cifras e tabelas do Neo Geo (CMC, PCM2 etc.) | `native/src/neogeo/cart.h`, `cmc_tables.inc` | BSD-3-Clause (MAME) |
| Lista de jogos do Neo Geo | `native/src/neogeo/sets.inc` | CC0-1.0 (lista de software do MAME) |
| Cifra do CPS2 e tabelas do CPS1/CPS2 | `native/src/cps/cps2crypt.cpp`, `tabela.inc` | BSD-3-Clause (MAME) |
| QSound HLE (superctr e Valley Bell) | `native/src/cps/qsound.*` | BSD-3-Clause |
| Lógica do drive do Famicom Disk System (temporização dos bytes, intervalos, CRC, IRQ) e da unidade de som, adaptada do Mesen 2 (Sour; `Core/NES/Mappers/FDS`, `FdsLoader`) e da wiki do NESdev | `native/src/fds/fds.cpp` | GPL-3.0 (Mesen 2) |
| CPUs Sharp SM5xx dos Game & Watch (comportamento reescrito do núcleo SM510 do MAME, de hap) e lista de jogos | `native/src/gnw/sm510.*`, `tabela.inc` | BSD-3-Clause (MAME) |
| nanosvg e nanosvgrast (Mikko Mononen), na versão do MAME com `<title>` | `native/src/third_party/nanosvg/` | zlib (`LICENSE.txt`) |
| CPU SM8500 e hardware do Game.com (decodificação, ciclos, DMA, timers, som, teclas), adaptados dos fontes `sm8500` e `gamecom` do MAME (Wilbert Pol e Robbbert) | `native/src/gamecom/` | BSD-3-Clause (aviso abaixo) |
| Modelo do gerador de som do SM8521 (comportamento, sem código copiado) do emulador de cam900 no Furnace | `native/src/gamecom/gamecom.cpp` | zlib (aviso abaixo) |
| Constantes do Intellivision tiradas do jzIntv (Joe Zbiciak): paleta do STIC, máscaras dos registros do STIC, tempos das fases do quadro e dos pedidos de barramento, tabela de volume e envelope do AY-3-8914, formato .rom do Intellicart. Há trechos derivados do código do jzIntv: a máquina de estados do envelope do PSG (`PSG::tick`, contagem até 16 e casos de hold/cont/altr, tradução quase linha a linha de `ay8910.c`), o ESHIFT e o LFSR de ruído ($10004), e o tratamento de `intr` nas instruções J/JE/JD da CPU. Da documentação do jzIntv (`doc/programming/intellicart.txt`, `memory_map.txt`, `cp1600_summary.txt`): o formato do .cfg (seções [mapping], [preload], [memattr], [bankswitch]), os registros de banco $40-$5F do Intellicart, a troca de páginas do ECS ($xA5y em $xFFF) e a tabela de ciclos do manual da GI usada no teste. O resto do núcleo (`native/src/intv/`) foi escrito para o Índigo, usando o jzIntv como referência | `native/src/intv/` | GPL-2.0-or-later |
| stb_truetype (Sean Barrett) | `native/src/third_party/stb_truetype.h` | domínio público / MIT |
| Fonte Poppins | `native/res/fonts/` | SIL Open Font License 1.1 (`OFL-Poppins.txt`) |
| SDL3 (biblioteca usada para compilar; no Linux vai no pacote, em `lib/`) | baixada pelo `setup-env.sh` | zlib |
| Gradle wrapper | `native/android/gradle/` | Apache-2.0 |
| Tabelas de cor CRY do Atari Jaguar (dados da documentação do console, na forma do Virtual Jaguar) | `native/src/jaguar/jaguar.cpp` | GPL-3.0 (Virtual Jaguar / virtualjaguar-libretro) |
| 3DO: conjunto de instruções e modelo de quadro/FIFO/DMA do DSPP, mapa dos registradores do CLIO e do MADAM (timers, interrupções, MSYSBITS, DMA do XBUS e do controle, VDL, motor de matrizes, campos do CCB/PRE1/PIXC) e comandos da unidade de CD CR-560-B, reescritos a partir dos fontes `src/devices/cpu/dspp`, `src/mame/misc/3do*` e `src/devices/machine/cr560b*` do MAME (Philip Bennett, Angelo Salese, Wilbert Pol) | `native/src/tdo/` | BSD-3-Clause (aviso abaixo) |

Partes do núcleo do PC-FX (`native/src/pcfx/`) são **adaptadas do código do Mednafen** (Beetle PC-FX, Mednafen
Team, GPL-2.0 ou posterior, usado aqui sob a GPL-3.0), reescritas no estilo do Índigo mas derivadas dele: o desenho
dos fundos do KING (`king.cpp`: estrutura do drawBg, tabela de tamanhos, máscaras do microprograma cg_ofbat), o
decodificador ADPCM e sua tabela de passos (`soundbox.cpp`), os coeficientes YUV→RGB e as prioridades/celofane do
HuC6261, o cabeçalho da memória de save formatada (bytes iguais aos do BRInit00/ExBRInit00 do libretro.cpp do
Beetle) e, em `pcfx.cpp`, o mapa de memória e de E/S, o controlador de interrupções, o temporizador e o controle.
O protocolo do leitor de CD (`scsicd.cpp`) segue o mesmo código como referência. O descompressor de vídeo RAINBOW
(`rainbow.cpp`) segue o rainbow.cpp do mesmo projeto: as tabelas de Huffman fixas do HuC6271 (dados de David
Michel, do MagicEngine-FX, como publicados no Mednafen), o formato dos blocos, os códigos especiais do DC, a
transferência do KING a cada 16 linhas e o chroma key; a IDCT é própria. O mouse (identificação 0xD, movimento de
-127 a 127) segue o input/mouse.cpp do Mednafen. Créditos ao Mednafen Team.
O formato do cabeçalho de partida dos CDs
de PC-FX segue o pcfx-cdlink do pcfxtools (Alex Marshall "trap15", MIT); o blob de partida que acompanha essa
ferramenta não é usado. A BIOS de teste (`native/tools/t7_pcfx_bios.c`) foi escrita do zero.

Outros emuladores (Mednafen/Beetle PSX, Genesis Plus GX, PicoDrive, Mesen 2, bsnes, Geolith, NeoCD, MAME e
outros) servem apenas de **régua nos testes**: são compilados fora do repositório e nenhum código deles faz parte
do Índigo. O núcleo do Atari Jaguar foi escrito para o Índigo; o código do Virtual Jaguar (virtualjaguar-libretro,
GPL-3.0) foi lido como documentação do hardware (mapas de registros, comportamento do Object Processor, do Blitter,
da EEPROM e do controle; na 2.5, os valores dos registros de vídeo PAL da partida, o bit 0 do VP como chave do
entrelaçado e o bit 11 do VC como campo), sem cópia de código além das tabelas de cor CRY citadas acima. Testes públicos (Blargg, mooneye, SingleStepTests, ps1-tests, PeterLemon e outros) são baixados pelo
`native/tools/setup-env.sh` e também não ficam no repositório.

Documentação de hardware consultada para o Mega Duck e o Watara Supervision (nenhum código copiado): megaduck-info
de bbbbbr (CC0-1.0: endereços e bits dos registradores do Mega Duck e os mapeadores dos cartuchos), o
`asminc/supervision.inc` do cc65 (zlib) e o driver `svision` do MAME (GPL-2.0-or-later, Peter Trauner: registradores,
som e LCD do Supervision). O teste do Supervision compila um programa próprio com o cc65 (crt0 e driver de
joystick do cc65, zlib), fora do repositório.

Atari 5200 (`native/src/a5200`, `native/src/atari/pokey.*`): escrito a partir da documentação pública do ANTIC,
do GTIA e do POKEY; o código do Atari800 (GPL-2.0-or-later) foi consultado para os códigos das teclas do controle,
os esquemas de bancos dos cartuchos (Bounty Bob e super cartucho) e as posições de ciclo do ANTIC, sem cópia de
código; na 2.5, o mapa de ciclos medido no aparelho em `cycle_map.c` do Atari800 (Perry McFarlane e equipe,
GPL-2.0-or-later: posição dos nomes, desenhos e bytes de bitmap, fim da DMA e o refresh com um só pedido pendente)
foi reaproveitado como dado, reescrito à mão na numeração de ciclos do Altirra (refresh no ciclo 25). A BIOS livre do Altirra para o 5200 (Avery Lee, licença permissiva de cópia e distribuição) é baixada pelo
`native/tools/t7_a5200.sh` só para um teste e não fica no repositório; o teste usa também uma BIOS própria escrita
em assembly (`native/tools/t7_a5200_bios.asm`), montada com o dasm (GPL-2.0).

O núcleo do Sega Saturn (`native/src/saturn/`) foi escrito do zero. O código do **Mednafen** (módulo `ss`, no
repositório Beetle Saturn, GPL-2.0 ou posterior) foi **lido como documentação** de registradores e comportamentos
(níveis e vetores de interrupção da SCU, codificação das instruções do DSP da SCU, formatos do VDP2 e dos comandos
do bloco de CD); nenhum trecho foi copiado. Na 2.5, a máquina de estados da unidade de CD (ocupado, busca, leitura,
pausa, estado periódico, varredura, busca por FAD, autenticação) a mistura das camadas do VDP2 (cálculo de cor
especial, estendido, cor de linha, gradação, sombras), o tempo de desenho do VDP1 e o envelope, o LFO e os volumes
do SCSP foram reescritos seguindo o comportamento descrito lá,
e algumas constantes de tempo medidas pela equipe do Mednafen no aparelho (tempo de busca, intervalo do estado
periódico, saltos da varredura, custo por ponto do VDP1, relógio do envelope e do LFO do SCSP) foram
reaproveitadas como números. A BIOS e o homebrew de teste do Saturn (`native/tools/t7_saturn_*`) são
próprios e não contêm nada da Sega.

Os nomes dos consoles aparecem só para indicar compatibilidade e são marcas dos seus donos. O Índigo não tem
ligação com nenhum fabricante.

## Aviso do MAME (Game.com e 3DO)

O núcleo do Game.com (`native/src/gamecom/`) foi escrito para o Índigo a partir do comportamento descrito nos
arquivos `src/devices/cpu/sm8500/*` e `src/mame/tiger/gamecom*` do MAME, distribuídos sob esta licença:

    Copyright (c) Wilbert Pol, Robbbert
    Redistribution and use in source and binary forms, with or without modification, are permitted provided
    that the following conditions are met:
    1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
       following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
       following disclaimer in the documentation and/or other materials provided with the distribution.
    3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or
       promote products derived from this software without specific prior written permission.
    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
    WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
    PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
    OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
    DAMAGE.

O mesmo texto de licença (com "Copyright (c) Philip Bennett, Angelo Salese, Wilbert Pol") vale para as partes do
núcleo do 3DO (`native/src/tdo/`) reescritas a partir do comportamento do DSPP, do CLIO, do MADAM e da unidade
CR-560-B do MAME, citadas na tabela acima. O resto do núcleo do 3DO (CPU ARM60, motor de cels por quadriláteros,
unidade de CD por fila, BIOS e homebrew de teste) foi escrito para o Índigo.

O formato do cabeçalho dos cartuchos ("TigerDMGC" em 0 ou $40000, título, entrada) foi conferido na documentação
do rom-properties (GerbilSoft, GPL-2.0-or-later) e do GameComHDK (Tpot, MIT); nenhum código deles foi copiado.

O gerador de som do Game.com (2.5) segue o modelo do emulador de SM8521 de cam900 (Furnace, arquivo
`src/engine/platform/sound/sm8521.c`, licença zlib, "(C) 2023-present cam900 and contributors"): período SGnT + 1
de 12 bits no relógio / 4, amostras de onda com sinal, volume com meio passo, DAC direto que toca sozinho, RAM da
onda presa com o canal tocando e mistura de 8 bits com corte. O código do Índigo foi escrito à parte; nenhum trecho
foi copiado. A escolha do encaixe de cartucho pelo P3 no momento da leitura segue a observação em aparelho
publicada no GameCom-Assembler (Kitrinx, GPL-3.0, README do `cycle_count_test`); nenhum código foi usado.
