# Índigo em portáteis e frontends

O Índigo roda em PCs de mão com Windows (Lenovo Legion Go, ASUS ROG Ally, MSI Claw, Steam Deck com Windows)
e em portáteis Android que instalam APK (Anbernic, Retroid Pocket, AYN Odin, Ayaneo, celular com controle).
Os controles embutidos desses aparelhos funcionam sem configurar nada: são reconhecidos como controle de
Xbox/Android pelo SDL3. Para mudar botões: Ajustes > Controles.

## No portátil

- Tudo funciona só com o controle: direcional ou analógico esquerdo para navegar, A para escolher, B para
  voltar, L e R trocam a aba de console na biblioteca, Start abre os Ajustes.
- Menu durante o jogo: botão Guide/Xbox/Home, ou Start + Select juntos (útil quando o sistema usa o botão
  Home, como a Barra de Jogo do Windows no Legion Go e no ROG Ally).
- Atalhos com Select segurado: Select + R salva o estado, Select + L carrega, Select + direita/esquerda troca
  o espaço de estado.
- Windows: em aparelhos com tela de toque (os portáteis) o Índigo já abre em tela cheia. Para mudar:
  Ajustes > "Tela cheia ao abrir". F11 ou Alt+Enter também alternam.
- A seta do mouse some quando o controle é usado e volta quando o mouse ou o touchpad mexe.
- Android: com controle (embutido ou ligado) os botões da tela somem sozinhos; voltam ao tocar na tela.

## Frontends (ES-DE, Daijisho, Playnite, LaunchBox, Steam)

Quando um frontend abre um jogo no Índigo, ele vai direto para o jogo em tela cheia, e "Fechar o jogo e
voltar" fecha o Índigo e devolve o portátil ao frontend.

### Windows (linha de comando)

    "C:\Users\<você>\AppData\Local\Programs\Indigo\indigo.exe" --frontend "<caminho do jogo>"

Opções: `--frontend` (tela cheia e sair ao fechar o jogo) e `--tela-cheia` (ou `--fullscreen`, `-f`: só a tela
cheia). O caminho do instalador muda se você escolheu outra pasta; na versão portátil (zip) use o
`indigo.exe` da pasta onde extraiu.

- **Playnite / LaunchBox**: adicione um emulador personalizado com o executável acima e os argumentos
  `--frontend "{ImagePath}"` (Playnite) ou `--frontend` (LaunchBox, que acrescenta o caminho do jogo no fim).
- **ES-DE para Windows**: no `es_find_rules.xml` personalizado, uma regra `INDIGO` apontando para o
  `indigo.exe`; no `es_systems.xml` o comando `%EMULATOR_INDIGO% --frontend %ROM%`.
- **Steam (Steam Deck, Legion Go com Steam)**: "Adicionar um jogo não Steam" com o `indigo.exe`; em Opções de
  inicialização: `--frontend "<caminho do jogo>"`. No Steam Deck com SteamOS o `indigo.exe` roda pelo Proton
  (Propriedades > Compatibilidade > forçar Proton).

### Android (Anbernic, Retroid, AYN, Ayaneo)

Pacote `net.abitante.indigo`, atividade `net.abitante.indigo.IndigoActivity`. O Índigo aceita o jogo:
- no endereço da intent (`-d`, `content://` ou `file://`), com a ação `android.intent.action.VIEW`;
- ou num extra de texto `ROM` (também `rom`, `path`, `file`, `filePath`, `LaunchFile`) com o caminho.

Prefira o endereço `content://` (o frontend dá a permissão de leitura junto). Caminhos de arquivo (`file://`
ou o extra `ROM`) funcionam no Android 12 ou anterior; no 13 ou mais novo o Android só deixa ler pelo
endereço `content://`.

- **ES-DE**: no `es_find_rules.xml` personalizado:

      <emulator name="INDIGO">
        <rule type="androidpackage">
          <entry>net.abitante.indigo/net.abitante.indigo.IndigoActivity</entry>
        </rule>
      </emulator>

  e no `es_systems.xml` personalizado, para cada sistema:

      <command label="Índigo">%EMULATOR_INDIGO% %ACTION%=android.intent.action.VIEW %DATA%=%ROMSAF%</command>

- **Daijisho / Beacon / Pegasus**: um "player" com a linha de `am start`:

      -n net.abitante.indigo/.IndigoActivity -a android.intent.action.VIEW -d {file.uri}

  (no Daijisho a variável é `{file.uri}`; em outros frontends, a que entrega o endereço `content://` do jogo).
- O Índigo também aparece no "Abrir com" do gerenciador de arquivos e nos lançadores de TV/portáteis
  (Android TV e lançadores como os da Anbernic e da Retroid).

## CDs (Sega CD, PC Engine CD, Neo Geo CD, PlayStation, 3DO)

- No PC, abra o `.cue` (com os `.bin` na mesma pasta), o `.chd` ou o `.iso`.
- No celular, use o **`.chd`** (um arquivo só): o Android só deixa o Índigo ler o arquivo escolhido, e o `.cue`
  precisa dos `.bin` ao lado. Para converter no computador: `chdman createcd -i jogo.cue -o jogo.chd` (o chdman
  vem com o MAME). O CD é copiado para o Índigo na primeira vez que é aberto.
- Cada console de CD precisa da BIOS do aparelho, aberta uma vez no Índigo como se fosse um jogo: Sega CD (BIOS do
  Mega-CD/Sega CD, 128 KB), PC Engine CD (System Card, ex.: `syscard3.pce`), Neo Geo CD (`neocd.bin`, 512 KB),
  PlayStation (BIOS do console, ex.: `SCPH1001.BIN`, 512 KB), 3DO (BIOS do console, 1 MB, ex.: `panafz10.bin`,
  `panafz1.bin` ou `goldstar.bin`).
- PlayStation no celular: o jogo em **`.chd`** (os jogos de várias faixas viram um arquivo só, com as músicas de
  CD-DA dentro). Jogos de vários discos: um `.chd` por disco. O save é o cartão de memória (128 KB, o mesmo
  formato `.mcr` dos outros emuladores; um `.mcr` com o nome do jogo ao lado do CD entra na primeira vez). O
  controle do PS1 usa os gatilhos como L2/R2, os analógicos (L3 + R3 juntos ligam o modo analógico do DualShock)
  e vibra quando o jogo pede. Na tela de toque aparecem Triângulo, Círculo, Xis, Quadrado, L1/R1 e L2/R2.
- 3DO: o jogo em `.chd` no celular (ou `.cue/.bin`/`.iso` no PC). O save é a NVRAM do console (32 KB). O controle do
  3DO tem A, B e C em fileira: no controle do portátil são o botão da esquerda (A), o de baixo (B) e o da direita
  (C); L e R são os gatilhos de cima; Start é o P (play/pausa) e Select é o X (stop). Na tela de toque aparecem A,
  B, C, L, R, P e X.
