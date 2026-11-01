#!/usr/bin/env python3
"""Gera a ROM de teste de save do Neo Geo Pocket (montada à mão em código de máquina TLCS-900H).
Grava na flash do cartucho pelos dois caminhos que os jogos usam:
  1. chamada da BIOS VECT_FLASHWRITE (SWI 1 com W3 = 6): 256 bytes da RAM para o deslocamento 0x70000;
  2. sequência de comandos direto na flash: programa 0x271000 = 0x12 e 0x278010 = 0x34, depois apaga o
     setor de 8 KB em 0x278000 (o segundo byte volta a 0xFF, o primeiro fica).
O save .ngf resultante tem de sair igual no núcleo JavaScript e no C++.
uso: mkflashtest.py saida.ngp"""
import sys

rom = bytearray([0xFF]) * 0x1000
rom[0:28] = b'COPYRIGHT BY SNK CORPORATION'
rom[0x1C:0x1F] = (0x200040).to_bytes(3, 'little')  # endereço de partida
rom[0x20:0x22] = (0x1234).to_bytes(2, 'little')    # catálogo
rom[0x22] = 0x00
rom[0x23] = 0x10                                   # Neo Geo Pocket Color
rom[0x24:0x30] = b'INDIGOFLASH '

def wr24(addr, value):  # LD (nnn),#8 : prefixo F2 (destino, endereço de 24 bits) + op 00
    return bytes([0xF2]) + addr.to_bytes(3, 'little') + bytes([0x00, value])

def flash_cmd(*seq):  # sequência de desbloqueio AA/55 nos endereços 5555/2AAA do chip
    out = b''
    for a, v in seq:
        out += wr24(0x200000 + a, v)
    return out

code = b''
code += bytes([0xC7, 0x31, 0x03, 0x06])                    # LD W3,6      (VECT_FLASHWRITE)
code += bytes([0xC7, 0x30, 0x03, 0x00])                    # LD A3,0      (banco 0)
code += bytes([0xD7, 0x34, 0x03, 0x01, 0x00])              # LD BC3,1     (1 bloco de 256 bytes)
code += bytes([0xE7, 0x3C, 0x03, 0x00, 0x40, 0x00, 0x00])  # LD XHL3,4000 (origem: RAM)
code += bytes([0xE7, 0x38, 0x03, 0x00, 0x00, 0x07, 0x00])  # LD XDE3,70000 (destino na flash)
code += bytes([0xF9])                                      # SWI 1
code += flash_cmd((0x5555, 0xAA), (0x2AAA, 0x55), (0x5555, 0xA0), (0x71000, 0x12))   # programa um byte
code += flash_cmd((0x5555, 0xAA), (0x2AAA, 0x55), (0x5555, 0xA0), (0x78010, 0x34))   # programa outro
code += flash_cmd((0x5555, 0xAA), (0x2AAA, 0x55), (0x5555, 0x80),
                  (0x5555, 0xAA), (0x2AAA, 0x55), (0x78000, 0x30))                    # apaga o setor de 8 KB
code += bytes([0x68, 0xFE])                                # JR $ (fica parado)
rom[0x40:0x40 + len(code)] = code
open(sys.argv[1], 'wb').write(rom)
print(sys.argv[1], len(code), 'bytes de código')
