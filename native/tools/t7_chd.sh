#!/usr/bin/env bash
# Prepara /root/indigo-env/roms/chd para o teste indigo-t7_chd (leitor de .chd).
#
# 1. Monta imagens sintéticas .cue/.bin com várias faixas: dados Mode 1 (com EDC/ECC de verdade, para o chdman
#    tirar a correção e o Índigo ter de refazê-la; alguns setores com ECC errada de propósito), Mode 2 (Form 1 e
#    Form 2), Mode 1/2048 e Mode 2/2336, áudio (senoides, silêncio e ruído), INDEX 00 dentro do arquivo, PREGAP e
#    POSTGAP fora dele, faixas com tamanhos que não são múltiplos de 4, um .bin por faixa e tudo num .bin só.
# 2. Converte cada uma com o chdman createcd (pacote mame-tools), nos codecs padrão (cdlz,cdzl,cdfl), em cada
#    codec de CD sozinho e combinados, nos de dados (zlib, lzma, huff, flac), sem compressão e com tamanhos de
#    hunk diferentes (2 quadros e 32 quadros: este passa de 64 KiB e usa o tamanho de 3 bytes nos codecs de CD).
# 3. Baixa imagens de CD livres (as de teste do libcdio), se houver rede, e converte também.
# Gera lista.txt com "arquivo.chd arquivo.cue" por linha, que o teste compara setor a setor.
set -euo pipefail
DEST=${1:-/root/indigo-env/roms/chd}
command -v chdman >/dev/null 2>&1 || {
  SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO=sudo
  $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq mame-tools >/dev/null
}
mkdir -p "$DEST"
cd "$DEST"
rm -f lista.txt

python3 - <<'EOF'
import math, random, struct

random.seed(1234)

# ---- EDC/ECC de setor de CD (como no CD de verdade)
EDC = []
for i in range(256):
    e = i
    for _ in range(8):
        e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
    EDC.append(e)
def edc(b):
    e = 0
    for x in b:
        e = (e >> 8) ^ EDC[(e ^ x) & 0xFF]
    return struct.pack('<I', e)
F = [0] * 256; B = [0] * 256
for i in range(256):
    j = (i << 1) ^ (0x11D if i & 0x80 else 0)
    F[i] = j & 0xFF; B[i ^ (j & 0xFF)] = i
def ecc_block(src, major_count, minor_count, major_mult, minor_inc):
    size = major_count * minor_count
    out = bytearray(major_count * 2)
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            t = src[index]
            index += minor_inc
            if index >= size: index -= size
            a ^= t; b ^= t; a = F[a]
        a = B[F[a] ^ b]
        out[major] = a; out[major + major_count] = a ^ b
    return out
def ecc(s, zero_header):
    s = bytearray(s)
    hdr = bytes(s[12:16])
    if zero_header: s[12:16] = b'\0\0\0\0'
    s[0x81C:0x8C8] = ecc_block(s[12:12 + 2064], 86, 24, 2, 86)
    s[0x8C8:0x930] = ecc_block(s[12:12 + 2236], 52, 43, 86, 88)
    s[12:16] = hdr
    return s

SYNC = b'\x00' + b'\xff' * 10 + b'\x00'
def bcd(v): return ((v // 10) << 4) | (v % 10)
def msf(lba):
    a = lba + 150
    return bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75)])

def payload(lba, n, kind):
    # dados variados: texto, zeros, aleatórios, padrões
    k = (lba * 7 + kind) % 5
    if k == 0: return bytes(n)
    if k == 1: return bytes(random.getrandbits(8) for _ in range(n))
    if k == 2: return (('setor %d do disco de teste do Indigo. ' % lba) * 200).encode()[:n]
    if k == 3: return bytes((lba + i * 3) & 0xFF for i in range(n))
    return bytes((i // 16 + lba) & 0xFF for i in range(n))

def mode1(lba, good=True):
    d = payload(lba, 2048, 1)
    s = bytearray(SYNC + msf(lba) + b'\x01' + d)
    s += edc(s[0:0x810]) + bytes(8)
    s += bytes(276)
    s = ecc(s, False)
    if not good: s[0x900] ^= 0x5A  # correção errada: o chdman tem de guardar o setor inteiro
    return bytes(s)

def mode2(lba):
    if lba % 3 == 2:  # Form 2
        sub = bytes([0, 0, 0x20, 0]) * 2
        d = payload(lba, 2324, 2)
        s = bytearray(SYNC + msf(lba) + b'\x02' + sub + d)
        s += edc(s[16:16 + 8 + 2324])
        return bytes(s)
    sub = bytes([0, 0, 0x08, 0]) * 2
    d = payload(lba, 2048, 3)
    s = bytearray(SYNC + msf(lba) + b'\x02' + sub + d)
    s += edc(s[16:16 + 8 + 2048])
    s += bytes(276)
    return bytes(ecc(s, True))

def audio(idx, n, style):
    out = bytearray()
    for k in range(n):
        if style == 'silencio':
            out += bytes(2352); continue
        for i in range(588):
            t = (idx * 1000 + k * 588 + i)
            if style == 'seno':
                l = int(9000 * math.sin(t * 2 * math.pi * 440 / 44100) + 3000 * math.sin(t * 0.0021))
                r = int(7000 * math.sin(t * 2 * math.pi * 523 / 44100 + 1))
            elif style == 'ruido':
                l = random.randint(-32768, 32767); r = random.randint(-32768, 32767)
            else:  # mistura de senoide com um pouco de ruído
                l = int(12000 * math.sin(t * 0.03)) + random.randint(-200, 200)
                r = l // 2 + random.randint(-50, 50)
            out += struct.pack('<hh', l, r)
    return bytes(out)

def audio_mix(idx, n):
    styles = ['seno', 'silencio', 'mistura', 'ruido', 'seno', 'mistura']
    out = bytearray()
    k = 0
    while k < n:
        m = min(n - k, 37)
        out += audio(idx + k, m, styles[(k // 37) % len(styles)])
        k += m
    return bytes(out)

def data_track(first_lba, n, kind):
    out = bytearray()
    for i in range(n):
        lba = first_lba + i
        if kind == 1: out += mode1(lba, good=(i % 17 != 5))
        else: out += mode2(lba)
    return bytes(out)

def msfs(n): return '%02d:%02d:%02d' % (n // 4500, n // 75 % 60, n % 75)
def write(name, b): open(name, 'wb').write(b)

# A) um .bin por faixa (estilo redump): Mode 1, áudio com INDEX 00, áudio com PREGAP, áudio, Mode 1 com INDEX 00
t1 = data_track(0, 1003, 1)
t2 = audio_mix(1, 150 + 777)
t3 = audio_mix(2, 301)
t4 = audio_mix(3, 75 + 46)
lba5 = 1003 + 927 + 75 + 301 + 121
t5 = data_track(lba5, 150 + 213, 1)
write('a_t1.bin', t1); write('a_t2.bin', t2); write('a_t3.bin', t3); write('a_t4.bin', t4); write('a_t5.bin', t5)
open('a.cue', 'w').write(
    'FILE "a_t1.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
    'FILE "a_t2.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n'
    'FILE "a_t3.bin" BINARY\n  TRACK 03 AUDIO\n    PREGAP 00:01:00\n    INDEX 01 00:00:00\n'
    'FILE "a_t4.bin" BINARY\n  TRACK 04 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:01:00\n'
    'FILE "a_t5.bin" BINARY\n  TRACK 05 MODE1/2352\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n')

# B) tudo num .bin só: Mode 2 (Form 1/2), áudio com INDEX 00 de 2 s, áudio com INDEX 00 curto, áudio
b1 = data_track(0, 523, 2)
b2 = audio_mix(10, 150 + 402)
b3 = audio_mix(11, 33 + 1)
b4 = audio_mix(12, 250)
write('b.bin', b1 + b2 + b3 + b4)
o2 = 523; o3 = o2 + 552; o4 = o3 + 34
open('b.cue', 'w').write(
    'FILE "b.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
    '  TRACK 02 AUDIO\n    INDEX 00 %s\n    INDEX 01 %s\n' % (msfs(o2), msfs(o2 + 150)) +
    '  TRACK 03 AUDIO\n    INDEX 00 %s\n    INDEX 01 %s\n' % (msfs(o3), msfs(o3 + 33)) +
    '  TRACK 04 AUDIO\n    INDEX 01 %s\n' % msfs(o4))

# C) setores "cozidos": Mode 1/2048 e Mode 2/2336, com POSTGAP depois da faixa de dados
c1 = b''.join(payload(i, 2048, 4) for i in range(611))
c2 = audio_mix(20, 150 + 299)
write('c_t1.bin', c1); write('c_t2.bin', c2)
open('c.cue', 'w').write(
    'FILE "c_t1.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n    POSTGAP 00:02:00\n'
    'FILE "c_t2.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n')
d1 = b''.join(payload(i, 2336, 5) for i in range(389))
write('d.bin', d1)
open('d.cue', 'w').write('FILE "d.bin" BINARY\n  TRACK 01 MODE2/2336\n    INDEX 01 00:00:00\n')

# E) faixa 1 com INDEX 00 (pregap de 2 s dentro do arquivo) e um disco minúsculo de 5 setores
e1 = data_track(0, 150 + 401, 1)
e2 = audio_mix(30, 89)
write('e.bin', e1 + e2)
open('e.cue', 'w').write(
    'FILE "e.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n'
    '  TRACK 02 AUDIO\n    INDEX 01 %s\n' % msfs(551))
write('f.bin', data_track(0, 5, 1))
open('f.cue', 'w').write('FILE "f.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n')

# G) só áudio (CD de música), com POSTGAP no meio
write('g_t1.bin', audio_mix(40, 402)); write('g_t2.bin', audio_mix(41, 150 + 199))
open('g.cue', 'w').write(
    'FILE "g_t1.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n    POSTGAP 00:00:10\n'
    'FILE "g_t2.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n')
EOF

conv() { # imagem(.cue ou .iso) sufixo opções...
  local src=$1 suf=$2; shift 2
  local out="${src%.*}_$suf.chd"
  chdman createcd -f -i "$src" -o "$out" "$@" >/dev/null 2>&1 || { echo "chdman falhou: $src $suf $*" >&2; return 1; }
  # o chdman 0.264 grava .chd de um hunk só que nem ele mesmo abre ("Decompression error"): fica de fora
  if ! chdman verify -i "$out" >/dev/null 2>&1; then echo "  (ignorado: o próprio chdman não abre $out)"; return 0; fi
  echo "$out $src" >> lista.txt
}
for c in a b c d e f g; do
  conv $c.cue padrao
  conv $c.cue cdlz -c cdlz
  conv $c.cue cdzl -c cdzl
  conv $c.cue cdfl -c cdfl
  conv $c.cue cdlz_cdfl -c cdlz,cdfl
  conv $c.cue cdzl_cdfl -c cdzl,cdfl
  conv $c.cue zlib -c zlib
  conv $c.cue lzma -c lzma
  conv $c.cue huff -c huff
  conv $c.cue flac -c flac
  conv $c.cue dados -c lzma,zlib,huff,flac
  conv $c.cue none -c none
  conv $c.cue hs2 -hs 4896
  conv $c.cue hs32 -hs 78336
  conv $c.cue hs32_cdzl_cdfl -hs 78336 -c cdzl,cdfl
done

# 3. imagens de CD livres: as de teste do libcdio (commit fixo): "cdda" (áudio, a Free Software Song),
#    "isofs-m1" (dados Mode 1 em ISO 9660) e "copying.iso" (.iso de 2048 bytes por setor). Só são baixadas para o
#    teste (não entram no repositório). Sem rede, o teste segue só com as sintéticas.
LIBCDIO=https://raw.githubusercontent.com/libcdio/libcdio/68ea374f83d5e63122168e4e182b15ba10e0d2eb/test/data
mkdir -p pub
for f in cdda.cue cdda.bin isofs-m1.cue isofs-m1.bin copying.iso; do
  [ -s "pub/$f" ] || curl -fsSL -o "pub/$f" "$LIBCDIO/$f" 2>/dev/null || { echo "  (sem pub/$f: não baixou)"; rm -f "pub/$f"; }
done
[ -s pub/cdda.bin ] && conv pub/cdda.cue chd
[ -s pub/cdda.bin ] && conv pub/cdda.cue cdfl -c cdfl
[ -s pub/isofs-m1.bin ] && conv pub/isofs-m1.cue chd
[ -s pub/copying.iso ] && conv pub/copying.iso chd
echo "t7_chd: $(wc -l < lista.txt) imagens .chd em $DEST"
