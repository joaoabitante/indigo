# Índigo Nexus

**O ponto de encontro de 50 anos de videogames.** Emulador grátis e de código aberto para **Windows**,
**Linux** e **Android** (o mesmo código em C++20 + SDL3), do Atari 2600 ao PlayStation, com um emulador que também roda no
navegador. Site e downloads: **https://indigo.nexus**

Versão 2.3.0. Um projeto do [Autista Empreende](https://autistaempreende.com.br), criado pelo João Abitante com a
ajuda de inteligência artificial (Claude).

## Consoles

Game Boy, Game Boy Color, Game Boy Advance, NES, SNES (com Super FX, SA-1, DSP-1, S-DD1 e SPC7110), Master System,
Game Gear, SG-1000, Mega Drive, 32X, Sega CD, PC Engine e PC Engine CD, Neo Geo (MVS/AES) e Neo Geo CD, Capcom
CPS1/CPS2, Atari 2600/7800, Lynx, WonderSwan/Color, Virtual Boy, Neo Geo Pocket/Color, Pokémon Mini, MSX,
ColecoVision, Vectrex, PlayStation e Panasonic 3DO (experimental).

## Jogos e BIOS

O Índigo **não traz jogos nem BIOS** de nenhum fabricante. Use cópias dos seus próprios cartuchos e discos. Os
consoles que precisam de BIOS (PlayStation, Sega CD, Neo Geo, Neo Geo CD, PC Engine CD, ColecoVision e 3DO) pedem o
arquivo do seu console na primeira vez.

## Compilar

- App (Linux, testes e build do Windows com MinGW): `native/tools/setup-env.sh ~/indigo-env` instala o que
  precisa, baixa as ROMs de teste públicas e compila. Detalhes em `native/README.md`.
- Conferência: `native/tools/validate.sh <build> ~/indigo-env/roms` compara milhares de quadros e sons contra as
  referências; cada versão só sai com 0 problemas.
- Android: `native/android` (Gradle). Assine com a sua própria chave.
- Site e emulador do navegador: `npm install` e `node tools/build.js` (gera `dist/`).

## Licença

GNU General Public License versão 3 (`LICENSE`). Código e dados de terceiros, todos compatíveis, estão em
`native/TERCEIROS.md`. Os nomes dos consoles servem só para indicar compatibilidade e são marcas dos seus donos;
o Índigo não tem ligação com nenhum fabricante.
