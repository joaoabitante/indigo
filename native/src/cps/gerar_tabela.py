#!/usr/bin/env python3
# Gera src/cps/tabela.inc (tabela de jogos do CPS1/CPS2) a partir dos fontes do MAME (licença BSD-3-Clause):
#   src/mame/capcom/cps1.cpp, cps1_v.cpp e cps2.cpp
# Uso: python3 gerar_tabela.py <pasta src/mame/capcom do MAME> > tabela.inc
#
# Só entram dados de fato (nomes de arquivos, CRC-32, tamanhos, posições na memória, configuração do CPS-B e dos
# PALs de cada placa, valores padrão das chaves DIP); nenhum código do MAME é copiado.
import re
import sys

base = sys.argv[1]
cps1 = open(base + '/cps1.cpp', encoding='utf-8').read()
cps1v = open(base + '/cps1_v.cpp', encoding='utf-8').read()
cps2 = open(base + '/cps2.cpp', encoding='utf-8').read()


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    return s


def num(x):
    x = x.strip()
    return int(x, 0)


# ---------------------------------------------------------------- GAME(...)
def parse_games(src, kind):
    out = []
    for m in re.finditer(r'^GAME\(\s*([^\n]*)\)\s*(?://.*)?$', src, re.M):
        line = m.group(1)
        # campos separados por vírgula, com strings entre aspas
        parts = re.findall(r'\s*("(?:[^"\\]|\\.)*"|[^,]+)\s*,?', line)
        parts = [p.strip() for p in parts]
        if len(parts) < 11:
            continue
        year, name, parent, machine, inp, cls, init, rot, company, title = parts[:10]
        out.append(dict(kind=kind, name=name, parent=parent, machine=machine, input=inp, init=init, rot=rot,
                        company=company.strip('"'), title=title.strip('"')))
    return out


# ---------------------------------------------------------------- ROM_START ... ROM_END
REGIONS = {'maincpu': 0, 'gfx': 1, 'audiocpu': 2, 'oki': 3, 'qsound': 4, 'key': 5, 'stars': 6}
MODES = {'ROM_LOAD': 0, 'ROM_LOAD16_BYTE': 1, 'ROM_LOAD16_WORD_SWAP': 2, 'ROM_LOAD64_WORD': 3,
         'ROM_LOAD64_BYTE': 4, 'ROM_LOAD32_BYTE': 5, 'ROM_LOAD16_WORD': 6}
FILL, COPY = 7, 8


def parse_roms(src):
    src = strip_comments(src)
    roms = {}
    for m in re.finditer(r'ROM_START\(\s*(\w+)\s*\)(.*?)ROM_END', src, re.S):
        name, body = m.group(1), m.group(2)
        regsize = {}
        loads = []
        region = None
        last = None  # (mode, fileidx(name,crc,size), srcEnd)
        ok = True
        for st in re.finditer(r'(ROM_\w+)\s*\((.*?)\)\s*(?=ROM_|$)', body, re.S):
            mac, args = st.group(1), st.group(2)
            if mac in ('ROM_REGION', 'ROM_REGION16_BE'):
                a = [x.strip() for x in args.split(',')]
                size = a[0]
                size = 0x400000 if size == 'CODE_SIZE' else (0x50000 if size == 'QSOUND_SIZE' else num(size))
                tag = a[1].strip('"')
                region = REGIONS.get(tag)
                if region is not None:
                    regsize[region] = size
                last = None
                continue
            if region is None:
                continue
            if mac in MODES:
                mm = re.match(r'\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(.*)$', args, re.S)
                if not mm:
                    ok = False
                    continue
                fname, off, size, rest = mm.group(1), num(mm.group(2)), num(mm.group(3)), mm.group(4)
                cm = re.search(r'CRC\((\w+)\)', rest)
                if not cm or 'NO_DUMP' in rest:
                    ok = False if region in (0, 1, 2, 5) else ok
                    last = None
                    continue
                crc = int(cm.group(1), 16)
                f = (fname, crc, size)
                loads.append((region, MODES[mac], f, off, size, 0))
                last = (MODES[mac], f, size)
            elif mac == 'ROM_CONTINUE':
                a = [num(x) for x in args.split(',')]
                if last is None:
                    continue
                mode, f, pos = last
                loads.append((region, mode, f, a[0], a[1], pos))
                last = (mode, f, pos + a[1])
            elif mac == 'ROM_RELOAD':
                a = [num(x) for x in args.split(',')]
                if last is None:
                    continue
                mode, f, pos = last
                loads.append((region, mode, f, a[0], a[1], 0))
                last = (mode, f, a[1])
            elif mac == 'ROM_IGNORE':
                if last is None:
                    continue
                mode, f, pos = last
                last = (mode, f, pos + num(args))
            elif mac == 'ROM_FILL':
                a = [num(x) for x in args.split(',')]
                loads.append((region, FILL, ('', a[2] & 0xFF, 0), a[0], a[1], 0))
            elif mac == 'ROM_COPY':
                a = [x.strip() for x in args.split(',')]
                src_reg = REGIONS.get(a[0].strip('"'))
                if src_reg is None:
                    ok = False
                    continue
                loads.append((region, COPY, ('', src_reg, 0), num(a[2]), num(a[3]), num(a[1])))
        roms[name] = dict(regsize=regsize, loads=loads, ok=ok)
    return roms


# ---------------------------------------------------------------- CPS-B e PALs (cps1_v.cpp)
def parse_cpsb(src):
    src_nc = strip_comments(src)
    defs = {}
    for m in re.finditer(r'#define\s+(CPS_B_\w+|HACK_B_\w+)\s+(.*)', src_nc):
        txt = m.group(2).replace('__not_applicable__', '-1,-1,-1,-1,-1,-1,-1')
        vals = [int(x, 0) for x in re.findall(r'-?0x[0-9a-fA-F]+|-?\d+', txt)]
        defs[m.group(1)] = vals  # 2 + 7 + 1 + 4 + 1 + 5 = 20 valores
    mappers = {}
    for m in re.finditer(r'#define\s+mapper_(\w+)\s*\{([^}]*)\}\s*,\s*mapper_\w+_table', src_nc):
        sizes = [int(x, 0) for x in m.group(2).split(',')]
        tm = re.search(r'gfx_range\s+mapper_' + m.group(1) + r'_table\[\]\s*=\s*\{(.*?)\n\};', src_nc, re.S)
        ranges = []
        if tm:
            for r in re.finditer(r'\{\s*([A-Z0-9_| ]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*\}', tm.group(1)):
                t = 0
                for part in r.group(1).split('|'):
                    part = part.strip()
                    t |= {'GFXTYPE_SPRITES': 1, 'GFXTYPE_SCROLL1': 2, 'GFXTYPE_SCROLL2': 4, 'GFXTYPE_SCROLL3': 8,
                          'GFXTYPE_STARS': 16}[part]
                ranges.append((t, int(r.group(2), 16), int(r.group(3), 16), int(r.group(4))))
        mappers[m.group(1)] = (sizes, ranges)
    table = {}
    tm = re.search(r'cps1_config_table\[\]\s*=\s*\{(.*?)\n\};', src_nc, re.S)
    for r in re.finditer(r'\{\s*"(\w+)"\s*,\s*(\w+)\s*,\s*mapper_(\w+)\s*((?:,\s*\w+\s*)*)\}', tm.group(1)):
        extra = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', r.group(4))]
        extra += [0] * (4 - len(extra))
        table[r.group(1)] = (r.group(2), r.group(3), extra[:4])
    return defs, mappers, table


# ---------------------------------------------------------------- chaves DIP padrão (CPS1)
def parse_dips(src):
    src_nc = strip_comments(src)
    macros = {}
    for m in re.finditer(r'#define\s+(CPS1_\w+)\(\w+\)\s*\\\n((?:.*\\\n)*.*)', src_nc):
        macros[m.group(1)] = m.group(2)
    ports = {}
    for m in re.finditer(r'INPUT_PORTS_START\(\s*(\w+)\s*\)(.*?)INPUT_PORTS_END', src_nc, re.S):
        ports[m.group(1)] = m.group(2)

    cache = {}

    def resolve(name):
        if name in cache:
            return cache[name]
        body = ports.get(name, '')
        for mac, txt in macros.items():
            body = re.sub(mac + r'\([^)]*\)', txt.replace('\\\n', '\n'), body)
        vals = {}
        cur = None
        for st in re.finditer(r'(PORT_INCLUDE|PORT_START|PORT_MODIFY|PORT_DIPNAME|PORT_DIPUNUSED_DIPLOC|PORT_DIPUNUSED|PORT_SERVICE_DIPLOC|PORT_SERVICE)\s*\(([^;\n]*?)\)(?=\s|$)', body):
            mac, args = st.group(1), st.group(2)
            if mac == 'PORT_INCLUDE':
                inc = resolve(args.strip())
                for k, v in inc.items():
                    vals[k] = dict(v)
            elif mac in ('PORT_START', 'PORT_MODIFY'):
                cur = args.strip().strip('"')
                if mac == 'PORT_START' or cur not in vals:
                    vals[cur] = {}
            elif cur and cur.startswith('DSW'):
                a = re.findall(r'0x[0-9a-fA-F]+|\d+', args)
                if mac.startswith('PORT_SERVICE'):
                    mask = int(a[0], 0)
                    vals[cur][mask] = mask  # serviço desligado (ativo em 0)
                elif len(a) >= 2:
                    vals[cur][int(a[0], 0)] = int(a[1], 0)
        cache[name] = vals
        return vals

    def dsw(name):
        v = resolve(name)
        res = []
        for p in ('DSWA', 'DSWB', 'DSWC'):
            x = 0xFF
            for mask, d in v.get(p, {}).items():
                x = (x & ~mask) | (d & mask)
            res.append(x & 0xFF)
        has6 = False
        body = ports.get(name, '')
        # 6 botões: a porta de entrada inclui cps1_6b direta ou indiretamente
        seen = set()

        def inc6(n):
            if n in seen:
                return False
            seen.add(n)
            if n == 'cps1_6b':
                return True
            b = ports.get(n, '')
            return any(inc6(i.strip()) for i in re.findall(r'PORT_INCLUDE\(\s*(\w+)\s*\)', b))
        has6 = inc6(name)
        return res, has6
    return dsw


games = parse_games(cps1, 1) + parse_games(cps2, 2)
roms = parse_roms(cps1)
roms.update(parse_roms(cps2))
cpsb, mappers, config = parse_cpsb(cps1v)
dsw = parse_dips(cps1)

CPS1_MACHINES = {'cps1_10MHz': 0, 'cps1_12MHz': 1, 'forgottn': 0}
CPS1_INITS = {'empty_init', 'init_sf2ee', 'init_sf2thndr'}
CPS2_MACHINES = {'cps2', 'cps2comm', 'dead_cps2', 'dead_cps2comm'}
CPS2_INITS = {'init_cps2', 'init_singbrd'}

sel = []
for g in games:
    if 'bootleg' in g['company'].lower() or 'hack' in g['company'].lower():
        continue
    r = roms.get(g['name'])
    if not r or not r['ok']:
        continue
    if g['kind'] == 1:
        if g['machine'] not in CPS1_MACHINES or g['init'] not in CPS1_INITS:
            continue
        if g['name'] not in config:
            continue
    else:
        if g['machine'] not in CPS2_MACHINES or g['init'] not in CPS2_INITS:
            continue
        if 5 not in r['regsize']:
            continue
    sel.append(g)

files = {}
file_list = []


def fidx(f):
    if f not in files:
        files[f] = len(file_list)
        file_list.append(f)
    return files[f]


cpsb_names = sorted({config[g['name']][0] for g in sel if g['kind'] == 1} | {'CPS_B_21_DEF'})
mapper_names = sorted({config[g['name']][1] for g in sel if g['kind'] == 1} | {'cps2'})


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


out = []
out.append('// Gerado por gerar_tabela.py a partir dos fontes do MAME (cps1.cpp, cps1_v.cpp, cps2.cpp; BSD-3-Clause).')
out.append('// Não editar à mão.')
out.append('')
out.append('static const CpsbConfig kCpsb[] = {')
for n in cpsb_names:
    v = cpsb[n]
    assert len(v) == 20, (n, v)
    out.append('    {%s, {%s}, %d, {%s}},  // %s' % (', '.join(str(x) for x in v[:10]), ', '.join(str(x) for x in v[10:14]),
                                                v[14], ', '.join(str(x) for x in v[15:20]), n))
out.append('};')
out.append('')
rng = []
out.append('static const GfxRange kRanges[] = {')
mapper_rows = []
for n in mapper_names:
    sizes, ranges = mappers[n]
    first = len(rng)
    for r in ranges:
        rng.append(r)
        out.append('    {%d, 0x%05x, 0x%05x, %d},' % r)
    mapper_rows.append((n, sizes, first, len(ranges)))
out.append('};')
out.append('')
out.append('static const GfxMapper kMappers[] = {')
for n, sizes, first, cnt in mapper_rows:
    out.append('    {{0x%x, 0x%x, 0x%x, 0x%x}, %d, %d},  // %s' % (sizes[0], sizes[1], sizes[2], sizes[3], first, cnt, n))
out.append('};')
out.append('')

load_rows = []
game_rows = []
for g in sel:
    r = roms[g['name']]
    first = len(load_rows)
    for (region, mode, f, dst, ln, srcoff) in r['loads']:
        if mode in (FILL, COPY):
            load_rows.append((region, mode, f[1], dst, ln, srcoff))
        else:
            load_rows.append((region, mode, fidx(f), dst, ln, srcoff))
    regs = [r['regsize'].get(i, 0) for i in range(7)]
    rot = {'ROT0': 0, 'ROT90': 1, 'ROT180': 2, 'ROT270': 3}.get(g['rot'], 0)
    parent = g['parent'] if g['parent'] != '0' else ''
    if g['kind'] == 1:
        cb, mp, extra = config[g['name']]
        d, six = dsw(g['input'])
        flags = 0
        if CPS1_MACHINES[g['machine']] == 1:
            flags |= 1  # 68000 a 12 MHz
        if g['init'] == 'init_sf2ee':
            flags |= 2  # CPS-B em 0x8001c0
        if g['init'] == 'init_sf2thndr':
            flags |= 4  # CPS-B espelhado em 0x8001c0
        if six:
            flags |= 8
        cbi, mpi = cpsb_names.index(cb), mapper_names.index(mp)
    else:
        extra = [0, 0, 0, 0]
        d = [0xFF, 0xFF, 0xFF]
        flags = 0
        if g['input'] in ('cps2_2p6b', 'cps2_2p6bt', 'cybots'):
            flags |= 8
        if g['input'].startswith('cps2_4p') or g['input'].startswith('cps2_3p'):
            flags |= 16
        if g['init'] == 'init_singbrd':
            flags |= 32
        cbi, mpi = cpsb_names.index('CPS_B_21_DEF'), mapper_names.index('cps2')
    game_rows.append('    {%s, %s, %s, %d, %d, %d, {%s}, %d, %d, %d, %d, %d, %d, %d, {0x%02x, 0x%02x, 0x%02x}},' % (
        cstr(g['name']), cstr(parent), cstr(g['title']), g['kind'], rot, flags,
        ', '.join('0x%x' % x for x in regs), first, len(load_rows) - first, cbi, mpi, extra[0], extra[1], extra[2],
        d[0], d[1], d[2]))

out.append('static const RomFile kFiles[] = {')
for (n, crc, size) in file_list:
    out.append('    {%s, 0x%08x, 0x%x},' % (cstr(n), crc, size))
out.append('};')
out.append('')
out.append('static const RomLoad kLoads[] = {')
for row in load_rows:
    out.append('    {%d, %d, %d, 0x%x, 0x%x, 0x%x},' % row)
out.append('};')
out.append('')
out.append('static const GameDef kGames[] = {')
out.extend(game_rows)
out.append('};')
print('\n'.join(out))
print('// %d jogos, %d arquivos, %d cargas' % (len(game_rows), len(file_list), len(load_rows)), file=sys.stderr)
