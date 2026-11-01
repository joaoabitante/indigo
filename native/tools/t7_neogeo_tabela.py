# Gera native/src/neogeo/sets.inc (tabela de pacotes do Neo Geo) a partir da lista de software do MAME,
# hash/neogeo.xml (licença CC0-1.0): para cada jogo, o nome do pacote, o pai (clones), o tipo de cartucho
# (proteções/cifras), o título, o tamanho de cada área (programa, camada fixa, Z80, Z80 cifrado, ADPCM-A, ADPCM-B,
# sprites) e cada arquivo: área, modo de carga, CRC-32, posição, tamanho e nome.
# uso: python3 t7_neogeo_tabela.py caminho/para/neogeo.xml saida/sets.inc
import sys
import xml.etree.ElementTree as ET

AREAS = {'maincpu': 0, 'fixed': 1, 'audiocpu': 2, 'audiocrypt': 3, 'ymsnd:adpcma': 4, 'ymsnd:adpcmb': 5,
         'sprites': 6}
FLAGS = {None: 0, 'load16_word_swap': 1, 'load16_byte': 2, 'continue': 3, 'fill': 4, 'ignore': 5,
         'load32_byte': 6, 'load32_word_swap': 7, 'load16_word': 8}


def num(s):
    return int(s, 0) if s else 0


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


root = ET.parse(sys.argv[1]).getroot()
roms = []
sets = []
for sw in root.findall('software'):
    name = sw.get('name')
    parent = sw.get('cloneof') or ''
    title = (sw.findtext('description') or name).strip()
    part = sw.find('part')
    slot = 'rom'
    for f in part.findall('feature'):
        if f.get('name') == 'slot':
            slot = f.get('value')
    sizes = [0] * 7
    first = len(roms)
    for da in part.findall('dataarea'):
        area = AREAS.get(da.get('name'))
        if area is None:
            continue
        sizes[area] = num(da.get('size'))
        for r in da.findall('rom'):
            flag = FLAGS[r.get('loadflag')]
            roms.append((area, flag, int(r.get('crc') or '0', 16), num(r.get('offset')), num(r.get('size')),
                         num(r.get('value')), r.get('name') or ''))
    sets.append((name, parent, slot, title, sizes, first, len(roms) - first))

out = open(sys.argv[2], 'w', encoding='utf-8')
out.write('// Gerado por native/tools/t7_neogeo_tabela.py a partir da lista de software do MAME (hash/neogeo.xml,\n')
out.write('// licença CC0-1.0). Não editar à mão.\n')
out.write('// clang-format off\n')
out.write('static const SetRom SET_ROMS[] = {\n')
for a, f, crc, off, size, val, n in roms:
    out.write('  {%d, %d, 0x%08x, 0x%x, 0x%x, %d, %s},\n' % (a, f, crc, off, size, val, cstr(n)))
out.write('};\n\n')
out.write('static const SetInfo SET_INFO[] = {\n')
for name, parent, slot, title, sizes, first, count in sets:
    out.write('  {%s, %s, %s, %s, {%s}, %d, %d},\n' % (cstr(name), cstr(parent), cstr(slot), cstr(title),
                                                    ', '.join('0x%x' % s for s in sizes), first, count))
out.write('};\n')
out.write('// clang-format on\n')
print(len(sets), 'pacotes,', len(roms), 'arquivos')
