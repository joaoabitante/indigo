// Página inicial: destaca o download do aparelho de quem abre a página.
// Android → APK; Windows → versão portátil (.zip); Linux (e Steam Deck) → .tar.gz; iPhone/iPad → o Índigo roda aqui no navegador.
// O botão principal do topo baixa direto no Windows e no Android e depois mostra os passos de instalação.
export function initDownloads() {
  const box = document.getElementById('baixar');
  if (!box) return;
  const ua = navigator.userAgent || '';
  const platform = (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || '';
  const ios = /iPhone|iPad|iPod/.test(ua) || (platform === 'MacIntel' && navigator.maxTouchPoints > 1);
  const android = /Android/i.test(ua);
  const xbox = /Xbox/i.test(ua);
  const windows = !android && !xbox && /Win/i.test(platform + ' ' + ua);
  const linux = !android && !windows && /Linux/i.test(platform + ' ' + ua) && !/CrOS/.test(ua) && !!box.querySelector('[data-platform="linux"]');
  const rec = android ? 'android' : windows ? 'windows' : linux ? 'linux' : '';
  for (const card of box.querySelectorAll('.get-card')) card.dataset.rec = card.dataset.platform === rec ? '1' : '0';
  // o cartão recomendado vem primeiro (no celular, um embaixo do outro)
  const first = rec && box.querySelector(`.get-card[data-platform="${rec}"]`);
  if (first && first.previousElementSibling) first.parentElement.prepend(first);
  const iosNote = box.querySelector('.get-ios');
  if (iosNote) iosNote.hidden = !ios;
  try { initHero(rec, ios || xbox, first); } catch { /* o topo continua com o link para #baixar */ }
  try { followGame(); } catch { /* opcional */ }
  try { initTabs(); } catch { /* sem abas: as duas listas aparecem */ }
  try { initFaqLinks(); } catch { /* opcional */ }
}

function initHero(rec, browserOnly, card) {
  const dl = document.getElementById('heroDl');
  if (!dl) return;
  const t = dl.querySelector('.b-t'), s = dl.querySelector('.b-s'), other = document.getElementById('heroOther');
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (rec) {
    const win = rec === 'windows', lin = rec === 'linux';
    dl.href = win ? dl.dataset.win : lin ? dl.dataset.linux : dl.dataset.android;
    dl.setAttribute('download', '');
    if (rec === 'android') dl.type = 'application/vnd.android.package-archive';
    t.textContent = win ? 'Baixar para Windows' : lin ? 'Baixar para Linux' : 'Baixar para Android';
    s.textContent = win ? `versão portátil · .zip · ${dl.dataset.winSize}` : lin ? `.tar.gz · ${dl.dataset.linuxSize}` : `.apk · ${dl.dataset.androidSize}`;
    if (other) other.innerHTML = ` · <a href="#baixar">${rec === 'android' ? 'também para Windows e Linux' : rec === 'linux' ? 'também para Windows e Android' : 'também para Linux e Android'}</a>`;
    // o download começa e a página mostra os passos de instalação do aparelho
    dl.addEventListener('click', () => {
      if (card) setTimeout(() => card.scrollIntoView({ behavior: reduce ? 'auto' : 'smooth', block: 'start' }), 300);
    });
  } else if (browserOnly) {
    // iPhone, iPad e Xbox: o app não existe para eles, então o principal é jogar no navegador
    const play = document.getElementById('heroPlay');
    dl.href = '#jogar';
    t.textContent = 'Jogar no navegador';
    s.textContent = 'grátis, sem instalar';
    dl.querySelector('svg').outerHTML = '<svg class="ico" viewBox="0 0 24 24" aria-hidden="true" focusable="false"><path d="M8 5.5v13l10.5-6.5z" fill="currentColor" stroke="none"/></svg>';
    if (play) { play.href = '#baixar'; play.lastElementChild.textContent = 'App para PC e Android'; }
  }
}

// abriu um jogo (arrastando a ROM em qualquer lugar da página, por exemplo) longe do emulador: rola até ele
function followGame() {
  const dev = document.getElementById('touch');
  if (!dev || !('MutationObserver' in window)) return;
  let was = document.body.dataset.loaded;
  new MutationObserver(() => {
    const now = document.body.dataset.loaded;
    if (now === '1' && was !== '1' && document.body.dataset.imm !== '1') {
      const r = dev.getBoundingClientRect();
      if (r.bottom < 80 || r.top > innerHeight * 0.6) dev.scrollIntoView({ behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth', block: 'start' });
    }
    was = now;
  }).observe(document.body, { attributes: true, attributeFilter: ['data-loaded'] });
}

// Abas "Linha do tempo" / "Por família" na seção de consoles. Sem JavaScript, as duas aparecem uma embaixo da outra.
export function initTabs() {
  const list = document.querySelector('#consoles .tabs');
  if (!list) return;
  const tabs = [...list.querySelectorAll('[role="tab"]')];
  const show = (tab, focus) => {
    for (const t of tabs) {
      const on = t === tab;
      t.setAttribute('aria-selected', on ? 'true' : 'false');
      t.tabIndex = on ? 0 : -1;
      document.getElementById(t.getAttribute('aria-controls')).hidden = !on;
    }
    if (focus) tab.focus();
  };
  for (const t of tabs) {
    document.getElementById(t.getAttribute('aria-controls')).tabIndex = 0;
    t.addEventListener('click', () => show(t));
    t.addEventListener('keydown', (e) => {
      const i = tabs.indexOf(t);
      const j = e.key === 'ArrowRight' ? (i + 1) % tabs.length : e.key === 'ArrowLeft' ? (i - 1 + tabs.length) % tabs.length : e.key === 'Home' ? 0 : e.key === 'End' ? tabs.length - 1 : -1;
      if (j >= 0) { e.preventDefault(); e.stopPropagation(); show(tabs[j], true); }
    });
  }
  list.hidden = false;
  document.getElementById('consoles').classList.add('tabs-on');
  show(tabs[0]);
}

// link para uma pergunta (#faq-jogos): abre a resposta
export function initFaqLinks() {
  const open = () => {
    const el = location.hash && document.getElementById(decodeURIComponent(location.hash.slice(1)));
    if (el && el.tagName === 'DETAILS') el.open = true;
  };
  window.addEventListener('hashchange', open);
  open();
}
