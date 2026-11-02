# Monta o CD do homebrew de teste do Saturn (escrito do zero): IP.BIN nos 16 primeiros setores, sistema de
# arquivos ISO 9660 com 1ST_READ.BIN (o programa) e DADOS.BIN (padrão conferido pelo teste) e uma faixa de áudio
# de 2 segundos (onda senoidal de 440 Hz). Saída: <pasta>/homebrew.cue, homebrew_1.bin (Mode 1/2352),
# homebrew_2.bin (áudio) e homebrew.iso (só os dados).
# uso: python3 -I t7_saturn_disco.py programa.bin pasta
import math, os, struct, sys

prog = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
os.makedirs(out, exist_ok=True)

def pad(b, n): return b + bytes(n - len(b))
def both16(v): return struct.pack('<H', v) + struct.pack('>H', v)
def both32(v): return struct.pack('<I', v) + struct.pack('>I', v)
def sect(n): return (n + 2047) // 2048

dados = bytes(((i * 7 + (i >> 8)) & 0xFF) for i in range(4096))
ROOT = 20
files = [(b'1ST_READ.BIN;1', prog), (b'DADOS.BIN;1', dados)]
lba = ROOT + 1
pos = []
for name, data in files:
    pos.append(lba)
    lba += sect(len(data))
total = lba

def rec(name, extent, size, flags):
    n = len(name)
    l = 33 + n + (1 - n % 2)
    r = bytes([l, 0]) + both32(extent) + both32(size) + bytes([126, 11, 2, 12, 0, 0, 0]) + bytes([flags, 0, 0]) + both16(1) + bytes([n]) + name
    return pad(r, l)

root = rec(b'\x00', ROOT, 2048, 2) + rec(b'\x01', ROOT, 2048, 2)
for (name, data), p in zip(files, pos):
    root += rec(name, p, len(data), 0)
root = pad(root, 2048)

# IP.BIN (cabeçalho do disco do Saturn)
ip = b'SEGA SEGASATURN ' + b'SEGA TP T-000   ' + b'T-00000   ' + b'V1.000' + b'20261102' + b'CD-1/1  '
ip += b'JTUBKAEL  ' + b' ' * 6 + pad(b'J', 16).replace(b'\x00', b' ')
ip += pad(b'INDIGO SATURN TESTE', 112).replace(b'\x00', b' ')
ip += bytes(16)
ip += struct.pack('>IIIIII', 0x1800, 0, 0, 0, 0x06004000, 0) + bytes(8)
assert len(ip) == 0x100, hex(len(ip))
system = pad(ip, 16 * 2048)

pathL = bytes([1, 0]) + struct.pack('<I', ROOT) + struct.pack('<H', 1) + b'\x00\x00'
pathM = bytes([1, 0]) + struct.pack('>I', ROOT) + struct.pack('>H', 1) + b'\x00\x00'
pvd = bytes([1]) + b'CD001' + bytes([1, 0]) + pad(b'SEGA SATURN', 32).replace(b'\x00', b' ') + pad(b'INDIGO_SATURN', 32).replace(b'\x00', b' ')
pvd += bytes(8) + both32(total) + bytes(32) + both16(1) + both16(1) + both16(2048) + both32(len(pathL))
pvd += struct.pack('<I', 18) + struct.pack('<I', 0) + struct.pack('>I', 19) + struct.pack('>I', 0)
pvd += rec(b'\x00', ROOT, 2048, 2)
pvd = pad(pvd, 2048)
term = pad(bytes([255]) + b'CD001' + bytes([1]), 2048)

image = system + pvd + term + pad(pathL, 2048) + pad(pathM, 2048) + root
for name, data in files:
    image += pad(data, sect(len(data)) * 2048)
assert len(image) == total * 2048
open(os.path.join(out, 'homebrew.iso'), 'wb').write(image)

def bcd(v): return ((v // 10) << 4) | (v % 10)
raw = bytearray()
for i in range(total):
    a = i + 150
    raw += b'\x00' + b'\xff' * 10 + b'\x00' + bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), 1])
    raw += image[i * 2048:(i + 1) * 2048] + bytes(288)
open(os.path.join(out, 'homebrew_1.bin'), 'wb').write(raw)
audio = bytearray()
for n in range(2 * 44100):
    v = int(12000 * math.sin(2 * math.pi * 440 * n / 44100))
    audio += struct.pack('<hh', v, v)
audio = pad(bytes(audio), (len(audio) + 2351) // 2352 * 2352)
open(os.path.join(out, 'homebrew_2.bin'), 'wb').write(audio)
open(os.path.join(out, 'homebrew.cue'), 'w').write(
    'FILE "homebrew_1.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
    'FILE "homebrew_2.bin" BINARY\n  TRACK 02 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 00:00:00\n')
print('disco do Saturn:', total, 'setores de dados')
