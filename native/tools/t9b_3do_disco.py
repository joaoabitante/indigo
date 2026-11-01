#!/usr/bin/env python3
# Etapa 9b (3DO): dados do homebrew de teste e o disco no sistema de arquivos Opera, escritos para o Índigo.
#   t9b_3do_disco.py dados SAIDA.inc            -> tabelas do homebrew (bola, PLUT, degradê, cel compactada, seno)
#   t9b_3do_disco.py disco PROGRAMA.bin PASTA   -> PASTA/homebrew.iso, homebrew.bin + homebrew.cue (Mode 1/2352)
# Formato Opera (documentação pública do 3DO): setor 0 = rótulo do volume (tipo 1, cinco bytes 0x5A, versão 1,
# comentário, identificador "CD-ROM", tamanho e quantidade de blocos, diretório raiz com a lista de cópias);
# diretório = blocos de 2048 bytes (próximo, anterior, flags, fim, início) com entradas de 72 bytes + cópias.
import math
import os
import struct
import sys


def be32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def dados(saida):
    out = ["@ gerado por t9b_3do_disco.py (não editar)"]

    def words(nome, ws):
        out.append(".balign 4")
        out.append(nome + ":")
        for i in range(0, len(ws), 8):
            out.append("  .word " + ", ".join("0x%08X" % (w & 0xFFFFFFFF) for w in ws[i:i + 8]))

    # bola 16x16, 8 bits com PLUT (índice 0 = transparente), linhas de 4 palavras (WOFFSET = 2)
    px = []
    for y in range(16):
        for x in range(16):
            dx, dy = x - 7.5, y - 7.5
            d = math.sqrt(dx * dx + dy * dy)
            if d >= 7.8:
                px.append(0)
            else:
                luz = math.sqrt((x - 5) ** 2 + (y - 5) ** 2)
                px.append(max(1, min(31, int(31 - luz * 2.4))))
    ws = []
    for y in range(16):
        row = px[y * 16:(y + 1) * 16]
        for k in range(0, 16, 4):
            ws.append(row[k] << 24 | row[k + 1] << 16 | row[k + 2] << 8 | row[k + 3])
    words("bola_px", ws)
    # PLUT da bola: do vermelho escuro ao amarelo
    plut = [0] + [((min(31, 8 + i)) << 10) | ((i * 31 // 31) << 5) | (i // 4) for i in range(1, 32)]
    words("bola_plut", [plut[i] << 16 | plut[i + 1] for i in range(0, 32, 2)])
    # degradê 16x16 de 16 bits (sem PLUT), 8 palavras por linha (WOFFSET = 6)
    g = []
    for y in range(16):
        for x in range(0, 16, 2):
            a = (x * 2) << 10 | (y * 2) << 5 | (31 - x) | 0
            b = ((x + 1) * 2) << 10 | (y * 2) << 5 | (31 - x - 1)
            g.append(a << 16 | b)
    words("degrade_px", g)
    # cel compactada de 4 bits: 24 linhas de listras com buracos transparentes (literal, transparente, repetição)
    bits = []

    def put(v, n):
        for i in range(n - 1, -1, -1):
            bits.append((v >> i) & 1)
    linhas = []
    for y in range(24):
        bits = []
        put(0, 8)  # deslocamento (preenchido depois)
        put(3, 2); put(5, 6); put(1 + (y % 15), 4)   # repetição de 6
        put(2, 2); put(3, 6)                          # 4 transparentes
        put(1, 2); put(3, 6)                          # literal de 4
        for k in range(4):
            put(1 + ((y + k * 3) % 15), 4)
        put(3, 2); put(7, 6); put(15 - (y % 15), 4)  # repetição de 8
        put(0, 2); put(0, 6)
        while len(bits) % 32:
            bits.append(0)
        nw = len(bits) // 32
        off = nw - 2
        for i in range(8):
            bits[i] = (off >> (7 - i)) & 1
        linhas.append(bits)
    ws = []
    for bl in linhas:
        for w in range(len(bl) // 32):
            v = 0
            for b in bl[w * 32:(w + 1) * 32]:
                v = v << 1 | b
            ws.append(v)
    words("listras_px", ws)
    lp = [0] + [(i * 2) << 10 | (31 - i * 2) << 5 | 16 for i in range(1, 16)]
    words("listras_plut", [lp[i] << 16 | lp[i + 1] for i in range(0, 16, 2)])
    # seno em 12.20 vezes 3 (64 passos por volta)
    words("seno", [int(round(math.sin(2 * math.pi * i / 64) * 3 * (1 << 20))) for i in range(64)])
    with open(saida, "w") as f:
        f.write("\n".join(out) + "\n")


def disco(prog, pasta):
    programa = open(prog, "rb").read()
    leia = ("Disco de teste do Indigo (3DO): homebrew proprio, licenca GPL-3.0.\n").encode()
    blocos_prog = (len(programa) + 2047) // 2048
    bloco_prog = 2
    bloco_leia = bloco_prog + blocos_prog
    total = bloco_leia + 1 + 16
    img = bytearray(total * 2048)
    # rótulo do volume
    lab = bytearray(2048)
    lab[0] = 1
    lab[1:6] = b"\x5A" * 5
    lab[6] = 1
    lab[8:8 + 16] = b"INDIGO HOMEBREW".ljust(16, b"\0")
    lab[40:46] = b"CD-ROM"
    lab[72:76] = be32(0x12345678)
    lab[76:80] = be32(2048)
    lab[80:84] = be32(total)
    lab[84:88] = be32(1)
    lab[88:92] = be32(1)      # blocos do diretório raiz
    lab[92:96] = be32(2048)
    lab[96:100] = be32(0)     # última cópia (uma só)
    lab[100:104] = be32(1)    # bloco da cópia 0 do diretório raiz
    img[0:2048] = lab
    # diretório raiz
    d = bytearray(2048)

    def entrada(nome, bloco, nbytes, tipo, ultimo):
        e = bytearray(72)
        flags = 2 | (0x80000000 if ultimo else 0)
        e[0:4] = be32(flags)
        e[4:8] = be32(bloco)
        e[8:12] = tipo
        e[12:16] = be32(2048)
        e[16:20] = be32(nbytes)
        e[20:24] = be32((nbytes + 2047) // 2048)
        e[32:32 + len(nome)] = nome
        e[64:68] = be32(0)
        e[68:72] = be32(bloco)
        return e
    ents = entrada(b"LaunchMe", bloco_prog, len(programa), b"*app", False) + \
        entrada(b"LEIAME.TXT", bloco_leia, len(leia), b"text", True)
    d[0:4] = be32(0xFFFFFFFF)
    d[4:8] = be32(0xFFFFFFFF)
    d[8:12] = be32(0)
    d[12:16] = be32(20 + len(ents))
    d[16:20] = be32(20)
    d[20:20 + len(ents)] = ents
    img[2048:4096] = d
    img[bloco_prog * 2048:bloco_prog * 2048 + len(programa)] = programa
    img[bloco_leia * 2048:bloco_leia * 2048 + len(leia)] = leia
    os.makedirs(pasta, exist_ok=True)
    with open(os.path.join(pasta, "homebrew.iso"), "wb") as f:
        f.write(img)
    with open(os.path.join(pasta, "homebrew.bin"), "wb") as f:
        for lba in range(total):
            a = lba + 150
            bcd = lambda v: (v // 10) << 4 | v % 10
            cab = b"\x00" + b"\xFF" * 10 + b"\x00" + bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), 1])
            f.write(cab + img[lba * 2048:(lba + 1) * 2048] + bytes(288))
    with open(os.path.join(pasta, "homebrew.cue"), "w") as f:
        f.write('FILE "homebrew.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n')


if __name__ == "__main__":
    if sys.argv[1] == "dados":
        dados(sys.argv[2])
    else:
        disco(sys.argv[2], sys.argv[3])
