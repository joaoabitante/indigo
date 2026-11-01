#!/usr/bin/env python3
# Disco de teste do PlayStation para o t9_ps1int, montado aqui (sem mkpsxiso):
#   faixa 1 Mode 2 (setores Form 1 de 2352 bytes): área do sistema com a licença "Sony Computer Entertainment Amer"
#   no setor 4 (a BIOS americana SCPH-1001 confere a região pelo GetID), ISO 9660 com SYSTEM.CNF, JOGO.EXE
#   (t9_ps1int_jogo.c), TEXTURA.BIN (64x64, 16 bits) e DADOS.BIN (160 setores conferidos pelo programa);
#   faixa 2 de áudio (CD-DA) com pregap de 2 s: seno de 440 Hz na esquerda e 660 Hz na direita, 20 s.
# Os setores levam EDC e ECC (P e Q) calculados como no padrão (o Beetle recusa setor de dados sem eles).
# Também grava sem_licenca.cue (o mesmo disco sem a licença, para o reconhecimento pelo ISO 9660).
# uso: t9_ps1int_disco.py jogo.exe pasta
import math, os, struct, sys

EXE, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def msf(lba):
    a = lba + 150
    return bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75)])


EDC = []
for i in range(256):
    e = i
    for _ in range(8):
        e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
    EDC.append(e)
F_LUT, B_LUT = [0] * 256, [0] * 256
for i in range(256):
    j = ((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xFF
    F_LUT[i] = j
    B_LUT[i ^ j] = i


def edc(b):
    e = 0
    for x in b:
        e = (e >> 8) ^ EDC[(e ^ x) & 0xFF]
    return e


def ecc(s, major_count, minor_count, major_mult, minor_inc, dest):
    size = major_count * minor_count
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            t = s[0xC + index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= t
            b ^= t
            a = F_LUT[a]
        a = B_LUT[F_LUT[a] ^ b]
        s[dest + major] = a
        s[dest + major + major_count] = a ^ b


def sector(lba, data, submode=0x08):
    data = data + bytes(2048 - len(data))
    sub = bytes([0, 0, submode, 0]) * 2
    s = bytearray(SYNC + bytes(4) + sub + data + bytes(4 + 276))
    s[0x818:0x81C] = struct.pack('<I', edc(s[0x10:0x818]))
    ecc(s, 86, 24, 2, 86, 0x81C)  # P (o cabeçalho conta como zeros no Mode 2)
    ecc(s, 52, 43, 86, 88, 0x8C8)  # Q
    s[12:16] = msf(lba) + b'\x02'
    return bytes(s)


def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def dirrec(name, lba, size, isdir):
    n = name.encode()
    r = bytearray([0, 0]) + both32(lba) + both32(size) + bytes([126, 1, 1, 0, 0, 0, 0])
    r += bytes([2 if isdir else 0, 0, 0]) + both16(1) + bytes([len(n)]) + n
    if len(r) % 2:
        r += b'\x00'
    r[0] = len(r)
    return bytes(r)


def data_word(w):
    x = (w * 2654435761) & 0xFFFFFFFF
    return x ^ (x >> 13) ^ 0x1D1C0DE


def texture():
    out = bytearray()
    for y in range(64):
        for x in range(64):
            px = ((x >> 1) & 31) | (((y >> 1) & 31) << 5) | (0x7C00 if ((x ^ y) >> 3) & 1 else 0x2000)
            out += struct.pack('<H', px)
    return bytes(out)


exe = open(EXE, 'rb').read()
cnf = b'BOOT = cdrom:\\JOGO.EXE;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n'
files = [('SYSTEM.CNF;1', cnf), ('JOGO.EXE;1', exe), ('TEXTURA.BIN;1', texture()),
         ('DADOS.BIN;1', b''.join(struct.pack('<I', data_word(w)) for w in range(160 * 512)))]
lba = 21
layout = []
for name, d in files:
    n = (len(d) + 2047) // 2048
    layout.append((name, lba, d, n))
    lba += n
total = lba + 150  # pós-gap da faixa de dados

root = bytearray(dirrec('\x00', 20, 2048, True) + dirrec('\x01', 20, 2048, True))
for name, l, d, n in sorted(layout):
    root += dirrec(name, l, len(d), False)
ptable = bytes([1, 0]) + struct.pack('<I', 20) + struct.pack('<H', 1) + b'\x00\x00'
mtable = bytes([1, 0]) + struct.pack('>I', 20) + struct.pack('>H', 1) + b'\x00\x00'


def pvd():
    p = bytearray(2048)
    p[0] = 1; p[1:6] = b'CD001'; p[6] = 1
    p[8:40] = b'PLAYSTATION'.ljust(32)
    p[40:72] = b'INDIGO_TESTE'.ljust(32)
    p[80:88] = both32(total)
    p[120:124] = both16(1); p[124:128] = both16(1); p[128:132] = both16(2048)
    p[132:140] = both32(len(ptable))
    p[140:144] = struct.pack('<I', 18); p[148:152] = struct.pack('>I', 19)
    p[156:190] = dirrec('\x00', 20, 2048, True)
    p[318:446] = b'INDIGO'.ljust(128)
    for o in (813, 830, 847, 864):
        p[o:o + 17] = b'0' * 16 + b'\x00'
    p[881] = 1
    p[1024:1032] = b'CD-XA001'
    return bytes(p)


def track1(license_ok):
    out = bytearray()
    for i in range(16):
        d = b''
        if i == 4 and license_ok:
            d = b'          Licensed  by          Sony Computer Entertainment Amer  ica '
        out += sector(i, d)
    out += sector(16, pvd(), 0x09)
    term = bytearray(2048); term[0] = 255; term[1:6] = b'CD001'; term[6] = 1
    out += sector(17, bytes(term), 0x89)
    out += sector(18, ptable, 0x89)
    out += sector(19, mtable, 0x89)
    out += sector(20, bytes(root), 0x89)
    for name, l, d, n in layout:
        for k in range(n):
            out += sector(l + k, d[k * 2048:(k + 1) * 2048], 0x89 if k == n - 1 else 0x08)
    for k in range(lba, total):
        out += sector(k, b'')
    return bytes(out)


def track2():
    out = bytearray(2352 * 150)  # pregap (INDEX 00)
    n = 20 * 44100
    for i in range(n):
        l = int(8000 * math.sin(2 * math.pi * 440 * i / 44100))
        r = int(8000 * math.sin(2 * math.pi * 660 * i / 44100))
        out += struct.pack('<hh', l, r)
    out += bytes((-len(out)) % 2352)
    return bytes(out)


def mm(l):
    return '%02d:%02d:%02d' % (l // 4500, l // 75 % 60, l % 75)


audio = track2()
for name, lic in (('teste', True), ('sem_licenca', False)):
    open(f'{OUT}/{name}.bin', 'wb').write(track1(lic) + audio)
    open(f'{OUT}/{name}.cue', 'w').write(
        f'FILE "{name}.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
        f'  TRACK 02 AUDIO\n    INDEX 00 {mm(total)}\n    INDEX 01 {mm(total + 150)}\n')
print(f'disco de teste em {OUT}/teste.cue: faixa 1 com {total} setores, faixa 2 com {len(audio) // 2352}')
