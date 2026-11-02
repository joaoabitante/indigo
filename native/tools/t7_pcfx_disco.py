#!/usr/bin/env python3
"""Monta o CD de teste do PC-FX do Índigo (.cue + 2 .bin), só com material gerado aqui.
  faixa 1 (MODE1/2352): setor 0 "PC-FX:Hu_CD-ROM", setor 1 com o cabeçalho de partida (título, setor e
           quantidade do programa, endereço de carga e de partida), programa a partir do setor 2 e, no setor
           100, um arquivo de 64 KB pseudoaleatório (o programa soma as palavras);
  faixa 2 (áudio): 5 s de seno de 441 Hz em estéreo (o lado direito com metade da amplitude).
uso: t7_pcfx_disco.py programa.bin pasta_saida"""
import math
import os
import struct
import sys

prog = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
SECT = 2048


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def raw_mode1(lba, data):
    a = lba + 150
    m, s, f = a // 4500, a // 75 % 60, a % 75
    return b'\x00' + b'\xff' * 10 + b'\x00' + bytes([bcd(m), bcd(s), bcd(f), 1]) + data + bytes(288)


def data_file():
    x = 0x13579BDF
    b = bytearray()
    for _ in range(32 * 1024):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        b += struct.pack('<H', x & 0xFFFF)
    return bytes(b)


n = (len(prog) + SECT - 1) // SECT
assert 2 + n <= 100, 'programa grande demais'
secs = [bytes(SECT)] * 300
secs[0] = b'PC-FX:Hu_CD-ROM ' + bytes(SECT - 16)
hdr = bytearray(SECT)
hdr[0:0x20] = b'INDIGO PC-FX TESTE'.ljust(0x20, b'\0')
struct.pack_into('<IIII', hdr, 0x20, 2, n, 0x8000, 0x8000)
hdr[0x30:0x34] = b'IND\0'
hdr[0x34:0x50] = b'Indigo (teste livre)'.ljust(0x1C, b'\0')
secs[1] = bytes(hdr)
p = prog + bytes(n * SECT - len(prog))
for i in range(n):
    secs[2 + i] = p[i * SECT:(i + 1) * SECT]
d = data_file()
for i in range(32):
    secs[100 + i] = d[i * SECT:(i + 1) * SECT]
with open(os.path.join(out, 'pcfxteste_1.bin'), 'wb') as f:
    for i, s in enumerate(secs):
        f.write(raw_mode1(i, s))
with open(os.path.join(out, 'pcfxteste_2.bin'), 'wb') as f:
    for i in range(44100 * 5):
        v = math.sin(2 * math.pi * 441 * i / 44100)
        f.write(struct.pack('<hh', int(v * 16000), int(v * 8000)))
with open(os.path.join(out, 'pcfxteste.cue'), 'w') as f:
    f.write('FILE "pcfxteste_1.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
            'FILE "pcfxteste_2.bin" BINARY\n  TRACK 02 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 00:00:00\n')
print('disco: %d setores de programa' % n)
