#!/usr/bin/env python3
# Monta os discos de teste do Famicom Disk System a partir do programa de teste (t7_fds_jogo.s montado em prog.bin):
#   teste.fds          dois lados, com o cabeçalho fwNES
#   teste_semcab.fds   os mesmos lados, sem cabeçalho
#   teste.qd           os mesmos lados no formato .qd (64 KB por lado, com o CRC de cada bloco)
#   teste_crcruim.qd   igual ao .qd, com o CRC do cabeçalho do primeiro arquivo errado (a BIOS tem de recusar)
# Uso: t7_fds_disco.py prog.bin pasta_de_saída
import sys

FDS_SIDE, QD_SIDE = 65500, 65536


def crc(bloco):
    """CRC do bloco como o adaptador calcula: polinômio 0x8408, a marca $80 do começo entra na conta"""
    c = 0
    for b in bytes([0x80]) + bloco:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return bytes([c & 0xFF, c >> 8])


def info(lado):
    b = bytearray(56)
    b[0] = 1
    b[1:15] = b'*NINTENDO-HVC*'
    b[15] = 0x00             # fabricante
    b[16:19] = b'IDG'        # nome do jogo
    b[19] = 0x20             # tipo
    b[20] = 0                # revisão
    b[21] = lado             # lado
    b[22] = 0                # número do disco
    b[25] = 0x0F             # arquivos de partida até o ID 15
    b[26:31] = b'\xff' * 5
    b[31:34] = bytes([0x61, 0x10, 0x31])  # data (era Showa 61)
    return bytes(b)


def arquivo(num, ident, nome, end, dados, tipo):
    cab = bytes([3, num, ident]) + nome.ljust(8).encode()[:8] + bytes([end & 0xFF, end >> 8, len(dados) & 0xFF,
                                                                       len(dados) >> 8, tipo])
    return [cab, bytes([4]) + dados]


def lado(n, arquivos):
    blocos = [info(n), bytes([2, len(arquivos)])]
    for k, a in enumerate(arquivos):
        blocos += arquivo(k, *a)
    return blocos


def chr_tiles():
    out = bytearray()
    for t in range(16):
        p0 = [((t * 37 + r * 11) ^ (r << t % 8)) & 0xFF for r in range(8)]
        p1 = [((t * 91 + r * 5) ^ (0xAA >> (r % 4))) & 0xFF for r in range(8)]
        out += bytes(p0) + bytes(p1)
    return bytes(out)


def main():
    prog = open(sys.argv[1], 'rb').read()
    dest = sys.argv[2]
    save = bytes([0]) + b'SAVE' + bytes(11)
    vet = bytes([0x03, 0x60] * 3 + [0x00, 0x60, 0x06, 0x60])  # NMI x3, RESET, IRQ em $DFF6
    ladoA = lado(0, [(0, 'PROG', 0x6000, prog, 0), (1, 'CHR', 0x0000, chr_tiles(), 1),
                     (2, 'SAVE', 0xD000, save, 0), (3, 'VETORES', 0xDFF6, vet, 0)])
    ladoB = lado(1, [(0, 'LADOB', 0xC000, b'LADO B OK' + bytes(7), 0)])

    def fds(lados):
        out = b''
        for bl in lados:
            s = b''.join(bl)
            assert len(s) <= FDS_SIDE
            out += s + bytes(FDS_SIDE - len(s))
        return out

    def qd(lados, estraga=None):
        out = b''
        for i, bl in enumerate(lados):
            s = b''
            for j, b in enumerate(bl):
                c = bytearray(crc(b))
                if estraga == (i, j):
                    c[0] ^= 0x5A
                s += b + bytes(c)
            assert len(s) <= QD_SIDE
            out += s + bytes(QD_SIDE - len(s))
        return out

    lados = [ladoA, ladoB]
    cab = b'FDS\x1a' + bytes([len(lados)]) + bytes(11)
    open(dest + '/teste.fds', 'wb').write(cab + fds(lados))
    open(dest + '/teste_semcab.fds', 'wb').write(fds(lados))
    open(dest + '/teste.qd', 'wb').write(qd(lados))
    open(dest + '/teste_crcruim.qd', 'wb').write(qd(lados, (0, 2)))


main()
