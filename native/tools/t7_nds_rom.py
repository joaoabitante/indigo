#!/usr/bin/env python3
# Ferramentas do teste do Nintendo DS (Índigo):
#   t7_nds_rom.py dados <saida.h>                 texto de teste + versões LZ77, RLE e Huffman (formato da BIOS do DS)
#   t7_nds_rom.py rom <arm9.bin> <arm7.bin> <saida.nds> [org9] [org7]
# O cartucho gerado não tem o logotipo da Nintendo (o Índigo reconhece pelo .nds e pelo cabeçalho coerente).
import struct
import sys

TEXTO = (b"Indigo DS: teste de descompressao. " * 6) + b"a" * 40 + bytes(range(32)) + b"fim."


def lz77(d):
    out = bytearray(struct.pack("<I", (len(d) << 8) | 0x10))
    i = 0
    while i < len(d):
        flags_pos = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= len(d):
                break
            best_len, best_disp = 0, 0
            for disp in range(2, min(i, 4096) + 1):  # deslocamento >= 2: seguro para VRAM
                n = 0
                while n < 18 and i + n < len(d) and d[i + n] == d[i + n - disp]:
                    n += 1
                if n > best_len:
                    best_len, best_disp = n, disp
            if best_len >= 3:
                flags |= 0x80 >> bit
                v = ((best_len - 3) << 12) | (best_disp - 1)
                out += bytes([v >> 8, v & 0xFF])
                i += best_len
            else:
                out.append(d[i])
                i += 1
        out[flags_pos] = flags
    while len(out) % 4:
        out.append(0)
    return bytes(out)


def rle(d):
    out = bytearray(struct.pack("<I", (len(d) << 8) | 0x30))
    i = 0
    raw = bytearray()

    def flush():
        while raw:
            chunk = raw[:128]
            del raw[:128]
            out.append(len(chunk) - 1)
            out.extend(chunk)

    while i < len(d):
        n = 1
        while i + n < len(d) and d[i + n] == d[i] and n < 130:
            n += 1
        if n >= 3:
            flush()
            out.append(0x80 | (n - 3))
            out.append(d[i])
            i += n
        else:
            raw.append(d[i])
            i += 1
    flush()
    while len(out) % 4:
        out.append(0)
    return bytes(out)


def huffman(d):
    """Huffman de 8 bits no formato da BIOS: árvore em pares de nós (largura primeiro) e bits em palavras de 32."""
    import heapq
    freq = {}
    for b in d:
        freq[b] = freq.get(b, 0) + 1
    heap = [(n, i, b) for i, (b, n) in enumerate(sorted(freq.items()))]
    heapq.heapify(heap)
    k = len(heap)
    while len(heap) > 1:
        a, b2 = heapq.heappop(heap), heapq.heappop(heap)
        heapq.heappush(heap, (a[0] + b2[0], k, (a[2], b2[2])))
        k += 1
    root = heap[0][2]
    codes = {}

    def walk(n, c):
        if isinstance(n, tuple):
            walk(n[0], c + "0")
            walk(n[1], c + "1")
        else:
            codes[n] = c
    walk(root, "")
    # tabela: índice 0 = tamanho, 1 = raiz, depois pares de filhos na ordem em que aparecem
    table = [0, None]
    fila = [(root, 1)]
    while fila:
        n, pos = fila.pop(0)
        if not isinstance(n, tuple):
            table[pos] = n
            continue
        par = len(table)
        table += [None, None]
        off = (par - (pos & ~1) - 2) // 2
        assert 0 <= off < 64, "árvore larga demais"
        v = off
        if not isinstance(n[0], tuple):
            v |= 0x80
        if not isinstance(n[1], tuple):
            v |= 0x40
        table[pos] = v
        fila.append((n[0], par))
        fila.append((n[1], par + 1))
    while (len(table)) % 4:
        table.append(0)
    table[0] = len(table) // 2 - 1
    out = bytearray(struct.pack("<I", (len(d) << 8) | 0x28)) + bytes(table)
    bits = "".join(codes[b] for b in d)
    bits += "0" * (-len(bits) % 32)
    for i in range(0, len(bits), 32):
        out += struct.pack("<I", int(bits[i:i + 32], 2))
    return bytes(out)


def carr(name, b):
    return "static const u8 %s[] __attribute__((aligned(4))) = {%s};\n" % (name, ",".join(str(x) for x in b))


def crc16(data, crc=0xFFFF):
    val = [0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001]
    for b in data:
        crc ^= b
        for j in range(8):
            carry = crc & 1
            crc >>= 1
            if carry:
                crc ^= (val[j] << (7 - j)) & 0xFFFF
    return crc


def main():
    if sys.argv[1] == "dados":
        with open(sys.argv[2], "w") as f:
            f.write("// gerado pelo t7_nds_rom.py\n")
            f.write(carr("texto", TEXTO))
            f.write(carr("lz77", lz77(TEXTO)))
            f.write(carr("rle", rle(TEXTO)))
            f.write(carr("huff", huffman(TEXTO)))
        return
    arm9 = open(sys.argv[2], "rb").read()
    arm7 = open(sys.argv[3], "rb").read()
    org9 = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x02000000
    org7 = int(sys.argv[6], 0) if len(sys.argv) > 6 else 0x037F8000
    rom = bytearray(0x40000)
    off9 = 0x4000
    off7 = (off9 + len(arm9) + 0xFFF) & ~0xFFF
    assert off7 + len(arm7) <= 0x20000, "programas grandes demais"
    rom[off9:off9 + len(arm9)] = arm9
    rom[off7:off7 + len(arm7)] = arm7
    # dados para o teste de leitura do cartucho em 0x20000
    for i in range(0x1000):
        rom[0x20000 + i] = (i * 7 + 3) & 0xFF
    h = rom
    h[0:12] = b"INDIGO TESTE"
    h[12:16] = b"INDT"
    h[16:18] = b"01"
    h[0x14] = 0x01  # 256 KB
    struct.pack_into("<IIII", h, 0x20, off9, org9, org9, len(arm9))
    struct.pack_into("<IIII", h, 0x30, off7, org7, org7, len(arm7))
    struct.pack_into("<II", h, 0x60, 0x00586000, 0x001808F8)
    struct.pack_into("<II", h, 0x80, len(rom), 0x4000)
    struct.pack_into("<H", h, 0x15E, crc16(h[0:0x15E]))
    open(sys.argv[4], "wb").write(rom)


if __name__ == "__main__":
    main()
