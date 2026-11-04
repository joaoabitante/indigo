#!/usr/bin/env python3
"""Monta o disco de teste do Dreamcast para o Índigo (tudo gerado aqui, sem material de terceiros).
  disco/teste.gdi: faixa 1 (dados, LBA 0), faixa 2 (áudio: senoide de 1 kHz, LBA 450) e faixa 3 (dados, LBA 45000)
  com o IP.BIN nos 16 primeiros setores e um ISO 9660 com 1ST_READ.BIN (o jogo de teste) e DADOS.BIN (padrão
  conhecido). Os endereços do ISO são absolutos (como num GD de verdade).
  disco/teste_cd.iso: o mesmo conteúdo como CD comum (.iso de 2048 bytes, IP.BIN no setor 0).
Uso: t7_dc_disco.py jogo.bin pasta_de_saida"""
import math, os, struct, sys

jogo = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
os.makedirs(out, exist_ok=True)


def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def dirrec(name, extent, size, isdir):
    n = name.encode()
    l = 33 + len(n)
    if l % 2:
        l += 1
    r = bytes([l, 0]) + both32(extent) + both32(size) + bytes([126, 1, 1, 0, 0, 0, 0]) + bytes([2 if isdir else 0, 0, 0]) + both16(1) + bytes([len(n)]) + n
    return r + b'\0' * (l - len(r))


def ip_bin():
    ip = bytearray(32768)
    def put(off, s, n):
        b = s.encode().ljust(n, b' ')
        ip[off:off + n] = b
    put(0x00, 'SEGA SEGAKATANA', 16)
    put(0x10, 'INDIGO TESTE', 16)
    put(0x20, '0000 GD-ROM1/1', 16)
    put(0x30, 'JUE', 8)
    put(0x38, '0799A10', 8)
    put(0x40, 'T-00000', 10)
    put(0x4A, 'V1.000', 6)
    put(0x50, '20261104', 16)
    put(0x60, '1ST_READ.BIN', 16)
    put(0x70, 'INDIGO', 16)
    put(0x80, 'INDIGO TESTE DC', 128)
    return bytes(ip)


def iso(base):
    """setores (2048 bytes) da faixa; base = LBA absoluto do primeiro setor"""
    dados = bytes((i * 7 + 3) & 0xFF for i in range(8192))
    files = [('1ST_READ.BIN;1', jogo), ('DADOS.BIN;1', dados)]
    sec = [ip_bin()[i * 2048:(i + 1) * 2048] for i in range(16)]
    pvd_i, term_i, ptl_i, ptm_i, root_i = 16, 17, 18, 19, 20
    nxt = 21
    ext = []
    for name, data in files:
        ext.append(nxt)
        nxt += (len(data) + 2047) // 2048
    total = nxt
    root = dirrec('\0', base + root_i, 2048, True) + dirrec('\1', base + root_i, 2048, True)
    for (name, data), e in zip(files, ext):
        root += dirrec(name, base + e, len(data), False)
    root = root.ljust(2048, b'\0')
    pt = bytes([1, 0]) + struct.pack('<I', base + root_i) + struct.pack('<H', 1) + b'\0\0'
    ptm = bytes([1, 0]) + struct.pack('>I', base + root_i) + struct.pack('>H', 1) + b'\0\0'
    pvd = bytearray(2048)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[8:40] = b'INDIGO'.ljust(32)
    pvd[40:72] = b'INDIGO_TESTE_DC'.ljust(32)
    pvd[80:88] = both32(base + total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(2048)
    pvd[132:140] = both32(len(pt))
    pvd[140:144] = struct.pack('<I', base + ptl_i)
    pvd[148:152] = struct.pack('>I', base + ptm_i)
    pvd[156:190] = dirrec('\0', base + root_i, 2048, True)
    pvd[881] = 1
    term = bytearray(2048)
    term[0] = 255
    term[1:6] = b'CD001'
    term[6] = 1
    sec += [bytes(pvd), bytes(term), pt.ljust(2048, b'\0'), ptm.ljust(2048, b'\0'), root]
    for name, data in files:
        n = (len(data) + 2047) // 2048
        d = data.ljust(n * 2048, b'\0')
        sec += [d[i * 2048:(i + 1) * 2048] for i in range(n)]
    return sec


def bcd(v):
    return (v // 10) << 4 | (v % 10)


def raw(lba, data):
    a = lba + 150
    hdr = b'\0' + b'\xff' * 10 + b'\0' + bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), 1])
    return hdr + data + b'\0' * 288


with open(os.path.join(out, 'track01.bin'), 'wb') as f:
    t1 = [b'\0' * 2048] * 16
    pvd = bytearray(2048)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    t1 += [bytes(pvd)] + [b'\0' * 2048] * 283
    for i, s in enumerate(t1):
        f.write(raw(i, s))
with open(os.path.join(out, 'track02.raw'), 'wb') as f:
    for i in range(300 * 588):
        v = int(12000 * math.sin(2 * math.pi * 1000 * i / 44100))
        f.write(struct.pack('<hh', v, v))
with open(os.path.join(out, 'track03.bin'), 'wb') as f:
    for i, s in enumerate(iso(45000)):
        f.write(raw(45000 + i, s))
with open(os.path.join(out, 'teste.gdi'), 'w') as f:
    f.write('3\n1 0 4 2352 track01.bin 0\n2 450 0 2352 track02.raw 0\n3 45000 4 2352 track03.bin 0\n')
with open(os.path.join(out, 'teste_cd.iso'), 'wb') as f:
    for s in iso(0):
        f.write(s)
print('disco de teste pronto em', out)
