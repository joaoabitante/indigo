// PIN das rotas de nuvem e transferência. Com o site público, só quem sabe o PIN lê ou grava saves e cartuchos.
// O PIN fica na variável de ambiente INDIGO_PIN do Vercel; o aparelho envia no cabeçalho x-indigo-pin.
import { createHash, timingSafeEqual } from 'node:crypto';

export const pinConfigured = () => !!process.env.INDIGO_PIN;

const digest = (s) => createHash('sha256').update(String(s)).digest();

// devolve null se liberado, ou a resposta 401 (com uma pausa para atrasar tentativas em série).
// Sem INDIGO_PIN configurado a rota fica fechada (503): um deploy sem a variável nunca abre a nuvem ao mundo.
export async function requirePin(request) {
  const pin = process.env.INDIGO_PIN;
  if (!pin) return new Response(JSON.stringify({ error: 'Nuvem do Índigo sem PIN configurado no Vercel.', pin: true }), {
    status: 503, headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' },
  });
  const got = request.headers.get('x-indigo-pin') || '';
  if (got && timingSafeEqual(digest(got), digest(pin))) return null;
  await new Promise(r => setTimeout(r, 900));
  return new Response(JSON.stringify({ error: got ? 'PIN incorreto.' : 'Digite o PIN da nuvem do Índigo.', pin: true }), {
    status: 401, headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' },
  });
}
