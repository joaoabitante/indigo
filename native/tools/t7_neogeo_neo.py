# Converte um .zip de jogo do Neo Geo no formato do MAME (sufixos .p1 .s1 .m1 .v1 .c1 .c2 ...) para .neo, o
# arquivo único do NeoSD/Geolith/MiSTer: cabeçalho de 4096 bytes ("NEO" 1, tamanhos P S M V1 V2 C em 32 bits
# little-endian, ano, gênero, tela, NGH, nome com 33 bytes em 44, fabricante com 17 bytes em 77) e depois os
# dados: P (na ordem do arquivo .p1), S, M, V1, V2 e C (C1/C2 intercaladas byte a byte).
# uso: python3 t7_neogeo_neo.py jogo.zip saida.neo nome [ngh]
import re
import struct
import sys
import zipfile

z = zipfile.ZipFile(sys.argv[1])
parts = {}
for n in z.namelist():
    m = re.search(r'\.([psmvc])(\d+)$', n.lower())
    if m:
        parts.setdefault(m.group(1), []).append((int(m.group(2)), z.read(n)))


def cat(k):
    return b''.join(d for _, d in sorted(parts.get(k, [])))


p, s, mm, v = cat('p'), cat('s'), cat('m'), cat('v')
cs = sorted(parts.get('c', []))
c = bytearray()
for i in range(0, len(cs) - 1, 2):
    a, b = cs[i][1], cs[i + 1][1]
    t = bytearray(len(a) * 2)
    t[0::2] = a
    t[1::2] = b
    c += t
h = bytearray(4096)
h[0:4] = b'NEO\x01'
ngh = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x1337
struct.pack_into('<10I', h, 4, len(p), len(s), len(mm), len(v), 0, len(c), 2026, 0, 0, ngh)
nome = sys.argv[3].encode()[:33]
h[44:44 + len(nome)] = nome
h[77:77 + 8] = b'ngdevkit'
open(sys.argv[2], 'wb').write(bytes(h) + p + s + mm + v + bytes(c))
