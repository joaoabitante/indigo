// PIN da nuvem e das transferências (o site é público; só quem sabe o PIN usa a nuvem). Guardado neste aparelho.
const KEY = 'indigo-pin';
export const pin = {
  get() { try { return localStorage.getItem(KEY) || ''; } catch { return ''; } },
  set(v) { try { localStorage.setItem(KEY, v); } catch { /* sem armazenamento */ } },
  clear() { try { localStorage.removeItem(KEY); } catch { /* ignorado */ } },
};
export class PinError extends Error { constructor(msg) { super(msg || 'Digite o PIN da nuvem do Índigo.'); this.pin = true; } }
export const pinHeaders = (h = {}) => { const p = pin.get(); return p ? Object.assign({}, h, { 'x-indigo-pin': p }) : h; };

// pede o PIN e confere no servidor; devolve true se ficou certo
export async function askPin(reason) {
  const v = window.prompt(`${reason ? reason + '\n\n' : ''}Digite o PIN da nuvem do Índigo (o mesmo em todos os seus aparelhos):`, '');
  if (v === null || !v.trim()) { askPin.last = 'cancel'; return false; }
  askPin.last = 'wrong';
  pin.set(v.trim());
  try {
    const r = await fetch('/api/transfer?action=auth', { headers: pinHeaders(), cache: 'no-store', credentials: 'same-origin' });
    if (r.ok) { askPin.last = 'ok'; return true; }
  } catch { /* sem rede */ }
  pin.clear();
  return false;
}

// fetch com o PIN; se o servidor recusar e for uma ação do usuário, pede o PIN e tenta mais uma vez
export async function pinFetch(url, init = {}, interactive = false) {
  const go = () => fetch(url, Object.assign({}, init, { headers: pinHeaders(init.headers || {}) }));
  let r = await go();
  if (r.status === 401) {
    if (!interactive) throw new PinError();
    const ok = await askPin(pin.get() ? 'PIN incorreto.' : '');
    if (!ok) throw new PinError(askPin.last === 'wrong' ? 'PIN incorreto.' : 'Sem o PIN não dá para usar a nuvem.');
    r = await go();
    if (r.status === 401) throw new PinError('PIN incorreto.');
  }
  return r;
}
