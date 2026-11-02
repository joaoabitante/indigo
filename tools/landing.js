// Página inicial do Índigo (landing): tudo o que fica em volta do emulador do navegador.
// tools/build.js monta a página juntando estes blocos com web/template.html (o emulador) e web/landing.css.
// Só nomes dos consoles em texto: nada de logotipos ou arte das marcas. Ícones e ilustrações são originais.

export const esc = (t) => String(t).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

// ---------------------------------------------------------------- ícones (traço, 24×24)
const svg = (body, cls = 'ico') => `<svg class="${cls}" viewBox="0 0 24 24" aria-hidden="true" focusable="false">${body}</svg>`;
export const ICON = {
  download: svg('<path d="M12 4v12M6.5 11 12 16.5 17.5 11M5 20h14"/>'),
  play: svg('<path d="M8 5.5v13l10.5-6.5z"/>'),
  pc: svg('<rect x="2.5" y="3.5" width="19" height="13" rx="2"/><path d="M8 20.5h8M12 16.5v4"/>'),
  phone: svg('<rect x="6" y="2.5" width="12" height="19" rx="2.5"/><path d="M10.5 18.5h3"/>'),
  joy: svg('<rect x="2.5" y="7.5" width="19" height="10" rx="5"/><path d="M7.5 10.5v4M5.5 12.5h4"/><circle cx="15.5" cy="11.5" r=".9"/><circle cx="18" cy="13.6" r=".9"/>'),
  cart: svg('<path d="M5.5 3.5h13v12.5l-2 4.5h-9l-2-4.5z"/><path d="M8.5 7h7M8.5 10h7"/>'),
  arcade: svg('<circle cx="12" cy="6.5" r="2.8"/><path d="M12 9.3v5.2"/><rect x="4" y="14.5" width="16" height="6" rx="1.6"/><path d="M8 17.5h.01M16 17.5h.01"/>'),
  handheld: svg('<rect x="6" y="2.5" width="12" height="19" rx="2.2"/><rect x="8.5" y="5" width="7" height="6" rx="1"/><path d="M9.5 14.5v3M8 16h3"/><circle cx="15" cy="15.2" r=".8"/><circle cx="13.6" cy="17" r=".8"/>'),
  disc: svg('<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="2.4"/><path d="M12 5.5a6.5 6.5 0 0 1 6.5 6.5"/>'),
  target: svg('<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="4.5"/><circle cx="12" cy="12" r=".9"/>'),
  offline: svg('<path d="M3 3l18 18"/><path d="M8.6 16.4a4.8 4.8 0 0 1 6.2-.4"/><path d="M5.2 13a9.6 9.6 0 0 1 3.6-2.2M18.8 13a9.6 9.6 0 0 0-2.6-1.8"/><path d="M2 9.6a14.6 14.6 0 0 1 4.3-2.8M22 9.6A14.6 14.6 0 0 0 11 6.1"/><circle cx="12" cy="19.6" r=".9"/>'),
  devices: svg('<rect x="2" y="4" width="14" height="10" rx="1.6"/><path d="M6 18h6M9 14v4"/><rect x="15.5" y="9" width="6.5" height="11.5" rx="1.6"/>'),
  cloud: svg('<path d="M7 18.5h10.3a4 4 0 0 0 .5-8 6 6 0 0 0-11.6 1.7A3.4 3.4 0 0 0 7 18.5z"/><path d="M12 11v5M9.8 13.3 12 11l2.2 2.3"/>'),
  pad: svg('<path d="M7 7.5h10a4.5 4.5 0 0 1 4.4 5.4l-.9 4.4a2.4 2.4 0 0 1-4.1 1.1L14 16h-4l-2.4 2.4a2.4 2.4 0 0 1-4.1-1.1l-.9-4.4A4.5 4.5 0 0 1 7 7.5z"/><path d="M8 10.5v3M6.5 12h3"/><circle cx="15.5" cy="11" r=".8"/><circle cx="17" cy="13" r=".8"/>'),
  rewind: svg('<path d="M3.5 12a8.5 8.5 0 1 0 2.6-6.1"/><path d="M3.5 4v4.2h4.2"/><path d="M12 7.5V12l3 2"/>'),
  code: svg('<path d="m8.5 7-5 5 5 5M15.5 7l5 5-5 5M13.6 4.5l-3.2 15"/>'),
  heart: svg('<path d="M12 20s-7.5-4.6-7.5-10.2A4.3 4.3 0 0 1 12 7.2a4.3 4.3 0 0 1 7.5 2.6C19.5 15.4 12 20 12 20z"/>'),
  ext: svg('<path d="M14 4.5h5.5V10M19.5 4.5 11 13M18 14v4.5a1 1 0 0 1-1 1H5.5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1H10"/>'),
  check: svg('<path d="m5 12.5 4.5 4.5L19 7.5"/>'),
  lock: svg('<rect x="5" y="10.5" width="14" height="10" rx="2"/><path d="M8.5 10.5V8a3.5 3.5 0 0 1 7 0v2.5"/>'),
};

// ---------------------------------------------------------------- consoles do app (por época e família)
// web: também roda no emulador do navegador · bios: o app pede o arquivo do console da pessoa · n: quantos sistemas a linha conta
export const CONSOLE_GROUPS = [
  { title: 'A era dos 8 bits', era: 'Anos 70 e 80', icon: 'joy', items: [
    { name: 'Atari 2600' }, { name: 'Atari 7800' }, { name: 'ColecoVision' }, { name: 'Vectrex', note: 'tela vetorial' },
    { name: 'SG-1000' }, { name: 'MSX' }, { name: 'NES' }, { name: 'Master System', web: 1 },
  ] },
  { title: '16 bits e CD', era: 'Anos 80 e 90', icon: 'cart', items: [
    { name: 'SNES', note: 'com os chips SuperFX, SA-1, DSP, S-DD1 e SPC7110' }, { name: 'Mega Drive' }, { name: '32X' },
    { name: 'Sega CD', bios: 1 }, { name: 'PC Engine' }, { name: 'PC Engine CD', bios: 1 }, { name: 'Neo Geo CD', bios: 1 },
  ] },
  { title: 'Fliperama', era: 'Arcade', icon: 'arcade', items: [
    { name: 'Neo Geo', note: 'MVS e AES', bios: 1 }, { name: 'Capcom CPS1' }, { name: 'Capcom CPS2' },
  ] },
  { title: 'Portáteis', era: 'Do bolso para a tela', icon: 'handheld', items: [
    { name: 'Game Boy', web: 1 }, { name: 'Game Boy Color', web: 1 }, { name: 'Game Boy Advance', web: 1 },
    { name: 'Game Gear', web: 1 }, { name: 'Lynx' }, { name: 'WonderSwan e WonderSwan Color', n: 2 },
    { name: 'Neo Geo Pocket e Pocket Color', n: 2, web: 1 }, { name: 'Virtual Boy' }, { name: 'Pokémon Mini' },
  ] },
  { title: 'O salto para o 3D', era: 'Desde a 2.0', icon: 'disc', fresh: 1, items: [
    { name: 'PlayStation', fresh: 1, bios: 1, note: '3D, CDs em .cue/.bin, .chd e .iso, memory card e DualShock com vibração' },
    { name: '3DO', bios: 1, note: 'experimental, desde a 2.2: ainda não testado com jogos de verdade' },
  ] },
];
export const SYSTEM_COUNT = CONSOLE_GROUPS.reduce((s, g) => s + g.items.reduce((t, i) => t + (i.n || 1), 0), 0);
// comparações automáticas feitas a cada versão (suítes de teste públicas e emuladores de referência)
const COMPARISONS = '1032';

// ---------------------------------------------------------------- "nexus": consoles ligados ao Índigo (atrás do desenho do topo)
const inner = (k) => ICON[k].replace(/^<svg[^>]*>/, '').replace(/<\/svg>$/, '');
function netArt() {
  const hub = [214, 188], phone = [440, 300], disc = [470, 78];
  const nodes = [['joy', 40, 34], ['cart', 200, 18], ['arcade', 186, 424], ['handheld', 64, 410], ['code', 300, 420], ['disc', 352, 26]];
  const lines = [];
  for (const [, x, y] of nodes) lines.push([x, y, ...hub]);
  lines.push([40, 34, 200, 18], [200, 18, 352, 26], [352, 26, ...disc], [64, 410, 186, 424], [186, 424, 300, 420], [300, 420, ...phone], [...hub, ...phone], [...hub, ...disc]);
  const d = lines.map(([a, b, c, e]) => `M${a} ${b}L${c} ${e}`).join('');
  return `<g class="net" aria-hidden="true">
    <path d="${d}" fill="none" stroke="#6763c9" stroke-width="1.4" stroke-opacity=".55"/>
    <path class="net-flow" d="${d}" fill="none" stroke="#f3b545" stroke-width="1.6" stroke-linecap="round" stroke-dasharray="2 22" stroke-opacity=".8"/>
    ${nodes.map(([k, x, y]) => `<g transform="translate(${x} ${y})"><circle r="19" fill="#1c1d2c" stroke="#6763c9" stroke-width="1.6"/><g transform="translate(-10 -10) scale(.84)" fill="none" stroke="#c3c1f0" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">${inner(k)}</g></g>`).join('')}
  </g>`;
}

// ---------------------------------------------------------------- ilustração do topo (original: um PC e um celular com a mesma cena)
const heroArt = () => `<svg class="hero-art" viewBox="0 0 560 440" role="img" aria-label="Ilustração: consoles clássicos ligados ao Índigo, aberto num computador e num celular com a mesma fase na tela">
  <defs>
    <linearGradient id="hSky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1b1950"/><stop offset=".62" stop-color="#5b56c4"/><stop offset="1" stop-color="#e8683f"/></linearGradient>
    <linearGradient id="hSun" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffd27a"/><stop offset="1" stop-color="#f3b545"/></linearGradient>
    <linearGradient id="hShell" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#6763c9"/><stop offset="1" stop-color="#322e80"/></linearGradient>
    <linearGradient id="hDisc" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#d8d6ff"/><stop offset=".35" stop-color="#8fe3dc"/><stop offset=".6" stop-color="#f3b545"/><stop offset="1" stop-color="#b9a4ff"/></linearGradient>
    <radialGradient id="hGlow"><stop offset="0" stop-color="#6763c9" stop-opacity=".45"/><stop offset="1" stop-color="#6763c9" stop-opacity="0"/></radialGradient>
    <symbol id="hScene" viewBox="0 0 160 100" preserveAspectRatio="xMidYMid slice">
      <rect width="160" height="100" fill="url(#hSky)"/>
      <g fill="#fff" opacity=".7"><rect x="14" y="10" width="2" height="2"/><rect x="40" y="20" width="1.5" height="1.5"/><rect x="70" y="8" width="2" height="2"/><rect x="96" y="18" width="1.5" height="1.5"/><rect x="146" y="12" width="2" height="2"/><rect x="28" y="32" width="1.5" height="1.5"/></g>
      <circle cx="112" cy="50" r="20" fill="url(#hSun)"/>
      <g fill="#7a5bb8" opacity=".9"><rect x="90" y="52" width="44" height="2.4"/><rect x="90" y="58" width="44" height="3"/><rect x="90" y="64" width="44" height="3.6"/></g>
      <polygon points="0,70 18,52 32,62 54,40 78,64 98,54 120,68 142,50 160,60 160,100 0,100" fill="#322e80"/>
      <polygon points="0,80 26,68 52,78 84,66 116,80 140,72 160,78 160,100 0,100" fill="#24205e"/>
      <rect y="84" width="160" height="16" fill="#14132a"/>
      <g stroke="#f3b545" stroke-width=".6" opacity=".55"><path d="M0 88h160M0 93h160M80 84 40 100M80 84 120 100M80 84 4 100M80 84 156 100M80 84v16"/></g>
      <g transform="translate(34 66)"><rect x="3" y="0" width="8" height="3" fill="#f3b545"/><rect x="2" y="3" width="10" height="5" fill="#ecebf6"/><rect x="4" y="4.5" width="2" height="2" fill="#11121c"/><rect x="0" y="8" width="14" height="6" fill="#6763c9"/><rect x="2" y="14" width="4" height="4" fill="#ecebf6"/><rect x="8" y="14" width="4" height="4" fill="#ecebf6"/></g>
      <g fill="#f3b545"><rect x="60" y="56" width="4" height="4"/><rect x="68" y="52" width="4" height="4"/><rect x="76" y="56" width="4" height="4"/></g>
    </symbol>
  </defs>
  <ellipse cx="280" cy="250" rx="270" ry="190" fill="url(#hGlow)"/>
  ${netArt()}
  <g class="ha-disc">
    <circle cx="470" cy="78" r="56" fill="url(#hDisc)" opacity=".95"/>
    <circle cx="470" cy="78" r="56" fill="none" stroke="#fff" stroke-opacity=".35"/>
    <circle cx="470" cy="78" r="16" fill="#11121c"/><circle cx="470" cy="78" r="22" fill="none" stroke="#fff" stroke-opacity=".5" stroke-width="2"/>
    <path d="M470 34a44 44 0 0 1 44 44" fill="none" stroke="#fff" stroke-opacity=".55" stroke-width="3" stroke-linecap="round"/>
  </g>
  <g>
    <rect x="18" y="58" width="392" height="262" rx="18" fill="#1c1d2c" stroke="#34374f" stroke-width="2"/>
    <svg x="36" y="76" width="356" height="216" viewBox="0 0 160 100" preserveAspectRatio="xMidYMid slice"><use href="#hScene"/></svg>
    <rect x="36" y="76" width="356" height="216" rx="4" fill="none" stroke="#000" stroke-opacity=".4" stroke-width="2"/>
    <circle cx="214" cy="306" r="3" fill="#f3b545"/>
    <path d="M178 320h72l12 46H166z" fill="#26283c"/>
    <rect x="132" y="364" width="164" height="12" rx="6" fill="#34374f"/>
  </g>
  <g class="ha-phone">
    <rect x="356" y="150" width="168" height="282" rx="28" fill="url(#hShell)" stroke="#24205e" stroke-width="2"/>
    <rect x="372" y="172" width="136" height="120" rx="10" fill="#17171f"/>
    <svg x="380" y="180" width="120" height="96" viewBox="0 0 160 100" preserveAspectRatio="xMidYMid slice"><use href="#hScene"/></svg>
    <text x="440" y="287" text-anchor="middle" font-family="Chakra Petch, sans-serif" font-style="italic" font-weight="700" font-size="9" fill="#b5b3e6" letter-spacing="1.5">ÍNDIGO</text>
    <g fill="#2a2a36"><rect x="384" y="330" width="16" height="48" rx="3"/><rect x="368" y="346" width="48" height="16" rx="3"/></g>
    <circle cx="486" cy="338" r="13" fill="#3b3a4e"/><circle cx="462" cy="362" r="13" fill="#3b3a4e"/>
    <rect x="408" y="400" width="24" height="7" rx="3.5" fill="#2a2a36"/><rect x="440" y="400" width="24" height="7" rx="3.5" fill="#2a2a36"/>
  </g>
</svg>`;

// ---------------------------------------------------------------- linha do tempo (ano do primeiro lançamento no mundo)
// Curiosidades só com fatos conhecidos; na dúvida, sem curiosidade. fresh: novo na versão atual · soon: ainda vai chegar
// exp: já está no app, mas ainda experimental
const DECADES = {
  1970: { label: 'Anos 70', blurb: 'O videogame chega à sala de casa, com jogos em cartucho.' },
  1980: { label: 'Anos 80', blurb: 'A era dos 8 bits: o videogame vira mania, chegam os portáteis e o primeiro CD.' },
  1990: { label: 'Anos 90', blurb: '16 bits, fliperamas em casa, jogos em CD e o salto para o 3D.' },
  2000: { label: 'Anos 2000', blurb: 'Portáteis com força de console de mesa.' },
};
export const TIMELINE = [
  { y: 1977, name: 'Atari 2600', kind: 'Console de mesa', fact: 'Lançado como Atari VCS, popularizou os jogos em cartucho.' },
  { y: 1982, name: 'ColecoVision', kind: 'Console de mesa', fact: 'Vinha com Donkey Kong na caixa.' },
  { y: 1982, name: 'Vectrex', kind: 'Console de mesa', fact: 'Tinha tela própria e desenhava com linhas (vetores) em vez de pixels.' },
  { y: 1983, name: 'SG-1000', kind: 'Console de mesa', fact: 'O primeiro console de mesa da Sega.' },
  { y: 1983, name: 'MSX', kind: 'Computador', fact: 'Não era um console, e sim um padrão de computador feito por várias marcas.' },
  { y: 1983, name: 'Famicom / NES', kind: 'Console de mesa', fact: 'Saiu no Japão como Famicom e chegou aos EUA em 1985 como NES.' },
  { y: 1985, name: 'Master System', alt: 'Mark III no Japão', kind: 'Console de mesa', fact: 'No Brasil fez sucesso por muitos anos, com a Tectoy.' },
  { y: 1986, name: 'Atari 7800', alt: 'testado em 1984', kind: 'Console de mesa', fact: 'Roda quase todos os jogos do Atari 2600.' },
  { y: 1987, name: 'PC Engine', kind: 'Console de mesa', fact: 'Nos EUA virou TurboGrafx-16. Os jogos vinham em cartões finos, os HuCards.' },
  { y: 1988, name: 'Mega Drive', kind: 'Console de mesa', fact: 'Nos EUA se chamou Genesis. No Brasil, foi lançado pela Tectoy.' },
  { y: 1988, name: 'PC Engine CD', kind: 'Acessório do PC Engine', fact: 'Foi o primeiro leitor de CD para videogame.' },
  { y: 1988, name: 'Capcom CPS1', kind: 'Fliperama', fact: 'A placa de fliperama em que saiu o Street Fighter II.' },
  { y: 1989, name: 'Game Boy', kind: 'Portátil', fact: 'Tela de 4 tons de verde. Nos EUA, vinha com Tetris.' },
  { y: 1989, name: 'Atari Lynx', kind: 'Portátil', fact: 'Um dos primeiros portáteis com tela colorida e iluminada.' },
  { y: 1990, name: 'Super Nintendo', alt: 'Super Famicom no Japão', kind: 'Console de mesa', fact: 'O Modo 7 girava e ampliava o cenário, como em F-Zero e Mario Kart.' },
  { y: 1990, name: 'Neo Geo MVS/AES', kind: 'Fliperama e console', fact: 'O AES levava para casa o mesmo hardware dos fliperamas (MVS).' },
  { y: 1990, name: 'Game Gear', kind: 'Portátil', fact: 'O portátil colorido da Sega, com peças parecidas com as do Master System.' },
  { y: 1991, name: 'Sega CD', alt: 'Mega-CD no Japão', kind: 'Acessório do Mega Drive', fact: 'Leitor de CD que se encaixa no Mega Drive.' },
  { y: 1993, name: 'Capcom CPS2', kind: 'Fliperama', fact: 'Sucessora da CPS1, com os jogos protegidos por criptografia.' },
  { y: 1993, name: '3DO', alt: 'Panasonic', kind: 'Console de mesa', fact: 'Era um padrão: várias fabricantes fizeram o aparelho.', exp: 'Experimental · desde a 2.2' },
  { y: 1994, name: 'PlayStation', kind: 'Console de mesa', fact: 'Lançado no Japão em dezembro de 1994, com os jogos em CD.', fresh: 1 },
  { y: 1994, name: 'Neo Geo CD', kind: 'Console de mesa', fact: 'Os jogos do Neo Geo em CD, bem mais baratos que os cartuchos.' },
  { y: 1994, name: '32X', kind: 'Acessório do Mega Drive', fact: 'Encaixava no Mega Drive como um cartucho e somava dois processadores.' },
  { y: 1995, name: 'Virtual Boy', kind: 'Portátil de mesa', fact: 'Mostrava imagens em 3D, só em vermelho e preto.' },
  { y: 1998, name: 'Game Boy Color', kind: 'Portátil', fact: 'Também roda os jogos do Game Boy original.' },
  { y: 1998, name: 'Neo Geo Pocket', kind: 'Portátil', fact: 'A versão colorida, Neo Geo Pocket Color, saiu em 1999.' },
  { y: 1999, name: 'WonderSwan', kind: 'Portátil', fact: 'Dava para jogar na horizontal ou na vertical. A versão colorida saiu em 2000.' },
  { y: 2001, name: 'Game Boy Advance', kind: 'Portátil', fact: 'Também roda os jogos de Game Boy e Game Boy Color.' },
  { y: 2001, name: 'Pokémon Mini', kind: 'Portátil', fact: 'Um console minúsculo, só com jogos de Pokémon.' },
];
function timelineHtml() {
  const byDec = new Map();
  for (const t of TIMELINE) { const d = Math.floor(t.y / 10) * 10; if (!byDec.has(d)) byDec.set(d, []); byDec.get(d).push(t); }
  const item = (t) => `<li class="tl-it${t.fresh ? ' tl-new' : ''}${t.soon ? ' tl-soon' : ''}">
          <span class="tl-dot" aria-hidden="true"></span>
          <div class="tl-card">
            <p class="tl-meta"><time datetime="${t.y}">${t.y}</time><span>${esc(t.kind)}</span></p>
            <h4>${esc(t.name)}${t.fresh ? ' <span class="tag-new">Novo</span>' : ''}${t.soon ? ` <span class="tag-soon">${esc(t.soon)}</span>` : ''}${t.exp ? ` <span class="tag-soon">${esc(t.exp)}</span>` : ''}</h4>
            ${t.alt ? `<p class="tl-alt">${esc(t.alt)}</p>` : ''}${t.fact ? `<p class="tl-fact">${esc(t.fact)}</p>` : ''}
          </div>
        </li>`;
  return `<ol class="tl" aria-label="Consoles do Índigo por ano de lançamento">
    ${[...byDec].map(([d, list]) => `<li class="tl-dec">
      <div class="tl-dec-head"><p class="tl-dec-y">${DECADES[d].label}</p><p>${esc(DECADES[d].blurb)}</p></div>
      <ol class="tl-items">
        ${list.map(item).join('\n        ')}
      </ol>
    </li>`).join('\n    ')}
    <li class="tl-end"><span class="tl-end-mark" aria-hidden="true"></span><p><time datetime="2026">2026</time> · <b>Índigo Nexus:</b> todos eles conectados num app só.</p></li>
  </ol>`;
}

// ---------------------------------------------------------------- "O que é um emulador?" (para quem está chegando)
const stepArt = {
  game: `<svg viewBox="0 0 160 110" aria-hidden="true" focusable="false">
    <rect x="18" y="14" width="62" height="74" rx="6" fill="#4a46a8" stroke="#6763c9" stroke-width="2"/>
    <rect x="26" y="24" width="46" height="34" rx="3" fill="#26283c"/>
    <path d="M30 34h38M30 41h28M30 48h34" stroke="#c3c1f0" stroke-width="3" stroke-linecap="round" opacity=".7"/>
    <g fill="#f3b545"><rect x="28" y="76" width="4" height="12"/><rect x="36" y="76" width="4" height="12"/><rect x="44" y="76" width="4" height="12"/><rect x="52" y="76" width="4" height="12"/><rect x="60" y="76" width="4" height="12"/><rect x="68" y="76" width="4" height="12"/></g>
    <circle cx="112" cy="54" r="34" fill="#26283c" stroke="#8fe3dc" stroke-width="2"/>
    <circle cx="112" cy="54" r="34" fill="none" stroke="#c3a6ff" stroke-width="10" opacity=".25"/>
    <circle cx="112" cy="54" r="9" fill="#11121c" stroke="#ecebf6" stroke-opacity=".5" stroke-width="2"/>
    <text x="80" y="104" text-anchor="middle" font-family="IBM Plex Mono, monospace" font-size="10" fill="#a9aac6">01001011 10110010</text>
  </svg>`,
  chip: `<svg viewBox="0 0 160 110" aria-hidden="true" focusable="false">
    <g stroke="#6763c9" stroke-width="3" stroke-linecap="round"><path d="M44 18v-8M60 18v-8M76 18v-8M92 18v-8M108 18v-8M44 92v8M60 92v8M76 92v8M92 92v8M108 92v8M30 32h-8M30 48h-8M30 64h-8M30 80h-8M130 32h8M130 48h8M130 64h8M130 80h8"/></g>
    <rect x="30" y="18" width="100" height="74" rx="8" fill="#1c1d2c" stroke="#6763c9" stroke-width="2"/>
    <rect x="40" y="28" width="38" height="24" rx="4" fill="#4a46a8"/><text x="59" y="44" text-anchor="middle" font-family="Chakra Petch, sans-serif" font-weight="700" font-size="10" fill="#fff">CPU</text>
    <rect x="84" y="28" width="38" height="24" rx="4" fill="#f3b545"/><text x="103" y="44" text-anchor="middle" font-family="Chakra Petch, sans-serif" font-weight="700" font-size="10" fill="#241a05">VÍDEO</text>
    <rect x="40" y="58" width="82" height="24" rx="4" fill="#26283c" stroke="#6fd3a0" stroke-width="1.5"/>
    <path d="M50 70h6l3-7 5 14 5-12 4 9 3-4h6l3-6 4 10 4-8 3 4h10" fill="none" stroke="#6fd3a0" stroke-width="1.8" stroke-linejoin="round"/>
  </svg>`,
  play: `<svg viewBox="0 0 160 110" aria-hidden="true" focusable="false">
    <rect x="10" y="12" width="96" height="62" rx="6" fill="#1c1d2c" stroke="#34374f" stroke-width="2"/>
    <rect x="16" y="18" width="84" height="50" rx="2" fill="#2a2766"/>
    <circle cx="80" cy="36" r="9" fill="#f3b545"/><polygon points="16,62 36,44 50,56 66,42 100,62 100,68 16,68" fill="#322e80"/>
    <g transform="translate(32 50)"><rect x="2" y="0" width="6" height="3" fill="#f3b545"/><rect x="1" y="3" width="8" height="4" fill="#ecebf6"/><rect x="0" y="7" width="10" height="5" fill="#6763c9"/></g>
    <path d="M48 74h20l4 12H44z" fill="#26283c"/>
    <rect x="112" y="30" width="40" height="72" rx="8" fill="#4a46a8" stroke="#24205e" stroke-width="2"/>
    <rect x="117" y="37" width="30" height="26" rx="3" fill="#2a2766"/><circle cx="138" cy="45" r="4" fill="#f3b545"/>
    <path d="M122 76v10M117 81h10" stroke="#2a2a36" stroke-width="4" stroke-linecap="round"/><circle cx="142" cy="78" r="3.5" fill="#3b3a4e"/><circle cx="136" cy="85" r="3.5" fill="#3b3a4e"/>
  </svg>`,
};
function explainHtml() {
  const steps = [
    ['game', 'O jogo é o mesmo', 'O arquivo do jogo é uma cópia do que está gravado no cartucho ou no CD. Nada é refeito: são exatamente os mesmos dados.'],
    ['chip', 'O Índigo imita o console', 'Ele recria, em software, as peças de dentro do videogame: o processador, o chip de vídeo e o de som, funcionando no mesmo ritmo.'],
    ['play', 'Você joga no PC ou no celular', 'A imagem e o som saem na sua tela, e o controle ou o toque fazem o papel do joystick original.'],
  ];
  return `<section class="l-sec explain" id="o-que-e" aria-labelledby="explainTitle">
  <div class="l-wrap">
    <div class="l-head">
      <p class="l-kicker">Para quem está chegando</p>
      <h2 id="explainTitle">O que é um emulador?</h2>
      <p>É um programa que <b>finge ser um videogame</b>. Ele imita, uma por uma, as peças de dentro do console, tão bem que o jogo acha que está rodando no aparelho de verdade.</p>
    </div>
    <ol class="steps3">
      ${steps.map(([k, t, d], i) => `<li class="step3"><div class="step3-art">${stepArt[k]}</div><p class="step3-n" aria-hidden="true">${i + 1}</p><h3>${esc(t)}</h3><p>${esc(d)}</p></li>`).join('\n      ')}
    </ol>
    <div class="explain-foot">
      <div class="keep">
        <span class="keep-ico">${ICON.heart}</span>
        <div>
          <h3>Por que isso preserva a história dos games</h3>
          <p>Os aparelhos antigos quebram, a bateria que guarda o save dos cartuchos acaba e os CDs riscam. Com um emulador, esses jogos continuam jogáveis para as próximas gerações, mesmo quando o console original não funcionar mais.</p>
        </div>
      </div>
      <p class="explain-legal">${ICON.lock}<span>Use cópias dos seus próprios jogos: o Índigo não traz jogos nem BIOS. <a href="#faq-jogos">Saiba mais nas perguntas</a>.</span></p>
    </div>
  </div>
</section>`;
}

// ---------------------------------------------------------------- blocos da página
// app: downloads/app.json · zip/apk: { href, size } · links: { autista }
export function landingTop({ app, zip, apk, tgz }) {
  const nav = `<a class="skip" href="#conteudo">Pular para o conteúdo</a>
<nav class="l-nav" aria-label="Principal">
  <div class="l-wrap l-nav-in">
    <a class="l-brand" href="#topo" aria-label="Índigo Nexus, início"><span class="l-mark" aria-hidden="true"></span>ÍNDIGO<span class="l-nexus">NEXUS</span></a>
    <ul class="l-links">
      <li><a href="#consoles">Consoles</a></li>
      <li><a href="#recursos">Recursos</a></li>
      <li><a href="#baixar">Baixar</a></li>
      <li><a href="#codigo">Código aberto</a></li>
      <li><a href="#perguntas">Perguntas</a></li>
    </ul>
    <a class="l-navplay" href="#jogar">${ICON.play}<span>Jogar</span></a>
    <a class="l-navcta" href="#baixar">${ICON.download}<span>Baixar</span></a>
  </div>
</nav>
<main id="conteudo" tabindex="-1">`;

  const hero = `<section class="hero" id="topo" aria-labelledby="heroTitle">
  <div class="l-wrap hero-in">
    <div class="hero-copy">
      <a class="hero-new" href="#ps1"><span class="tag-new">Novo</span>O PlayStation chegou ao Índigo</a>
      <h1 id="heroTitle"><span class="h1-brand">Índigo Nexus</span> O ponto de encontro de <span class="hl">50 anos de videogames.</span></h1>
      <p class="hero-lead">Do Atari ao PlayStation, ${SYSTEM_COUNT} consoles de várias gerações se conectam num emulador só, grátis para <b>Windows</b>, <b>Linux</b> e <b>Android</b>. Roda sem internet, guarda seus saves na nuvem e tem a precisão conferida a cada versão. E também joga aqui mesmo, no navegador.</p>
      <div class="hero-cta">
        <a class="b-primary" id="heroDl" href="#baixar" data-win="${esc(zip.href)}" data-win-size="${esc(zip.size)}" data-android="${esc(apk.href)}" data-android-size="${esc(apk.size)}"${tgz ? ` data-linux="${esc(tgz.href)}" data-linux-size="${esc(tgz.size)}"` : ''}>${ICON.download}<span class="b-txt"><span class="b-t">Baixar o Índigo</span><small class="b-s">para Windows, Linux e Android · grátis</small></span></a>
        <a class="b-ghost" id="heroPlay" href="#jogar">${ICON.play}<span>Jogar no navegador</span></a>
      </div>
      <p class="hero-meta"><span class="dot-ok" aria-hidden="true"></span>Versão ${esc(app.version)} · Windows 10 e 11 · Linux · Android 7 ou mais novo<span id="heroOther"></span></p>
    </div>
    <div class="hero-visual">${heroArt()}</div>
  </div>
  <div class="l-wrap">
    <ul class="stats" aria-label="O Índigo em números">
      <li><b>${SYSTEM_COUNT}</b><span>sistemas, do Atari 2600 ao PlayStation</span></li>
      <li><b>${COMPARISONS}</b><span>comparações automáticas a cada versão, com 0 problemas</span></li>
      <li><b>100%</b><span>offline: a internet só entra se você ligar a nuvem</span></li>
      <li><b>R$ 0</b><span>grátis, e de código aberto (GPL-3.0)</span></li>
    </ul>
  </div>
</section>`;

  const ps1 = `<section class="ps1" id="ps1" aria-labelledby="ps1Title">
  <div class="l-wrap ps1-in">
    <div class="ps1-disc" aria-hidden="true"><span></span></div>
    <div class="ps1-copy">
      <p class="l-kicker">Novo na 2.0</p>
      <h2 id="ps1Title">O PlayStation chegou</h2>
      <p>O primeiro console 3D do Índigo, com a mesma atenção à precisão de todos os outros. Use o disco que é seu, do jeito que você já tem guardado.</p>
      <ul class="ticks">
        <li>${ICON.check}Gráficos 3D</li>
        <li>${ICON.check}CDs em .cue/.bin, .chd e .iso</li>
        <li>${ICON.check}Memory card</li>
        <li>${ICON.check}DualShock com vibração</li>
      </ul>
      <p class="ps1-note">${ICON.lock}Pede a BIOS do seu próprio console, como o Sega CD, o PC Engine CD e o Neo Geo.</p>
    </div>
  </div>
</section>`;

  const groups = CONSOLE_GROUPS.map((g) => `<article class="cg${g.fresh ? ' cg-new' : ''}">
      <header class="cg-head"><span class="cg-ico">${ICON[g.icon]}</span><div><h3>${esc(g.title)}</h3><p>${esc(g.era)}</p></div></header>
      <ul class="cg-list">${g.items.map((i) => `<li><span class="cg-name">${esc(i.name)}${i.fresh ? ' <span class="tag-new">Novo</span>' : ''}</span>${i.note ? `<small>${esc(i.note)}</small>` : ''}${i.web || i.bios ? `<span class="cg-tags">${i.web ? '<span class="t-web" title="Também roda no navegador">navegador</span>' : ''}${i.bios ? '<span class="t-bios" title="Pede a BIOS do seu console">BIOS</span>' : ''}</span>` : ''}</li>`).join('')}</ul>
    </article>`).join('\n    ');
  const consoles = `<section class="l-sec" id="consoles" aria-labelledby="consTitle">
  <div class="l-wrap">
    <div class="l-head">
      <p class="l-kicker">Consoles</p>
      <h2 id="consTitle">${SYSTEM_COUNT} sistemas, um app só</h2>
      <p>De 1977 a 2001, dos fliperamas aos portáteis. O mesmo Índigo no PC e no celular.</p>
    </div>
    <div class="tabs" role="tablist" aria-label="Ver os consoles" hidden>
      <button type="button" role="tab" id="tabTl" aria-controls="panelTl" aria-selected="true">Linha do tempo</button>
      <button type="button" role="tab" id="tabFam" aria-controls="panelFam" aria-selected="false" tabindex="-1">Por família</button>
    </div>
    <div class="tab-panel" id="panelTl" role="tabpanel" aria-labelledby="tabTl">
      <h3 class="tab-title">Linha do tempo</h3>
      ${timelineHtml()}
    </div>
    <div class="tab-panel" id="panelFam" role="tabpanel" aria-labelledby="tabFam">
      <h3 class="tab-title">Por família</h3>
      <ul class="legend" aria-label="Legenda">
        <li><span class="t-web">navegador</span>também roda aqui no site, sem instalar</li>
        <li><span class="t-bios">BIOS</span>pede o arquivo do seu próprio console</li>
      </ul>
      <div class="cg-grid">
      ${groups}
      </div>
    </div>
  </div>
</section>`;

  const feats = [
    ['target', 'Precisão de verdade', `Cada versão passa por suítes de teste públicas e é comparada com emuladores de referência: ${COMPARISONS} comparações automáticas, 0 problemas.`],
    ['offline', 'Funciona sem internet', 'Tudo roda no seu aparelho, direto no processador. No avião, na estrada ou sem sinal, seus jogos abrem do mesmo jeito.'],
    ['devices', 'O mesmo app no PC e no celular', 'Windows, Linux e Android com a mesma cara e os mesmos recursos. Comece no computador e continue no celular.'],
    ['cloud', 'Saves na nuvem com PIN', 'Seus saves sobem e descem sozinhos entre os aparelhos com um PIN, o mesmo deste site. Sem conta, sem e-mail. Seus jogos nunca saem do aparelho.'],
    ['rewind', 'Estados salvos', 'Congele o jogo em qualquer ponto e volte quando quiser, até naquela fase sem save.'],
    ['pad', 'Controle ou toque', 'Funciona com controles como Xbox, DualSense, DualShock, 8BitDo e GameSir, ou com os botões na tela do celular.'],
  ];
  const features = `<section class="l-sec" id="recursos" aria-labelledby="featTitle">
  <div class="l-wrap">
    <div class="l-head">
      <p class="l-kicker">Por que o Índigo</p>
      <h2 id="featTitle">Feito para durar, e para ser jogado</h2>
    </div>
    <div class="feat-grid">
      ${feats.map(([i, t, d]) => `<article class="feat"><span class="feat-ico">${ICON[i]}</span><h3>${esc(t)}</h3><p>${esc(d)}</p></article>`).join('\n      ')}
    </div>
  </div>
</section>`;
  return [nav, hero, explainHtml(), ps1, consoles, features].join('\n');
}

// área de downloads (mantém os passos de instalação de cada sistema)
export function downloadsSection({ app, zip, apk, tgz }) {
  return `<section class="get" id="baixar" aria-labelledby="getTitle">
 <div class="l-wrap">
  <div class="get-head">
    <div class="get-intro">
      <p class="l-kicker">Baixar</p>
      <h2 id="getTitle">Índigo para PC e celular</h2>
      <p class="get-lead">Grátis, sem cadastro e sem precisar de internet para jogar. Os saves na nuvem usam o mesmo PIN deste site.</p>
    </div>
    <div class="get-ver"><span class="dot-ok" aria-hidden="true"></span>Versão ${esc(app.version)}<small>${esc(app.date)}</small></div>
  </div>
  <div class="get-grid">
    <article class="get-card" data-platform="windows" id="baixar-windows">
      <span class="dl-badge">Recomendado para este aparelho</span>
      <div class="dl-top"><span class="dl-icon">${ICON.pc}</span><div><h3>Windows</h3><p>Windows 10 e 11 · 64 bits</p></div></div>
      <a class="dl-main" href="${zip.href}" download>${ICON.download}<span>Baixar o Índigo</span><small>versão portátil, sem instalar · .zip · ${zip.size}</small></a>
      <p class="dl-sha">SHA-256: <code>${esc(zip.sha)}</code></p>
      <ol class="dl-steps">
        <li><span>Clique com o botão direito no arquivo baixado, escolha <b>Propriedades</b>, marque <b>Desbloquear</b> e clique em <b>OK</b>. Assim o Windows não bloqueia o Índigo.</span></li>
        <li><span>Clique de novo com o botão direito, escolha <b>Extrair tudo</b> e abra o <b>indigo.exe</b> da pasta extraída.</span></li>
        <li><span>Se ainda aparecer “O Windows protegeu o computador”, clique em <b>Mais informações</b> e depois em <b>Executar assim mesmo</b>.</span></li>
        <li><span>Arraste um jogo para a janela ou aperte <kbd>Ctrl</kbd>+<kbd>O</kbd>.</span></li>
      </ol>
    </article>
    <article class="get-card" data-platform="android" id="baixar-android">
      <span class="dl-badge">Recomendado para este aparelho</span>
      <div class="dl-top"><span class="dl-icon">${ICON.phone}</span><div><h3>Android</h3><p>Android 7 ou mais novo · celulares de 64 bits</p></div></div>
      <a class="dl-main" href="${apk.href}" download type="application/vnd.android.package-archive">${ICON.download}<span>Baixar o app</span><small>.apk · ${apk.size}</small></a>
      <p class="dl-sha">SHA-256: <code>${esc(apk.sha)}</code></p>
      <p class="dl-note">Funciona com o toque e com controles como GameSir, Xbox, DualSense e 8BitDo.</p>
      <ol class="dl-steps">
        <li><span>Toque no arquivo baixado e depois em <b>Instalar</b>.</span></li>
        <li><span>Se o Android pedir, permita <b>instalar apps desta fonte</b> para o navegador.</span></li>
        <li><span>Já tem uma versão anterior? Instale por cima: os saves continuam.</span></li>
      </ol>
    </article>${tgz ? `
    <article class="get-card" data-platform="linux" id="baixar-linux">
      <span class="dl-badge">Recomendado para este aparelho</span>
      <div class="dl-top"><span class="dl-icon">${ICON.pc}</span><div><h3>Linux</h3><p>PC de 64 bits e Steam Deck · Ubuntu 24.04, Fedora 39, Debian 13 ou mais novo</p></div></div>
      <a class="dl-main" href="${tgz.href}" download>${ICON.download}<span>Baixar o Índigo</span><small>.tar.gz · ${tgz.size}</small></a>
      <p class="dl-sha">SHA-256: <code>${esc(tgz.sha)}</code></p>
      <p class="dl-note">Primeira versão para Linux. A nuvem dos saves ainda não funciona nela.</p>
      <ol class="dl-steps">
        <li><span>Clique com o botão direito no arquivo baixado e escolha <b>Extrair aqui</b>.</span></li>
        <li><span>Abra o <b>indigo</b> da pasta extraída. Para ter o atalho no menu de aplicativos, rode <kbd>./instalar-atalho.sh</kbd>.</span></li>
        <li><span>Arraste um jogo para a janela ou aperte <kbd>Ctrl</kbd>+<kbd>O</kbd>.</span></li>
      </ol>
    </article>` : ''}
  </div>
  <p class="get-ios" hidden>No iPhone e no iPad, o Índigo funciona aqui mesmo no navegador: <a href="#jogar">jogar agora</a>.</p>
  <div class="get-foot">
    <div class="get-news">
      <h3>Novo na ${esc(app.version.replace(/\.0$/, ''))}</h3>
      <ul>${app.news.map((n) => `<li>${esc(n)}</li>`).join('')}</ul>
    </div>
    <div class="get-other">
      <h3>Mac, iPhone ou Xbox?</h3>
      <p>O app é para Windows, Linux e Android. Nos outros aparelhos, o Índigo joga Game Boy, GBA, Master System, Game Gear e Neo Geo Pocket <a href="#jogar">aqui no navegador</a>.</p>
    </div>
  </div>
 </div>
</section>`;
}

// código aberto, planos e perguntas: entre os downloads e o emulador do navegador
export function landingMid({ press = [] } = {}) {
  const phases = [
    ['Fase 1', 'No navegador', 'O Índigo nasce como emulador web: Game Boy, Game Boy Color e Game Boy Advance, depois Master System, Game Gear e Neo Geo Pocket.', 'done'],
    ['Fase 2', 'App para PC e celular', 'Um app nativo para Windows e Android (e agora Linux), console por console, dos 8 bits até o PlayStation na versão 2.0.', 'done'],
    ['Fase 3', 'Manutenção e melhorias', 'Sem consoles novos depois do 3DO: o foco agora é deixar cada um melhor. Precisão, compatibilidade e velocidade, e recursos como resolução aumentada no PlayStation, rebobinar e filtros de imagem.', 'now'],
  ];
  const open = `<section class="l-sec" id="codigo" aria-labelledby="codeTitle">
  <div class="l-wrap code-grid">
    <div class="code-copy">
      <p class="l-kicker">Código aberto</p>
      <h2 id="codeTitle">Aberto, e com um ponto final de propósito</h2>
      <p>O código do Índigo é aberto, sob a licença <b>GPL-3.0</b>: qualquer pessoa pode ler, estudar e melhorar, e ele continua livre para sempre.</p>
      <p>A lista vai até o PlayStation e o 3DO (experimental, desde a 2.2). Depois disso, em vez de correr atrás do próximo console, o Índigo para e investe em qualidade: a meta é ser o melhor emulador de código aberto até a geração do PlayStation.</p>
      <div class="code-badges">
        <span class="badge">${ICON.code}GPL-3.0</span>
        <a class="badge" href="https://github.com/joaoabitante/indigo" target="_blank" rel="noopener">${ICON.code}Código no GitHub</a>
      </div>
    </div>
    <ol class="road" aria-label="Plano do projeto">
      ${phases.map(([f, t, d, st]) => `<li class="road-${st}"><span class="road-dot" aria-hidden="true"></span><p class="road-f">${f}${st === 'now' ? ' <span class="tag-now">Agora</span>' : ''}</p><h3>${esc(t)}</h3><p>${esc(d)}</p></li>`).join('\n      ')}
    </ol>
  </div>
</section>`;

  const faq = [
    ['O Índigo é grátis mesmo?', 'Sim. O app e o site são grátis, sem cadastro. O código é aberto (GPL-3.0), em github.com/joaoabitante/indigo.'],
    ['O Índigo vem com jogos ou BIOS?', 'Não. O Índigo não traz nenhum jogo e nenhuma BIOS, e não indica onde baixar. Use cópias dos seus próprios cartuchos e discos.', 'faq-jogos'],
    ['Quais consoles pedem BIOS?', 'O PlayStation, o Sega CD, o Neo Geo, o Neo Geo CD e o PC Engine CD. Na primeira vez, o app pede o arquivo da BIOS do seu console. Os outros sistemas rodam sem nada além do jogo.'],
    ['Quais arquivos de jogo ele abre?', 'Os arquivos comuns de cada console, inclusive dentro de .zip. Os jogos em CD abrem em .cue/.bin, .chd ou .iso.'],
    ['Precisa de internet?', 'Não. O app roda tudo no aparelho. A internet só é usada se você ligar os saves na nuvem.'],
    ['Como funcionam os saves na nuvem?', 'Você escolhe um PIN e usa o mesmo em todos os aparelhos e neste site. Os saves vão para a nuvem e os mais novos descem para os outros aparelhos. Os jogos nunca saem do seu aparelho.'],
    ['Que dados a nuvem guarda?', 'Só os seus saves, com o nome do jogo e do aparelho, ligados ao seu PIN. O Índigo não pede nome, e-mail nem documento, e os jogos nunca saem do seu aparelho. Quem não liga a nuvem não manda nada para a internet.'],
    ['O Windows avisou que o arquivo pode ser perigoso. E agora?', 'O Índigo ainda não tem assinatura digital, por isso o Windows desconfia de todo programa novo. Antes de extrair, clique com o botão direito no .zip, abra Propriedades e marque Desbloquear. Se aparecer “O Windows protegeu o computador”, clique em Mais informações e em Executar assim mesmo.'],
    ['Tem para iPhone ou Mac?', 'O app é para Windows, Linux (PC e Steam Deck) e Android. Em qualquer outro aparelho, use o emulador do navegador aqui do site, que roda Game Boy, Game Boy Color, GBA, Master System, Game Gear e Neo Geo Pocket.'],
    ['Por que parar na geração do PlayStation?', 'Para fazer bem feito. O Índigo vai até a geração do PlayStation: o 3DO, de 1993, é da mesma época e chegou na versão 2.2 (ainda experimental), como o último console novo. Depois disso, o tempo vai só para melhorar os que já estão lá: precisão, compatibilidade e velocidade.'],
  ];
  const faqHtml = `<section class="l-sec" id="perguntas" aria-labelledby="faqTitle">
  <div class="l-wrap">
    <div class="l-head">
      <p class="l-kicker">Perguntas</p>
      <h2 id="faqTitle">Perguntas frequentes</h2>
    </div>
    <div class="faq">
      ${faq.map(([q, a, id]) => `<details${id ? ` id="${id}"` : ''}><summary>${esc(q)}</summary><p>${esc(a)}</p></details>`).join('\n      ')}
    </div>
  </div>
</section>`;
  const pressHtml = press.length ? `<section class="l-sec" id="midia" aria-labelledby="pressTitle">
  <div class="l-wrap">
    <div class="l-head">
      <p class="l-kicker">Na mídia</p>
      <h2 id="pressTitle">O Índigo na mídia</h2>
    </div>
    <ul class="press">
      ${press.map((m) => `<li class="press-card">
        <p class="press-src"><span class="press-outlet">${esc(m.outlet)}</span><span>${esc(m.author)} · ${esc(m.date)}</span></p>
        <h3><a href="${esc(m.url)}" target="_blank" rel="noopener">${esc(m.title)}<span class="sr"> (abre em nova aba)</span></a></h3>
        <p>${esc(m.summary)}</p>
        <span class="press-more" aria-hidden="true">Ler a matéria ${ICON.ext}</span>
      </li>`).join('\n      ')}
    </ul>
  </div>
</section>` : '';
  return [open, pressHtml, faqHtml].join('\n');
}

// Autista Empreende: fecha o conteúdo, logo depois do emulador do navegador
export function landingEnd({ autista }) {
  const link = autista
    ? `<a class="b-primary b-warm" href="${esc(autista)}" target="_blank" rel="noopener">${ICON.heart}<span class="b-txt"><span class="b-t">Conhecer o Autista Empreende</span></span>${ICON.ext}</a>`
    : '';
  return `<section class="ae" id="autista-empreende" aria-labelledby="aeTitle">
  <div class="l-wrap">
    <div class="ae-card">
      <svg class="ae-art" viewBox="0 0 120 60" aria-hidden="true" focusable="false">
        <defs><linearGradient id="aeGrad" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#e8683f"/><stop offset=".25" stop-color="#f3b545"/><stop offset=".5" stop-color="#6fd3a0"/><stop offset=".75" stop-color="#5fb7f0"/><stop offset="1" stop-color="#9d8cff"/></linearGradient></defs>
        <path d="M60 30c-9-11-15-17-26-17a17 17 0 0 0 0 34c11 0 17-6 26-17s15-17 26-17a17 17 0 0 1 0 34c-11 0-17-6-26-17z" fill="none" stroke="url(#aeGrad)" stroke-width="7" stroke-linecap="round"/>
      </svg>
      <div class="ae-copy">
        <p class="l-kicker">Um projeto do Autista Empreende</p>
        <h2 id="aeTitle">Feito por quem joga.</h2>
        <blockquote class="ae-quote">
          <p>Eu jogo com emuladores desde a época da escola, quando o GBA era o meu companheiro no recreio e no caminho de casa. O Índigo nasceu dessa paixão: um projeto meu, construído com a ajuda de inteligência artificial, para reunir num só lugar os consoles que marcaram a minha infância, do Game Boy ao PlayStation, rodando no PC e no celular, de graça e com código aberto.</p>
          <footer>— João, criador do Índigo e do Autista Empreende</footer>
        </blockquote>
        ${link}
      </div>
    </div>
  </div>
</section>`;
}

export function landingFooter({ app, autista }) {
  const ae = autista ? `<a href="${esc(autista)}" target="_blank" rel="noopener">Autista Empreende</a>` : 'Autista Empreende';
  return `<footer class="l-foot">
  <div class="l-wrap foot-in">
    <div class="foot-brand">
      <a class="l-brand" href="#topo" aria-label="Índigo Nexus, início"><span class="l-mark" aria-hidden="true"></span>ÍNDIGO<span class="l-nexus">NEXUS</span></a>
      <p class="foot-tag">O ponto de encontro de 50 anos de videogames: consoles, gerações e aparelhos conectados num app só.</p>
      <p>Um projeto do ${ae}, criado pelo João com a ajuda de inteligência artificial (Claude).</p>
      <p>Versão ${esc(app.version)} · Código aberto (GPL-3.0): <a href="https://github.com/joaoabitante/indigo" target="_blank" rel="noopener">github.com/joaoabitante/indigo</a>.</p>
    </div>
    <ul class="foot-links" aria-label="Atalhos">
      <li><a href="#consoles">Consoles</a></li>
      <li><a href="#recursos">Recursos</a></li>
      <li><a href="#baixar">Baixar</a></li>
      <li><a href="#jogar">Jogar no navegador</a></li>
      <li><a href="#codigo">Código aberto</a></li>
      <li><a href="#perguntas">Perguntas</a></li>
    </ul>
    <p class="foot-legal">O Índigo não traz jogos nem BIOS: use cópias suas. Os nomes dos consoles aparecem só para identificar compatibilidade e são marcas dos seus donos. O Índigo não tem ligação com nenhum fabricante.</p>
  </div>
</footer>`;
}

// ---------------------------------------------------------------- metadados (título, descrição, Open Graph, JSON-LD)
export function metaHtml({ app, siteUrl, autista }) {
  const base = siteUrl.replace(/\/+$/, '') + '/';
  const title = 'Índigo Nexus — emulador de videogames do Atari ao PlayStation';
  const desc = `Índigo Nexus: o ponto de encontro de 50 anos de videogames. Emulador grátis e de código aberto para Windows, Linux e Android, com ${SYSTEM_COUNT} consoles do Atari ao PlayStation, sem internet e com saves na nuvem. Também roda no navegador.`;
  const ld = {
    '@context': 'https://schema.org',
    '@type': 'SoftwareApplication',
    name: 'Índigo Nexus',
    alternateName: 'Índigo',
    description: desc,
    applicationCategory: 'GameApplication',
    applicationSubCategory: 'Emulador de videogames',
    operatingSystem: 'Windows, Linux, Android',
    softwareVersion: app ? app.version : undefined,
    inLanguage: 'pt-BR',
    url: base,
    image: base + 'og.png',
    downloadUrl: base + '#baixar',
    license: 'https://www.gnu.org/licenses/gpl-3.0.html',
    isAccessibleForFree: true,
    offers: { '@type': 'Offer', price: '0', priceCurrency: 'BRL' },
    author: { '@type': 'Organization', name: 'Autista Empreende', ...(autista ? { url: autista } : {}) },
  };
  const json = JSON.stringify(ld).replace(/</g, '\\u003c');
  return `<title>${esc(title)}</title>
<meta name="description" content="${esc(desc)}">
<meta property="og:type" content="website">
<meta property="og:locale" content="pt_BR">
<meta property="og:site_name" content="Índigo Nexus">
<meta property="og:title" content="Índigo Nexus · o ponto de encontro de 50 anos de videogames">
<meta property="og:description" content="Emulador grátis e de código aberto para Windows, Linux e Android, com ${SYSTEM_COUNT} consoles do Atari ao PlayStation, saves na nuvem e precisão conferida a cada versão. Também roda no navegador.">
<link rel="canonical" href="${esc(base)}">
<meta property="og:url" content="${esc(base)}">
<meta property="og:image" content="${esc(base)}og.png">
<meta property="og:image:width" content="1200">
<meta property="og:image:height" content="630">
<meta property="og:image:alt" content="Índigo Nexus: emulador de videogames clássicos para PC, celular e navegador">
<meta name="twitter:card" content="summary_large_image">
<script type="application/ld+json">${json}</script>`;
}
