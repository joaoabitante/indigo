// Menu do console: interface de "sofá" navegável só com o controle (ou setas/Enter/Esc).
// Resolve o Edge do Xbox: no modo "Usar controles de jogo" o cursor some, então tudo é feito por aqui.
//
// Botões (layout padrão): direcional/analógico = mover, A = confirmar, B = voltar, X = enviar (na estante),
// LB/RB = filtrar a estante por console, Y = mudar a ordem, View + Menu juntos = abrir/fechar o menu.
// Telas: principal, estante, cartucho (continuar, estados, versões do save, remover), nuvem e backup,
// listas genéricas e confirmação.

import { esc, imgSrc } from './safe.js';

export class ConsoleMenu {
  constructor(root, api) {
    this.root = root;      // elemento do overlay
    this.api = api;        // ações do emulador
    this.open = false;
    this.screen = 'main';
    this.index = 0;
    this.digits = '';
    this.shelf = [];
    this.message = null;
    this.prevPressed = new Set();
    this.prevAxis = [0, 0];
    this.repeatAt = 0;
    this.wasPaused = false;
    // toque e mouse: tocar num item escolhe; ◀ ▶ mudam opções laterais; fora do painel fecha
    root.addEventListener('click', (e) => {
      if (!this.open) return;
      if (e.target === root) { this.hide(); return; }
      const nav = e.target.closest('[data-cm]');
      if (nav) { if (nav.dataset.cm === 'close') this.hide(); else this.back(); return; }
      const it = e.target.closest('[data-i]');
      if (this.screen === 'busy') return;
      if (!it) { if (this.screen === 'done' || this.screen === 'error') this.confirm(); return; }
      this.index = +it.dataset.i;
      const side = e.target.closest('[data-side]');
      if (side) { this.move(side.dataset.side); this.render(); return; }
      this.confirm();
    });
  }

  // ---------------- abrir / fechar ----------------
  show(screen = 'main') {
    if (!this.open) {
      this.wasPaused = this.api.isPaused();
      if (this.api.isRunning() && !this.wasPaused) this.api.setPaused(true);
    }
    this.open = true;
    this.go(screen);
    this.root.hidden = false;
    this.api.mount(this.root);
  }
  hide() {
    this.open = false;
    this.root.hidden = true;
    if (this.api.isRunning() && !this.wasPaused) this.api.setPaused(false);
  }
  toggle() { this.open ? this.hide() : this.show(); }

  async go(screen, index = 0) {
    this.screen = screen;
    this.index = index;
    if (screen === 'shelf') { this.shelf = await this.api.listShelf(); if (this.index >= this.shelf.length) this.index = Math.max(0, this.shelf.length - 1); }
    if (screen === 'keypad') this.digits = '';
    this.render();
  }

  // ---------------- itens de cada tela ----------------
  mainItems() {
    const a = this.api;
    const items = [];
    if (a.isRunning()) items.push({ label: 'Voltar ao jogo', hint: a.currentName(), run: () => this.hide() });
    if (a.isImmersive && a.isImmersive()) items.push({ label: 'Ver a página', hint: 'Sai da tela cheia (estante, opções); deite o celular de novo para voltar', run: () => { this.hide(); a.exitImmersive(); } });
    const res = a.resumeTarget();
    if (res) items.push({ label: 'Continuar de onde parou', hint: res.name, run: () => { this.hide(); a.resume(res.id); } });
    items.push({ label: 'Estante', hint: 'Escolher um jogo, ver estados e versões do save', run: () => this.go('shelf', this.shelfIndex || 0) });
    if (a.cloudAvailable()) items.push({ label: 'Nuvem e backup', hint: a.cloudStatus() || 'Saves na nuvem', run: () => this.openCloud() });
    if (a.canTransfer()) items.push({ label: 'Receber por código', hint: 'Um cartucho ou a estante inteira: digitar o código de 6 dígitos', run: () => this.go('keypad') });
    if (a.canTransfer()) items.push({ label: 'Enviar estante inteira', hint: 'Todos os jogos, saves e estados para outro aparelho', run: () => this.sendShelf() });
    if (a.isTouchPlay && a.isTouchPlay()) { if (!a.isImmersive()) items.push({ label: 'Tela cheia', hint: 'Jogo e botões de toque ocupando a tela', run: () => { this.hide(); a.toggleTv(); } }); }
    else items.push({ label: a.isTv() ? 'Sair do Modo TV' : 'Modo TV', hint: 'Tela cheia em 1080p', run: () => { a.toggleTv(); this.render(); } });
    items.push({ label: 'Filtro', hint: a.filterName(), side: true, run: () => { a.cycleFilter(1); this.render(); }, left: () => { a.cycleFilter(-1); this.render(); }, right: () => { a.cycleFilter(1); this.render(); } });
    if (a.isRunning()) items.push({ label: 'Estados salvos', hint: 'Ver os 9 espaços com miniatura', run: () => this.openSlots() });
    if (a.isRunning()) {
      const slot = a.stateSlot();
      items.push({ label: 'Salvar estado', hint: `Espaço ${slot}`, side: true, run: () => { a.saveState(); this.hide(); }, left: () => { a.cycleSlot(-1); this.render(); }, right: () => { a.cycleSlot(1); this.render(); } });
      items.push({ label: 'Carregar estado', hint: a.stateInfo(), side: true, run: () => { a.loadState(); this.hide(); }, left: () => { a.cycleSlot(-1); this.render(); }, right: () => { a.cycleSlot(1); this.render(); } });
    }
    if (a.isGameBoy()) items.push({ label: 'Paleta do Game Boy', hint: a.paletteName(), side: true, run: () => { a.cyclePalette(1); this.render(); }, left: () => { a.cyclePalette(-1); this.render(); }, right: () => { a.cyclePalette(1); this.render(); } });
    items.push({ label: 'Carcaça', hint: a.shellName(), side: true, run: () => { a.cycleShell(1); this.render(); }, left: () => { a.cycleShell(-1); this.render(); }, right: () => { a.cycleShell(1); this.render(); } });
    if (a.isRunning()) {
      items.push({ label: 'Reiniciar jogo', hint: 'Volta ao início; o save continua', run: () => { a.reset(); this.hide(); } });
    }
    items.push({ label: 'Fechar menu', hint: 'View + Menu abre de novo', run: () => this.hide() });
    return items;
  }

  // ---------------- entrada ----------------
  // chamada a cada quadro com os botões apertados (índices do layout padrão) e os eixos
  input(pressed, axes) {
    const now = performance.now();
    const fresh = (i) => pressed.has(i) && !this.prevPressed.has(i);
    const ax = axes[0] || 0, ay = axes[1] || 0;
    let dir = null;
    const held = (i) => pressed.has(i);
    const want = held(12) || ay < -0.6 ? 'up' : held(13) || ay > 0.6 ? 'down' : held(14) || ax < -0.6 ? 'left' : held(15) || ax > 0.6 ? 'right' : null;
    const prevWant = this.prevDir || null;
    if (want && (want !== prevWant || now >= this.repeatAt)) {
      dir = want;
      this.repeatAt = now + (want !== prevWant ? 380 : 140);
    }
    this.prevDir = want;
    if (dir) this.move(dir);
    if (fresh(0)) this.confirm();
    if (fresh(1)) this.back();
    if (fresh(2)) this.extra();
    if (this.screen === 'shelf') {
      if (fresh(4)) { this.api.cycleShelfSys(-1); this.go('shelf'); }
      if (fresh(5)) { this.api.cycleShelfSys(1); this.go('shelf'); }
      if (fresh(3)) { this.api.cycleShelfSort(); this.go('shelf'); }
    }
    this.prevPressed = new Set(pressed);
  }

  key(code) {
    const map = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' };
    if (map[code]) { this.move(map[code]); return true; }
    if (code === 'Enter' || code === 'NumpadEnter') { this.confirm(); return true; }
    if (code === 'Escape' || code === 'Backspace') { this.back(); return true; }
    if (this.screen === 'keypad' && /^(Digit|Numpad)\d$/.test(code)) { this.typeDigit(code.slice(-1)); return true; }
    return false;
  }

  move(dir) {
    if (this.screen === 'busy') return;
    if (this.screen === 'main') {
      const items = this.mainItems();
      if (dir === 'up') this.index = (this.index - 1 + items.length) % items.length;
      else if (dir === 'down') this.index = (this.index + 1) % items.length;
      else { const it = items[this.index]; if (it && it[dir]) it[dir](); }
    } else if (this.screen === 'list' || this.screen === 'confirm') {
      const n = this.list.items.length;
      if (dir === 'up') this.index = (this.index - 1 + n) % n;
      else if (dir === 'down') this.index = (this.index + 1) % n;
      else { const it = this.list.items[this.index]; if (it && it[dir]) it[dir](); return; }
    } else if (this.screen === 'shelf') {
      const n = this.shelf.length, cols = this.cols();
      if (!n) return;
      if (dir === 'left') this.index = Math.max(0, this.index - 1);
      if (dir === 'right') this.index = Math.min(n - 1, this.index + 1);
      if (dir === 'up') this.index = Math.max(0, this.index - cols);
      if (dir === 'down') this.index = Math.min(n - 1, this.index + cols);
    } else if (this.screen === 'keypad') {
      // grade 3×4: 1 2 3 / 4 5 6 / 7 8 9 / ⌫ 0 OK
      const r = Math.floor(this.index / 3), c = this.index % 3;
      if (dir === 'left') this.index = r * 3 + Math.max(0, c - 1);
      if (dir === 'right') this.index = r * 3 + Math.min(2, c + 1);
      if (dir === 'up') this.index = Math.max(0, r - 1) * 3 + c;
      if (dir === 'down') this.index = Math.min(3, r + 1) * 3 + c;
    } else return;
    this.render();
  }

  cols() {
    const grid = this.root.querySelector('.cm-shelf');
    if (!grid) return 4;
    const cs = getComputedStyle(grid).gridTemplateColumns.split(' ').filter(Boolean).length;
    return cs || 4;
  }

  confirm() {
    if (this.screen === 'main') { const it = this.mainItems()[this.index]; if (it) it.run(); }
    else if (this.screen === 'shelf') { const r = this.shelf[this.index]; if (r) { this.shelfIndex = this.index; this.openCart(r.id); } }
    else if (this.screen === 'list' || this.screen === 'confirm') { const it = this.list.items[this.index]; if (it) it.run(); }
    else if (this.screen === 'keypad') {
      const keys = ['1', '2', '3', '4', '5', '6', '7', '8', '9', 'del', '0', 'ok'];
      const k = keys[this.index];
      if (k === 'del') { this.digits = this.digits.slice(0, -1); this.render(); }
      else if (k === 'ok') this.submitCode();
      else this.typeDigit(k);
    } else if (this.screen === 'done') {
      const m = this.message;
      if (m && m.playId) { this.hide(); this.api.play(m.playId); } else this.go('main');
    } else if (this.screen === 'error') this.go(this.message && this.message.backTo || 'main');
  }

  back() {
    if (this.screen === 'busy') return;
    if (this.screen === 'main') this.hide();
    else if ((this.screen === 'list' || this.screen === 'confirm') && this.list.back) this.list.back();
    else this.go('main');
  }

  // ---------------- telas de lista (cartucho, versões, nuvem, confirmação) ----------------
  showList(list, screen = 'list') {
    this.list = list;
    this.screen = screen;
    this.index = 0;
    this.render();
  }

  async openCart(id) {
    const a = this.api;
    const c = await a.cartInfo(id);
    if (!c) { this.go('shelf'); return; }
    const toShelf = () => this.go('shelf', this.shelfIndex || 0);
    const items = [];
    if (c.resume) items.push({ label: 'Continuar de onde parou', hint: c.resumeHint, run: () => { this.hide(); a.resume(id); } });
    items.push({ label: c.current ? 'Voltar ao jogo' : 'Jogar do início', hint: c.saveHint, run: () => { this.hide(); if (!c.current) a.play(id); } });
    for (const st of c.states) items.push({ label: `Carregar estado ${st.slot}`, hint: st.hint, run: () => { this.hide(); a.loadSlot(id, st.slot); } });
    items.push({ label: 'Favorito', hint: c.fav ? '★ Sim' : '☆ Não', side: true, run: async () => { await a.toggleFav(id); this.openCart(id); }, left: async () => { await a.toggleFav(id); this.openCart(id); }, right: async () => { await a.toggleFav(id); this.openCart(id); } });
    if (c.current) items.push({ label: 'Usar a tela atual como capa', hint: '', run: () => { a.coverFromScreen(id); this.openCart(id); } });
    if (c.versions.length) items.push({ label: 'Versões anteriores do save', hint: `${c.versions.length} guardada(s) neste aparelho`, run: () => this.openVersions(id, c) });
    if (a.canTransfer()) items.push({ label: 'Enviar para outro aparelho', hint: 'Gera um código de 6 dígitos', run: () => this.sendId(id, c.name) });
    items.push({ label: 'Remover da estante', hint: 'Pede confirmação', run: () => this.showList({
      title: `Remover ${c.name}?`, sub: 'O save pode ficar guardado para quando você abrir a ROM de novo.', back: () => this.openCart(id),
      items: [
        { label: 'Remover e manter o save', hint: 'Recomendado', run: async () => { await a.remove(id, false); toShelf(); } },
        { label: 'Remover tudo', hint: 'ROM, save e estados (uma cópia do save fica no histórico)', run: async () => { await a.remove(id, true); toShelf(); } },
        { label: 'Cancelar', hint: '', run: () => this.openCart(id) },
      ] }, 'confirm') });
    items.push({ label: 'Voltar à estante', hint: '', run: toShelf });
    this.showList({ title: c.name, sub: c.sub, cover: c.cover, items, back: toShelf });
  }

  openSlots() {
    const a = this.api;
    const items = a.slotList().map(s => ({ label: `Espaço ${s.slot}`, hint: s.used ? s.hint : 'vazio · A salva aqui', img: s.thumb, run: () => {
      if (!s.used) { a.saveSlot(s.slot); this.hide(); return; }
      this.showList({ title: `Espaço ${s.slot}`, sub: s.hint, cover: s.thumb, back: () => this.openSlots(), items: [
        { label: 'Carregar este estado', hint: 'Volta a este ponto', run: () => { this.hide(); a.loadSlotHere(s.slot); } },
        { label: 'Salvar por cima', hint: 'Troca pelo momento atual', run: () => { this.hide(); a.saveSlot(s.slot); } },
        { label: 'Voltar', hint: '', run: () => this.openSlots() },
      ] }, 'confirm');
    } }));
    items.push({ label: 'Voltar', hint: '', run: () => this.go('main') });
    this.showList({ title: 'Estados salvos', sub: 'A: carregar ou salvar · os espaços vazios salvam direto', items, back: () => this.go('main'), grid: true });
  }

  openVersions(id, c) {
    const a = this.api;
    this.showList({
      title: 'Versões do save', sub: `${c.name} · escolha uma para restaurar (a atual também fica guardada)`, back: () => this.openCart(id),
      items: c.versions.map(v => ({ label: v.label, hint: v.hint, run: () => this.showList({
        title: 'Restaurar esta versão?', sub: `${v.label} · ${v.hint}`, back: () => this.openVersions(id, c),
        items: [
          { label: 'Restaurar', hint: c.current ? 'O jogo reinicia com este save' : '', run: async () => { await a.restoreVersion(id, v.hid); this.openCart(id); } },
          { label: 'Cancelar', hint: '', run: () => this.openVersions(id, c) },
        ] }, 'confirm') })).concat([{ label: 'Voltar', hint: '', run: () => this.openCart(id) }]),
    });
  }

  openCloud() {
    const a = this.api;
    const on = a.cloudEnabled();
    this.showList({
      title: 'Nuvem e backup', sub: a.cloudStatus(), back: () => this.go('main'),
      items: [
        { label: 'Sincronizar agora', hint: on ? 'Envia e recebe saves' : 'Ligue a nuvem primeiro', run: async () => {
          if (!on) return;
          this.busy('Sincronizando com a nuvem…', 0.5);
          const r = await a.cloudSync();
          this.message = { title: r ? 'Nuvem sincronizada' : 'Não deu certo', body: r ? `${r.up} save(s) enviado(s), ${r.down} recebido(s).` : a.cloudStatus() };
          this.screen = 'done'; this.render();
        } },
        { label: 'Saves na nuvem', hint: on ? 'Ligado' : 'Desligado', side: true, run: () => { a.cloudToggle(); this.openCloud(); }, left: () => { a.cloudToggle(); this.openCloud(); }, right: () => { a.cloudToggle(); this.openCloud(); } },
        { label: 'Baixar backup (só saves e estados)', hint: 'Arquivo .indigo', run: () => { this.hide(); a.backup(false); } },
        { label: 'Voltar', hint: '', run: () => this.go('main') },
      ],
    });
  }

  async sendId(id, name) {
    this.busy(`Enviando ${name}…`, 0);
    try {
      const code = await this.api.send(id, (f) => this.busy(`Enviando ${name}…`, f));
      this.message = { title: 'Pronto para receber', code, body: 'No outro aparelho, abra o Índigo, escolha Receber por código e digite o código. Vale por 1 hora.' };
      this.screen = 'done'; this.render();
    } catch (e) { this.fail(e.message, 'shelf'); }
  }

  async sendShelf() {
    this.busy('Enviando a estante…', 0);
    try {
      const r = await this.api.sendShelf((f) => this.busy('Enviando a estante…', f));
      this.message = { title: 'Pronto para receber', code: r.code, body: `Estante inteira: ${r.counts.roms} jogo(s), ${r.counts.saves} save(s), ${r.counts.states} estado(s). No outro aparelho, escolha Receber por código e digite o código. Vale por 1 hora.` };
      this.screen = 'done'; this.render();
    } catch (e) { this.fail(e.message, 'main'); }
  }

  // X na estante: enviar o cartucho selecionado para outro aparelho
  async extra() {
    if (this.screen !== 'shelf' || !this.api.canTransfer()) return;
    const r = this.shelf[this.index];
    if (!r) return;
    this.sendId(r.id, r.name);
  }

  typeDigit(d) {
    if (this.digits.length < 6) this.digits += d;
    if (this.digits.length === 6) this.index = 11; // pula para OK
    this.render();
  }

  async submitCode() {
    if (this.digits.length !== 6) { this.render('O código tem 6 números.'); return; }
    const code = this.digits;
    this.busy('Recebendo…', 0);
    try {
      const res = await this.api.receive(code, (f) => this.busy('Recebendo…', f));
      this.message = { title: `${res.name} chegou`, body: res.note, playId: res.id, action: res.id ? 'A  Jogar agora   ·   B  Voltar' : '<b>A</b> ok' };
      this.screen = 'done'; this.render();
    } catch (e) { this.fail(e.message, 'keypad'); }
  }

  busy(title, frac) {
    this.screen = 'busy';
    this.message = { title, frac };
    this.render();
  }
  fail(text, backTo) {
    this.message = { title: 'Não deu certo', body: text || 'Erro desconhecido.', backTo };
    this.screen = 'error';
    this.render();
  }

  // ---------------- desenho ----------------
  render(note) {
    if (!this.open) return;
    let html = '';
    const legend = (t) => `<div class="cm-legend">${t}</div>`;
    if (this.screen === 'main') {
      const items = this.mainItems();
      if (this.index >= items.length) this.index = items.length - 1;
      html = `<div class="cm-title">ÍNDIGO</div><ul class="cm-list">` + items.map((it, i) =>
        `<li data-i="${i}" class="${i === this.index ? 'cm-on' : ''}"><span>${esc(it.label)}</span><small>${it.side ? '<b class="cm-arrow" data-side="left">◀</b> ' : ''}${esc(it.hint || '')}${it.side ? ' <b class="cm-arrow" data-side="right">▶</b>' : ''}</small></li>`).join('') + `</ul>`
        + legend('<b>A</b> escolher · <b>B</b> fechar · <b>View + Menu</b> abre este menu durante o jogo');
    } else if (this.screen === 'shelf') {
      if (!this.shelf.length) {
        html = `<div class="cm-title">Estante</div><p class="cm-body">A estante deste aparelho está vazia. ${this.api.canTransfer() ? 'Use “Receber por código” para trazer jogos de outro aparelho.' : 'Abra uma ROM pela página.'}</p>` + legend('<b>B</b> voltar');
      } else {
        const v = this.api.shelfView();
        html = `<div class="cm-title">Estante</div><div class="cm-filter"><b>LB</b> ${v.groups.map(g => `<span class="${g.on ? 'on' : ''}">${esc(g.label)}</span>`).join('')} <b>RB</b><span class="cm-sort"><b>Y</b> ${esc(v.sortLabel)}</span></div>`
          + (this.shelf.length ? `<div class="cm-shelf">` + this.shelf.map((r, i) =>
          `<div data-i="${i}" class="cm-cart ${i === this.index ? 'cm-on' : ''}">${r.cover ? `<img src="${imgSrc(r.cover)}" alt="">` : `<span>${esc(r.name.slice(0, 2).toUpperCase())}</span>`}<b>${esc(r.name)}</b><small>${esc(r.tag || '')}${r.resume ? ' · pode continuar' : ''}</small></div>`).join('') + `</div>`
          : `<p class="cm-body">Nenhum cartucho deste console.</p>`)
          + legend(`<b>A</b> abrir · ${this.api.canTransfer() ? '<b>X</b> enviar · ' : ''}<b>LB</b>/<b>RB</b> console · <b>Y</b> ordem · <b>B</b> voltar`);
      }
    } else if (this.screen === 'list' || this.screen === 'confirm') {
      const L = this.list;
      if (this.index >= L.items.length) this.index = L.items.length - 1;
      html = `<div class="cm-head">${L.cover ? `<img src="${imgSrc(L.cover)}" alt="">` : ''}<div><div class="cm-title">${esc(L.title)}</div>${L.sub ? `<p class="cm-body">${esc(L.sub)}</p>` : ''}</div></div>`
        + `<ul class="cm-list${this.screen === 'confirm' ? ' cm-confirm' : ''}">` + L.items.map((it, i) =>
        `<li data-i="${i}" class="${i === this.index ? 'cm-on' : ''}${it.img !== undefined ? ' cm-thumb' : ''}">${it.img !== undefined ? (it.img ? `<img src="${imgSrc(it.img)}" alt="">` : '<i></i>') : ''}<span>${esc(it.label)}</span><small>${it.side ? '<b class="cm-arrow" data-side="left">◀</b> ' : ''}${esc(it.hint || '')}${it.side ? ' <b class="cm-arrow" data-side="right">▶</b>' : ''}</small></li>`).join('') + `</ul>`
        + legend('<b>A</b> escolher · <b>B</b> voltar');
    } else if (this.screen === 'keypad') {
      const keys = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '⌫', '0', 'OK'];
      const shown = (this.digits + '······').slice(0, 6);
      html = `<div class="cm-title">Receber por código</div><div class="cm-code">${shown.slice(0, 3)} ${shown.slice(3)}</div>`
        + (note ? `<p class="cm-body warn">${esc(note)}</p>` : '')
        + `<div class="cm-keys">` + keys.map((k, i) => `<div data-i="${i}" class="${i === this.index ? 'cm-on' : ''}">${k}</div>`).join('') + `</div>`
        + legend('<b>A</b> digitar · <b>B</b> voltar');
    } else if (this.screen === 'busy') {
      const pct = Math.round((this.message.frac || 0) * 100);
      html = `<div class="cm-title">${esc(this.message.title)}</div><div class="bar"><span style="width:${pct}%"></span></div><p class="cm-body">${pct}%</p>`;
    } else if (this.screen === 'done') {
      const m = this.message;
      html = `<div class="cm-title">${esc(m.title)}</div>` + (m.code ? `<div class="cm-code big">${m.code.slice(0, 3)} ${m.code.slice(3)}</div>` : '')
        + `<p class="cm-body">${esc(m.body || '')}</p>` + legend(m.action || '<b>A</b> ok');
    } else if (this.screen === 'error') {
      html = `<div class="cm-title">${esc(this.message.title)}</div><p class="cm-body warn">${esc(this.message.body)}</p>` + legend('<b>A</b> voltar');
    }
    const nav = (this.screen === 'main' ? '' : '<button class="cm-nav" type="button" data-cm="back" aria-label="Voltar">‹</button>')
      + '<button class="cm-nav cm-close" type="button" data-cm="close" aria-label="Fechar">✕</button>';
    this.root.innerHTML = `<div class="cm-panel" role="dialog" aria-label="Menu do Índigo">${this.screen === 'busy' ? '' : nav}${html}</div>`;
    const sel = this.root.querySelector('.cm-on');
    if (sel && sel.scrollIntoView) sel.scrollIntoView({ block: 'nearest' });
  }
}
