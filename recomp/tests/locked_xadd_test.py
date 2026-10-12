"""Check the EAX provider's LOCK XADD lowering, including x86 flags and atomicity.
Run with recomp/.venv/bin/python recomp/tests/locked_xadd_test.py.
"""
from pathlib import Path
import sys, tempfile, subprocess
import capstone
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools'))
from lift import Lifter
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);md.detail=True
lifter=Lifter.__new__(Lifter)
code=['#include "rt.h"\n#include <assert.h>\n#include <stdlib.h>\n#include <stdio.h>\n#include <pthread.h>\nuint8_t *G_MEM;\n']
for size,encoding in [(1,'f00fc002'),(2,'66f00fc102'),(4,'f00fc102')]:
    ins=next(md.disasm(bytes.fromhex(encoding),0x1000))
    body=lifter.locked(ins,'xadd')
    code.append(f'static void run{size}(Ctx *c) {{ REGS; {body} SPILL; }}\n')
ins=next(md.disasm(bytes.fromhex('660fafc1'),0x2000))
code.append('static void imul16(Ctx *c) { REGS; '+lifter.muldiv(ins,'imul')+' SPILL; }\n')
code.append(r'''
static uint32_t rng=123;
static uint32_t random32(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void *worker(void *unused){for(int i=0;i<20000;i++){Ctx c={.eax=1,.edx=128};run4(&c);}return 0;}
int main(void){
 G_MEM=calloc(1,4096);
 for(int sz=1;sz<=4;sz*=2)for(int i=0;i<10000;i++){
  uint32_t mask=sz==4?0xffffffffu:(1u<<(sz*8))-1,sign=1u<<(sz*8-1),a=random32()&mask,b=random32()&mask;
  rt_w32(G_MEM,128,a);Ctx c={.eax=(0x98765400u&~mask)|b,.edx=128};uint32_t high=c.eax&~mask;
  if(sz==1)run1(&c);else if(sz==2)run2(&c);else run4(&c);
  uint64_t wide=(uint64_t)a+b;uint32_t sum=wide&mask;
  uint32_t expected=(wide>mask?1:0)|(!__builtin_parity(sum&255)?4:0)|((a^b^sum)&16)|(!sum?64:0)|(sum&sign?128:0)|((~(a^b)&(a^sum)&sign)?2048:0);
  assert(rt_r32(G_MEM,128)==sum && c.eax==(high|a) && (flags_get(&c.f)&0x8d5)==expected);
 }
 for(int i=0;i<10000;i++) {
  uint32_t a=random32(),b=random32();Ctx c={.eax=a,.ecx=b};imul16(&c);
  int32_t product=(int32_t)(int16_t)a*(int32_t)(int16_t)b;
  assert(c.eax==((a&0xffff0000u)|(uint16_t)product));
  assert((flags_get(&c.f)&(F_CF|F_OF))==((product!=(int16_t)product)?(F_CF|F_OF):0));
 }
 rt_w32(G_MEM,128,0);pthread_t t[4];for(int i=0;i<4;i++)assert(!pthread_create(t+i,0,worker,0));for(int i=0;i<4;i++)pthread_join(t[i],0);
 assert(rt_r32(G_MEM,128)==80000);free(G_MEM);
 puts("IMUL16: 10000 cases; LOCK XADD: 30000 register/memory/flag cases and 80000 contended increments passed");return 0;
}
''')
with tempfile.TemporaryDirectory() as temp:
    p=Path(temp);(p/'test.c').write_text(''.join(code))
    subprocess.run(['clang','-O2','-I'+str(root/'runtime'),str(p/'test.c'),'-o',str(p/'test'),'-pthread'],check=True)
    subprocess.run([p/'test'],check=True)
