# Monta o que o t9_ps1int precisa além do t9_ps1_build.py (que já montou as bibliotecas do PSn00bSDK):
#  - as variantes dos testes do MDEC do ps1-tests que têm mais de um makefile (frame 15/24 bits com e sem DMA na
#    saída, movie 15/24 bits);
#  - o programa do disco de teste (t9_ps1int_jogo.c) como JOGO.EXE.
# Uso: python3 -I t9_ps1int_build.py pasta-do-ps1 saida   (pasta com build/lib, ps1-tests/ e psn00bsdk/)
import importlib.util, os, subprocess, sys

sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT, OUT = sys.argv[1], sys.argv[2]
sys.argv = [sys.argv[0], ROOT]  # o t9_ps1_build lê a pasta de sys.argv[1]
spec = importlib.util.spec_from_file_location('t9_ps1_build', os.path.join(HERE, 't9_ps1_build.py'))
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)

LIB = ROOT + '/build/lib'
LIBS = [LIB + '/lib' + n + '.a' for n in ['c', 'psxgpu', 'psxapi', 'psxgte', 'psxetc', 'psxsio', 'psxspu', 'psxcd', 'extra']]
COMMON = LIB + '/libcommon.a'
INC = [b.SDK + '/include']
libgcc = subprocess.run(['mipsel-linux-gnu-gcc', '-print-libgcc-file-name'], capture_output=True, text=True).stdout.strip()
os.makedirs(OUT + '/obj', exist_ok=True)


def program(name, srcs, incs, defines, common=True):
    saved = list(b.CFLAGS), list(b.CXXFLAGS)
    b.CFLAGS += defines
    b.CXXFLAGS += defines
    objs, ok = [], True
    for f in srcs:
        o = f'{OUT}/obj/{name}_{os.path.basename(f)}.o'
        ok = b.compile_one(f, o, incs) and ok
        objs.append(o)
    b.CFLAGS[:], b.CXXFLAGS[:] = saved
    if not ok:
        return False
    elf = f'{OUT}/obj/{name}.elf'
    if not b.run(['mipsel-linux-gnu-ld', '-Ttext=0x80010000', '--gc-sections', '-e', '_start', '-o', elf] + objs +
                 ['--start-group'] + ([COMMON] if common else []) + LIBS + [libgcc, '--end-group']):
        return False
    b.elf2exe(elf, f'{OUT}/{name}.exe')
    return True


n = 0
T = ROOT + '/ps1-tests'
# frame: o tamanho do quadro vem de uma variável global iniciada por outra (inicialização dinâmica do C++, que o
# começo do programa do PSn00bSDK de 2020 não roda com o ligador de hoje): vira sizeof, que é constante
d = T + '/mdec/frame/'
src = open(d + 'main.cpp').read().replace('size_t frame_len = sunset_mdec_len;', 'size_t frame_len = sizeof(sunset_mdec);')
open(OUT + '/obj/frame_main.cpp', 'w').write(src)
for depth, dma in [(15, False), (15, True), (24, False), (24, True)]:
    defs = [f'-DCOLOR_DEPTH={depth}'] + (['-DUSE_MDECOUT_DMA'] if dma else [])
    n += program(f'mdec-frame-{depth}bit' + ('-dma' if dma else ''), [OUT + '/obj/frame_main.cpp'], INC + [T + '/common', d], defs)
# 4bit e 8bit: o psx.log do console foi gravado com blocos de 8 palavras na leitura por DMA (o código de hoje usa
# 0x20, que com 8 ou 16 palavras dá zero blocos)
mdec = open(T + '/common/mdec.cpp').read().replace('int BS = 0x20;', 'int BS = 0x8;')
open(OUT + '/obj/mdec_bs8.cpp', 'w').write(mdec)
for t in ('4bit', '8bit'):
    d = T + '/mdec/' + t + '/'
    n += program('mdec-' + t, [d + 'main.cpp', OUT + '/obj/mdec_bs8.cpp'], INC + [T + '/common', d], [])
for depth in (15, 24):
    d = T + '/mdec/movie/'
    n += program(f'mdec-movie-{depth}bit', [d + 'main.cpp'], INC + [T + '/common', d], ['-DUSE_24BIT'] if depth == 24 else [])
for t in ['mdec/frame', 'mdec/movie']:
    for v in ['15bit', '24bit']:
        if os.path.exists(f'{T}/{t}/vram-{v}.png'):
            b.png2ppm(f'{T}/{t}/vram-{v}.png', f'{OUT}/{t.replace("/", "-")}-{v}.ppm')
n += program('jogo', [HERE + '/t9_ps1int_jogo.c'], INC, [], common=False)
print(f'{n} programas montados em {OUT}')
