# Índigo para PC (nativo, C++)

Versão para Windows do Índigo, escrita em C++20 com SDL3. Os núcleos são portes linha a linha dos
núcleos JavaScript de `src/`. A versão web e o site no Vercel não são afetados por esta pasta (o build
do Vercel só usa `tools/build.js`, que não lê `native/`).

## Situação

| Console | Núcleo | Validação |
|---|---|---|
| Game Boy / Game Boy Color | `src/gb/` (SM83, PPU, APU, MBC1/2/3/5, relógio) | 58 ROMs de teste × DMG/CGB × 1.500 quadros: imagem e som **idênticos ao núcleo JS em todos os quadros** |
| Master System / Game Gear | `src/sms/` (Z80, VDP modo 4, SN76489, mapeadores Sega e Codemasters) | ZEXDOC e ZEXALL 100%; 9 ROMs (exemplos do devkitSMS, Winter Shooter e a ROM de teste do Índigo) × 3 padrões de botões × 1.500 quadros **idênticos ao JS**, inclusive o save da RAM do cartucho |
| Game Boy Advance | `src/gba/` (ARM7TDMI ARM + Thumb, barramento com estados de espera e prefetch, vídeo modos 0–5, PSG + DirectSound, DMA, timers, BIOS própria, SRAM/Flash/EEPROM, relógio do cartucho) | 222 ROMs de teste (jsmolka, alyosha-tas, PeterLemon) × 3 padrões de botões × 1.500 quadros **idênticos ao JS** (imagem, som e save); testes do jsmolka passando; sistema inteiro com milhares de ROMs de código aleatório idêntico ao JS; relógio do cartucho idêntico ao JS em 3 fusos horários |
| NES / Famicom (NTSC) | `src/nes/` (2A03 com as instruções ilegais, PPU ponto a ponto, APU com DMC, DMA, mapeadores 0, 1, 2, 3, 4, 5, 7, 9, 10, 11, 19, 21, 22, 23, 24, 25, 26, 34, 66, 69, 71, 79, 85, 140, 206, com o som extra do MMC5, VRC6, VRC7 (FM), Namco 163 e Sunsoft 5B em `mappers_ext.cpp`) — núcleo próprio do nativo (a versão web não tem NES) | CPU: 2,44 milhões de casos dos SingleStepTests do 6502 iguais ciclo a ciclo e o `nestest` igual ao registro de referência; 71 ROMs de teste públicas passando pelo protocolo `$6000` (blargg e outros: CPU, interrupções, APU, PPU, vblank/NMI, open bus, OAM, MMC3) e 53 de resultado na tela (sprite 0, excesso de sprites, tempo de desvio, DMA do DMC); `indigo-nesmappertest`: bancos, IRQ e frequência do som de cada chip conferidos com a documentação; ROMs de teste do MMC5 (ExRAM executável, bancos de CHR, IRQ por linha) |
| SNES / Super Famicom (NTSC) | `src/snes/` (65816 com os ciclos do chip, SPC700, DSP com BRR/ADSR/eco, PPU por linha com os modos 0–7, Mode 7, janelas, soma de cores, mosaico, alta resolução; DMA/HDMA; LoROM, HiROM e ExHiROM com SRAM; Super FX em `gsu.cpp`: as instruções com ALT1/2/3, cache de 512 bytes, buffers de ROM/RAM, PLOT/RPIX com o cache de pixels, multiplicações e a ROM vista pelo SNES enquanto o GSU roda; DSP-1 em `dsp1.cpp`: µPD7725 para o arquivo do chip `dsp1b.rom` e os comandos imitados quando ele não existe; SA-1 em `sa1.cpp`: segundo 65816, I-RAM/BW-RAM, Super MMC, DMA, conversão de bitmap e contas; S-DD1 em `sdd1.cpp`: janelas de 1 MB e descompressão durante o DMA; SPC7110 em `spc7110.cpp`: descompressão aritmética de 1/2/4 bits por pixel, porta de dados, contas, janelas da ROM de dados e o relógio RTC-4513) — núcleo próprio do nativo. Outros chips (CX4, DSP-2/3/4…) ainda não | CPU: **5.080.400 casos dos SingleStepTests do 65816 iguais** (registradores, memória e cada ciclo do barramento); SPC700: **256.000 casos iguais**; 30 de 30 ROMs de teste de CPU/SPC700 do PeterLemon com PASS em todas as páginas (`indigo-snestest`); telas dos testes de vídeo comparadas com as imagens de referência do PeterLemon; Super FX: 31 de 31 ROMs de teste por instrução do PeterLemon com PASS e 24 de 27 demos de PLOT (2, 4 e 8 bits; 128, 160 e 192 linhas) com a tela igual à de referência (`indigo-snestest compara`; as 3 restantes diferem em 2 linhas que a própria imagem de referência mostra acesas); DSP-1: 126 verificações (`indigo-dsp1test`); SA-1: 41 verificações (`indigo-sa1test`); S-DD1: 91 verificações (`indigo-sdd1test`: 64 fluxos iguais ao descompressor do bsnes e o chip no barramento); SPC7110: 120 verificações (`indigo-spc7110test`: 48 roteiros do chip e 32 do relógio iguais ao bsnes e o chip no barramento); save state determinístico |
| Neo Geo Pocket / Color | `src/ngp/` (TLCS-900H, Z80 de som, K2GE, T6W28 + DACs, timers, micro-DMA, flash, BIOS própria) | CPU: 2 milhões de instruções aleatórias idênticas ao JS (registradores, flags, gravações, ciclos); 5 jogos homebrew × 3 padrões de botões × 1.500 quadros + ROM de teste de save **idênticos ao JS**, inclusive o save `.ngf` |

## Usar

Instale com `Indigo-<versão>-instalador.exe` (só para o usuário, sem administrador; desinstalar não apaga os
saves) ou use a versão portátil (zip). Abra `indigo.exe` e arraste um jogo para a janela (ou Ctrl+O, ou dois
cliques no jogo, se a opção "abrir jogos com dois cliques" estiver ligada no instalador ou em "Sobre e novidades").

- **Biblioteca** (tela inicial): capas dos jogos em grade, abas por console (Todos, Favoritos, NES, GB...), o
  cartão "Continuar" com o último jogo e os botões "+ Abrir" e "Ajustes". A capa é a tela do jogo ao fechar (o
  menu do jogo tem "Usar esta tela como capa" e "Favorito"). No PC, "Pastas de jogos" (em Ajustes) acrescenta
  todas as ROMs de uma pasta e das subpastas. Texto com a fonte Poppins (licença OFL, `res/fonts/`), desenhada
  com o stb_truetype (domínio público, `src/third_party/`).
- **Filtros de tela** (menu do jogo): nítido, suave, LCD (grade), TV de tubo (linhas) e EPX.
- **Controles**: trocar teclas e botões do controle (gravados em `controles.txt`). Atalhos no controle, como
  num Anbernic: Select + R salva estado, Select + L carrega, Select + ←/→ troca o espaço, Select + Start menu.
- **Sobre e novidades**: versão e o que mudou em cada uma (`src/app/novidades.h`).

| Tecla | Função |
|---|---|
| Setas | Direcional |
| X / Z | A / B (no Master System e Game Gear: botão 2 / botão 1) |
| D / C | X / Y do SNES (no controle: botão de cima e da esquerda; no SNES os botões seguem a posição do original) |
| A / S | L / R |
| Enter / Backspace | Start / Select (no Master System, Enter é o Pause; no Neo Geo Pocket, os dois são o Option) |
| Espaço (segurar) | Acelerar 4× |
| F5 / F8 | Salvar / carregar estado |
| 1–9 | Escolher o espaço de estado |
| P | Pausar |
| Ctrl+R | Reiniciar |
| F3 | Trocar a paleta do Game Boy (as mesmas da versão web) |
| F11 ou Alt+Enter | Tela cheia |

Controle de Xbox: mesmo mapeamento "letras" da versão web (A = A; B e X = B; LB/RB = L/R; Menu = Start;
View = Select; RT = acelerar; clique do analógico esquerdo = pausar).

Saves (`.sav`) e estados ficam em `%APPDATA%\Indigo\Indigo\`, com o nome completo da ROM (`Sonic.gg.sav`,
`Sonic.gg.1.state`), para que jogos com o mesmo nome em consoles diferentes não dividam o save. Os nomes da
versão 0.3 e anteriores (sem a extensão) ainda são lidos e passam para o nome novo na primeira gravação. O save é gravado sozinho a cada 2 s
quando muda e ao fechar. Na primeira vez que um jogo é aberto, um `.sav` com o mesmo nome ao lado da
ROM é importado (útil para trazer saves de outros emuladores ou da versão web).

### Game Boy Advance: BIOS

O GBA usa a BIOS própria do Índigo (o jogo começa direto). Se você tiver a BIOS do seu aparelho (16 KB), abra
o arquivo no Índigo uma vez (o nome precisa ter "bios", ex.: `gba_bios.bin`): ele é copiado para
`%APPDATA%\Indigo\Indigo\gba_bios.bin` e passa a ser usado nos jogos de GBA, com a animação de abertura.
Para voltar à BIOS própria, apague esse arquivo. O ajuste do relógio dos cartuchos com relógio (Pokémon
Ruby/Sapphire/Emerald, Boktai) vai junto no `.sav`, no mesmo formato da versão web.

## Backup e nuvem (os mesmos do site)

- **Backup `.indigo`**: menu da tela inicial → *Backup (.indigo)*. Exporta com ou sem as ROMs; importar é
  só abrir o arquivo (ou *Importar backup...*). É o mesmo formato do site: um backup feito no site abre aqui
  e vice-versa. Os jogos são reconhecidos pela mesma identidade da web (`src/core/backup.cpp`, conferida
  contra o código do site). Na importação nada é apagado: fica o save mais novo e o outro vai para as versões
  anteriores. Os estados salvos do site não vêm (o formato é outro).
- **Nuvem dos saves**: menu → *Nuvem dos saves*. Usa a API do próprio site (`/api/cloud`, com o mesmo PIN),
  sem mudar nada no Vercel. Ao abrir o app sincroniza; ao abrir um jogo traz o save se houver um mais novo
  na nuvem; depois de gravar, sobe em 20 s (ou ao fechar o jogo). Em conflito vale o mais novo e o outro vai
  para as versões anteriores. O horário de cada save fica em `saves/<jogo>.sav.at`.
- Teste local da nuvem com o código de verdade do site (`api/cloud.js`) e um depósito na memória:
  `INDIGO_PIN=1234 node native/tools/cloudmock.mjs 8787` e o app com `INDIGO_CLOUD_URL=http://127.0.0.1:8787`.

## Android (celular)

`android/` é o mesmo app com os mesmos núcleos, para o celular (testado para o Motorola Edge 70 com o
controle GameSir X2). Os APKs oficiais são publicados em https://indigo.nexus; para compilar o seu, assine com a sua própria chave.

No celular:
- Tela inicial com **Abrir jogo...** (seletor de arquivos do Android) e os últimos jogos abertos (o app guarda
  uma cópia de cada um na pasta dele).
- Controles na tela, com vibração, em pé (embaixo do jogo) ou deitado (dos lados). Eles somem quando um
  controle é usado e voltam ao tocar na tela.
- **MENU** na tela, o "voltar" do Android ou, no controle, o botão Home/Guia (ou Start + Select juntos):
  salvar/carregar estado, espaço de estado, acelerar 4×, reiniciar, paleta, abrir ou fechar jogo.
- `>>` (segurar) acelera 4×; no controle, o gatilho direito.
- Ao sair da tela (ligação, bloqueio, outro app) o save é gravado e o jogo pausa no menu.

Para testar a interface do celular no PC: `INDIGO_MOBILE=540x1200 indigo.exe` (em pé) ou `1200x540`
(deitado); o mouse faz o papel do dedo.

## Compilar

Do Linux, gerando o `.exe` do Windows (MinGW-w64 + SDL3 estático):

```bash
cmake -S native -B build-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/native/toolchain-mingw.cmake -DCMAKE_PREFIX_PATH=/caminho/do/sdl3-win
cmake --build build-win     # gera indigo.exe (sem DLLs extras) e indigo-run.exe
```

A versão fica em `native/VERSION` (a mesma do `.exe`, do instalador e do APK). Pacotes do PC (zip portátil +
instalador NSIS de 64 bits):

```bash
native/tools/pacote-pc.sh build-win saida/   # Indigo-PC-<versão>.zip e Indigo-<versão>-instalador.exe
```

Só os núcleos e a ferramenta de teste (sem SDL):

```bash
cmake -S native -B build -G Ninja -DINDIGO_APP=OFF && cmake --build build
```

## Validar o porte contra o núcleo JavaScript

Tudo de uma vez (ROMs de teste em `pasta_das_roms`, ver o cabeçalho do script):

```bash
native/tools/validate.sh build pasta_das_roms      # termina com "RESULTADO: N comparações iguais, 0 problemas"
```

Robustez, num build com AddressSanitizer + UndefinedBehaviorSanitizer: ROMs de lixo aleatório com cabeçalhos
válidos, arquivos vazios ou minúsculos e save states corrompidos, truncados ou de outro console. Estados
inválidos são recusados sem alterar o jogo; ao carregar um estado, cada núcleo limita os campos aos valores
que o hardware pode ter (um arquivo danificado não leva a acesso fora da memória nem a laço sem fim).

```bash
cmake -S native -B build-asan -DINDIGO_APP=OFF -DCMAKE_CXX_FLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
cmake --build build-asan && build-asan/indigo-robustez 12 250
```

Item por item:

```bash
build/indigo-run rom.gb 1500 dmg 37 > c.txt         # C++
node native/tools/jsref.mjs rom.gb 1500 dmg 37 > j.txt   # JS (src/gb)
cmp c.txt j.txt && echo IGUAL
build/indigo-run --statetest rom.gb                  # save state determinístico
build/indigo-zextest zexall.com                      # CPU Z80
build/indigo-tlcsfuzz 1000000 > c.txt; node native/tools/tlcsfuzz.mjs 1000000 > j.txt; cmp c.txt j.txt   # CPU TLCS-900H
build/indigo-run rom.gba 1500 gba 37 > c.txt; node native/tools/jsref.mjs rom.gba 1500 gba 37 > j.txt   # GBA
build/indigo-gbafuzz 1000 > c.txt; node native/tools/gbafuzz.mjs 1000 > j.txt; cmp c.txt j.txt   # GBA com ROMs aleatórias
INDIGO_GBA_BIOS=bios.bin build/indigo-run rom.gba 600 gba   # GBA com uma BIOS de verdade (o jsref lê a mesma variável)
```

O relógio do Neo Geo Pocket e o dos cartuchos de GBA ficam fixos em 19/10/2026 12:34:56 (hora local) nas
duas ferramentas de comparação.

**Diferenças deliberadas em relação ao JS** (só em entradas que travam o núcleo JS para sempre; nenhum jogo
as usa):

- Neo Geo Pocket: uma forma inválida da instrução CPIR (com o próprio XBC como ponteiro) para em 65.536 voltas.
- GBA, BIOS própria: BitUnPack com largura de origem 0, e Huffman com 0 bits por símbolo ou com uma árvore
  sem folhas, retornam em vez de travar.

**Comportamento do JS mantido de propósito** (para o porte continuar idêntico; corrigir nos dois lados juntos):
no GBA, a multiplicação com operando negativo sempre custa 4 ciclos, porque no JS
`(v & 0xFFFFFF00) === 0xFFFFFF00` compara um número negativo com um positivo e nunca é verdade (no aparelho
custaria de 1 a 3). Só muda a contagem de ciclos, não o resultado.

A ROM de teste do Master System/Game Gear é compilada com SDCC + devkitSMS (`tests/sms/indigotest.c`)
e completada até 128 KB por `tests/sms/mkbanks.py` (`--codemasters` gera a variante do mapeador Codemasters).

Cada linha traz o quadro, o hash da imagem, o hash do som e o número de amostras; o último argumento
aperta botões em sequência a cada N quadros, para exercitar o jogo.

## Estrutura

| Pasta | O que tem |
|---|---|
| `src/core/` | Interface comum dos núcleos (`core.h`), escolha do núcleo pela ROM, serialização de estados |
| `src/gb/` | Núcleo Game Boy / Game Boy Color |
| `src/sms/` | Núcleo Master System / Game Gear (o Z80 também é o processador de som do Neo Geo Pocket) |
| `src/ngp/` | Núcleo Neo Geo Pocket / Pocket Color com a BIOS própria do Índigo (nenhum byte da BIOS da SNK) |
| `src/gba/` | Núcleo Game Boy Advance com a BIOS própria do Índigo (nenhum byte da BIOS da Nintendo) |
| `src/app/` | Aplicativo SDL3 (PC e Android): janela, som, teclado, controle, toque, menu, saves e estados |
| `android/` | Projeto Android (Gradle + NDK) que empacota `src/` no APK |
| `tools/` | `validate.sh` (revalidação completa), `robustez.cpp` (ROMs e estados corrompidos), `runrom.cpp` (teste sem interface), `jsref.mjs` (mesma saída a partir do núcleo JS), `zextest.cpp` (ZEXDOC/ZEXALL), `tlcsfuzz.cpp` + `tlcsfuzz.mjs` (teste diferencial da CPU do Neo Geo Pocket), `gbafuzz.cpp` + `gbafuzz.mjs` (teste diferencial do GBA inteiro com ROMs aleatórias) |
| `tests/ngp/` | Gerador da ROM de teste de save do Neo Geo Pocket (código de máquina montado à mão) |
| `tests/sms/` | ROM de teste própria (bancos de 128 KB, Codemasters, save, interrupção de linha, sprites, som, estéreo do Game Gear) |
