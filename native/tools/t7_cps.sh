#!/bin/sh
# Prepara /root/indigo-env/roms/cps para o teste indigo-t7_cps (CPS1/CPS2).
#
# Não existem ROMs de teste públicas prontas para o CPS1/CPS2 (só o SDK CCPS, que precisa compilar o próprio
# driver em Go); os programas de teste do 68000/Z80 ficam dentro do t7_cps.cpp. Aqui só se monta a régua da
# decifração do CPS2: o arquivo capcom/cps2crypt.cpp do MAME (BSD-3-Clause) é baixado e compilado fora do
# repositório com um "main" mínimo, e gera vetores (chave .key aleatória + bloco cifrado -> bloco decifrado
# pelo MAME) que o teste compara com a implementação do Índigo.
set -e
DEST=${1:-/root/indigo-env/roms/cps}
mkdir -p "$DEST/crypt"
cd "$DEST/crypt"
curl -sSL -o cps2crypt.cpp https://raw.githubusercontent.com/mamedev/mame/master/src/mame/capcom/cps2crypt.cpp
python3 - <<'EOF'
import re
src = open('cps2crypt.cpp').read()
src = re.sub(r'#include[^\n]*\n', '', src)
src = src.replace('void cps2_decrypt(running_machine &machine,', 'void cps2_decrypt(')
src = re.sub(r'\n\t\tif \(\(i & 0xff\) == 0\)\n\t\t\{.*?\n\t\t\}\n', '\n', src, flags=re.S)
stub = '''#include <cstdint>
#include <cstdio>
#include <cstdlib>
template<typename T> static inline int BIT(T x,int n){return (x>>n)&1;}
template<int N, typename T, typename... B> static inline T bitswap(T v, B... b){ int bits[]={b...}; T r=0; for(int i=0;i<N;i++) r=(r<<1)|((v>>bits[i])&1); return r;}
'''
main = '''
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); uint8_t k[20]; if(!f||fread(k,1,20,f)!=20) return 1; fclose(f);
  int n=atoi(argv[2]);
  uint16_t*rom=(uint16_t*)calloc(n,2),*dec=(uint16_t*)calloc(n,2);
  for(int i=0;i<n;i++){int a=getchar(),b=getchar(); rom[i]=(a<<8)|b;}
  uint16_t decoded[10]={0};
  for(int b=0;b<160;b++){int bit=(317-b)%160; if((k[bit/8]>>((bit^7)%8))&1) decoded[b/16]|=(0x8000>>(b%16));}
  uint32_t key[2]={((uint32_t)decoded[0]<<16)|decoded[1],((uint32_t)decoded[2]<<16)|decoded[3]};
  uint32_t lower,upper; if(decoded[9]==0xffff){upper=0xffffff;lower=0xff0000;} else {upper=(((~decoded[9]&0x3ff)<<14)|0x3fff)+1; lower=0;}
  cps2_decrypt(rom,dec,n*2,key,lower/2,upper/2);
  for(int i=0;i<n;i++){putchar(dec[i]>>8);putchar(dec[i]&0xff);}
  return 0;
}
'''
open('ref.cpp', 'w').write(stub + src + main)
EOF
g++ -O2 -std=c++17 -w -o refcrypt ref.cpp
# vetores: 8 chaves aleatórias (semente fixa), 256 KB cifrados cada (cobre os 2^16 endereços e a borda da faixa)
python3 - <<'EOF'
import random, subprocess
r = random.Random(1234)
for i in range(8):
    key = bytes(r.randrange(256) for _ in range(20))
    open('key%d.bin' % i, 'wb').write(key)
    words = 0x20000
    data = bytes(r.randrange(256) for _ in range(words * 2))
    open('enc%d.bin' % i, 'wb').write(data)
    out = subprocess.run(['./refcrypt', 'key%d.bin' % i, str(words)], input=data, capture_output=True, check=True).stdout
    open('dec%d.bin' % i, 'wb').write(out)
print('vetores de decifração do CPS2 gerados')
EOF
