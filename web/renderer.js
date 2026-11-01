// Saída de vídeo em WebGL: leva a imagem do console (240×160 no GBA, 160×144 no GB/GBC) até a resolução
// nativa da tela (1080p, 1440p, 4K)
//
// Pipeline por quadro:
//   1. upload do quadro
//   2. pré-passo na resolução original: anti-cintilação (mescla com o quadro anterior) + cores do LCD original
//   3. filtro de ampliação (alguns usam um passo intermediário em 4×)
//
// Filtros:
//   xbr       HD: xBR nível 2 em escala livre, bordas redesenhadas com suavidade ajustável
//   remaster  HD Remaster: xBR 4× sem mistura + reamostragem bicúbica (visual liso de remasterização)
//   sharp     pixel original nítido (bilinear afiado: pixels retos em escala quebrada)
//   bicubic   bicúbico Catmull-Rom (suave, sem detectar bordas)
//   lcd       tela de LCD com subpixels RGB e grade entre os pixels
//   crt       TV de tubo: feixe gaussiano por linha + máscara de abertura
//   epx       EPX/Scale2x duas vezes (4×) + bilinear afiado

const VS = `
attribute vec2 a_pos;
attribute vec2 a_uv;
varying vec2 v_uv;
void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }`;

const HEAD = `
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_texSize;
uniform vec2 u_outScale;
uniform float u_param;
vec3 T(vec2 p) { return texture2D(u_tex, (p + 0.5) / u_texSize).rgb; }
`;

// ---------- pré-passo: anti-cintilação + cor do LCD ----------
// matriz de cor baseada no perfil "gba-color" de Pokefan531 (domínio público, libretro)
const FS_PRE = HEAD + `
uniform sampler2D u_prev;
uniform float u_blend;
uniform float u_lcdColor;
uniform mat3 u_lcdMat;
uniform float u_lcdGamma;
uniform float u_lcdLum;
void main() {
  vec3 c = texture2D(u_tex, v_uv).rgb;
  vec3 p = texture2D(u_prev, v_uv).rgb;
  c = mix(c, p, u_blend);
  if (u_lcdColor > 0.5) {
    vec3 lin = pow(c, vec3(u_lcdGamma)) * u_lcdLum;
    lin = clamp(u_lcdMat * lin, 0.0, 1.0);
    c = pow(lin, vec3(1.0 / 2.2));
  }
  gl_FragColor = vec4(c, 1.0);
}`;

const FS_SHARP = HEAD + `
void main() {
  vec2 texel = v_uv * u_texSize;
  vec2 pre = max(floor(u_outScale), vec2(1.0));
  vec2 base = floor(texel);
  vec2 d = fract(texel) - 0.5;
  vec2 r = 0.5 - 0.5 / pre;
  vec2 f = (d - clamp(d, -r, r)) * pre + 0.5;
  gl_FragColor = texture2D(u_tex, (base + f) / u_texSize);
}`;

// Catmull-Rom 4×4 (amostragem por texel, sem depender do filtro da textura)
const FS_BICUBIC = HEAD + `
vec4 w(float t) {
  float t2 = t * t, t3 = t2 * t;
  return vec4(-0.5*t3 + t2 - 0.5*t, 1.5*t3 - 2.5*t2 + 1.0, -1.5*t3 + 2.0*t2 + 0.5*t, 0.5*t3 - 0.5*t2);
}
void main() {
  vec2 p = v_uv * u_texSize - 0.5;
  vec2 b = floor(p), f = p - b;
  vec4 wx = w(f.x), wy = w(f.y);
  vec3 acc = vec3(0.0);
  for (int j = 0; j < 4; j++) {
    vec3 row = T(b + vec2(-1.0, float(j) - 1.0)) * wx.x + T(b + vec2(0.0, float(j) - 1.0)) * wx.y
             + T(b + vec2(1.0, float(j) - 1.0)) * wx.z + T(b + vec2(2.0, float(j) - 1.0)) * wx.w;
    acc += row * (j == 0 ? wy.x : j == 1 ? wy.y : j == 2 ? wy.z : wy.w);
  }
  gl_FragColor = vec4(clamp(acc, 0.0, 1.0), 1.0);
}`;

// xBR nível 2 em escala livre (algoritmo de Hyllian). u_param = suavidade das bordas (0..1)
const FS_XBR = HEAD + `
const vec3 Y = vec3(14.352, 28.176, 5.472);
vec2 base; vec2 dir;
vec3 S(float x, float y) { return T(base + vec2(x * dir.x, y * dir.y)); }
float L(vec3 c) { return dot(c, Y); }
float df(float a, float b) { return abs(a - b); }
bool eq(float a, float b) { return df(a, b) < 15.0; }
void main() {
  vec2 texel = v_uv * u_texSize;
  vec2 fl = floor(texel);
  vec2 fp = texel - fl;
  base = fl;
  dir = vec2(fp.x >= 0.5 ? 1.0 : -1.0, fp.y >= 0.5 ? 1.0 : -1.0);
  vec2 f = vec2(dir.x > 0.0 ? fp.x : 1.0 - fp.x, dir.y > 0.0 ? fp.y : 1.0 - fp.y);

  vec3 cE = S(0.0, 0.0), cF = S(1.0, 0.0), cH = S(0.0, 1.0);
  float B = L(S(0.0, -1.0)), C = L(S(1.0, -1.0)), D = L(S(-1.0, 0.0)), E = L(cE), F = L(cF);
  float G = L(S(-1.0, 1.0)), H = L(cH), I = L(S(1.0, 1.0));
  float F4 = L(S(2.0, 0.0)), I4 = L(S(2.0, 1.0)), H5 = L(S(0.0, 2.0)), I5 = L(S(1.0, 2.0));

  bool r1 = E != F && E != H && (
      (!eq(F, B) && !eq(F, C)) || (!eq(H, D) && !eq(H, G)) ||
      (eq(E, I) && ((!eq(F, F4) && !eq(F, I4)) || (!eq(H, H5) && !eq(H, I5)))) ||
      eq(E, G) || eq(E, C));
  bool r2left = E != G && D != G;
  bool r2up = E != C && B != C;

  float d1 = df(E, C) + df(E, G) + df(I, H5) + df(I, F4) + 4.0 * df(H, F);
  float d2 = df(H, D) + df(H, I5) + df(F, I4) + df(F, B) + 4.0 * df(E, I);
  bool edr = d1 < d2 && r1;
  bool edrLeft = 2.0 * df(F, G) <= df(H, C) && r2left && edr;
  bool edrUp = df(F, G) >= 2.0 * df(H, C) && r2up && edr;

  // transição: de ~1 pixel de saída (u_param=0) até ~0,35 texel (u_param=1)
  float px = 0.5 / max(min(u_outScale.x, u_outScale.y), 1.0);
  float w = mix(px, 0.5, u_param * u_param);
  float a = edr ? smoothstep(-w, w, f.x + f.y - 1.5) : 0.0;
  if (edrLeft) a = max(a, smoothstep(-w, w, f.y + 0.5 * f.x - 1.0));
  if (edrUp) a = max(a, smoothstep(-w, w, 2.0 * f.x + f.y - 2.0));
  vec3 nc = df(E, F) <= df(E, H) ? cF : cH;
  gl_FragColor = vec4(mix(cE, nc, a), 1.0);
}`;

// LCD com subpixels RGB verticais e grade escura; u_param = intensidade da grade
const FS_LCD = HEAD + `
void main() {
  vec2 texel = v_uv * u_texSize;
  vec3 c = T(floor(texel));
  vec2 f = fract(texel);
  float s = min(u_outScale.x, u_outScale.y);
  // subpixels só fazem sentido com espaço suficiente (>= 3 pixels por texel)
  float sub = clamp((s - 2.0) / 3.0, 0.0, 1.0) * u_param;
  vec3 mask = vec3(
    smoothstep(0.0, 0.08, f.x) * (1.0 - smoothstep(0.30, 0.40, f.x)),
    smoothstep(0.27, 0.37, f.x) * (1.0 - smoothstep(0.63, 0.73, f.x)),
    smoothstep(0.60, 0.70, f.x) * (1.0 - smoothstep(0.92, 1.0, f.x)));
  // normalizado: o branco continua branco, só ganha a textura dos subpixels
  mask = mix(vec3(1.0), mask * 1.5 + 0.5, sub * 0.35);
  float gy = smoothstep(0.0, 0.12, f.y) * smoothstep(0.0, 0.12, 1.0 - f.y);
  float gx = smoothstep(0.0, 0.06, f.x) * smoothstep(0.0, 0.06, 1.0 - f.x);
  float grid = mix(1.0, mix(0.6, 1.0, gx * gy), u_param);
  gl_FragColor = vec4(clamp(c * mask * grid * (1.0 + 0.18 * u_param), 0.0, 1.0), 1.0);
}`;

// CRT: cada linha do GBA é um feixe gaussiano; máscara de abertura leve; u_param = intensidade
const FS_CRT = HEAD + `
vec3 lin(vec3 c) { return c * c; }
void main() {
  vec2 p = v_uv * u_texSize;
  float y = p.y - 0.5;
  float yb = floor(y);
  float fy = y - yb;
  // horizontal: bilinear afiado; vertical: duas linhas vizinhas com pesos gaussianos
  float x = p.x - 0.5;
  float xb = floor(x), fx = x - xb;
  fx = smoothstep(0.2, 0.8, fx);
  vec3 l0 = mix(lin(T(vec2(xb, yb))), lin(T(vec2(xb + 1.0, yb))), fx);
  vec3 l1 = mix(lin(T(vec2(xb, yb + 1.0))), lin(T(vec2(xb + 1.0, yb + 1.0))), fx);
  float width0 = mix(0.42, 0.28, dot(l0, vec3(0.33)));
  float width1 = mix(0.42, 0.28, dot(l1, vec3(0.33)));
  float w0 = exp(-pow(fy / width0, 2.0) * 0.5);
  float w1 = exp(-pow((1.0 - fy) / width1, 2.0) * 0.5);
  vec3 c = l0 * w0 + l1 * w1;
  float mx = mod(gl_FragCoord.x, 3.0);
  vec3 mask = mx < 1.0 ? vec3(1.0, 0.75, 0.75) : mx < 2.0 ? vec3(0.75, 1.0, 0.75) : vec3(0.75, 0.75, 1.0);
  c *= mix(vec3(1.0), mask, 0.6 * u_param);
  c = sqrt(c * mix(1.0, 1.55, u_param));
  gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}`;

const FS_EPX = HEAD + `
bool eq(vec3 a, vec3 b) { return dot(abs(a - b), vec3(1.0)) < 0.02; }
void main() {
  vec2 o = floor(v_uv * u_texSize * 2.0);
  vec2 s = floor(o / 2.0);
  vec2 q = o - s * 2.0;
  vec3 P = T(s), A = T(s + vec2(0.0, -1.0)), B = T(s + vec2(1.0, 0.0)), C = T(s + vec2(-1.0, 0.0)), D = T(s + vec2(0.0, 1.0));
  vec3 outc = P;
  if (q.x < 0.5 && q.y < 0.5) { if (eq(C, A) && !eq(C, D) && !eq(A, B)) outc = A; }
  else if (q.x > 0.5 && q.y < 0.5) { if (eq(A, B) && !eq(A, C) && !eq(B, D)) outc = B; }
  else if (q.x < 0.5 && q.y > 0.5) { if (eq(D, C) && !eq(D, B) && !eq(C, A)) outc = C; }
  else { if (eq(B, D) && !eq(B, A) && !eq(D, C)) outc = D; }
  gl_FragColor = vec4(outc, 1.0);
}`;

export const FILTERS = {
  xbr: 'HD (xBR)', remaster: 'HD Remaster', sharp: 'Pixel original', bicubic: 'Bicúbico',
  lcd: 'Tela LCD', crt: 'TV de tubo', epx: 'EPX 4×',
};

function compile(gl, type, src) {
  const s = gl.createShader(type);
  gl.shaderSource(s, src);
  gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
  return s;
}
function program(gl, fs) {
  const p = gl.createProgram();
  gl.attachShader(p, compile(gl, gl.VERTEX_SHADER, VS));
  gl.attachShader(p, compile(gl, gl.FRAGMENT_SHADER, fs));
  gl.bindAttribLocation(p, 0, 'a_pos');
  gl.bindAttribLocation(p, 1, 'a_uv');
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
  const u = (n) => gl.getUniformLocation(p, n);
  return { p, tex: u('u_tex'), size: u('u_texSize'), scale: u('u_outScale'), param: u('u_param'),
    prev: u('u_prev'), blend: u('u_blend'), lcdColor: u('u_lcdColor'), lcdMat: u('u_lcdMat'), lcdGamma: u('u_lcdGamma'), lcdLum: u('u_lcdLum') };
}

// perfis de cor das telas originais (Pokefan531, domínio público, libretro): matriz em colunas
const LCD_PROFILES = {
  gba: { mat: [0.82, 0.125, 0.195, 0.24, 0.665, 0.075, -0.06, 0.21, 0.73], gamma: 2.45, lum: 0.94 },
  gbc: { mat: [0.78824, 0.025, 0.12039, 0.12157, 0.72941, 0.12157, 0.0, 0.275, 0.82], gamma: 2.2, lum: 0.91 },
};

export class Renderer {
  constructor(canvas) {
    this.canvas = canvas;
    this.filter = 'xbr';
    this.fit = 'fill';      // 'fill' (ocupa a caixa), 'integer', 'aspect', 'stretch'
    this.params = { xbr: 0.25, lcd: 0.7, crt: 0.7 };
    this.lcdColor = false;
    this.frameBlend = false;
    this.lastRect = { w: 240, h: 160, scale: 1 };
    this.profile = 'gba';
    this.sw = 240; this.sh = 160;   // tamanho da imagem do console
    const gl = canvas.getContext('webgl', { alpha: false, antialias: false, preserveDrawingBuffer: false })
      || canvas.getContext('experimental-webgl');
    this.gl = gl;
    if (!gl) { this.ctx2d = canvas.getContext('2d'); this.img = this.ctx2d.createImageData(240, 160); return; }
    this.progs = {
      pre: program(gl, FS_PRE), sharp: program(gl, FS_SHARP), bicubic: program(gl, FS_BICUBIC),
      xbr: program(gl, FS_XBR), lcd: program(gl, FS_LCD), crt: program(gl, FS_CRT), epx: program(gl, FS_EPX),
    };
    this.buf = gl.createBuffer();
    this.allocate();
  }

  // texturas no tamanho da imagem do console (e passos intermediários em 2× e 4×)
  allocate() {
    const gl = this.gl, w = this.sw, h = this.sh;
    for (const t of [this.srcA, this.srcB, this.pre, this.mid, this.big]) if (t) gl.deleteTexture(t);
    for (const f of [this.preFb, this.midFb, this.bigFb]) if (f) gl.deleteFramebuffer(f);
    this.srcA = this.makeTex(w, h); this.srcB = this.makeTex(w, h);
    this.cur = this.srcA; this.prev = this.srcB;
    this.pre = this.makeTex(w, h); this.preFb = this.makeFb(this.pre);
    this.mid = this.makeTex(w * 2, h * 2); this.midFb = this.makeFb(this.mid);
    this.big = this.makeTex(w * 4, h * 4); this.bigFb = this.makeFb(this.big);
    this.hasFrame = false;
  }

  setSize(w, h) {
    if (w === this.sw && h === this.sh) return;
    this.sw = w; this.sh = h;
    if (!this.gl) { this.img = this.ctx2d.createImageData(w, h); return; }
    this.allocate();
  }

  makeTex(w, h) {
    const gl = this.gl, t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    this.texFilter(t, false);
    return t;
  }
  texFilter(t, linear) {
    const gl = this.gl, m = linear ? gl.LINEAR : gl.NEAREST;
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, m);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, m);
  }
  makeFb(t) {
    const gl = this.gl, fb = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return fb;
  }

  resize() {
    const c = this.canvas;
    const dpr = window.devicePixelRatio || 1;
    if (!this.gl) { if (c.width !== this.sw || c.height !== this.sh) { c.width = this.sw; c.height = this.sh; } return; }
    const w = Math.max(1, Math.round(c.clientWidth * dpr)), h = Math.max(1, Math.round(c.clientHeight * dpr));
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  }

  setQuad(x0, y0, x1, y1, flipV) {
    const gl = this.gl;
    // offscreen: v=0 embaixo (linha 0 da textura = linha 0 da imagem); tela: v=0 em cima
    const vb = flipV ? 1 : 0, vt = flipV ? 0 : 1;
    const data = new Float32Array([x0, y0, 0, vb, x1, y0, 1, vb, x0, y1, 0, vt, x1, y1, 1, vt]);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buf);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.STREAM_DRAW);
    gl.enableVertexAttribArray(0); gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 16, 0);
    gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 16, 8);
  }

  run(prog, tex, tw, th, outW, outH, param) {
    const gl = this.gl;
    gl.useProgram(prog.p);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.uniform1i(prog.tex, 0);
    gl.uniform2f(prog.size, tw, th);
    if (prog.scale) gl.uniform2f(prog.scale, outW / tw, outH / th);
    if (prog.param) gl.uniform1f(prog.param, param || 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }

  offscreen(fb, w, h) {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.viewport(0, 0, w, h);
    this.setQuad(-1, -1, 1, 1, false);
  }

  targetRect(W, H) {
    if (this.fit === 'stretch' || this.fit === 'fill') return { x: 0, y: 0, w: W, h: H };
    let s = Math.min(W / this.sw, H / this.sh);
    if (this.fit === 'integer' && s >= 1) s = Math.floor(s);
    const w = Math.round(this.sw * s), h = Math.round(this.sh * s);
    return { x: Math.floor((W - w) / 2), y: Math.floor((H - h) / 2), w, h };
  }

  // envia um quadro novo (chamar uma vez por quadro emulado)
  upload(fb32, w = this.sw, h = this.sh) {
    this.setSize(w, h);
    if (!this.gl) { new Uint32Array(this.img.data.buffer).set(fb32.subarray(0, w * h)); return; }
    const gl = this.gl;
    const t = this.prev; this.prev = this.cur; this.cur = t;
    const px = new Uint8Array(fb32.buffer, fb32.byteOffset, w * h * 4);
    gl.bindTexture(gl.TEXTURE_2D, this.cur);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    if (!this.hasFrame) {
      gl.bindTexture(gl.TEXTURE_2D, this.prev);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
      this.hasFrame = true;
    }
  }

  draw(fb32, w, h) {
    if (fb32) this.upload(fb32, w, h);
    this.resize();
    if (!this.gl) { this.ctx2d.putImageData(this.img, 0, 0); return; }
    const gl = this.gl, P = this.progs, c = this.canvas;
    const W = c.width, H = c.height;
    const r = this.targetRect(W, H);
    this.lastRect = { w: r.w, h: r.h, scale: r.w / this.sw };
    const sw = this.sw, sh = this.sh;

    // 1) pré-passo
    let src = this.cur;
    if (this.frameBlend || this.lcdColor) {
      this.offscreen(this.preFb, sw, sh);
      const pp = P.pre;
      gl.useProgram(pp.p);
      gl.activeTexture(gl.TEXTURE1); this.texFilter(this.prev, false); gl.bindTexture(gl.TEXTURE_2D, this.prev);
      gl.uniform1i(pp.prev, 1);
      gl.uniform1f(pp.blend, this.frameBlend ? 0.5 : 0.0);
      gl.uniform1f(pp.lcdColor, this.lcdColor ? 1 : 0);
      const prof = LCD_PROFILES[this.profile] || LCD_PROFILES.gba;
      gl.uniformMatrix3fv(pp.lcdMat, false, prof.mat);
      gl.uniform1f(pp.lcdGamma, prof.gamma); gl.uniform1f(pp.lcdLum, prof.lum);
      gl.activeTexture(gl.TEXTURE0);
      this.texFilter(this.cur, false);
      this.run(pp, this.cur, sw, sh, sw, sh, 0);
      src = this.pre;
    }

    // 2) passos intermediários
    let tex = src, tw = sw, th = sh, prog, param = 0, linear = false;
    switch (this.filter) {
      case 'remaster':
        this.texFilter(src, false);
        // xBR sem mistura em 4× → bicúbico até a tela
        this.offscreen(this.bigFb, sw * 4, sh * 4);
        this.run(P.xbr, src, sw, sh, sw * 4, sh * 4, 0);
        tex = this.big; tw = sw * 4; th = sh * 4; prog = P.bicubic; break;
      case 'epx':
        this.texFilter(src, false);
        this.offscreen(this.midFb, sw * 2, sh * 2); this.run(P.epx, src, sw, sh, sw * 2, sh * 2, 0);
        this.texFilter(this.mid, false);
        this.offscreen(this.bigFb, sw * 4, sh * 4); this.run(P.epx, this.mid, sw * 2, sh * 2, sw * 4, sh * 4, 0);
        tex = this.big; tw = sw * 4; th = sh * 4; prog = P.sharp; linear = true; break;
      case 'sharp': prog = P.sharp; linear = true; break;
      case 'bicubic': prog = P.bicubic; break;
      case 'lcd': prog = P.lcd; param = this.params.lcd; break;
      case 'crt': prog = P.crt; param = this.params.crt; break;
      default: prog = P.xbr; param = this.params.xbr; break;
    }
    this.texFilter(tex, linear);

    // 3) saída na tela
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, W, H);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const x0 = (r.x / W) * 2 - 1, x1 = ((r.x + r.w) / W) * 2 - 1;
    const y1 = 1 - (r.y / H) * 2, y0 = 1 - ((r.y + r.h) / H) * 2;
    this.setQuad(x0, y0, x1, y1, true);
    this.run(prog, tex, tw, th, r.w, r.h, param);
  }
}
