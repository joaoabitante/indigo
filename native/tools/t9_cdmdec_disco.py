#!/usr/bin/env python3
# Discos sintéticos do PlayStation para o t9_cdmdec (controlador de CD-ROM):
#   teste.cue/.bin  faixa 1 Mode 2 (licença "Amer", dados Form 1 e Form 2, setores XA-ADPCM intercalados em
#                   quatro formatos) + faixas 2 e 3 de áudio com pregap de 2 s (INDEX 00)
#   europa.cue/.bin e japao.cue/.bin   só a faixa de dados, com as licenças "Euro" e "Inc."
#   audio.cue/.bin  CD de música (uma faixa)
# Tudo determinístico (LCG próprio). EDC/ECC ficam zerados (o controlador não confere).
# uso: t9_cdmdec_disco.py pasta
import math
import os
import struct
import sys

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)


class Lcg:
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def next(self):
        self.s = (self.s * 1103515245 + 12345) & 0xFFFFFFFF
        return self.s >> 8

    def below(self, n):
        return self.next() % n


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def msf(lba):
    a = lba + 150
    return bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75)])


SYNC = b'\x00' + b'\xff' * 10 + b'\x00'


def mode2(lba, sub, payload, form2=False):
    """setor Mode 2 bruto: sincronismo, cabeçalho, subcabeçalho duplicado, dados (2048 ou 2324) e EDC"""
    n = 2324 if form2 else 2048
    payload = payload[:n].ljust(n, b'\x00')
    s = SYNC + msf(lba) + b'\x02' + sub + sub + payload
    s += b'\x00' * (2352 - len(s))
    assert len(s) == 2352
    return s


def data_payload(lba, n):
    r = Lcg(lba * 2654435761 + 7)
    head = struct.pack('<I', lba) + b'INDIGO-PS1-SETOR'
    return head + bytes(r.below(256) for _ in range(n - len(head)))


LICENSE = {
    'A': b'          Licensed  by          Sony Computer Entertainment Amer  ica ',
    'E': b'          Licensed  by          Sony Computer Entertainment Euro pe   ',
    'I': b'          Licensed  by          Sony Computer Entertainment Inc.',
}


def system_area(lba, region):
    if lba == 4:
        return mode2(lba, bytes([0, 0, 0x08, 0]), LICENSE[region].ljust(2048, b' '))
    if lba == 16:
        pvd = b'\x01CD001\x01\x00' + b'PLAYSTATION'.ljust(32) + b'INDIGO_TESTE'.ljust(32)
        return mode2(lba, bytes([0, 0, 0x89, 0]), pvd)
    return mode2(lba, bytes([0, 0, 0x08, 0]), b'')


def xa_sector(lba, filen, chan, coding, r):
    """setor XA-ADPCM (Form 2, tempo real, áudio) com parâmetros e amostras aleatórios"""
    bits8 = coding & 0x10
    units = 4 if bits8 else 8
    out = bytearray()
    for g in range(18):
        hdr = []
        for u in range(units):
            filt = r.below(4)
            shift = (r.below(5) if bits8 else 3 + r.below(8))
            hdr.append((filt << 4) | shift)
        params = bytearray(16)
        if bits8:
            for u in range(4):
                params[4 + u] = hdr[u]
            params[0:4] = params[4:8]
            params[8:12] = params[4:8]
            params[12:16] = params[4:8]
        else:
            for u in range(8):
                params[4 + u] = hdr[u]
            params[0:4] = params[4:8]
            params[12:16] = params[8:12]
        out += params
        out += bytes(r.below(256) for _ in range(112))
    sub = bytes([filen, chan, 0x64, coding])
    return mode2(lba, sub, bytes(out), form2=True)


XA_POS = (0, 60, 115, 98)
XA_NEG = (0, 0, -52, -55)


def xa_encode_unit(x, state, bits8):
    """codifica 28 amostras num bloco ADPCM (escolhe o filtro de menor erro); devolve (cabeçalho, códigos)"""
    best = None
    for f in range(4):
        old, older = state
        # deslocamento pelo maior resíduo previsto sem realimentação
        peak = 0
        po, pn = old, older
        for v in x:
            p = (po * XA_POS[f] + pn * XA_NEG[f] + 32) >> 6
            peak = max(peak, abs(v - p))
            pn, po = po, v
        lim = 127 if bits8 else 7
        step = 0
        while step < (8 if bits8 else 12) and (lim << step) < peak:
            step += 1
        shift = (8 if bits8 else 12) - step
        codes, err = [], 0
        o, on = old, older
        for v in x:
            p = (o * XA_POS[f] + on * XA_NEG[f] + 32) >> 6
            c = int(round((v - p) / float(1 << step)))
            c = max(-lim - 1, min(lim, c))
            t = ((c << (8 if bits8 else 12)) & 0xFFFF)
            t = (t - 0x10000 if t & 0x8000 else t) >> shift
            d = max(-32768, min(32767, t + p))
            err += (d - v) * (d - v)
            codes.append(c & (0xFF if bits8 else 0x0F))
            on, o = o, d
        if best is None or err < best[0]:
            best = (err, (f << 4) | shift, codes, (o, on))
    state[0], state[1] = best[3]
    return best[1], best[2]


def xa_signal_sector(lba, filen, chan, coding, t0, freqs, amp, st):
    """setor XA com um sinal de baixa frequência codificado (o canal que os testes tocam)"""
    stereo, half, bits8 = coding & 1, coding & 4, coding & 0x10
    rate = 18900.0 if half else 37800.0
    units = 4 if bits8 else 8
    out = bytearray()
    n = t0
    for g in range(18):
        blocks = []
        for u in range(units):
            ch = (u & 1) if stereo else 0
            if stereo:
                base = n + (u >> 1) * 28
            else:
                base = n + u * 28
            x = []
            for j in range(28):
                tt = (base + j) / rate
                v = sum(a * math.sin(2 * math.pi * fq * tt + ch) for fq, a in zip(freqs[ch], amp))
                x.append(int(v))
            blocks.append(xa_encode_unit(x, st[ch], bits8))
        n += (units // 2 if stereo else units) * 28
        params = bytearray(16)
        for u in range(units):
            params[4 + u] = blocks[u][0]
        if bits8:
            params[0:4] = params[4:8]
            params[8:12] = params[4:8]
            params[12:16] = params[4:8]
        else:
            params[0:4] = params[4:8]
            params[12:16] = params[8:12]
        data = bytearray(112)
        for j in range(28):
            for u in range(units):
                if bits8:
                    data[j * 4 + u] = blocks[u][1][j]
                else:
                    data[j * 4 + (u >> 1)] |= blocks[u][1][j] << ((u & 1) * 4)
        out += params + data
    sub = bytes([filen, chan, 0x64, coding])
    return mode2(lba, sub, bytes(out), form2=True), n


def write_disc(name, sectors, tracks):
    """tracks: lista (número, tipo, lba_index00 ou None, lba_index01)"""
    with open(os.path.join(OUT, name + '.bin'), 'wb') as f:
        for s in sectors:
            assert len(s) == 2352
            f.write(s)
    lines = ['FILE "%s.bin" BINARY' % name]
    for num, typ, i0, i1 in tracks:
        lines.append('  TRACK %02d %s' % (num, typ))
        for idx, l in ((0, i0), (1, i1)):
            if l is None:
                continue
            lines.append('    INDEX %02d %02d:%02d:%02d' % (idx, l // 4500, l // 75 % 60, l % 75))
    with open(os.path.join(OUT, name + '.cue'), 'w') as f:
        f.write('\n'.join(lines) + '\n')


def audio_sector(n0, fl, fr, amp, r):
    b = bytearray()
    for i in range(588):
        t = n0 + i
        env = 0.55 + 0.45 * math.sin(2 * math.pi * t / 44100 * 0.7)
        l = int(amp * env * math.sin(2 * math.pi * fl * t / 44100)) + r.below(64) - 32
        rr = int(amp * 0.8 * math.cos(2 * math.pi * fr * t / 44100)) + r.below(64) - 32
        b += struct.pack('<hh', max(-32768, min(32767, l)), max(-32768, min(32767, rr)))
    return bytes(b)


def data_track(region, n, xa):
    secs = []
    r = Lcg(1234)
    xt = 0
    xst = [[0, 0], [0, 0]]
    last_region = -1
    for lba in range(n):
        if lba < 17:
            secs.append(system_area(lba, region))
        elif not xa or lba < 300:
            if lba % 7 == 3:  # Form 2 (vídeo/dados de 2324 bytes)
                secs.append(mode2(lba, bytes([1, 0, 0x28, 0]), data_payload(lba, 2324), form2=True))
            else:
                secs.append(mode2(lba, bytes([0, 0, 0x08, 0]), data_payload(lba, 2048)))
        else:
            # regiões XA de 300 setores: (codificação, passo da intercalação)
            region_i = (lba - 300) // 300
            coding, step = [(0x01, 8), (0x00, 8), (0x05, 8), (0x11, 4)][region_i]
            k = lba % step
            if region_i != last_region:
                xt, xst, last_region = 0, [[0, 0], [0, 0]], region_i
            if k == 1:
                freqs = [(220.0 + 110 * region_i, 1250.0), (330.0 + 55 * region_i, 2100.0)]
                sec, xt = xa_signal_sector(lba, 1, 1, coding, xt, freqs, (6000, 2500), xst)
                secs.append(sec)
            elif k == 0:
                secs.append(mode2(lba, bytes([1, 0, 0x08, 0]), data_payload(lba, 2048)))
            else:
                secs.append(xa_sector(lba, 1, k, coding, r))
    return secs


# disco principal: faixa 1 com 1500 setores (300..1499: XA), faixa 2 (150 de pregap + 900), faixa 3 (150 + 600)
secs = data_track('A', 1500, True)
r = Lcg(99)
for i in range(150):
    secs.append(b'\x00' * 2352)
for i in range(900):
    secs.append(audio_sector(i * 588, 440.0, 660.0, 12000, r))
for i in range(150):
    secs.append(b'\x00' * 2352)
for i in range(600):
    secs.append(audio_sector(i * 588, 330.0, 995.0, 20000, r))
write_disc('teste', secs, [(1, 'MODE2/2352', None, 0), (2, 'AUDIO', 1500, 1650), (3, 'AUDIO', 2550, 2700)])

write_disc('europa', data_track('E', 64, False), [(1, 'MODE2/2352', None, 0)])
write_disc('japao', data_track('I', 64, False), [(1, 'MODE2/2352', None, 0)])
r = Lcg(5)
write_disc('audio', [audio_sector(i * 588, 523.0, 784.0, 15000, r) for i in range(450)], [(1, 'AUDIO', None, 0)])
print('discos prontos em', OUT)
