/* Native versions of the game's hottest small maths routines (docs/PERFORMANCE-PLAN.md, step 6). Each one gives the
 * same bits as the recompiled original: the game runs the x87 in single precision (Direct3D's FPU setup), so every
 * x87 result is a float rounded with the current rounding mode, which is what float arithmetic here gives (the host's
 * rounding mode follows the game's: rt_fpcr_sync) without fused multiply-add. When the precision isn't single the original runs.
 *
 * DS_NATIVE_CHECK=1 runs the original as well on every call and reports any difference (differential test);
 * DS_NO_NATIVE=1 turns these off. */
#include "w32.h"
#include "ext.h"

#pragma clang fp contract(off)

static int native_on(void) { static int on = -1; if (on < 0) on = !getenv("DS_NO_NATIVE"); return on; }
static int check_on(void) { static int on = -1; if (on < 0) on = getenv("DS_NATIVE_CHECK") != 0; return on; }
static float rf(uint32_t a) { uint32_t b = rt_r32(G_MEM, a); float f; memcpy(&f, &b, 4); return f; }
static uint32_t fb(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
/* 0x5338e9: Quat::RotateVector(vector_3 &out, const vector_3 &in) const (ecx: x, y, z, w), the inner loop of
 * character animation. The original's operation order, and its memory order too: each component is stored before the
 * next one's inputs are read (out may be in). */
static void quat_rotate(uint32_t q, uint32_t o, uint32_t v)
{
    float B = rf(q + 12) + rf(q + 12), A = B * rf(q + 12) - 1.0f;
    float D = rf(v + 8) * rf(q + 8) + rf(v + 4) * rf(q + 4); D = D + rf(v) * rf(q); D = D + D;
    float t;
    t = (rf(v + 8) * rf(q + 4) - rf(v + 4) * rf(q + 8)) * B; t = t + A * rf(v); t = t + D * rf(q); rt_w32(G_MEM, o, fb(t));
    t = (rf(v) * rf(q + 8) - rf(v + 8) * rf(q)) * B; t = t + A * rf(v + 4); t = t + D * rf(q + 4); rt_w32(G_MEM, o + 4, fb(t));
    t = (rf(v + 4) * rf(q) - rf(v) * rf(q + 4)) * B; t = t + A * rf(v + 8); t = t + D * rf(q + 8); rt_w32(G_MEM, o + 8, fb(t));
}

int native_override(Ctx *c, uint32_t addr)
{
    if (!native_on() || (c->fcw & 0x300u)) return 0;      /* only with single precision */
    if (addr == 0x5338e9u) {
        uint32_t q = c->ecx, o = ARG(0), v = ARG(1), before[3], mine[3];
        int chk = check_on(); static int inside; if (inside) return 0;
        if (chk) for (int k = 0; k < 3; k++) before[k] = rt_r32(G_MEM, o + 4u * (uint32_t)k);
        quat_rotate(q, o, v);                              /* the host's rounding mode already follows the game's */
        if (chk) {                                         /* the original too, from the same memory, and compare */
            static unsigned long calls, diffs;
            for (int k = 0; k < 3; k++) { mine[k] = rt_r32(G_MEM, o + 4u * (uint32_t)k); rt_w32(G_MEM, o + 4u * (uint32_t)k, before[k]); }
            uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp, a[2] = {o, v};
            inside = 1; ext_thiscall(c, addr, q, 2, a); inside = 0;
            c->ebx = ebx; c->esi = esi; c->edi = edi; c->ebp = ebp; calls++;
            for (int k = 0; k < 3; k++) if (rt_r32(G_MEM, o + 4u * (uint32_t)k) != mine[k] && diffs++ < 20)
                fprintf(stderr, "native: quat_rotate differs (fcw %04x) out[%d] %08x, original %08x; q %08x %08x %08x %08x v %08x %08x %08x%s\n", c->fcw, k, mine[k], rt_r32(G_MEM, o + 4u * (uint32_t)k),
                        rt_r32(G_MEM, q), rt_r32(G_MEM, q + 4), rt_r32(G_MEM, q + 8), rt_r32(G_MEM, q + 12), rt_r32(G_MEM, v), rt_r32(G_MEM, v + 4), rt_r32(G_MEM, v + 8), o == v ? " (out = in)" : "");
            if (calls % 1000000 == 0) fprintf(stderr, "native: quat_rotate %lu calls checked, %lu differences\n", calls, diffs);
        }
        c->esp += 4 + 8; return 1;
    }
    return 0;
}
