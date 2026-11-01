# Cronograma do Índigo (PC e celular)

Do porte atual (Game Boy) até o PlayStation 1 (decisão do João em 31/10/2026: o Índigo para no PS1 e passa a só melhorar),
no PC e no celular (Motorola Edge 70 + GameSir X2), em etapas com checkpoints verificáveis.
Cada check só é marcado quando o critério de aceite foi testado, não quando o código "parece pronto".

**Início:** 19/10/2026 · **Plataformas:** Windows e Android (C++20 + SDL3, o mesmo código) · **Branch:** `native-pc`

> **Ritmo: uma sessão por dia.** As datas assumem trabalho diário. Elas servem para medir o avanço:
> quando uma etapa adiantar ou atrasar, as seguintes são recalculadas aqui.

## Visão geral

| Etapa | O que entrega | Previsão (ritmo diário) | Antes (ritmo semanal) | Situação |
|---|---|---|---|---|
| 0 | Base C++ + Game Boy / Game Boy Color | 19/10/2026 | 19/10/2026 | ✅ Concluída |
| 1 | Master System / Game Gear | 20/10/2026 (1 dia) | 20/10 – 26/10 | ✅ Concluída em 19/10 |
| 2 | Neo Geo Pocket / Pocket Color | 21/10 – 22/10/2026 | 27/10 – 09/11 | ✅ Concluída em 20/10 |
| 3 | Game Boy Advance | 23/10 – 26/10/2026 | 10/11 – 30/11 | ✅ Concluída em 21/10 |
| 4b | Android (Motorola Edge 70 + controle GameSir) | 22/10 – 26/10/2026 | — | ✅ Concluída em 21/10 |
| 4 | Interface no nível da versão web (PC e celular) | 22/10 – 26/10/2026 | dezembro | ✅ Concluída em 21/10 (atalhos do controle aguardando seu teste) |
| 5 | Índigo 1.0 para PC e Android (distribuição) | 27/10 – 31/10/2026 | até 11/01/2027 | ✅ Concluída em 21/10 (1.0 na `main`) |
| **Fase 2** | **Tudo o que roda num Anbernic** (pedido em 21/10) | | | |
| 6 | 8 e 16 bits: NES, Mega Drive, SNES, PC Engine | 01/11 – 07/12/2026 | — | ✅ Concluída em 25/10 (versão 1.7; aguardando seu teste com jogos) |
| 7 | Clássicos e portáteis menores: Atari 2600/7800, Lynx, WonderSwan, Virtual Boy, Pokémon Mini, MSX, ColecoVision, SG-1000, Vectrex | 08/12/2026 – 08/01/2027 | — | ✅ Concluída em 24/10 (versão 1.6; aguardando seu teste com jogos) |
| 8 | Arcade e acessórios de CD: Neo Geo (MVS/AES/CD), CPS1/CPS2, Sega CD, 32X, PC Engine CD | 09/01 – 19/02/2027 | — | ⏳ Tudo feito (1.8 e 1.9); aguardando teste do João com BIOS e jogos dele (Sega CD, PC Engine CD, Neo Geo CD, 32X) |
| 9 | PlayStation 1 | 20/02 – 31/03/2027 | jan – mar/2027 | ⏳ Feito na 2.0 (9.1 a 9.7 com testes e disco próprio); aguardando jogos do João; 9.8 (resolução 2×/4×) feito em 01/11/2026 |
| 9b | Panasonic 3DO (último console, aprovado pelo João; mesma geração do PS1) | 01/11/2026 | — | ⏳ Núcleo feito para a 2.2 com testes, BIOS e homebrew próprios; aguardando a BIOS e os jogos do João |
| 10 | ~~Nintendo 64~~ | ❌ Cancelada em 31/10/2026 (o Índigo para no PS1) |
| 11 | ~~Nintendo DS~~ | ❌ Cancelada em 31/10/2026 (o Índigo para no PS1) |
| 12 | ~~PSP~~ | ❌ Cancelada em 31/10/2026 (o Índigo para no PS1) |
| 13 | ~~PlayStation 2~~ | ❌ Cancelada em 31/10/2026 (o Índigo para no PS1) |
| 14 | ~~Sega Saturn (+ CPS3)~~ | ❌ Cancelada em 31/10/2026 (o Índigo para no PS1) |
| 15 | ~~Dreamcast (+ NAOMI)~~ | ❌ Cancelada em 31/10/2026 (o Índigo para no PS1) |
| 16 | ~~GameCube / Wii~~ | ❌ Cancelada em 31/10/2026 (o Índigo para no PS1) |
| 17 | ~~Nintendo 3DS~~ | ❌ Cancelada em 31/10/2026 (o Índigo para no PS1) |
| **Fase 3** | **Manutenção e melhorias até o PS1** (decisão de 31/10/2026): código aberto, precisão, compatibilidade, velocidade e recursos | contínua, em versões 2.x | — | ⏳ Começou na 2.0 |

**Ganho do ritmo diário:** a versão 1.0 chega cerca de 2 meses antes do previsto no ritmo semanal.

**Com a Fase 2 (21/10):** entrar com tudo o que roda num Anbernic, na ordem de dificuldade, empurra o PS1 de
novembro/2026 para fevereiro/2027 e o PS2 de janeiro/2027 para dezembro/2027. As datas depois de 2027 são
indicativas: são projetos de meses cada um e serão recalculadas ao fim de cada etapa.

### Por que esta ordem

1. **8 e 16 bits primeiro:** é o que mais se joga num Anbernic, cada console sai em dias e todos rodam com
   folga no celular. Os processadores se repetem e são reaproveitados adiante: o 6502 do NES serve ao Atari e ao
   Lynx; o 68000 do Mega Drive serve ao Neo Geo, ao CPS1/CPS2 e ao Sega CD; o Z80 já existe.
2. **PS1 antes dos 3D maiores:** o R3000A do PS1 é o processador de entrada/saída do PS2, e a técnica de
   desenhar 3D na GPU com resolução aumentada nasce nele.
3. **N64, DS e PSP antes do PS2:** são menores e criam o recompilador (JIT) que o PS2 vai precisar.
4. **Saturn, Dreamcast, GameCube/Wii e 3DS por último:** são os mais pesados ou os que só os Anbernic mais
   fortes rodam.

**Se o PS2 for mais importante para você do que o resto,** dá para trocar: PS2 logo depois do PS1
(abril/2027) e N64, DS e PSP depois dele. É só dizer.

### Todo console novo vale para o PC e o celular

O mesmo código roda nos dois. Cada etapa só fecha quando o console roda em velocidade cheia também no Edge 70
com o X2, e com o APK enviado junto com o `.exe`.



### Por que o ritmo diário encurta tanto as etapas 1 a 3

Elas são portes de núcleos que já existem e já estão testados em JavaScript. O trabalho é traduzir e
comparar quadro a quadro, e a comparação é automática. O Game Boy inteiro saiu em uma sessão.

### O que continua limitando, mesmo trabalhando todo dia

- **Seus testes no notebook.** Cada `.exe` precisa de uns 15 minutos seus jogando antes de virar check.
  Sem esse retorno, a etapa seguinte começa, mas a anterior não é fechada.
- **Da Fase 2 em diante não há versão JS para comparar.** A régua passa a ser a dos testes públicos de cada
  console (ROMs de teste e os testes de processador instrução a instrução, como os ProcessorTests). Cada
  defeito vira investigação, e é isso que toma tempo, não escrever o código.
- **Arquivos que só você pode fornecer** (extraídos dos seus consoles e jogos): BIOS do PS1, PS2, Saturn,
  Dreamcast, Sega CD, PC Engine CD (System Card), Neo Geo e Lynx; imagens dos seus jogos para testar. Onde der,
  o Índigo terá BIOS própria (como já faz no GBA e no Neo Geo Pocket); onde não der, o console espera o arquivo.
- **O processador do notebook** ainda não está registrado. Ele define a meta realista do JIT do PS2 (marco 7.4).

Legenda: ✅ concluído · ⏳ em andamento / próximo · ⬜ não iniciado

## Regras que valem para todas as etapas

1. **Porte fiel antes de melhorar.** Um núcleo portado do JS só está pronto quando `indigo-run` e
   `jsref` dão a mesma saída (imagem e som) quadro a quadro. Melhorias de precisão vêm depois, nos dois lados.
2. **Testes públicos como régua.** Cada console tem suas ROMs de teste de referência (listadas em cada etapa).
3. **Velocidade cheia no seu notebook.** Medida com `indigo-run` (deve rodar bem acima de 1× em um núcleo).
4. **A versão web e o Vercel nunca mudam por causa desta pasta.** Junção na `main` só com a sua aprovação.
5. **Cada etapa termina com um `.exe` enviado para você testar.**
6. **Uma sessão por dia, com começo e fim definidos:** cada sessão fecha com o que foi feito, o próximo passo e o registro atualizado.

---

## Etapa 0 · Base + Game Boy / Game Boy Color ✅

- [x] Estrutura `native/` com CMake, C++20 e SDL3 estático (um único `.exe`, sem DLLs)
- [x] Interface comum dos núcleos (`core.h`) e save states
- [x] Porte de `src/gb`: CPU SM83, vídeo, som, MBC1/2/3/5 com relógio
- [x] Ferramentas de comparação `indigo-run` × `jsref.mjs`
- [x] App: abrir/arrastar jogo, teclado da web, controle de Xbox, saves automáticos, estados, paletas, tela cheia

**Critério de aceite (cumprido):** 58 ROMs de teste do Blargg × DMG/CGB × 1.500 quadros idênticos ao JS;
save state determinístico; o `.exe` do Windows dá a mesma saída.

## Etapa 1 · Master System / Game Gear ✅

- [x] Porte do Z80 (`src/sms/z80.js`), com instruções e flags não documentadas
- [x] VDP modo 4, sprites, interrupções de linha, janela 160×144 do Game Gear
- [x] Som SN76489 com o estéreo do Game Gear
- [x] Mapeadores Sega e Codemasters, RAM do cartucho, save
- [x] App: tela 4:3 (SMS) e 10:9 (GG), botões 1/2 e Pause/Start
- [x] ROM de teste própria (`tests/sms/`) para o que os exemplos não cobrem

**Critério de aceite (cumprido):**
- ZEXDOC e ZEXALL 100% no C++ (67 de 67 cada, também no `.exe` do Windows)
- 9 ROMs (exemplos do devkitSMS, Winter Shooter, ROM de teste nas variantes SMS, Game Gear e Codemasters)
  × 3 padrões de botões × 1.500 quadros idênticos ao JS (imagem, som e save); save state determinístico
- `.exe` 0.2 enviado

## Etapa 2 · Neo Geo Pocket / Pocket Color ✅

- [x] Porte da CPU TLCS-900H (`src/ngp/tlcs900h.js`)
- [x] Vídeo K2GE/K1GE, som T6W28 + DACs, Z80 de som (o mesmo da etapa 1)
- [x] BIOS própria do Índigo (HLE), relógio, flash do cartucho (save `.ngf`)
- [x] App: tela 160×152, Option no Enter/Backspace, importa `.ngf` de outros emuladores

**Critério de aceite (cumprido):**
- CPU: em vez do rastro da referência (que não está no repositório), teste diferencial com
  2 milhões de instruções aleatórias: registradores, flags, DMA, gravações e ciclos idênticos ao JS
- 5 jogos homebrew (Tutankham, Blueprint, Asteroids, Pengu, Zork I) × 3 padrões de botões × 1.500 quadros
  idênticos ao JS; ROM de teste de save (BIOS + comandos diretos na flash) com o mesmo `.ngf`
- `.exe` 0.3 enviado

Achado no caminho: uma forma inválida da instrução CPIR trava o núcleo JS para sempre; no C++ ela tem limite
(documentado no README). A versão web não foi alterada.

## Etapa 3 · Game Boy Advance ✅

- [x] Porte da CPU ARM7TDMI (ARM + Thumb), barramento com estados de espera e prefetch
- [x] Vídeo modos 0–5, afins, janelas, blending, mosaico
- [x] Som (PSG + DirectSound com FIFO/DMA), DMA, timers
- [x] BIOS HLE, saves (SRAM, Flash, EEPROM), relógio GPIO (Pokémon Ruby/Sapphire/Emerald)
- [x] Opção de BIOS original fornecida por você (abrir o arquivo da BIOS no Índigo instala)

**Critério de aceite (cumprido):**
- jsmolka/gba-tests (arm, thumb, memory, bios, sram, flash64, flash128, none) passando no C++
- 222 ROMs (demos do krom/PeterLemon, testes do alyosha-tas e do jsmolka) × 3 padrões de botões × 1.500
  quadros idênticos ao JS (imagem, som e save); caminho com BIOS de verdade testado com a BIOS livre Cult-of-GBA
- Teste diferencial do GBA inteiro com ROMs de código aleatório e do relógio do cartucho em 3 fusos horários
- Velocidade: 5,5× no pior caso medido (vídeo em tela cheia) no Xeon de 2,1 GHz do ambiente de testes;
  no notebook deve passar disso com folga
- `.exe` 0.4 enviado

Achados no caminho: na versão web, a multiplicação com operando negativo sempre custa 4 ciclos (comparação
de número negativo com positivo no JS); o C++ mantém igual para continuar idêntico, e a correção fica para os
dois lados juntos. Dois casos da BIOS própria que travam o JS para sempre (BitUnPack e Huffman com tamanho 0)
retornam no C++. A versão web não foi alterada.

## Etapa 4b · Android (celular portátil) ✅

Pedido em 21/10: jogar no Motorola Edge 70 (Snapdragon 7 Gen 4, 12 GB, Android 16) com controle GameSir.
Os núcleos em C++ e o SDL3 são os mesmos do PC; o APK é gerado pelo GitHub Actions (o ambiente de trabalho
não acessa as ferramentas do Android). O site no Vercel não muda.

- [x] Projeto Android (NDK + SDL3) com os mesmos núcleos; APK gerado a cada envio na branch `native-pc`
- [x] Controle GameSir (Bluetooth e encaixe USB-C) e outros controles padrão do Android — testado por você no Edge 70
- [x] Botões na tela com vibração (só aparecem sem controle), tela deitada e em pé
- [x] Abrir jogos pelo seletor de arquivos do Android; saves e estados na pasta do app
- [x] Pausa e save automáticos quando o app sai da tela (ligação, bloqueio)

**Critério de aceite:** no Edge 70, um jogo de cada console em velocidade cheia com o GameSir; salvar,
fechar o app e continuar; APK 0.5 enviado.

## Etapa 4 · Interface no nível da versão web ✅

- [x] Estante de jogos: todos os jogos em páginas, filtro por console, favoritos (primeiro da lista), capas automáticas (a tela do jogo ao fechar, ou "usar esta tela como capa") e, no PC, pastas de jogos com subpastas
- [ ] Coleções (listas com nome escolhido por você) — fica para depois da 1.0
- [x] Continuar de onde parou (estado automático ao fechar o jogo, sair do app ou o Android tirar o app da tela)
- [x] 9 espaços de estado com data e miniatura
- [x] Saves com versões anteriores (12, restauráveis pelo menu)
- [x] Importar/exportar backup `.indigo` da versão web (identidade dos jogos conferida contra o código do site)
- [x] Menu navegável só com o controle, o toque ou o teclado (tela inicial com continuar e recentes)
- [x] Remapeamento de teclado e controle (menu Controles: aperte a tecla/botão de cada ação; teclado testado no PC, controle aguardando teste do João)
- [x] Atalhos no controle, como num Anbernic: Select + R salva estado, Select + L carrega, Select + ←/→ troca o espaço, Select + Start menu (aguardando teste do João com o GameSir)
- [x] Leitura de `.zip` (DEFLATE próprio, testado contra o Python e com arquivos danificados)
- [x] Filtros de tela: nítido, suave, LCD (grade), TV de tubo (linhas), EPX — escolhidos no menu do jogo e lembrados
- [ ] Filtros avançados xBR e HD Remaster (shaders) — fica para depois da 1.0
- [x] Saves na nuvem com o mesmo PIN do site (só **lê/grava** pela API que já existe; o Vercel não muda)

**Critério de aceite:**
- Um save feito no site abre no PC e vice-versa
- Dá para jogar do início ao fim de uma sessão sem tocar no teclado (só controle)
- `.exe` 0.9 enviado

## Etapa 5 · Índigo 1.0 para PC e Android ✅

- [x] Ícone e informações do programa no `.exe` (versão única em `native/VERSION`, a mesma do APK)
- [x] APK assinado para atualizar por cima e tela "Sobre e novidades" com a versão (PC e celular)
- [x] Instalador simples (só para o usuário, sem administrador; desinstala sem apagar os saves) e versão portátil (zip) — `tools/pacote-pc.sh`
- [x] Associação de arquivos (`.gb`, `.gbc`, `.gba`, `.gg`, `.sms`, `.ngp`, `.ngc`) pelo instalador ou em "Sobre e novidades"; não tira o padrão de outro programa
- [x] Registro de melhorias (`src/app/novidades.h`, mostrado no app)
- [x] Junção da branch `native-pc` na `main` (aprovada por você em 21/10; o site continuou igual)

**Critério de aceite:** instalar num Windows limpo, abrir um jogo de cada console, salvar, fechar e continuar.

---

# Fase 2 · Tudo o que roda num Anbernic

## Etapa 6 · 8 e 16 bits

| Check | Previsão | Console | Como verificar |
|---|---|---|---|
| 6.1 | 01 – 03/11/2026 | **NES** (6502, PPU, APU, mapeadores mais comuns: NROM, MMC1, UxROM, CNROM, MMC3) | `nestest` com o registro de referência; ROMs de teste do blargg; ProcessorTests do 6502 |
| 6.2 | 04 – 11/11 | **Mega Drive / Genesis** (68000, VDP, YM2612 + PSG; Z80 reaproveitado) | ProcessorTests do 68000; 240p Test Suite; jogos homebrew |
| 6.3 | 12/11 – 02/12 | **SNES** (65816, PPU com os modos 0–7, SPC700 + DSP, DMA/HDMA, chips mais comuns: DSP-1 e Super FX ficam para o fim) | ProcessorTests do 65816 e do SPC700; ROMs de teste do PeterLemon e do undisbeliever |
| 6.4 | 03 – 07/12 | **PC Engine / TurboGrafx-16** (HuC6280, VDC, PSG) | ROMs de teste públicas; homebrew |

Andamento do 6.3 (SNES), adiantado para 22/10 a pedido do João (antes do Mega Drive):
- [x] CPU 65816: 5.080.400 casos dos SingleStepTests iguais (registradores, memória, cada ciclo e o endereço dos
  ciclos internos), modo nativo e emulação, decimal, cópia de blocos
- [x] SPC700: 256.000 casos dos SingleStepTests iguais, inclusive leituras falsas e ciclos internos
- [x] Sistema: LoROM/HiROM/ExHiROM com SRAM, WRAM, multiplicação/divisão, DMA, HDMA (direto e indireto), NMI, IRQ
  de H/V, leitura automática dos controles, relógio por linha com a renovação da DRAM
- [x] Som: IPL, timers, portas, DSP com BRR, interpolação gaussiana, ADSR/GAIN, ruído, modulação de pitch e eco
- [x] Vídeo: modos 0–7, OPT, Mode 7 + EXTBG, sprites (32/34), janelas, cores, mosaico, alta resolução e entrelaçado
  nos modos 5/6
- [x] ROMs de teste do PeterLemon: 30 de 30 de CPU/SPC700 com PASS; telas de vídeo iguais às de referência
- [x] App do PC e do celular: `.sfc`/`.smc`, aba SNES, botões X e Y (toque, controle e teclado), saves e estados
- [ ] Teste do João com jogos dele (aguardando teste do João)
- [x] Super FX (22/10, sessão da madrugada): GSU-1/GSU-2 com todas as instruções (ALT1/2/3, WITH/FROM/TO), cache
  de 512 bytes, buffers de ROM/RAM, PLOT/RPIX com o cache de pixels (2, 4 e 8 bits; 128, 160, 192 linhas e o modo
  de sprites), multiplicações, IRQ e a ROM que o SNES vê enquanto o GSU roda; save de bateria. 31 de 31 ROMs de
  teste por instrução do PeterLemon com PASS e 24 de 27 demos de PLOT com a tela igual à referência (as 3 restantes:
  2 linhas que a própria imagem de referência mostra acesas). De quebra: o HDMA agora interrompe o DMA comum do
  mesmo canal, como no console. Teste com Star Fox, Yoshi's Island e outros (aguardando teste do João)
- [x] DSP-1 (22/10, decisão do João: "os dois"): com o arquivo do chip (`dsp1b.rom`, 8 KB, aberto no app como um
  jogo) o Índigo roda o programa original num µPD7725 completo; sem ele, imita as contas de todos os comandos
  (multiplicação, inverso, seno/cosseno, distância, rotação, projeção, Raster/Mode 7, atitude e coordenadas,
  mapas LoROM pequeno/grande e HiROM). 126 verificações (`indigo-dsp1test`): µPD7725 com programas montados no
  teste, as contas imitadas e o chip no barramento do SNES. Teste com Super Mario Kart e Pilotwings (aguardando
  teste do João). DSP-2/3/4 (Dungeon Master, SD Gundam GX, Top Gear 3000) avisam que falta o chip
- [x] SA-1 (22/10): segundo 65816 a 10,74 MHz, I-RAM e BW-RAM (inclusive como bitmap de 2/4 bits), Super MMC até
  8 MB, mensagens e IRQ/NMI entre as CPUs com a troca de vetores, DMA, conversão de bitmap tipos 1 e 2,
  multiplicação/divisão/soma de produtos, bits de tamanho variável e timer H/V. 41 verificações (`indigo-sa1test`,
  cartucho de mentira com um programa para o SA-1 montado no teste; não há ROMs de teste públicas do chip).
  Teste com Super Mario RPG, Kirby Super Star e Kirby's Dream Land 3 (aguardando teste do João)
- [x] S-DD1 (24/10, sessão da madrugada): janelas de 1 MB em $C0-$FF (ROMs de até 16 MB) e descompressão durante o
  DMA, com o fim da transferência desligando o canal. 91 verificações (`indigo-sdd1test`): 64 fluxos iguais ao
  descompressor do bsnes (e, fora do repositório, 6 mil fluxos e 13,8 milhões de bytes iguais) e o chip no barramento
  do SNES num cartucho de mentira (não há ROMs de teste públicas do chip). Teste com Star Ocean e Street Fighter
  Alpha 2 (aguardando teste do João). Feito na branch `noite-2026-10-24` e trazido para a `native-pc` em 30/10 (versão 1.7.1)
- [x] SPC7110 (31/10, sessão da madrugada): descompressão (codificação aritmética com modelo de contexto, 1, 2 e 4
  bits por pixel, tabela de fluxos, pulos e passos entre linhas), porta de dados com todos os modos de $4818, contas
  16×16 e 32÷16 com e sem sinal (com o tempo de espera), janelas de 1 MB da ROM de dados, RAM ligada pelo $4830 e o
  relógio RTC-4513 do Far East of Eden Zero; save de bateria (só a RAM de 8 KB). 120 verificações
  (`indigo-spc7110test`): 48 roteiros do chip e 32 do relógio iguais ao bsnes (fora do repositório, 3 mil e 20 mil
  roteiros iguais) e o chip no barramento num cartucho de mentira (não há ROMs de teste públicas do chip). O relógio
  segue a hora do aparelho (decisão do João em 31/10: ao ligar, ao carregar estado e na troca 12/24 horas). Teste com Far East of Eden Zero,
  Momotarou Dentetsu Happy e Super Power League 4 (aguardando teste do João)
- [ ] Outros chips especiais: CX4, DSP-2/3/4
- [ ] Entrelaçado completo (448 linhas nos outros modos) e jogos PAL

Andamento do 6.1 (NES), começado em 21/10:
- [x] CPU 2A03: 2,44 milhões de casos dos SingleStepTests do 6502 iguais (registradores, memória e cada ciclo do
  barramento, inclusive as instruções ilegais); `nestest` igual ao registro de referência instrução por instrução
- [x] PPU ponto a ponto, APU com os 5 canais, DMA de sprites e do DMC, controles, save de bateria e estados
- [x] Mapeadores 0, 1, 2, 3, 4, 7, 9, 10, 11, 21, 22, 23, 25, 34, 66, 69, 71, 79, 140 e 206 (NROM, MMC1,
  UxROM, CNROM, MMC3, AxROM, MMC2, MMC4, Color Dreams, VRC2/VRC4 da Konami, BNROM, GxROM, Sunsoft FME-7,
  Camerica, NINA, Jaleco, Namco 108: a maior parte dos jogos americanos e japoneses)
- [x] ROMs de teste públicas: 71 com o resultado "passou" pelo protocolo $6000 (CPU, interrupções, APU, PPU,
  vblank/NMI, open bus, OAM, MMC3) e mais 53 de tela (sprite 0, excesso de sprites com o defeito do chip,
  tempo de desvio, vblank, MMC3, APU de 2005) — fora só as que testam outras revisões do MMC3 e o MMC6
- [x] No app do PC e do celular: abre `.nes` (e `.nes` dentro de `.zip`), estante, capas, nuvem e estados
- [ ] Teste do João com jogos dele (aguardando teste do João)
- [x] Mapeadores com chips próprios (22/10): MMC5 (5: bancos, RAM de até 64 KB, ExRAM, atributos estendidos, tela
  dividida, preenchimento, IRQ por linha, multiplicador e som), VRC6 (24/26: som de 2 quadradas + dente de serra),
  VRC7 (85: som FM de 6 canais), Namco 163 (19: até 8 canais de onda, tabelas de nomes na CHR ROM) e o som do
  Sunsoft 5B (69). Conferidos por `indigo-nesmappertest` (bancos, IRQ e a frequência do som de cada chip) e pelas
  ROMs de teste públicas do MMC5; o teste com os jogos de verdade fica com o João (aguardando teste do João)
- [ ] PAL (jogos europeus) e o Famicom Disk System ficam para depois

**Critério de aceite:** um jogo seu de cada console do início ao fim da primeira fase, em velocidade cheia no
notebook e no Edge 70; saves, estados e nuvem funcionando como nos portáteis.

## Etapa 7 · Clássicos e portáteis menores

| Check | Previsão | Console |
|---|---|---|
| 7.1 | 08 – 09/12/2026 | Atari 2600 (6507 = o 6502 do NES; TIA) |
| 7.2 | 10 – 12/12 | Atari 7800 |
| 7.3 | 13 – 17/12 | Atari Lynx (65C02 + Suzy/Mikey) |
| 7.4 | 18 – 21/12 | WonderSwan / Color (V30MZ) |
| 7.5 | 22 – 26/12 | Virtual Boy (V810; vermelho e preto, com opção de 3D anaglífico) |
| 7.6 | 27 – 28/12 | Pokémon Mini |
| 7.7 | 29/12/2026 – 08/01/2027 | MSX, ColecoVision, SG-1000 (o núcleo do Master System já cobre quase tudo) e Vectrex |

**Critério de aceite:** ROMs de teste públicas de cada um e um jogo seu (ou homebrew) de cada.

Andamento da etapa 7 (24/10, adiantada a pedido do João; todos com save state, robustez e revalidação):
- [x] 7.1 Atari 2600: telas de 81 ROMs de teste iguais ao Stella em 300 quadros; 14 esquemas de bancos
- [x] 7.2 Atari 7800: tempos de DMA, tons e ciclos medidos; 36 exemplos do 7800basic; SaveKey
- [x] 7.3 Atari Lynx: CPU igual a 2,56 milhões de casos; telas iguais ao Handy; partida imitada ou com a
  lynxboot.img do usuário (testada com a do João)
- [x] 7.4 WonderSwan / Color: SingleStepTests do V20, WSCPUTest e WSHWTest
- [x] 7.5 Virtual Boy: 565 quadros dos dois olhos iguais ao Mednafen; 3D anaglifo ou lado a lado
- [x] 7.6 Pokémon Mini: CPU contra 300 mil vetores do PokeMini; BIOS livre embutida
- [x] 7.7 MSX/MSX2 (C-BIOS embutida), ColecoVision (BIOS do usuário), SG-1000 (vídeo TMS9918 próprio) e
  Vectrex (BIOS do usuário; 15 milhões de instruções iguais ao vecx)
- [ ] Teste do João com jogos dele (aguardando teste do João)

## Etapa 8 · Arcade e acessórios de CD

| Check | Previsão | O que entrega |
|---|---|---|
| 8.1 | 09 – 15/01/2027 | Neo Geo MVS/AES (68000 + Z80, BIOS sua) e Neo Geo CD |
| 8.2 | 16 – 25/01 | Capcom CPS1 e CPS2 (68000; CPS2 com a decodificação das ROMs) |
| 8.3 | 26/01 – 05/02 | Sega CD / Mega-CD (segundo 68000, CD, BIOS sua) |
| 8.4 | 06 – 14/02 | Sega 32X (dois SH-2: o mesmo processador do Saturn e do CPS3, adiantando a etapa 14) |
| 8.5 | 15 – 19/02 | PC Engine CD (System Card sua) |

**Critério de aceite:** jogos seus de cada sistema até a primeira fase, com os arquivos `.cue/.bin` e `.chd`.

## Etapa 9 · PlayStation 1

Primeiro console 3D. O R3000A feito aqui vira o processador de entrada/saída (IOP) do PS2.

| Check | Previsão | O que entrega | Como verificar |
|---|---|---|---|
| 9.1 | 20 – 24/02/2027 | CPU R3000A (interpretador), memória, exceções | Testes de CPU do amidog e de `ps1-tests` (JaCzekanski) |
| 9.2 | 25 – 26/02 | BIOS: inicialização com a BIOS original fornecida por você | Logo da Sony e menu do console aparecem |
| 9.3 | 27/02 – 05/03 | GPU por software (2D, polígonos, texturas) | Testes de GPU do `ps1-tests`; demos homebrew |
| 9.4 | 06 – 10/03 | GTE (geometria 3D) e DMA | Testes de GTE; primeiras cenas 3D corretas |
| 9.5 | 11 – 17/03 | CD-ROM (`.cue/.bin`, `.chd`), MDEC (vídeos) | Jogo comercial seu inicia e toca a abertura |
| 9.6 | 18 – 22/03 | SPU (som, reverb), timers | Música e efeitos corretos nos jogos testados |
| 9.7 | 23 – 25/03 | Cartões de memória, controle analógico, vibração | Salvar e carregar no cartão; vibração no controle |
| 9.8 | 26 – 31/03 | Resolução aumentada (1×/2×/4×) — feita na GPU por software, sem OpenGL/Vulkan (ver abaixo) | Mesmo jogo em 2×/4× sem defeitos, no PC e no celular |

**Critério de aceite da etapa:** 5 jogos comerciais seus jogáveis do início ao fim da primeira fase, em
velocidade cheia no notebook e no Edge 70.

**Situação (31/10/2026, versão 2.0):**
- ✅ 9.1 a 9.7 feitos e juntos no console inteiro: CPU, BIOS até o logo e o menu, GPU por software, GTE, DMA
  (blocos sob pedido do MDEC, CD e SPU), CD-ROM (`.cue/.bin`, `.chd`, `.iso`; CD-DA e XA), MDEC, SPU (com o
  áudio do CD), timers, cartão de memória (save `.mcr`), DualShock (analógicos e vibração) e o app (BIOS
  instalada ao abrir, aba PS1, toque com os símbolos do PlayStation).
- ✅ Conferido sem jogos comerciais: ps1-tests (psx.log e VRAM do console) pela CPU e pelo DMA, um disco de teste
  montado aqui (PSn00bSDK + ISO 9660 próprio) que dá a partida pela BIOS do João e roda 3D com GTE, som da SPU,
  CD-DA e leitura do CD, com o quadro igual ao do Beetle PSX (`t9_ps1int`); ~370 quadros/s no PC de testes.
- ✅ Precisão contra o console (01/11/2026), pelos resultados do ps1-tests tirados de um PlayStation de verdade:
  `dma/chopping` com o chopping emulado e os tempos dos blocos (128 de 130 transferências a 5% dos ciclos do
  console, todas a 10%); `mdec/step-by-step-log` com o fluxo das FIFOs do MDEC como no chip (status igual nas
  777 leituras); `cdrom/getloc` com a busca além do fim do disco dando erro e o Init parando no pregap (44 de
  46 linhas iguais fora as posições; o "Test passed" se perde por um defeito do próprio .exe compilado aqui);
  `cdrom/timing` com o Init completo em 474 663 x 476 300 ciclos. Fica a IDCT do MDEC (±1 no bit menos
  significativo, como no Beetle: nenhuma variante de arredondamento testada bate com o console).
- ⏳ Aguarda os jogos do João: jogo comercial iniciando e tocando a abertura (9.5), música e efeitos nos jogos
  (9.6), salvar no cartão dentro de um jogo (9.7), o critério de aceite (5 jogos) e a velocidade no Edge 70.
- ✅ 9.8 (01/11/2026): resolução interna 1×, 2× ou 4× ("Resolução do PlayStation" no menu do jogo e nos Ajustes;
  `ps1res=` no `opcoes.txt`; sem escolha, 2× no PC e 1× no celular). Feita dentro da GPU por software, como o
  modo "internal resolution" do Beetle PSX, em vez de OpenGL/Vulkan: o mesmo código no PC e no Android, sem API
  gráfica nova. A VRAM de 1024 × 512 continua sendo a de verdade (CPU, DMA, cópias, texturas, estado salvo); os
  polígonos também são desenhados numa cópia ampliada da VRAM (vértices × escala, cor e textura interpoladas em
  cada ponto ampliado), que dá a imagem. Escritas da CPU/DMA vão repetidas para a cópia, retângulos e linhas saem
  como blocos, cópias VRAM→VRAM e preenchimentos também na escala; vídeo de 24 bits (MDEC) sai ampliado por
  repetição. O estado salvo não leva a cópia (refeita ao carregar) e é o mesmo byte a byte em 1×, 2× e 4×; dá
  para trocar no meio do jogo. 1× continua idêntico ao de antes (t9_ps1 e t9_ps1int com as mesmas saídas).
  Conferido: o canto de cada bloco ampliado (o ponto que o console amostra) é igual à imagem de 1× em ≥ 99,6%
  dos pontos (11 testes de GPU do ps1-tests e o disco de teste), a VRAM de verdade igual à de 1×, e as bordas
  ganham detalhe abaixo de um ponto em ~80% dos pontos de borda. A média de cada bloco (filtro de caixa) fica
  igual à de 1× em 86% (2×) e 83% (4×) dos pontos com tolerância 16: o resto é o xadrez de 1 texel dos cubos, que
  em 1× sai serrilhado e na ampliada sai como média — não é defeito. Velocidade e celular: ver o registro.
  Sem precisão abaixo do ponto nos vértices (PGXP): o PS1 manda vértices inteiros, então as bordas ficam nítidas
  mas a geometria treme como no console.
- Novidades da próxima versão (para entrar em `src/app/novidades.h` quando a versão for decidida):
  "PlayStation: resolução 2x e 4x (Ajustes ou menu do jogo)".
- ✅ Varredura noturna de compatibilidade (01/11/2026, branch `ps1-noite`): NASCAR 2001 e Ace Combat 3 do João
  jogados com botões por roteiro (`PS1KEYS`/`PS1SHOTS` no `indigo-t9_ps1int disco`, `REF_KEYS` no `t9_ps1_ref.c`)
  e comparados com o Beetle PSX com os mesmos botões: NASCAR até a corrida (cartão de memória: criar o save
  funciona) e Ace Combat 3 do menu de idiomas até a missão (pausa, mísseis, voo pelo analógico, replay), sem
  travas, cores erradas, gráficos faltando nem som mudo. As cópias não têm vídeos nem XA (o MDEC e o XA seguem só
  nos testes sintéticos). O som sai 4/3 mais alto que o do Beetle, igual na SPU e no CD-DA; o nosso bate com a
  conta do psx-spx (seno de 8000 no CD com volume 7FFFh sai com 5657 de RMS), então fica como está.
- ✅ ps1-tests completos contra o console na mesma noite: `cpu/access-time` (50 de 50 tempos a até 2 ciclos: os
  registradores com 3 ciclos, CD e SPU só com o tempo deles, COM0 só nos acessos seguidos, expansão 3),
  `cpu/code-in-io` (7 de 7: busca de instruções na SPU e no DMA), `cpu/io-access-bitwidth` (30 linhas: JOY_MODE de
  32 bits, SIO_MODE, escrita larga no CD-ROM) e `timers` (volta a 0 no alvo como no console, leitura esperando o
  buffer de escrita: as 10000 leituras por valor do contador iguais às do console). Ficam diferentes: o lixo do
  barramento nas leituras sem registrador (expansão 3, bits altos do I_MASK e do alvo do timer), o MDECSTAT antes
  do primeiro reset, os modos de sincronia do timer 0 com o hblank (o log do console foi tirado num aparelho com
  cristal PAL, as linhas não batem), a sobra de 5 ciclos no `timers` "1000 cycles delay" e um caso do
  `dma/chopping` (2 palavras por 1 ciclo da CPU a 10,3%; a média dos 130 melhorou de 1,16% para 1,10%).
  Revalidação: `validate.sh` com 1031 comparações iguais e 0 problemas; robustez com sanitizadores (12 3 20261106):
  312 ROMs, 0 problemas.

## Etapa 9b · Panasonic 3DO (versão 2.2)

Último console do Índigo (aprovado pelo João em 01/11/2026: mesma geração do PS1). Núcleo escrito do zero
(`src/tdo`), sem ler nem traduzir código de outro emulador, a partir da documentação pública do hardware do 3DO
(formato do CCB e das cels, PIXC, VDL, mapa de memória e registradores descritos para os programadores do
Portfolio) e do manual de arquitetura ARM (ARMv3). Detalhes em `relatorios/2026-11-01-3do.md`.

| Check | O que entrega | Como verificar | Situação |
|---|---|---|---|
| 9b.1 | CPU ARM60 (ARMv3, 32 bits, big-endian, sem Thumb) | SingleStepTests ARM7TDMI (MIT) nas instruções que o ARMv3 tem | ✅ 596.960 casos iguais, 0 diferentes |
| 9b.2 | MADAM: motor de cels (CCB, cels compactadas e não, PLUT, 1 a 16 bits, cantos/perspectiva, PIXC, recorte), motor de matrizes, VDL (CLUT, fundo) → 320×240 | `indigo-t9b_3do`: 13 casos de cels contra imagens esperadas, matrizes, VDL | ✅ (régua própria; sem comparação com o console) |
| 9b.3 | CLIO: timers (encadeados, SLACK), interrupções pela FIQ, VINT, DMA para o DSP, DMA do XBUS, leitura do controle (PBUS) | testes dirigidos | ✅ |
| 9b.4 | DSP (DSPP) rodando microcódigo, som a 44,1 kHz | microcódigo de teste próprio (ULA, multiplicador, desvios, FIFO de DMA, DAC) | ✅ com o microcódigo próprio; ⏳ a codificação das instruções precisa ser conferida com o microcódigo do Portfolio (BIOS e disco do João) |
| 9b.5 | Unidade de CD pelo XBUS (comandos, estado, leitura por porta e por DMA), sistema de arquivos Opera, `.cue/.bin`, `.chd`, `.iso` | disco sintético; `detectSystem` = "3do" | ✅ |
| 9b.6 | NVRAM de 32 KB como save; save state | salvar → carregar → salvar igual; estados danificados recusados; robustez | ✅ |
| 9b.7 | BIOS de teste e homebrew próprios (assembly ARM, GPL-3.0) num disco Opera montado aqui | a BIOS de teste dá a partida pelo CD e roda o homebrew (cels, DSP, controle) | ✅ |
| 9b.8 | App: aba 3DO na estante, BIOS instalada ao abrir o arquivo (`3do_bios.bin`), aviso sem BIOS, controle (A/B/C, L/R, P, X) e toque | Wine (aba, homebrew rodando, toque no modo celular) | ✅ PC; ⏳ aguardando teste do João no Edge 70 |
| 9b.9 | BIOS do João (ex.: `panafz10.bin`) até o menu e jogos comerciais | BIOS e jogos do João | ⏳ aguarda os arquivos; o mapa de registradores provavelmente vai precisar de ajustes quando a BIOS verdadeira rodar |
| 9b.10 | Quadros comparados com outro emulador de 3DO (Opera/MAME só como caixa-preta, compilados fora) | quadro a quadro | ⏳ precisa da BIOS (os emuladores de 3DO não rodam sem ela) |
| 9b.11 | Modos interpolados do vídeo (alta resolução) | — | ❌ ainda não feito (sai 320×240) |

**Situação (01/11/2026):** núcleo inteiro funcionando com a BIOS de teste do Índigo e o homebrew próprio
(~600 quadros/s no PC de testes); `indigo-t9b_3do` 88 verificações, 0 falhas; revalidação e robustez no registro
abaixo. O 3DO comercial depende da BIOS e dos jogos do João (9b.9).

# Fase 3 · Manutenção e melhorias (até o PS1)

**Decisão do João (31/10/2026):** o Índigo vai até o PlayStation 1 e não ganha consoles novos. Daqui em diante o
trabalho é só manutenção e melhoria dos emuladores que já existem. A meta é ser o melhor emulador open source da
internet do Atari ao PS1, sem problema de direitos autorais. As etapas 10 a 17 (N64, DS, PSP, PS2, Saturn,
Dreamcast, GameCube/Wii e 3DS) foram canceladas.

A fase não tem fim. Ela anda em ciclos: cada versão (2.1, 2.2, …) leva itens das frentes abaixo, sempre com a
revalidação e a robustez em 0 problemas. A ordem das frentes é a ordem de prioridade; o João pode mudá-la.

### Frente A · Código aberto e direitos autorais (primeiro)

| Check | O que entrega | Como verificar |
|---|---|---|
| A.1 | Varredura do repositório inteiro (feita em 31/10, relatório em `relatorios/2026-10-31-auditoria.md`): nenhuma BIOS nem ROM comercial em nenhuma versão; achou o que está nas linhas abaixo | ✅ |
| A.2 | **Sega CD reescrito a partir da documentação** (`segacd/gfx.cpp` e `cdhw.cpp` seguem o Genesis Plus GX, cuja licença proíbe uso comercial e não combina com a GPL-3.0); rever também `segacd.cpp` e `pcm.cpp`, e a tabela de horários do VDP do Mega Drive com outra fonte | Mesmos testes de hoje iguais ao Genesis Plus GX (só como régua, compilado fora) e revisão lado a lado sem trechos parecidos |
| A.3 | ROMs de teste sem licença clara trocadas por homebrew próprio ou de licença conhecida (4 de Neo Geo Pocket, incluindo *Blueprint* e *Tutankham*; conferir devkitSMS e Winter Shooter) | Cada ROM em `tests/roms` com a licença anotada |
| A.4 | Licença GPL-3.0 (`LICENSE`) e aviso de terceiros (C-BIOS, FreeBIOS, tabelas do MAME, QSound HLE, `sets.inc`, stb_truetype, Poppins, SDL3, Gradle) no repositório, no "Sobre" do app e no LEIA-ME | Toda licença de terceiros é compatível com a GPL-3.0 |
| A.5 | Chave de assinatura do APK fora do código (vira segredo do GitHub Actions, sem senha escrita no `build.gradle`) | APK novo com a mesma assinatura de hoje |
| A.6 | **Dois repositórios andando juntos** (decisão do João, 31/10): este continua privado e é onde o trabalho acontece; o público novo recebe, a cada versão, uma cópia limpa feita por `native/tools/publicar.sh` (sem o histórico, as branches de trabalho, `downloads/`, a chave, os relatórios e os arquivos internos das sessões; autor `270025200+joaoabitante@users.noreply.github.com`). O primeiro envio só depois da A.2 a A.5 | Clone anônimo do público compila e passa no `validate.sh`; cada versão nova aparece nos dois no mesmo dia |
| A.7 | BIOS livre própria (escrita do zero) para os consoles que hoje exigem a do usuário, na ordem: PS1, PC Engine CD (System Card), Neo Geo, Neo Geo CD, Sega CD, 32X | Jogos e homebrew dão a partida sem nenhum arquivo da Sony, NEC, SNK ou Sega; a BIOS original continua aceita e é usada quando existir |
| A.8 | Página no site: o Índigo não vem com jogos nem BIOS; como fazer a cópia dos seus próprios | Página no ar |

### Frente B · Precisão

| Check | O que entrega | Como verificar |
|---|---|---|
| B.1 | Placar de precisão: por console, quantos testes públicos passam do total (Blargg, mooneye, PeterLemon, ps1-tests, SingleStepTests, …) | Tabela gerada pelo `validate.sh`, publicada no site |
| B.2 | Fechar as diferenças conhecidas do PS1: CD (busca além do fim do disco, tempo do Init), chopping do DMA, status passo a passo e IDCT do MDEC | Os 4 testes do ps1-tests iguais ao console (01/11: também access-time, code-in-io, io-access-bitwidth e timers; ver a etapa 9) |
| B.3 | Subir o placar dos outros consoles, um por ciclo, começando pelo de pior resultado | Placar maior a cada versão, nunca menor |

### Frente C · Compatibilidade

| Check | O que entrega | Como verificar |
|---|---|---|
| C.1 | Lista de jogos testados por console (situação: perfeito, jogável, com defeito, não abre), alimentada pelos testes do João | Lista no repositório e no site (PS1, 01/11: NASCAR 2001 e Ace Combat 3 jogáveis até a corrida e a missão) |
| C.2 | Cada defeito vira um caso de teste com homebrew ou ROM de teste (nunca o jogo comercial no repositório) | O caso entra no `validate.sh` |
| C.3 | Banco de dados de jogos (só somas de verificação e nomes, sem as ROMs) para reconhecer região, mapper e save certos sozinho | Jogos com cabeçalho errado abrem sem ajuste manual |

### Frente D · Velocidade

| Check | O que entrega | Como verificar |
|---|---|---|
| D.1 | Medição de cada núcleo no Edge 70 e no notebook (quadros/s e bateria) | Tabela no CRONOGRAMA |
| D.2 | Otimizar os que não passam de 2× no celular, a começar pelo PS1 (recompilador da CPU para ARM64 e x86-64, com o interpretador como régua) | Todos os consoles acima de 2× no Edge 70 |

### Frente E · Recursos

| Check | O que entrega | Como verificar |
|---|---|---|
| E.1 | PS1 na GPU com resolução aumentada (o antigo 9.8) | ✅ Feito em 01/11 (2.0.1) |
| E.2 | Voltar no tempo (rebobinar) e câmera lenta | Rebobinar 30 s em todos os consoles sem travar |
| E.3 | Filtros de imagem (CRT, LCD dos portáteis, suavização) | Capturas de cada filtro |
| E.4 | Códigos de trapaça (Game Genie, Action Replay, GameShark) | Códigos conhecidos de homebrew funcionando |
| E.5 | Conquistas (RetroAchievements) — precisa da decisão do João, porque usa a conta dele no serviço | — |
| E.6 | Jogo em rede (netplay) entre dois Índigos | Partida de 10 min sem dessincronizar |

### Frente F · Manutenção contínua (todo ciclo)

- Defeitos relatados pelo João entram primeiro, antes de qualquer frente.
- SDL3, Android (versão alvo da Play) e compiladores em dia.
- Revalidação, robustez e o APK automático a cada versão, como hoje.

### Etapas 10 a 17 (canceladas em 31/10/2026)

N64, DS, PSP, PS2, Saturn, Dreamcast, GameCube/Wii e 3DS saíram do plano: o Índigo vai até o PlayStation 1.

---

## Registro de checks

| Data | Etapa | Check | Observação |
|---|---|---|---|
| 19/10/2026 | 0 | Base + Game Boy / Game Boy Color | 116 comparações idênticas ao JS; `.exe` 0.1 enviado |
| 19/10/2026 | — | Cronograma passa para o ritmo diário | Etapas 1–6 encurtadas; PS2 em marcos mensais |
| 19/10/2026 | 1 | Master System / Game Gear | 1 dia antes do previsto; ZEXALL 100%; 27 comparações idênticas ao JS; `.exe` 0.2 enviado |
| 20/10/2026 | 2 | Neo Geo Pocket / Pocket Color | 1 dia antes do previsto; 2 milhões de instruções e 16 comparações idênticas ao JS; `.exe` 0.3 enviado |
| 20/10/2026 | — | Revisão geral (etapas 0–2) | Build do zero, revalidação completa, sanitizadores e robustez; corrigidos o save compartilhado entre ROMs de mesmo nome e a leitura de estados danificados; `.exe` 0.3.1 |
| 21/10/2026 | 3 | Game Boy Advance | 2 dias antes do previsto; 666 comparações de ROMs idênticas ao JS + testes diferenciais; `.exe` 0.4 enviado |
| 21/10/2026 | 4b | Android: primeiro APK | Gerado pelo GitHub Actions; interface testada simulando o celular no PC; teste no Edge 70 pendente |
| 21/10/2026 | 4 | Primeira parte da interface | Continuar de onde parou, .zip, estados com miniatura, versões do save; `.exe` 0.5 e APK de teste 2 |
| 21/10/2026 | 4b | Android | Testado por você no Edge 70 com o GameSir X2: funcionou |
| 21/10/2026 | 4 | Backup `.indigo` e nuvem dos saves | Mesmo formato e mesma nuvem do site (PIN); testados contra o código do site; `.exe` 0.6 e APK 0.6 |
| 21/10/2026 | — | Fase 2 entra no cronograma | Tudo o que roda num Anbernic, do Atari ao 3DS; PS1 passa para fev/2027 e PS2 para dez/2027 |
| 21/10/2026 | 4 | Estante, filtros, controles e atalhos | Estante com capas, favoritos, filtro por console e pastas; filtros LCD/TV/EPX; remapeamento e atalhos do controle; testado no Wine |
| 21/10/2026 | 5 | Índigo 0.9 | Ícone e versão no `.exe`, "Sobre e novidades", instalador + zip, abrir jogos com dois cliques; `.exe` 0.9, instalador e APK 0.9 |
| 21/10/2026 | 5 | Índigo 1.0 | 0.9 testada e aprovada por você; versão 1.0 para PC (instalador + zip) e Android; junção na `main` aprovada, site publicado de novo sem mudanças |
| 21/10/2026 | 6.1 | NES (primeira versão) | 10 dias antes do previsto; CPU igual a 2,44 milhões de casos de referência; 71 ROMs de teste passando pelo protocolo e 53 de tela; `.exe` e APK 1.1 |
| 21/10/2026 | — | Interface nova (pedido do João) | Biblioteca com capas e abas por console, letra lisa (Poppins), menus que sobem de baixo no celular, botões de toque redesenhados, toque vale ao soltar (corrige o toque duplo) |
| 22/10/2026 | 6.1 | Mapeadores com chips próprios (NES) | MMC5, VRC6, VRC7, Namco 163 e som do Sunsoft 5B; 134 verificações de bancos, IRQ e som; versão 1.2 |
| 22/10/2026 | 6.3 | SNES (primeira versão) | Adiantado a pedido do João; CPU e SPC700 iguais a 5,3 milhões de casos de referência; 30 de 30 ROMs de teste; versão 1.2 |
| 22/10/2026 | 6.3 | Super FX (SNES) | Sessão da madrugada; 31 de 31 ROMs de teste do chip e 24 de 27 telas iguais à referência; versão 1.3 |
| 22/10/2026 | 6.3 | DSP-1 e SA-1 (SNES) | DSP-1 com o arquivo do chip ou imitado (126 verificações); SA-1 (41 verificações); versão 1.4 |
| 24/10/2026 | — | Portáteis e frontends (pedido do João) | Tudo pelo controle (analógico nos menus), tela cheia nos PCs de mão, jogos abertos por ES-DE/Daijisho/Playnite/Steam (`PORTATEIS.md`); versão 1.5 |
| 24/10/2026 | 7 | Etapa 7 inteira (10 consoles) | Pokémon Mini, Atari 2600 e 7800, Lynx, WonderSwan, Virtual Boy, MSX, ColecoVision, SG-1000 e Vectrex; revalidação 1012 iguais, 0 problemas; versão 1.6 |
| 25/10/2026 | 6.2 e 6.4 | Mega Drive e PC Engine / SuperGrafx | 68000 igual a 1 milhão de casos e 22 de 28 ROMs do SGDK iguais ao Genesis Plus GX; HuC6280 igual a 640 mil casos e 30 casos iguais ao Mesen 2; revalidação 1015 iguais, 0 problemas; versão 1.7 |
| 30/10/2026 | 6.3 | S-DD1 (SNES) | Sessão da madrugada; feito em 24/10 na branch `noite-2026-10-24` e trazido para cima da 1.7; descompressão igual à do bsnes em 64 fluxos fixos + 6 mil aleatórios; 91 verificações; versão 1.7.1 |
| 30/10/2026 | 8.1 e 8.2 | Neo Geo MVS/AES e Capcom CPS1/CPS2 | Neo Geo: 25 programas do ngdevkit iguais ao Geolith (MVS e AES); CPS: decifração do CPS2 igual ao MAME, 408 jogos na tabela (sem comparação de imagem: não há ROM livre); revalidação 1018 iguais, 0 problemas; versão 1.8 publicada (APK e zip do PC no site) |
| 30/10/2026 | 8.1, 8.3, 8.4, 8.5 | Leitor de CD (.cue/.bin, .iso, .chd), Sega CD, 32X, PC Engine CD, Neo Geo CD | CD: 24 verificações com imagens sintéticas; .chd: 104 arquivos do chdman (todos os codecs) iguais ao .cue setor a setor, decodificadores próprios. 32X: 200 mil casos do SH-2 iguais ao PicoDrive/MAME, 8 ROMs com todos os quadros iguais ao PicoDrive. Sega CD: BIOS de teste própria com 42/42 quadros iguais ao Genesis Plus GX; ASIC 300/300 e PCM 200/200 iguais. PC Engine CD: System Cards de teste e scsitest/okitest iguais ao Mesen 2 (14 mil quadros). Neo Geo CD: 954/955 quadros iguais ao NeoCD em 17 CDs. Neo Geo CD também com a BIOS do João: abertura e menu iguais ao NeoCD. Interface nova (DPI, estante responsiva, celular). Revalidação 1024 iguais, 0 problemas; robustez 0 problemas; versão 1.9 publicada |
| 31/10/2026 | 6.3 | SPC7110 (SNES) e o relógio RTC-4513 | Sessão da madrugada; 48 roteiros do chip e 32 do relógio iguais ao bsnes no teste (fora dele 3 mil e 20 mil); 120 verificações; revalidação 1025 iguais, 0 problemas; robustez 0 problemas; versão 1.9.1 (relógio seguindo a hora do aparelho, decisão do João) |
| 31/10/2026 | 9 | PlayStation (versão 2.0) | Quatro partes juntas no console inteiro; ps1-tests pela CPU (GTE 1150/1150, SPU, CD com disco sintético e tampa, MDEC, DMA) e disco de teste pela BIOS do João (3D, som, CD-DA, leitura do CD, controle, save state) igual ao Beetle PSX: `t9_ps1int` 34 verificações, 0 falhas; app no Wine com a BIOS instalada ao abrir; revalidação 1030 iguais, 0 problemas (commit 480522c); robustez 0 problemas; jogos comerciais aguardam o João |
| 01/11/2026 | 9.8 | PlayStation: resolução interna 1×/2×/4× | Na GPU por software (VRAM ampliada ao lado da de verdade), PC e Android; 1× idêntico ao de antes (t9_ps1 140 verificações e t9_ps1int 48, 0 falhas; as 118 e 34 antigas com a mesma saída); 2×/4×: VRAM de verdade e estado salvo iguais aos de 1×, canto dos blocos ≥ 99,6% igual a 1×, bordas com detalhe em ~80% dos pontos de borda, troca no meio do jogo; disco de teste a 366 / 274 / 148 quadros/s em 1× / 2× / 4× no PC de testes (no celular, estimado ~1/3: 2× cabe, 4× fica abaixo de 60); revalidação 1030 iguais, 0 problemas; robustez com sanitizadores 0 problemas; branch `etapa9-res` |
| 01/11/2026 | 9b | Panasonic 3DO (branch `etapa9b-3do`, para a 2.2) | Núcleo do zero (ARM60, MADAM, CLIO, DSP, CD Opera, NVRAM); CPU igual a 596.960 casos dos SingleStepTests ARM7TDMI; `indigo-t9b_3do` 88 verificações, 0 falhas; BIOS de teste e homebrew próprios; revalidação 1031 iguais, 0 problemas; robustez com sanitizadores (12 3 20261101) 312 ROMs, 0 problemas; app no Wine (aba 3DO, BIOS ao abrir, homebrew); BIOS e jogos do João pendentes |
