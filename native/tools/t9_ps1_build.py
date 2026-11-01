# Monta os testes do ps1-tests (JaCzekanski) com o gcc mipsel do sistema, para o t9_ps1.sh. Os binários prontos
# ficam nas "releases" do GitHub, fora do alcance daqui; então a biblioteca do PSn00bSDK da época (2020) e os
# testes são compilados chamando o compilador diretamente (sem rodar os makefiles dos repositórios baixados).
# Diferenças do gcc 7.4 original, ajustadas aqui:
#  - -fno-delayed-branch: o gcc novo põe as instruções de teste (swc2...) no atraso dos desvios, e o tratador de
#    exceções do teste não sabe voltar delas;
#  - -fno-strict-aliasing: o gcc novo apaga escritas nas estruturas dos primitivos lidas por ponteiro uint32_t;
#  - ponto flutuante por software (soft-fp do código do GCC, baixado pelo t7_32x.sh): a libgcc do mipsel-linux-gnu
#    usa a FPU e a instrução clz, que o R3000A não tem.
# Também converte os vram.png (VRAM do console de verdade) para .ppm, para o teste em C++ não ler PNG.
# Uso: python3 -I t9_ps1_build.py pasta   (pasta com ps1-tests/ e psn00bsdk/; saída em pasta/build/exe)
import glob, os, struct, subprocess, sys, zlib

ROOT = sys.argv[1]
SDK = ROOT + '/psn00bsdk/libpsn00b'
TESTS = ROOT + '/ps1-tests'
OUT = ROOT + '/build'
GCC_SRC = os.environ.get('GCC_SRC', os.path.dirname(ROOT) + '/32x/tc/src-gcc')
CFLAGS = ['-march=r3000', '-mtune=r3000', '-mabi=32', '-mno-abicalls', '-fno-pic', '-G0', '-mno-gpopt',
          '-fno-stack-protector', '-fno-asynchronous-unwind-tables', '-msoft-float', '-O2', '-fno-builtin',
          '-fdata-sections', '-ffunction-sections', '-D__PLAYSTATION__', '-w', '-fno-delayed-branch',
          '-fno-strict-aliasing']
CXXFLAGS = CFLAGS + ['-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-std=c++17']
ASFLAGS = ['-march=r3000', '-non_shared', '-mno-shared', '-msoft-float']
EXTRA_S = '''	.set noreorder
	.text
	.global FlushCache
FlushCache:
	li	$t2, 0xA0
	jr	$t2
	li	$t1, 0x44
'''
EXTRA_C = '''int __clzsi2(unsigned int x) { int n = 0; if (!x) return 32; while (!(x & 0x80000000u)) { x <<= 1; n++; } return n; }
int __ctzsi2(unsigned int x) { int n = 0; if (!x) return 32; while (!(x & 1)) { x >>= 1; n++; } return n; }
'''


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print('ERRO:', ' '.join(cmd)); print(r.stderr[-2000:])
        return False
    return True


def compile_one(f, o, incs):
    if f.endswith('.s'):
        return run(['mipsel-linux-gnu-as'] + ASFLAGS + ['-I' + i for i in incs] + [f, '-o', o])
    if f.endswith('.cpp') or f.endswith('.cxx'):
        return run(['mipsel-linux-gnu-g++'] + CXXFLAGS + ['-I' + i for i in incs] + ['-x', 'c++', '-c', f, '-o', o])
    return run(['mipsel-linux-gnu-gcc'] + CFLAGS + ['-I' + i for i in incs] + ['-c', f, '-o', o])


def build_lib(name, srcdir, incs):
    objdir = f'{OUT}/obj/{name}'
    os.makedirs(objdir, exist_ok=True)
    objs = []
    for ext in ('c', 'cpp', 'cxx', 's'):
        for f in sorted(glob.glob(f'{srcdir}/**/*.{ext}', recursive=True)):
            o = objdir + '/' + os.path.relpath(f, srcdir).replace('/', '_') + '.o'
            if compile_one(f, o, incs):
                objs.append(o)
    lib = f'{OUT}/lib/lib{name}.a'
    os.makedirs(OUT + '/lib', exist_ok=True)
    if os.path.exists(lib):
        os.remove(lib)
    run(['mipsel-linux-gnu-ar', 'crs', lib] + objs)
    return lib


def softfp():
    g = GCC_SRC + '/libgcc'
    objs = []
    os.makedirs(OUT + '/obj/softfp', exist_ok=True)
    for n in ['addsf3', 'subsf3', 'mulsf3', 'divsf3', 'negsf2', 'eqsf2', 'lesf2', 'gesf2', 'unordsf2', 'fixsfsi',
              'fixunssfsi', 'floatsisf', 'floatunsisf', 'extendsfdf2', 'truncdfsf2', 'adddf3', 'subdf3', 'muldf3',
              'divdf3', 'negdf2', 'eqdf2', 'ledf2', 'gedf2', 'unorddf2', 'fixdfsi', 'fixunsdfsi', 'floatsidf',
              'floatunsidf', 'fixsfdi', 'fixdfdi', 'floatdisf', 'floatdidf']:
        o = OUT + '/obj/softfp/' + n + '.o'
        if run(['mipsel-linux-gnu-gcc'] + CFLAGS + ['-I' + g + '/soft-fp', '-I' + g + '/config/mips', '-I' + g,
                '-I' + GCC_SRC + '/include', '-DFP_NO_EXCEPTIONS', '-c', g + '/soft-fp/' + n + '.c', '-o', o]):
            objs.append(o)
    open(OUT + '/obj/extra.c', 'w').write(EXTRA_C)
    open(OUT + '/obj/extra.s', 'w').write(EXTRA_S)
    if compile_one(OUT + '/obj/extra.c', OUT + '/obj/extra_c.o', []):
        objs.append(OUT + '/obj/extra_c.o')
    if compile_one(OUT + '/obj/extra.s', OUT + '/obj/extra_s.o', []):
        objs.append(OUT + '/obj/extra_s.o')
    lib = OUT + '/lib/libextra.a'
    if os.path.exists(lib):
        os.remove(lib)
    run(['mipsel-linux-gnu-ar', 'crs', lib] + objs)
    return lib


def elf2exe(elf, exe):
    binf = exe + '.bin'
    run(['mipsel-linux-gnu-objcopy', '-O', 'binary', '-j', '.text', '-j', '.rodata', '-j', '.data', '-j', '.sdata',
         '-j', '.ctors', '-j', '.dtors', '-j', '.init', '-j', '.fini', elf, binf])
    out = subprocess.run(['mipsel-linux-gnu-readelf', '-h', elf], capture_output=True, text=True).stdout
    entry = int([l for l in out.splitlines() if 'Entry point' in l][0].split()[-1], 16)
    data = open(binf, 'rb').read()
    data += bytes((-len(data)) % 2048)
    hdr = bytearray(2048)
    hdr[0:8] = b'PS-X EXE'
    struct.pack_into('<IIIIIIIIIIII', hdr, 0x10, entry, 0, 0x80010000, len(data), 0, 0, 0, 0, 0x801FFFF0, 0, 0, 0)
    open(exe, 'wb').write(bytes(hdr) + data)
    os.remove(binf)


def png2ppm(src, dst):
    # PNG RGB/RGBA de 8 bits sem entrelaçamento (os vram.png do ps1-tests)
    d = open(src, 'rb').read()
    pos, idat, w, pal = 8, b'', 0, b''
    while pos < len(d):
        n = struct.unpack('>I', d[pos:pos + 4])[0]
        t = d[pos + 4:pos + 8]
        c = d[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', c[:10])
        elif t == b'PLTE':
            pal = c
        elif t == b'IDAT':
            idat += c
        pos += 12 + n
    if (depth != 8 and ctype != 3) or ctype not in (2, 3, 6):
        return False
    bpp = {2: 3, 3: 1, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp if ctype != 3 else (w * depth + 7) // 8
    prev = bytearray(stride)
    out = bytearray()
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if ctype == 3:
            per = 8 // depth
            for x in range(w):
                i = (line[x // per] >> ((per - 1 - x % per) * depth)) & ((1 << depth) - 1)
                out += pal[i * 3:i * 3 + 3]
        else:
            for x in range(w):
                out += line[x * bpp:x * bpp + 3]
    open(dst, 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + bytes(out))
    return True


def main():
    inc = [SDK + '/include']
    libs = [build_lib(l if l != 'libc' else 'c', SDK + '/' + l, inc)
            for l in ['libc', 'psxgpu', 'psxapi', 'psxgte', 'psxetc', 'psxsio', 'psxspu', 'psxcd']]
    common = build_lib('common', TESTS + '/common', inc + [TESTS + '/common'])
    libs.append(softfp())
    libgcc = subprocess.run(['mipsel-linux-gnu-gcc', '-print-libgcc-file-name'], capture_output=True, text=True).stdout.strip()
    os.makedirs(OUT + '/exe', exist_ok=True)
    os.makedirs(OUT + '/vram', exist_ok=True)
    n = 0
    for area in ['cpu', 'gpu', 'dma', 'timers', 'gte', 'input', 'spu', 'mdec', 'cdrom']:
        for d in sorted(glob.glob(f'{TESTS}/{area}/*/')):
            srcs = sorted(glob.glob(d + '*.c') + glob.glob(d + '*.cpp') + glob.glob(d + '*.s'))
            if not srcs:
                continue
            name = area + '-' + os.path.basename(d.rstrip('/'))
            objdir = f'{OUT}/obj/t/{name}'
            os.makedirs(objdir, exist_ok=True)
            objs = []
            ok = True
            for f in srcs:
                o = objdir + '/' + os.path.basename(f) + '.o'
                ok = compile_one(f, o, inc + [TESTS + '/common', d]) and ok
                objs.append(o)
            if not ok:
                continue
            elf = f'{OUT}/obj/t/{name}.elf'
            if run(['mipsel-linux-gnu-ld', '-Ttext=0x80010000', '--gc-sections', '-e', '_start', '-o', elf] + objs +
                   ['--start-group', common] + libs + [libgcc, '--end-group']):
                elf2exe(elf, f'{OUT}/exe/{name}.exe')
                n += 1
            if os.path.exists(d + 'vram.png'):
                png2ppm(d + 'vram.png', f'{OUT}/vram/{name}.ppm')
    print(f'{n} testes montados em {OUT}/exe')


if __name__ == '__main__':
    main()
