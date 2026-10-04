/* Runtime core for recompiled code: guest memory, indirect calls, faults, x87 helpers. */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <mach/mach_time.h>

uint8_t *G_MEM;

struct rt_fn { uint32_t addr; GuestFn fn; };
extern const struct rt_fn rt_fntab[];
extern const unsigned rt_fntab_n;
struct rt_imp { uint32_t iat; const char *name; };
extern const struct rt_imp rt_imptab[];
extern const unsigned rt_imptab_n;

/* Import thunks: the loader fills each IAT slot with RT_IMPORT_BASE + 16 * index, so code that loads an import's
 * address into a register and calls it later still lands in rt_import. */
#define RT_IMPORT_BASE 0xfff00000u

/* ---- faults: abort, or return to a test harness ---- */
__thread jmp_buf *fault_jmp;            /* set by a harness to catch faults instead of aborting */
__thread uint32_t fault_pc;
__thread const char *fault_what;
__attribute__((weak)) void rt_fault_hook(Ctx *c, uint32_t pc, const char *what) { (void)c; (void)pc; (void)what; }
static void fault(Ctx *c, uint32_t pc, const char *what)
{
    fault_pc = pc; fault_what = what;
    rt_fault_hook(c, pc, what);                               /* crash report (native app); nothing in tests */
    if (fault_jmp) longjmp(*fault_jmp, 1);
    fprintf(stderr, "recomp: fatal: %s at %08x (eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x)\n",
            what, pc, c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    abort();
}
void rt_unhandled(Ctx *c, uint32_t pc, const char *what) { fault(c, pc, what); __builtin_unreachable(); }
void rt_div_fault(Ctx *c, uint32_t pc) { fault(c, pc, "integer divide fault"); __builtin_unreachable(); }

GuestFn rt_lookup(uint32_t addr)
{
    unsigned lo = 0, hi = rt_fntab_n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (rt_fntab[mid].addr == addr) return rt_fntab[mid].fn;
        if (rt_fntab[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return 0;
}
static __thread RtSehPad *seh_pads;
void rt_seh_push(RtSehPad *p) { p->next = seh_pads; seh_pads = p; }
void rt_seh_pop(RtSehPad *p)
{
    for (RtSehPad **q = &seh_pads; *q; q = &(*q)->next) if (*q == p) { *q = p->next; return; }
}
__attribute__((weak)) void w32_seh_abandon(void) {}
void rt_call(Ctx *c, uint32_t t)
{
    for (RtSehPad *p = seh_pads; p; p = p->next)              /* control transfer to an __except block? */
        for (const uint32_t *h = p->handlers; *h; h++) if (*h == t) {
            Ctx *dst = p->c;
            if (dst != c) { uint32_t fs = dst->fs_base; *dst = *c; dst->fs_base = fs; }   /* registers as the dispatcher left them */
            p->target = t; seh_pads = p;                      /* frames above the target are gone */
            w32_seh_abandon();
            _longjmp(p->jb, 1);
        }
    if (t >= RT_IMPORT_BASE) { rt_import(c, (t - RT_IMPORT_BASE) / 16); return; }
    GuestFn fn = rt_lookup(t);
    if (!fn) {                                                /* name the image, so a report says which binary needs analysis */
        extern const struct rt_image { const char *name; uint32_t base, imp_first, imp_count; } rt_images[]; extern const unsigned rt_images_n;
        static __thread char m[160]; const char *img = "outside every image";
        for (unsigned k = 0; k < rt_images_n; k++) {
            uint32_t b = rt_images[k].base, sz = k ? 0x01000000u : 0x01000000u - b;   /* address windows: headers may be unmapped */
            if (t - b < sz) { img = *rt_images[k].name ? rt_images[k].name : "DungeonSiege.exe"; break; }
        }
        snprintf(m, sizeof m, "indirect call to unknown code in %s", img); fault(c, t, m);
    }
    fn(c);
}
/* Weak default: the platform layer (Win32 on macOS) provides the real one. */
__attribute__((weak)) void rt_import(Ctx *c, uint32_t index)
{
    static char buf[160];
    snprintf(buf, sizeof buf, "unimplemented import %s", index < rt_imptab_n ? rt_imptab[index].name : "?");
    fault(c, rt_r32(G_MEM, c->esp), buf);
}
uint64_t rt_rdtsc(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom * 3;   /* a nominal 3 GHz counter */
}

/* ---- x87 helpers ---- */
double rt_f80_load(const uint8_t *M, uint32_t a)
{
    uint64_t mant; uint16_t se; memcpy(&mant, M + a, 8); memcpy(&se, M + a + 8, 2);
    int e = se & 0x7fff; double s = (se & 0x8000) ? -1.0 : 1.0;
    if (e == 0 && mant == 0) return s * 0.0;
    if (e == 0x7fff) return (mant << 1) ? NAN : s * INFINITY;
    return s * ldexp((double)mant, e - 16383 - 63);
}
void rt_f80_store(uint8_t *M, uint32_t a, double v)
{
    uint64_t mant = 0; uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) { se |= 0x7fff; mant = 0xc000000000000000ull; }
    else if (isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e; double m = frexp(fabs(v), &e);               /* v = m * 2^e, m in [0.5, 1) */
        mant = (uint64_t)ldexp(m, 64); se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(M + a, &mant, 8); memcpy(M + a + 8, &se, 2);
}
void rt_fxam(Ctx *c)
{
    double v = ST(0); uint32_t s = c->fsw & ~(uint32_t)(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
    if (signbit(v)) s |= FSW_C1;
    switch (fpclassify(v)) {
    case FP_NAN: s |= FSW_C0; break;
    case FP_INFINITE: s |= FSW_C2 | FSW_C0; break;
    case FP_ZERO: s |= FSW_C3; break;
    case FP_SUBNORMAL: s |= FSW_C3 | FSW_C2; break;
    default: s |= FSW_C2; break;
    }
    c->fsw = s;
}
void rt_fprem(Ctx *c, int ieee)
{
    double a = ST(0), b = ST(1); int q;
    double rn = remquo(a, b, &q);                            /* low quotient bits, exact, for the nearest quotient */
    double r = ieee ? rn : fmod(a, b);
    unsigned ql = (unsigned)(q < 0 ? -q : q) & 7;
    if (!ieee && r != rn) ql = (ql - 1) & 7;                 /* the truncated quotient is one less in magnitude */
    uint32_t s = c->fsw & ~(uint32_t)(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
    if (ql & 1) s |= FSW_C1;
    if (ql & 2) s |= FSW_C3;
    if (ql & 4) s |= FSW_C0;
    c->fsw = s; ST(0) = r;
}
/* 32-bit protected-mode environment layout: fcw, fsw, tag, fip, fcs, foo, fos (7 dwords) */
void rt_fnstenv(Ctx *c, uint32_t a)
{
    uint8_t *M = G_MEM;
    rt_w32(M, a, c->fcw | 0xffff0000u); rt_w32(M, a + 4, rt_fnstsw(c) | 0xffff0000u); rt_w32(M, a + 8, 0xffffffffu);
    for (int k = 3; k < 7; k++) rt_w32(M, a + 4 * k, 0);
    c->fcw |= 0x3f;                                          /* fnstenv masks all exceptions */
}
void rt_fldenv(Ctx *c, uint32_t a)
{
    uint8_t *M = G_MEM;
    c->fcw = rt_r16(M, a); uint32_t sw = rt_r16(M, a + 4);
    c->fsw = sw & ~0x3800u; c->top = (sw >> 11) & 7;
}
void rt_fnsave(Ctx *c, uint32_t a)
{
    rt_fnstenv(c, a); c->fcw &= ~0x3fu;
    rt_w32(G_MEM, a, c->fcw | 0xffff0000u);
    for (int k = 0; k < 8; k++) rt_f80_store(G_MEM, a + 28 + 10 * k, ST(k));
    c->fcw = 0x37f; c->fsw = 0; c->top = 0;                  /* then FNINIT */
}
void rt_frstor(Ctx *c, uint32_t a)
{
    rt_fldenv(c, a);
    for (int k = 0; k < 8; k++) ST(k) = rt_f80_load(G_MEM, a + 28 + 10 * k);
}

/* ---- test harness entry points ---- */
uint8_t *rt_test_mem(void)
{
    if (!G_MEM) {
        void *p = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return 0;
        G_MEM = p; mprotect(G_MEM, 0x10000, PROT_NONE);       /* guest NULL page faults */
    }
    return G_MEM;
}
int rt_test_run(Ctx *c, uint32_t fn_addr, uint32_t *pc, const char **what)
{
    GuestFn fn = rt_lookup(fn_addr);
    if (!fn) { *pc = fn_addr; *what = "no such function"; return 2; }
    jmp_buf jb; fault_jmp = &jb;
    if (setjmp(jb)) { fault_jmp = 0; *pc = fault_pc; *what = fault_what; return 1; }
    fn(c); fault_jmp = 0; return 0;
}
uint32_t rt_test_flags(const Ctx *c) { return flags_get(&c->f); }

/* cpuid: an Intel Pentium (P54C) without MMX, so code with CPU-specific paths takes its plain x86/x87 path */
void rt_cpuid(Ctx *c, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    (void)c;
    if (*eax == 0) { *eax = 1; *ebx = 0x756e6547u; *edx = 0x49656e69u; *ecx = 0x6c65746eu; return; }   /* "GenuineIntel" */
    *eax = 0x524; *ebx = 0; *ecx = 0; *edx = 0x1bf;                                                      /* family 5: FPU, TSC, CX8... no MMX */
}
/* weak default without the Win32 layer: fatal */
__attribute__((weak)) int w32_raise_cpu_exception(Ctx *c, uint32_t pc, uint32_t code) { (void)c; (void)pc; (void)code; return 0; }
void rt_exception(Ctx *c, uint32_t pc, uint32_t code)
{
    if (w32_raise_cpu_exception(c, pc, code)) return;
    static __thread char buf[64]; snprintf(buf, sizeof buf, "unhandled CPU exception %08x", code);
    fault(c, pc, buf);
}

/* weak default without the Win32 layer: no native features */
__attribute__((weak)) void rt_hook(Ctx *c, uint32_t addr) { (void)c; (void)addr; }
