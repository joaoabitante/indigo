# Gera pacotes .zip "falsos" de jogos comerciais do Neo Geo: os nomes e tamanhos dos arquivos vêm da lista de
# software do MAME (hash/neogeo.xml), o conteúdo é aleatório. Não testam se o jogo roda (não há ROM de verdade
# aqui), mas fazem o núcleo passar por todo o caminho de carga de cada tipo de cartucho (tabela, ROM_CONTINUE,
# decifração CMC42/CMC50/SMA/PCM2/PVC/KOF98/KOF2002, troca de bancos) e rodar alguns quadros com sanitizers.
# Também gera um clone "split" (só os arquivos próprios) e um pirata sem suporte, que devem dar erro com mensagem.
# uso: python3 t7_neogeo_falsos.py neogeo.xml pasta_saida
import os
import random
import sys
import xml.etree.ElementTree as ET
import zipfile

root = ET.parse(sys.argv[1]).getroot()
out = sys.argv[2]
os.makedirs(out, exist_ok=True)
sw = {s.get('name'): s for s in root.findall('software')}

# pacote, resultado esperado (ok/erro), placa esperada (número de indigo::neogeo::Board)
SETS = [
    ('mslug', 'ok', 0), ('kof98', 'ok', 2), ('mslugx', 'ok', 3), ('fatfury2', 'ok', 1), ('kof99', 'ok', 4),
    ('garou', 'ok', 4), ('garouh', 'ok', 4), ('mslug3', 'ok', 4), ('mslug3a', 'ok', 4), ('kof2000', 'ok', 4),
    ('zupapa', 'ok', 0), ('sengoku3', 'ok', 0), ('kof2001', 'ok', 0), ('mslug4', 'ok', 0), ('rotd', 'ok', 0),
    ('pnyaa', 'ok', 0), ('kof2002', 'ok', 0), ('matrim', 'ok', 0), ('samsho5', 'ok', 0), ('samsh5sp', 'ok', 0),
    ('mslug5', 'ok', 5), ('svc', 'ok', 5), ('kof2003', 'ok', 5), ('kof2003h', 'ok', 5), ('jockeygp', 'ok', 6),
    ('svcboot', 'erro', 0),
]
rng = random.Random(2026)


def files(name):
    s = sw[name]
    for da in s.find('part').findall('dataarea'):
        for r in da.findall('rom'):
            if r.get('name'):
                yield r.get('name'), int(r.get('size'), 0)


with open(os.path.join(out, 'lista.txt'), 'w') as lst:
    for name, expect, board in SETS:
        path = os.path.join(out, name + '.zip')
        if not os.path.exists(path):
            with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED) as z:
                seen = set()
                for fn, size in files(name):
                    if fn in seen:
                        continue
                    seen.add(fn)
                    z.writestr(fn, rng.randbytes(size))
        lst.write('%s %s %d\n' % (name, expect, board))
    # clone "split": só os arquivos que diferem do pai
    clone, parent = 'kof99h', 'kof99'
    path = os.path.join(out, clone + '.zip')
    if not os.path.exists(path):
        pf = set(f for f, _ in files(parent))
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED) as z:
            for fn, size in files(clone):
                if fn not in pf:
                    z.writestr(fn, rng.randbytes(size))
    lst.write('%s erro 0\n' % clone)
