#!/usr/bin/env python3
# Monta um cartucho .z64 de teste para o Índigo: cabeçalho (com o código "ED" do cabeçalho homebrew, que diz o
# tipo de save), sem código de partida (o Índigo faz a partida em HLE) e o programa em 0x1000, carregado em
# 0x80000400. Uso: t7_n64_rom.py programa.bin saida.z64 "TITULO" [save: 0 nenhum, 1 EEPROM 4K, 3 SRAM, 5 Flash]
import struct
import sys

prog = open(sys.argv[1], 'rb').read()
titulo = (sys.argv[3] if len(sys.argv) > 3 else 'INDIGO TESTE').encode('ascii')[:20].ljust(20, b' ')
save = int(sys.argv[4]) if len(sys.argv) > 4 else 1
cab = bytearray(0x40)
struct.pack_into('>IIII', cab, 0, 0x80371240, 0x0000000F, 0x80000400, 0x0000144C)
cab[0x20:0x34] = titulo
cab[0x3B:0x3F] = b'NEDE'
cab[0x3F] = (save & 15) << 4
rom = bytes(cab) + bytes(0x1000 - 0x40) + prog
rom += bytes((-len(rom)) % 4)
open(sys.argv[2], 'wb').write(rom)
