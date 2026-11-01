#!/usr/bin/env python3
# Disco sintético do teste do PC Engine CD (t7_pcecd): teste.cue + teste.bin (um arquivo, 4 faixas, 800 setores)
#   faixa 1 (LBA 0-299, Mode 1/2352): dados em padrão; "PC Engine CD-ROM SYSTEM" no setor 1 (byte 32);
#            setores 100-115 = 32 KB de um seno de 500 Hz codificado em ADPCM (MSM5205, 16 kHz)
#   faixa 2 (LBA 300-449, áudio): amostras constantes L = $1234, R = $5678
#   faixa 3 (LBA 450-749, áudio): tom de 440 Hz à esquerda e 660 Hz à direita
#   faixa 4 (LBA 750-799, Mode 1/2352): dados em padrão
# O padrão (o t7_pcecd.cpp calcula o mesmo): byte i do setor n = (n*31 + i + (i>>8)*7 + (n>>8)) & 255
# Uso: t7_pcecd_disco.py pasta
import math, os, struct, sys

dest = sys.argv[1]
os.makedirs(dest, exist_ok=True)

def bcd(v): return ((v // 10) << 4) | (v % 10)

def pattern(n):
    return bytes(((n * 31 + i + (i >> 8) * 7 + (n >> 8)) & 255) for i in range(2048))

# ---- ADPCM (o mesmo decodificador do núcleo, com saturação em 0..4095)
steps = []
for n in range(49):
    s = int(math.floor(16.0 * (1.1 ** n) + 1e-9))
    steps.append([s // 8 + (s if k & 4 else 0) + (s // 2 if k & 2 else 0) + (s // 4 if k & 1 else 0) for k in range(8)])
shift = [-1, -1, -1, -1, 2, 4, 6, 8]

def adpcm_encode(samples):
    out, mag, nib = 2048, 0, []
    for t in samples:
        best = None
        for code in range(16):
            d = steps[mag][code & 7]
            v = min(4095, max(0, out + (-d if code & 8 else d)))
            e = abs(v - t)
            if best is None or e < best[0]: best = (e, code, v)
        _, code, out = best
        mag = min(48, max(0, mag + shift[code & 7]))
        nib.append(code)
    return bytes((nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2))

sine = [2048 + int(round(1500 * math.sin(2 * math.pi * 500 * i / 16000))) for i in range(65536)]
adpcm = adpcm_encode(sine)
assert len(adpcm) == 32768

def data_sector(lba):
    d = bytearray(pattern(lba))
    if lba == 1: d[32:32 + 23] = b"PC Engine CD-ROM SYSTEM"
    if 100 <= lba < 116: d[:] = adpcm[(lba - 100) * 2048:(lba - 99) * 2048]
    a = lba + 150
    hdr = bytes([0] + [0xFF] * 10 + [0, bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), 1])
    return hdr + bytes(d) + bytes(288)

with open(os.path.join(dest, "teste.bin"), "wb") as f:
    for lba in range(0, 300): f.write(data_sector(lba))
    const = struct.pack("<hh", 0x1234, 0x5678) * 588
    for lba in range(300, 450): f.write(const)
    k = 0
    for lba in range(450, 750):
        b = bytearray()
        for _ in range(588):
            l = int(round(12000 * math.sin(2 * math.pi * 440 * k / 44100)))
            r = int(round(8000 * math.sin(2 * math.pi * 660 * k / 44100)))
            b += struct.pack("<hh", l, r)
            k += 1
        f.write(b)
    for lba in range(750, 800): f.write(data_sector(lba))

with open(os.path.join(dest, "teste.cue"), "w") as f:
    f.write('FILE "teste.bin" BINARY\n'
            '  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
            '  TRACK 02 AUDIO\n    INDEX 01 00:04:00\n'
            '  TRACK 03 AUDIO\n    INDEX 01 00:06:00\n'
            '  TRACK 04 MODE1/2352\n    INDEX 01 00:10:00\n')
