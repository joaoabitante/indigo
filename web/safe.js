// Ajudas de segurança para montar HTML: texto escapado e imagens só de fontes conhecidas.
// Capas e miniaturas podem vir de um backup .indigo ou de uma estante transferida, então nunca entram cruas no HTML.
export const esc = (t) => String(t).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

const IMG_DATA = /^data:image\/(png|jpeg|webp|gif);base64,[A-Za-z0-9+/]+={0,2}$/;

// a própria imagem se for uma data: URL de imagem; senão null
export const safeImg = (u) => (typeof u === 'string' && u.length < 8_000_000 && IMG_DATA.test(u)) ? u : null;

// valor pronto para src="..." (vazio se a imagem não for aceita)
export const imgSrc = (u) => { const s = safeImg(u); return s ? esc(s) : ''; };
