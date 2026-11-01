#!/usr/bin/env python3
# Monta os CDs de teste do Neo Geo CD (usado pelo t7_neogeocd.sh; nada disto vai para o repositório):
#   - converte um exemplo do ngdevkit no formato do MAME (.zip com -p1.p1, -s1.s1, -m1.m1, -v1.v1, -c1.c1, -c2.c2)
#     nos arquivos do CD: .PRG (o mesmo programa, com os vetores de interrupção do CD como o ngdevkit-cd-crt0.o
#     põe: quadro no nível 2 = $68, linha no nível 1 = $64, e os vetores $54/$58/$5C do CD), .FIX (= S ROM),
#     .SPR (as C ROMs no formato do CD: em cada linha de 4 bytes os planos 2, 1, 4, 3), .Z80 (= M ROM, até
#     $F7FF) e .PCM (= V ROM), mais o IPL.TXT;
#   - grava um ISO 9660 (nível 1) e o .cue/.bin (faixa 1 MODE1/2352; com --audio, uma faixa 2 de áudio de 10 s
#     com 2 s de pregap, tons que mudam de volume, para conferir o CD-DA).
# uso: t7_neogeocd_disco.py pacote.zip saida_sem_extensao NOME [--audio]
import struct
import sys
import zipfile
import math


def trim(b, pad, align):
    n = len(b)
    while n > 0 and b[n - 1] == pad:
        n -= 1
    n = (n + align - 1) // align * align
    return bytes(b[:n])


def convert(zpath, name):
    z = zipfile.ZipFile(zpath)
    get = {}
    for n in z.namelist():
        for ext in ('p1', 's1', 'm1', 'v1', 'c1', 'c2'):
            if n.lower().endswith('.' + ext):
                get[ext] = z.read(n)
    files = {}
    p = bytearray(get['p1'])
    if p[0:4] == b'\x10\x00\x00\xf3':  # palavras com os bytes trocados (como nas P ROMs do MAME)
        for i in range(0, len(p) - 1, 2):
            p[i], p[i + 1] = p[i + 1], p[i]
    assert p[0:4] == b'\x00\x10\xf3\x00', 'programa sem a pilha $10F300'
    v64, v68 = p[0x64:0x68], p[0x68:0x6C]
    p[0x64:0x68], p[0x68:0x6C] = v68, v64
    p[0x54:0x58] = struct.pack('>I', 0xC004F2)
    p[0x58:0x5C] = struct.pack('>I', 0xC004EC)
    p[0x5C:0x60] = struct.pack('>I', 0xC004E6)
    files[name + '.PRG'] = trim(p, 0xFF, 2)
    if 's1' in get:
        files[name + '.FIX'] = trim(get['s1'], 0x00, 32)
    if 'c1' in get and 'c2' in get:
        c1, c2 = get['c1'], get['c2']
        n = min(len(c1), len(c2))
        spr = bytearray(2 * n)
        spr[0::4] = c1[1:n:2]
        spr[1::4] = c1[0:n:2]
        spr[2::4] = c2[1:n:2]
        spr[3::4] = c2[0:n:2]
        spr = trim(spr, 0x00, 128)
        if spr:
            files[name + '.SPR'] = spr
    if 'm1' in get:
        files[name + '.Z80'] = trim(get['m1'][:0xF800], 0x00, 2)
    if 'v1' in get:
        pcm = trim(get['v1'], 0x00, 256)
        if pcm:
            files[name + '.PCM'] = pcm
    ipl = ''
    for ext in ('FIX', 'SPR', 'PCM', 'Z80', 'PRG'):
        if name + '.' + ext in files:
            ipl += '%s.%s,0,0\r\n' % (name, ext)
    files['IPL.TXT'] = ipl.encode() + b'\x1a'
    return files


def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def dirrec(lba, size, name, flags):
    n = len(name)
    rec = bytearray(33 + n + ((n + 1) & 1))
    rec[0] = len(rec)
    rec[2:10] = both32(lba)
    rec[10:18] = both32(size)
    rec[18:25] = bytes([126, 10, 30, 12, 0, 0, 0])
    rec[25] = flags
    rec[28:32] = both16(1)
    rec[32] = n
    rec[33:33 + n] = name
    return bytes(rec)


def iso(files, volume):
    names = sorted(files)
    lba = 21
    where = {}
    for n in names:
        where[n] = lba
        lba += max(1, (len(files[n]) + 2047) // 2048)
    total = lba
    root = bytearray()
    root += dirrec(20, 2048, b'\x00', 2)
    root += dirrec(20, 2048, b'\x01', 2)
    for n in names:
        root += dirrec(where[n], len(files[n]), (n + ';1').encode(), 0)
    assert len(root) <= 2048
    img = bytearray(total * 2048)
    pvd = bytearray(2048)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[8:40] = b' ' * 32
    pvd[40:72] = volume.encode().ljust(32)[:32]
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(2048)
    pvd[132:140] = both32(10)
    pvd[140:144] = struct.pack('<I', 18)
    pvd[148:152] = struct.pack('>I', 19)
    pvd[156:190] = dirrec(20, 2048, b'\x00', 2)
    pvd[190:813] = b' ' * 623
    for o in (813, 830, 847, 864):
        pvd[o:o + 17] = b'0' * 16 + b'\x00'
    pvd[881] = 1
    img[16 * 2048:17 * 2048] = pvd
    term = bytearray(2048)
    term[0] = 255
    term[1:6] = b'CD001'
    term[6] = 1
    img[17 * 2048:18 * 2048] = term
    img[18 * 2048:18 * 2048 + 10] = bytes([1, 0]) + struct.pack('<I', 20) + struct.pack('<H', 1) + b'\x00\x00'
    img[19 * 2048:19 * 2048 + 10] = bytes([1, 0]) + struct.pack('>I', 20) + struct.pack('>H', 1) + b'\x00\x00'
    img[20 * 2048:20 * 2048 + len(root)] = root
    for n in names:
        o = where[n] * 2048
        img[o:o + len(files[n])] = files[n]
    return bytes(img)


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def mode1(data):
    out = bytearray()
    for i in range(0, len(data), 2048):
        a = i // 2048 + 150
        s = bytearray(2352)
        s[1:11] = b'\xff' * 10
        s[12] = bcd(a // 4500)
        s[13] = bcd(a // 75 % 60)
        s[14] = bcd(a % 75)
        s[15] = 1
        s[16:2064] = data[i:i + 2048]
        out += s
    return bytes(out)


def audio(seconds):
    # 44 100 Hz estéreo: dois tons, com o volume subindo e descendo a cada meio segundo (o RMS por trecho muda)
    n = 44100 * seconds
    out = bytearray(n * 4)
    for i in range(n):
        env = 0.15 + 0.6 * (0.5 + 0.5 * math.sin(2 * math.pi * i / 44100))
        l = int(12000 * env * math.sin(2 * math.pi * 440 * i / 44100))
        r = int(12000 * env * math.sin(2 * math.pi * 660 * i / 44100 + 1.0))
        struct.pack_into('<hh', out, i * 4, l, r)
    return bytes(out)


def main():
    zpath, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
    with_audio = '--audio' in sys.argv
    files = convert(zpath, name)
    data = mode1(iso(files, name))
    base = out.rsplit('/', 1)[-1]
    cue = 'FILE "%s.bin" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n' % base
    if with_audio:
        sectors = len(data) // 2352
        pre = sectors + 0

        def msf(l):
            return '%02d:%02d:%02d' % (l // 4500, l // 75 % 60, l % 75)
        cue += '  TRACK 02 AUDIO\r\n    INDEX 00 %s\r\n    INDEX 01 %s\r\n' % (msf(pre), msf(pre + 150))
        data += bytes(150 * 2352) + audio(10)
    open(out + '.bin', 'wb').write(data)
    open(out + '.cue', 'w', newline='').write(cue)


main()
