#!/usr/bin/env python3
"""Completa a ROM de teste até 128 KB: os bancos 2 a 7 recebem padrões conhecidos (tiles e dados).
Com --codemasters, grava no cabeçalho o checksum que faz o Índigo escolher o mapeador Codemasters.
uso: mkbanks.py entrada.sms saida.sms [--codemasters]"""
import sys

src, dst = sys.argv[1], sys.argv[2]
codemasters = '--codemasters' in sys.argv
rom = bytearray(open(src, 'rb').read())
rom = rom[:0x8000] + bytes([0xFF]) * (0x8000 - len(rom[:0x8000]))
for bank in range(2, 8):
    data = bytearray(0x4000)
    seed = bank * 0x9E3779B1 & 0xFFFFFFFF
    for i in range(0x4000):
        seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
        data[i] = ((seed >> 16) ^ (i * bank)) & 0xFF
    rom += data
if codemasters:
    # cs + inv = 0 (mod 65536) em 0x7FE6..0x7FE9, como o detector de src/sms/sms.js espera
    cs = 0x1234
    rom[0x7FE6:0x7FE8] = cs.to_bytes(2, 'little')
    rom[0x7FE8:0x7FEA] = ((0x10000 - cs) & 0xFFFF).to_bytes(2, 'little')
open(dst, 'wb').write(rom)
print(dst, len(rom), 'bytes', 'codemasters' if codemasters else 'sega')
