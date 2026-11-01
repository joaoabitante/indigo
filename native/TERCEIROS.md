# Código e dados de terceiros no Índigo

O Índigo é software livre sob a **GNU General Public License versão 3** (arquivo `LICENSE` na raiz do
repositório). Copyright © 2026 João Carlos Bueno Abitante (projeto do Autista Empreende).

O Índigo **não traz jogos nem BIOS de nenhum fabricante**. Os consoles que precisam de BIOS (PlayStation, Sega CD,
Neo Geo, Neo Geo CD, PC Engine CD, ColecoVision e 3DO) usam o arquivo do console do próprio usuário.

Partes de terceiros incluídas, todas com licença compatível com a GPL-3.0:

| O quê | Onde | Licença |
|---|---|---|
| C-BIOS 0.29a (BIOS livre do MSX) | `native/src/msx/cbios.*` | tipo BSD (aviso completo mantido no arquivo) |
| FreeBIOS 1.3 do Pokémon Mini (Team Pokeme) | `native/src/pm/freebios.cpp` | domínio público |
| Cifras e tabelas do Neo Geo (CMC, PCM2 etc.) | `native/src/neogeo/cart.h`, `cmc_tables.inc` | BSD-3-Clause (MAME) |
| Lista de jogos do Neo Geo | `native/src/neogeo/sets.inc` | CC0-1.0 (lista de software do MAME) |
| Cifra do CPS2 e tabelas do CPS1/CPS2 | `native/src/cps/cps2crypt.cpp`, `tabela.inc` | BSD-3-Clause (MAME) |
| QSound HLE (superctr e Valley Bell) | `native/src/cps/qsound.*` | BSD-3-Clause |
| stb_truetype (Sean Barrett) | `native/src/third_party/stb_truetype.h` | domínio público / MIT |
| Fonte Poppins | `native/res/fonts/` | SIL Open Font License 1.1 (`OFL-Poppins.txt`) |
| SDL3 (biblioteca usada para compilar) | baixada pelo `setup-env.sh` | zlib |
| Gradle wrapper | `native/android/gradle/` | Apache-2.0 |

Outros emuladores (Mednafen/Beetle PSX, Genesis Plus GX, PicoDrive, Mesen 2, bsnes, Geolith, NeoCD, MAME e
outros) servem apenas de **régua nos testes**: são compilados fora do repositório e nenhum código deles faz parte
do Índigo. Testes públicos (Blargg, mooneye, SingleStepTests, ps1-tests, PeterLemon e outros) são baixados pelo
`native/tools/setup-env.sh` e também não ficam no repositório.

Os nomes dos consoles aparecem só para indicar compatibilidade e são marcas dos seus donos. O Índigo não tem
ligação com nenhum fabricante.
