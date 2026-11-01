#!/usr/bin/env python3
# Gera os arquivos da BIOS de teste do Sega CD e o disco sintético (Índigo):
#   t7_segacd_fonte.h  fonte 8x8 de 1 bit (0-9, A-Z, ':', '-', '.')
#   t7_segacd_seno.c   tabela de seno em Q14 (256 passos por volta)
#   teste.cue + teste1.bin (dados, Mode 1/2352) + teste2.bin e teste3.bin (áudio, com 2 s de pregap no arquivo)
# Uso: t7_segacd_gera.py pasta
import math
import os
import struct
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

# ---------------------------------------------------------------- fonte (5x7 em células de 8x8)
GLIFOS = {
    '0': ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    '1': ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    '2': ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    '3': ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    '4': ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    '5': ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    '6': ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    '7': ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    '8': ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    '9': ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    'A': ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    'B': ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    'C': ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    'D': ["11100", "10010", "10001", "10001", "10001", "10010", "11100"],
    'E': ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    'F': ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    'G': ["01110", "10001", "10000", "10111", "10001", "10001", "01111"],
    'H': ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    'I': ["01110", "00100", "00100", "00100", "00100", "00100", "01110"],
    'J': ["00111", "00010", "00010", "00010", "00010", "10010", "01100"],
    'K': ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    'L': ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    'M': ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    'N': ["10001", "10001", "11001", "10101", "10011", "10001", "10001"],
    'O': ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    'P': ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    'Q': ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    'R': ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    'S': ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    'T': ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    'U': ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    'V': ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    'W': ["10001", "10001", "10001", "10101", "10101", "10101", "01010"],
    'X': ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    'Y': ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    'Z': ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
    ':': ["00000", "01100", "01100", "00000", "01100", "01100", "00000"],
    '-': ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    '.': ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
}
ordem = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-."
linhas = []
for c in ordem:
    g = GLIFOS[c]
    for y in range(8):
        b = int(g[y], 2) << 2 if y < 7 else 0
        linhas.append(b)
with open(os.path.join(out, "t7_segacd_fonte.h"), "w") as f:
    f.write("/* gerado por t7_segacd_gera.py */\nstatic const u8 fonte[%d] = {\n" % len(linhas))
    for i in range(0, len(linhas), 16):
        f.write("  " + ", ".join("0x%02X" % v for v in linhas[i:i + 16]) + ",\n")
    f.write("};\n")

# ---------------------------------------------------------------- seno
with open(os.path.join(out, "t7_segacd_seno.c"), "w") as f:
    f.write("/* gerado por t7_segacd_gera.py */\nconst short sintab[256] = {\n")
    v = [int(round(math.sin(i * 2 * math.pi / 256) * 16384)) for i in range(256)]
    v = [min(16383, max(-16383, x)) for x in v]
    for i in range(0, 256, 16):
        f.write("  " + ", ".join(str(x) for x in v[i:i + 16]) + ",\n")
    f.write("};\n")

# ---------------------------------------------------------------- disco
def bcd(n):
    return ((n // 10) << 4) | (n % 10)

SYNC = bytes([0x00] + [0xFF] * 10 + [0x00])
DADOS = 200  # setores da faixa de dados (o mínimo do BIOS são 150)

def setor_dados(lba, user):
    a = lba + 150
    hdr = bytes([bcd(a // 4500), bcd((a // 75) % 60), bcd(a % 75), 1])
    return SYNC + hdr + user + bytes(288)  # EDC/ECC não conferidos

with open(os.path.join(out, "teste1.bin"), "wb") as f:
    for lba in range(DADOS):
        if lba == 0:
            u = bytearray(2048)
            u[0:16] = b"SEGADISCSYSTEM  "
            u[0x10:0x1B] = b"INDIGOTESTE"
            u[0x100:0x110] = b"SEGA MEGA DRIVE "
            u[0x110:0x120] = b"(C)INDIGO 2026  "
            u[0x120:0x150] = b"INDIGO DISCO DE TESTE".ljust(48)
            u[0x150:0x180] = b"INDIGO DISCO DE TESTE".ljust(48)
            u[0x180:0x18E] = b"GM 00000000-00"
            u[0x190:0x1A0] = b"J".ljust(16)
            u[0x1F0:0x200] = b"U".ljust(16)
            user = bytes(u)
        else:
            user = b"".join(struct.pack(">H", (((lba << 10) | k) ^ 0x5A5A) & 0xFFFF) for k in range(1024))
        f.write(setor_dados(lba, user))

def faixa_audio(nome, segundos, fn):
    with open(os.path.join(out, nome), "wb") as f:
        f.write(bytes(150 * 2352))  # pregap de 2 s (silêncio) no arquivo
        n = segundos * 44100
        n = (n + 587) // 588 * 588
        buf = bytearray()
        for i in range(n):
            l, r = fn(i)
            buf += struct.pack("<hh", l, r)
        f.write(buf)

faixa_audio("teste2.bin", 8, lambda i: (int(12000 * math.sin(2 * math.pi * 441 * i / 44100)),
                                       int(8000 * math.sin(2 * math.pi * 882 * i / 44100))))
faixa_audio("teste3.bin", 3, lambda i: ((6000 if (i // 73) % 2 else -6000), (4000 if (i // 50) % 2 else -4000)))

with open(os.path.join(out, "teste.cue"), "w") as f:
    f.write('FILE "teste1.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n')
    f.write('FILE "teste2.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n')
    f.write('FILE "teste3.bin" BINARY\n  TRACK 03 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n')
print("disco e fontes gerados em", out)
