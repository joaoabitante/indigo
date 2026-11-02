#!/usr/bin/env python3
# Gera src/gnw/tabela.inc (lista dos Game & Watch) a partir do driver do MAME (licença BSD-3-Clause):
#   src/mame/handheld/hh_sm510.cpp
# Uso: python3 gerar_tabela.py <caminho do hh_sm510.cpp> > tabela.inc
# (tabela.inc atual: MAME commit 079ac58c453b09a3ffd8267d1c611b4d2c63795b)
#
# Só entram dados de fato: nome do pacote, título, CPU, nomes/CRC-32 dos arquivos de dentro do .zip, quantas
# telas e em que disposição, e a ligação de cada botão (linha S/R, bit K, BA, B) traduzida para as teclas do
# Índigo. Nenhum código do MAME é copiado.
import re
import sys

src = open(sys.argv[1], encoding='utf-8').read()


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


code = strip_comments(src)

# ------------------------------------------------------------------ jogos: SYST(...)
games = []
for m in re.finditer(r'^SYST\(\s*(\S+?),\s*(\w+),\s*(\w+),\s*\w+,\s*(\w+),\s*(\w+),\s*(\w+),\s*\w+,\s*"([^"]*)",\s*"([^"]*)"',
                     src, re.M):
    year, name, parent, machine, inp, cls, maker, title = m.groups()
    if name.startswith('gnw_'):
        games.append(dict(name=name, parent=parent, machine=machine, inp=inp, cls=cls, title=title))

# ------------------------------------------------------------------ ROMs
def rom_def(name):
    m = re.search(r'ROM_START\(\s*' + name + r'\s*\)(.*?)ROM_END', code, re.S)
    body = m.group(1)
    prog = melody = None
    svgs = []
    region = None
    for line in body.split('\n'):
        r = re.search(r'ROM_REGION\(\s*[^,]+,\s*"([^"]+)"', line)
        if r:
            region = r.group(1)
            continue
        l = re.search(r'ROM_LOAD\(\s*"([^"]+)",\s*([^,]+),\s*([^,]+),\s*(?:BAD_DUMP\s*)?CRC\((\w+)\)', line)
        if l:
            f, off, size, crc = l.groups()
            ent = (f, int(size.strip(), 0), int(crc, 16))
            if region == 'maincpu':
                prog = ent
            elif region == 'maincpu:melody':
                melody = ent
            elif region.startswith('screen'):
                svgs.append(ent)
    return prog, melody, svgs


# ------------------------------------------------------------------ configuração da máquina e da classe
def machine_def(cls, machine):
    m = re.search(r'void\s+' + cls + r'::' + machine + r'\(machine_config\s*&config\)\s*\{(.*?)\n\}', code, re.S)
    body = m.group(1)
    c = re.search(r'(sm5a|sm510|sm511|sm512)_(common|dualv|dualh|tripleh)\(', body)
    cpu, layout = c.groups()
    rbit = 1 if 'piezo_r2_w' in body else 0
    return cpu, layout, rbit


def class_def(cls):
    m = re.search(r'class\s+' + cls + r'\s*:.*?\n\};', code, re.S)
    body = m.group(0)
    fixed_last = 'inp_fixed_last()' in body
    lines = re.search(r'm_inp_lines\s*=\s*(\d+)', body)
    pivot = re.search(r'm_decay_pivot\s*=\s*(\d+)', body)
    dlen = re.search(r'm_decay_len\s*=\s*(\d+)', body)
    return dict(fixed_last=fixed_last, lines=int(lines.group(1)) if lines else None,
                pivot=int(pivot.group(1)) if pivot else 8, dlen=int(dlen.group(1)) if dlen else 17)


# ------------------------------------------------------------------ entradas
def ports_def(name):
    m = re.search(r'INPUT_PORTS_START\(\s*' + name + r'\s*\)(.*?)INPUT_PORTS_END', code, re.S)
    ports = {}
    order = []
    cur = None
    for line in m.group(1).split('\n'):
        inc = re.search(r'PORT_INCLUDE\(\s*(\w+)\s*\)', line)
        if inc:
            sub, sorder = ports_def(inc.group(1))
            for k in sorder:
                if k not in ports:
                    order.append(k)
                ports[k] = sub[k]
            continue
        st = re.search(r'PORT_START\("([^"]+)"\)', line)
        if st:
            cur = st.group(1)
            if cur not in ports:
                order.append(cur)
            ports[cur] = dict(base=0, bits=[])
            continue
        b = re.search(r'PORT_BIT\(\s*(0x[0-9a-fA-F]+),\s*(IP_ACTIVE_HIGH|IP_ACTIVE_LOW),\s*(IPT_\w+)\s*\)', line)
        if b:
            mask, act, ipt = int(b.group(1), 16), b.group(2), b.group(3)
            low = act == 'IP_ACTIVE_LOW'
            if low:
                ports[cur]['base'] |= mask
            if ipt != 'IPT_UNUSED':
                pl = re.search(r'PORT_PLAYER\((\d)\)', line)
                ports[cur]['bits'].append((mask, ipt, low, int(pl.group(1)) if pl else 1))
            continue
        c = re.search(r'PORT_CONFNAME\(\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)', line)
        if c:
            ports[cur]['base'] |= int(c.group(2), 16)
    return ports, order


# teclas do Índigo (core.h)
K = dict(A=1 << 0, B=1 << 1, SELECT=1 << 2, START=1 << 3, RIGHT=1 << 4, LEFT=1 << 5, UP=1 << 6, DOWN=1 << 7,
         R=1 << 8, L=1 << 9, X=1 << 10, Y=1 << 11)
P1 = {'IPT_JOYSTICK_UP': 'UP', 'IPT_JOYSTICK_DOWN': 'DOWN', 'IPT_JOYSTICK_LEFT': 'LEFT', 'IPT_JOYSTICK_RIGHT': 'RIGHT',
      'IPT_JOYSTICKLEFT_UP': 'UP', 'IPT_JOYSTICKLEFT_DOWN': 'DOWN', 'IPT_JOYSTICKLEFT_LEFT': 'LEFT',
      'IPT_JOYSTICKLEFT_RIGHT': 'RIGHT',
      'IPT_JOYSTICKRIGHT_UP': 'X', 'IPT_JOYSTICKRIGHT_DOWN': 'B', 'IPT_JOYSTICKRIGHT_LEFT': 'Y',
      'IPT_JOYSTICKRIGHT_RIGHT': 'A',
      'IPT_BUTTON1': 'A', 'IPT_BUTTON2': 'B', 'IPT_BUTTON3': 'X', 'IPT_BUTTON4': 'Y',
      'IPT_START1': 'START', 'IPT_START': 'START', 'IPT_START2': 'R', 'IPT_SELECT': 'SELECT', 'IPT_SERVICE2': 'L'}
# Micro Vs. System (2 jogadores): jogador 2 em X/B/Y/L (direcional) e R (botão); Game B no SELECT
P2 = {'IPT_JOYSTICK_UP': 'X', 'IPT_JOYSTICK_DOWN': 'B', 'IPT_JOYSTICK_LEFT': 'Y', 'IPT_JOYSTICK_RIGHT': 'L',
      'IPT_BUTTON1': 'R'}

CPU = dict(sm5a='Cpu::SM5A', sm510='Cpu::SM510', sm511='Cpu::SM511', sm512='Cpu::SM512')
LAYOUT = dict(common=0, dualv=1, dualh=2, tripleh=3)

out = []
out.append('// Gerado por gerar_tabela.py a partir do hh_sm510.cpp do MAME (BSD-3-Clause). Não editar à mão.')
out.append('// {nome, título, pai, CPU, máscara R, bit R do som, disposição, programa, CRC, melodia, CRC,')
out.append('//  telas, linhas de teclas, linha fixa, valores base das portas, atraso do LCD, entradas}')
erros = 0
for g in games:
    prog, melody, svgs = rom_def(g["name"])
    if prog is None: print("sem prog", g["name"], file=sys.stderr); continue
    cpu, layout, rbit = machine_def(g['cls'], g['machine'])
    cd = class_def(g['cls'])
    ports, order = ports_def(g['inp'])
    nin = len([p for p in order if re.fullmatch(r'IN\.\d', p)])
    if cd['lines'] is not None:
        lines, fixed = cd['lines'], -1
    elif cd['fixed_last']:
        lines, fixed = nin - 1, nin - 1
    else:
        lines, fixed = nin, -1
    dois = any(pl == 2 for p in ports.values() for (_, _, _, pl) in p['bits'])
    ents = []
    base = [0] * 11
    pres = 0
    for p in order:
        if p == 'ACL':
            continue  # ACL = reset do aparelho
        if re.fullmatch(r'IN\.\d', p):
            idx = int(p[3:])
        elif p == 'BA':
            idx = 9
        elif p == 'B':
            idx = 10
        else:
            print('// aviso: porta desconhecida %s em %s' % (p, g['name']), file=sys.stderr)
            continue
        pres |= 1 << idx
        base[idx] = ports[p]['base']
        for (mask, ipt, low, pl) in ports[p]['bits']:
            tab = P2 if pl == 2 else P1
            if dois and pl == 1 and ipt == 'IPT_START2':
                key = 'SELECT'
            elif dois and pl == 1 and ipt in ('IPT_SELECT', 'IPT_SERVICE2'):
                continue  # Time e Alarm ficam sem tecla nos jogos de 2 jogadores
            else:
                key = tab.get(ipt)
            if key is None:
                print('// aviso: %s sem tecla em %s' % (ipt, g['name']), file=sys.stderr)
                erros += 1
                continue
            ents.append('{%d, 0x%X, 0x%X, %d}' % (idx, mask, K[key], 1 if low else 0))
    lay = LAYOUT[layout]
    sv = ', '.join('"%s"' % s[0] for s in svgs[:2])
    if len(svgs) == 1:
        sv += ', nullptr'
    if len(svgs) > 2:
        print('// aviso: mais de 2 telas em %s' % g['name'], file=sys.stderr)
    mel = ('"%s", 0x%08X' % (melody[0], melody[2])) if melody else 'nullptr, 0'
    title = g['title'].replace('Game & Watch: ', '')
    parent = g['parent'] if g['parent'] != '0' else ''
    out.append('{"%s", "%s", "%s", %s, %d, %d, %d, "%s", 0x%08X, %s, {%s}, %d, %d, {%s}, 0x%X, %d, %d, {%s}},' % (
        g['name'], title, parent, CPU[cpu], -1 if cpu == 'sm5a' else 2, rbit, lay, prog[0], prog[2], mel, sv,
        lines, fixed, ', '.join('0x%X' % v for v in base), pres, cd['pivot'], cd['dlen'], ', '.join(ents)))
print('\n'.join(out))
if erros:
    sys.exit(1)
