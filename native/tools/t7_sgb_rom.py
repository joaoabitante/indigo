#!/usr/bin/env python3
# Gera a ROM de teste do Super Game Boy do Índigo (código próprio, sem material de terceiros):
#   t7_sgb_rom.py <saída.gb>
# A ROM manda pacotes SGB pelo registrador do joypad e faz transferências pela imagem (PAL_TRN, ATTR_TRN,
# CHR_TRN, PCT_TRN). Depois de cada etapa grava o número da etapa em $C000 e espera 8 quadros; o teste
# (t7_sgb.cpp) roda até a etapa aparecer e confere a imagem 256x224. As cores usadas estão nos dois arquivos.
import sys

LOGO = bytes.fromhex("CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999BBBB67636E0EECCCDDDC999FBBB9333E")

rom = bytearray(0x8000)
code = bytearray()
labels, fixups = {}, []
BASE = 0x150

def emit(*b):
    for x in b: code.append(x & 0xFF)
def here(): return BASE + len(code)
def label(n): labels[n] = here()
def jr(op, n): emit(op, 0); fixups.append(("rel", len(code) - 1, n))
def abs16(op, n): emit(op, 0, 0); fixups.append(("abs", len(code) - 2, n))
def ld_a(v): emit(0x3E, v)
def ldh_w(r, v): ld_a(v); emit(0xE0, r)
def ld_hl(v): emit(0x21, v & 0xFF, v >> 8)
def ld_de(v): emit(0x11, v & 0xFF, v >> 8)
def ld_bc(v): emit(0x01, v & 0xFF, v >> 8)
def st(addr): emit(0xEA, addr & 0xFF, addr >> 8)   # ld (addr),a

data = {}   # blocos de dados (endereço definido depois do código)
def blob(name, b): data[name] = bytes(b)
def ld_de_lab(n): abs16(0x11, n)
def ld_hl_lab(n): abs16(0x21, n)
def call(n): abs16(0xCD, n)

# ---------------------------------------------------------------- pacotes
def le(c): return [c & 0xFF, c >> 8]
def pkt(cmd, length, body):
    b = [cmd * 8 + length] + list(body)
    b += [0] * (16 * length - len(b))
    return b

pkts = []
def send(name, b, stop=0x20):
    blob(name, b)
    if stop != 0x20: ld_a(stop); st(0xC0F0)
    for i in range(len(b) // 16):
        ld_hl_lab(name); emit(0x01, (i * 16) & 0xFF, 0); emit(0x09)  # hl += i*16
        call("send_packet")
        wait(4)                                                       # 4 quadros entre pacotes
    if stop != 0x20: ld_a(0x20); st(0xC0F0)

def wait(n): emit(0x06, n); call("wait_frames")
step_no = [0]
def step():
    step_no[0] += 1
    ld_a(step_no[0]); st(0xC000); wait(8)

# cores (BGR555) — as mesmas de t7_sgb.cpp
X0 = 0x001F
A = [0x03E0, 0x7C00, 0x0011]
B = [0x0210, 0x4210, 0x1084]
C = [0x2222, 0x3333, 0x4444]
D = [0x5555, 0x6666, 0x7777]
def palfn(i, k): return ((i * 4 + k) * 37 + 5) & 0x7FFF

# ---------------------------------------------------------------- carga de 4 KB pela imagem
def show_transfer(name, payload):
    blob(name, payload)
    call("lcd_off")
    ld_de_lab(name); ld_hl(0x8000); ld_bc(0x1000); call("memcpy")
    ldh_w(0x40, 0x91)        # LCD ligado, tiles em 8000 sem sinal, mapa 9800 (0..255 em ordem)
    wait(2)

def show_normal():
    ldh_w(0x40, 0x89)        # tiles em 8800/9000 com sinal, mapa 9C00 (todo tile 0 = faixas de 4 tons)

# ================================================================ programa
emit(0xF3)                    # di
emit(0x31, 0xFE, 0xFF)        # ld sp,$FFFE
emit(0x79); st(0xC001)        # registrador C do boot (0x14 no SGB)
ld_a(0x20); st(0xC0F0)        # valor do bit de parada
call("lcd_off")
# tile de faixas em $9000: colunas 0-1 tom 0, 2-3 tom 1, 4-5 tom 2, 6-7 tom 3 (lo=00110011, hi=00001111)
blob("stripe", [0x33, 0x0F] * 8)
ld_de_lab("stripe"); ld_hl(0x9000); ld_bc(16); call("memcpy")
blob("map9800", bytes((i % 32 < 20 and (i // 32) * 20 + i % 32 or 0) & 0xFF for i in range(1024)))
ld_de_lab("map9800"); ld_hl(0x9800); ld_bc(1024); call("memcpy")
ld_hl(0x9C00); ld_bc(1024); call("memzero")
ldh_w(0x47, 0xE4); ldh_w(0x48, 0xE4); ldh_w(0x49, 0xE4)
show_normal()
step()                        # 1: nada mandado ainda

# 2: MLT_REQ e leitura dos números dos controles
send("mlt2", pkt(0x11, 1, [1]))
ldh_w(0x00, 0x30); emit(0xF0, 0x00); st(0xC002)
ldh_w(0x00, 0x10); ldh_w(0x00, 0x30); emit(0xF0, 0x00); st(0xC003)
ldh_w(0x00, 0x10); ldh_w(0x00, 0x30); emit(0xF0, 0x00); st(0xC004)
send("mlt1", pkt(0x11, 1, [0]))
ldh_w(0x00, 0x30); emit(0xF0, 0x00); st(0xC005)
step()

# 3: PAL01 + PAL23 + ATTR_DIV (esquerda pal1, linha x=10 pal2, direita pal3)
send("pal01", pkt(0x00, 1, le(X0) + sum((le(c) for c in A), []) + sum((le(c) for c in B), [])))
send("pal23", pkt(0x01, 1, le(X0) + sum((le(c) for c in C), []) + sum((le(c) for c in D), [])))
send("div", pkt(0x06, 1, [3 | (1 << 2) | (2 << 4), 10]))
step()

# 4: ATTR_BLK com 3 conjuntos (2 pacotes)
blk = [3,
       1, 2, 2, 2, 6, 6,        # só dentro, pal 2 (a linha em volta também)
       2, 3 << 2, 8, 8, 12, 12, # só a linha em volta, pal 3
       0, 0, 0, 0, 0, 0]
send("blk", pkt(0x04, 2, blk))
step()

# 5: ATTR_LIN: linha horizontal y=17 pal 0, coluna x=19 pal 1
send("lin", pkt(0x05, 1, [2, 0x80 | 17, (1 << 5) | 19]))
step()

# 6: ATTR_CHR a partir de (0,0), 5 blocos, da esquerda para a direita: 0,1,2,3,3
send("chr", pkt(0x07, 1, [0, 0, 5, 0, 0, 0b00011011, 0b11000000]))
step()

# 7: MASK_EN 2 (preto)
send("mask2", pkt(0x17, 1, [2]))
step()
# 8: MASK_EN 3 (cor 0)
send("mask3", pkt(0x17, 1, [3]))
step()
# 9: pacote com bit de parada errado (deve ser ignorado: a máscara continua 3)
send("maskbad", pkt(0x17, 1, [0]), stop=0x10)
step()
# 10: MASK_EN 0
send("mask0", pkt(0x17, 1, [0]))
step()

# 11: congela, faz PAL_TRN e ATTR_TRN (a imagem congelada não muda)
send("mask1", pkt(0x17, 1, [1]))
paltrn = bytearray()
for i in range(512):
    for k in range(4): paltrn += bytes(le(palfn(i, k)))
show_transfer("paltrn", paltrn)
send("paltrn_cmd", pkt(0x0B, 1, []))
wait(6)
atf = bytearray(4096)
for n in range(45):
    for y in range(18):
        for x in range(20):
            p = (1 if y == 0 else 3) if n == 5 else (n + x + y) & 3
            atf[n * 90 + y * 5 + x // 4] |= p << (6 - 2 * (x % 4))
show_transfer("attrtrn", atf)
send("attrtrn_cmd", pkt(0x15, 1, []))
wait(6)
show_normal()
step()
# 12: PAL_SET (10, 20, 30, 40) aplicando o ATF 5 e cancelando a máscara
send("palset", pkt(0x0A, 1, le(10) + le(20) + le(30) + le(40) + [0x80 | 0x40 | 5]))
step()

# 13: moldura: CHR_TRN (0-127 e 128-255) e PCT_TRN
def tile4(colors):  # 64 índices -> 32 bytes SNES 4bpp
    t = bytearray(32)
    for r in range(8):
        for k in range(8):
            c = colors[r * 8 + k]
            for pl in range(4):
                if c >> pl & 1:
                    off = (pl >> 1) * 16 + r * 2 + (pl & 1)
                    t[off] |= 0x80 >> k
    return t
chr0 = bytearray(4096)
chr0[32:64] = tile4([5] * 64)
chr0[64:96] = tile4([15] * 64)
chr0[96:128] = tile4([(k % 8) + 1 for k in range(64)])
chr1 = bytearray(4096)
chr1[0:32] = tile4([9] * 64)
pct = bytearray(4096)
for ty in range(28):
    for tx in range(32):
        inside = 6 <= tx < 26 and 5 <= ty < 23
        e = 0 if inside else (1 | (4 << 10))
        if ty == 0: e = 2 | (5 << 10)
        if (tx, ty) == (1, 1): e = 3 | (6 << 10) | 0x4000
        if (tx, ty) == (6, 5): e = 0x80 | (4 << 10)
        pct[(ty * 32 + tx) * 2:(ty * 32 + tx) * 2 + 2] = bytes(le(e))
def setc(pn, ci, c): pct[0x800 + (pn - 4) * 32 + ci * 2:0x800 + (pn - 4) * 32 + ci * 2 + 2] = bytes(le(c))
setc(4, 5, 0x1234); setc(4, 9, 0x0ABC); setc(5, 15, 0x7001)
for ci in range(1, 9): setc(6, ci, 0x100 * ci + ci)
send("mask1b", pkt(0x17, 1, [1]))
show_transfer("chr0", chr0); send("chr0_cmd", pkt(0x13, 1, [0])); wait(6)
show_transfer("chr1", chr1); send("chr1_cmd", pkt(0x13, 1, [1])); wait(6)
show_transfer("pct", pct); send("pct_cmd", pkt(0x14, 1, [])); wait(6)
show_normal()
send("mask0b", pkt(0x17, 1, [0]))
step()

ld_a(0xFF); st(0xC000)
label("fim"); jr(0x18, "fim")

# ---------------------------------------------------------------- rotinas
label("send_packet")          # HL = 16 bytes
ldh_w(0x00, 0x00); ldh_w(0x00, 0x30)
emit(0x06, 16)
label("sp_byte"); emit(0x2A, 0x4F, 0x16, 8)          # ld a,(hl+); ld c,a; ld d,8
label("sp_bit"); emit(0x79, 0xE6, 1); ld_a(0x10); jr(0x20, "sp_one"); ld_a(0x20)
label("sp_one"); emit(0xE0, 0x00); ld_a(0x30); emit(0xE0, 0x00)
emit(0xCB, 0x39, 0x15); jr(0x20, "sp_bit"); emit(0x05); jr(0x20, "sp_byte")
emit(0xFA, 0xF0, 0xC0, 0xE0, 0x00); ld_a(0x30); emit(0xE0, 0x00)  # bit de parada
emit(0xC9)

label("wait_frames")          # B quadros (espera o começo do VBlank)
label("wf_a"); emit(0xF0, 0x44, 0xFE, 144); jr(0x28, "wf_a")
label("wf_b"); emit(0xF0, 0x44, 0xFE, 144); jr(0x20, "wf_b")
emit(0x05); jr(0x20, "wf_a"); emit(0xC9)

label("lcd_off")
emit(0xF0, 0x40, 0xE6, 0x80, 0xC8)                   # já desligado: ret z
label("lo_w"); emit(0xF0, 0x44, 0xFE, 144); jr(0x20, "lo_w")
ldh_w(0x40, 0x00); emit(0xC9)

label("memcpy")               # DE -> HL, BC bytes
label("mc_l"); emit(0x1A, 0x22, 0x13, 0x0B, 0x78, 0xB1); jr(0x20, "mc_l"); emit(0xC9)
label("memzero")
label("mz_l"); emit(0xAF, 0x22, 0x0B, 0x78, 0xB1); jr(0x20, "mz_l"); emit(0xC9)

# ---------------------------------------------------------------- montagem
addr = BASE + len(code)
dpos = {}
blobs = bytearray()
for n, b in data.items():
    dpos[n] = addr + len(blobs); blobs += b
labels.update(dpos)
for kind, pos, n in fixups:
    t = labels[n]
    if kind == "rel":
        d = t - (BASE + pos + 1)
        assert -128 <= d <= 127, n
        code[pos] = d & 0xFF
    else:
        code[pos] = t & 0xFF; code[pos + 1] = t >> 8
img = code + blobs
assert BASE + len(img) <= 0x8000, hex(BASE + len(img))
rom[BASE:BASE + len(img)] = img
rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
rom[0x104:0x134] = LOGO
rom[0x134:0x13F] = b"INDIGO SGB\0"
rom[0x146] = 0x03; rom[0x147] = 0x00; rom[0x14B] = 0x33
chk = 0
for a in range(0x134, 0x14D): chk = (chk - rom[a] - 1) & 0xFF
rom[0x14D] = chk
s = sum(rom) - rom[0x14E] - rom[0x14F]
rom[0x14E] = (s >> 8) & 0xFF; rom[0x14F] = s & 0xFF
open(sys.argv[1], "wb").write(rom)
print("ROM", sys.argv[1], len(img), "bytes de código e dados,", step_no[0], "etapas")
