// Gera as páginas com o código embutido:
//   dist/index.html   documento completo (Vercel, GitHub, abrir direto no navegador)
//   dist/indigo.html  fragmento para o visualizador de artefatos do Claude (ele acrescenta o cabeçalho)
import fs from 'node:fs';
import { build } from 'esbuild';
import crypto from 'node:crypto';
import path from 'node:path';
import { metaHtml, landingTop, downloadsSection as downloadsHtml, landingMid, landingEnd, landingFooter } from './landing.js';

// Site do Autista Empreende, o projeto do João que criou o Índigo. Vazio: o bloco aparece sem o botão.
const AUTISTA_EMPREENDE_URL = 'https://autistaempreende.com.br';
// Endereço público do site (og:url, og:image e JSON-LD). Troque aqui quando o domínio novo estiver no ar.
const SITE_URL = 'https://indigo.nexus';
// "O Índigo na mídia": uma entrada por matéria (só o nome do veículo em texto, sem logotipo). Lista vazia: a seção some.
const PRESS = [
  {
    outlet: 'Technobit',
    title: 'Inteligência artificial pode transformar a emulação de games e mudar o futuro dos consoles',
    author: 'Alan Leocadio',
    date: '30/10/2026',
    summary: 'A matéria conta que o João criou o próprio emulador com a ajuda de inteligência artificial e cita o Índigo como exemplo de como a IA pode transformar ideias em aplicativos funcionais. Fala também do canal Autista Empreende.',
    url: 'https://technobit.com.br/2026/10/30/inteligencia-artificial-pode-transformar-a-emulacao-de-games/',
  },
];

async function bundle(entry, define) {
  const r = await build({ entryPoints: [entry], bundle: true, minify: true, format: 'iife', write: false, target: 'es2020', define });
  return r.outputFiles[0].text.replace(/<\/script/gi, '<\\/script');
}
const head = (title) => `<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#11121c">
<meta name="robots" content="index, follow">
<style>:root{padding-top:env(safe-area-inset-top,0px);padding-bottom:env(safe-area-inset-bottom,0px)}body{margin:0}[hidden]{display:none!important}img{max-width:100%}</style>
`;
const full = (body) => head() + body.replace(/(<\/style>\s*)/, '$1</head>\n<body>\n') + '\n</body>\n</html>\n';

fs.mkdirSync('dist', { recursive: true });

// Página inicial: lê downloads/app.json, mede os arquivos, copia tudo para dist/downloads e monta os blocos
// (topo, consoles, recursos, downloads, código aberto, perguntas, Autista Empreende, rodapé) de tools/landing.js.
// Windows: só a versão portátil (.zip). O instalador .exe sem assinatura digital é barrado pelo navegador e
// pelo SmartScreen; o zip baixa normalmente e, desbloqueado nas Propriedades, o Índigo abre sem aviso.
function landing() {
  const empty = { meta: metaHtml({ siteUrl: SITE_URL, autista: AUTISTA_EMPREENDE_URL.trim() }), top: '<main id="conteudo" tabindex="-1">', downloads: '', mid: '', end: '', footer: '' };
  if (!fs.existsSync('downloads/app.json')) return empty;
  const app = JSON.parse(fs.readFileSync('downloads/app.json', 'utf8'));
  fs.rmSync('dist/downloads', { recursive: true, force: true });
  fs.mkdirSync('dist/downloads', { recursive: true });
  const file = (name) => {
    const src = path.join('downloads', name);
    fs.copyFileSync(src, path.join('dist/downloads', name));
    const mb = (fs.statSync(src).size / 1e6).toFixed(1).replace('.', ',');
    return { href: `downloads/${encodeURIComponent(name)}`, size: `${mb} MB` };
  };
  const d = { app, zip: file(app.portable), apk: file(app.android), autista: AUTISTA_EMPREENDE_URL.trim(), press: PRESS };
  return { meta: metaHtml({ ...d, siteUrl: SITE_URL }), top: landingTop(d), downloads: downloadsHtml(d), mid: landingMid(d), end: landingEnd(d), footer: landingFooter(d) };
}
// substituição com função: o código minificado tem '$' que String.replace interpretaria
// o núcleo (Web Worker) vai embutido como texto dentro da página: continua sendo um arquivo só
const workerJs = await bundle('web/emu-worker.js');
// identificador da versão: a página publicada compara com dist/version.txt e se recarrega quando há versão nova
const hash = crypto.createHash('sha256');
const walk = (d) => { for (const f of fs.readdirSync(d).sort()) { const p = path.join(d, f); if (fs.statSync(p).isDirectory()) walk(p); else hash.update(p).update(fs.readFileSync(p)); } };
walk('web'); walk('src'); hash.update(fs.readFileSync('tools/landing.js'));
const BUILD = hash.digest('hex').slice(0, 12);
const mainJs = await bundle('web/main.js', { __WORKER_SRC__: JSON.stringify(workerJs), __BUILD__: JSON.stringify(BUILD) });
const page = landing();
// imagem para pré-visualização em redes sociais e apps de mensagem (og:image)
if (fs.existsSync('web/og.png')) fs.copyFileSync('web/og.png', 'dist/og.png');
const landingCss = fs.readFileSync('web/landing.css', 'utf8');
const mainBody = fs.readFileSync('web/template.html', 'utf8')
  .replace('</style>', () => landingCss + '</style>')
  .replace('<!--META-->', () => page.meta)
  .replace('<!--LANDING_TOP-->', () => page.top)
  .replace('<!--DOWNLOADS-->', () => page.downloads)
  .replace('<!--LANDING_MID-->', () => page.mid)
  .replace('<!--LANDING_END-->', () => page.end)
  .replace('<!--LANDING_BOTTOM-->', () => page.footer)
  .replace('/*BUNDLE*/', () => mainJs);
fs.writeFileSync('dist/indigo.html', mainBody);
fs.writeFileSync('dist/index.html', full(mainBody));
fs.writeFileSync('dist/version.txt', BUILD);
// CHANGELOG.md a partir do registro mostrado na página
{
  const { CHANGELOG } = await import('../web/changelog.js');
  const md = ['# Registro de melhorias do Índigo', '', 'Gerado de `web/changelog.js` (o mesmo texto aparece no cartão “Registro” da página).', ''];
  for (const c of CHANGELOG) {
    md.push(`## ${c.date.split('-').reverse().join('/')} · ${c.title}`, '');
    for (const i of c.items) md.push(`- ${i}`);
    if (c.tests) md.push('', `**Testes:** ${c.tests}`);
    md.push('');
  }
  fs.writeFileSync('CHANGELOG.md', md.join('\n'));
}
fs.rmSync('dist/ds.html', { force: true });
fs.rmSync('dist/ejs', { recursive: true, force: true });
console.log('dist/index.html', (mainBody.length / 1024).toFixed(1), 'KB (núcleo no Worker:', (workerJs.length / 1024).toFixed(1), 'KB)');
