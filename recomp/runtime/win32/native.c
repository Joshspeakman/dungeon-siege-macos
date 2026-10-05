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

/* 0x694970: Quat slerp, in place: a = slerp(a, b, t) (cdecl: a, b, t), with the original's branches (the shorter arc;
 * linear blending when nearly parallel), its arc cosine (0x41ac51: atan2(sqrt(1 - x*x), x)) and which values it keeps
 * as floats and which at full precision */
static void quat_slerp(uint32_t a, uint32_t b, float t)
{
    float d = rf(a) * rf(b); d = d + rf(b + 12) * rf(a + 12); d = d + rf(b + 8) * rf(a + 8); d = d + rf(b + 4) * rf(a + 4);
    int neg = d < 0.0f; if (neg) d = -d;
    float w0, w1;
    if ((double)(float)(1.0 - (double)d) < 0.001) { w0 = 1.0f - t; w1 = t; }
    else {
        float x = d, sq = (float)sqrt((double)(1.0f - x * x));
        double th = rt_x87_2(1, sq, x); float thf = (float)th;
        float inv = (float)(1.0 / rt_x87_1(2, th));
        w0 = (float)(rt_x87_1(2, (double)(float)((double)(1.0f - t) * th)) * (double)inv);
        w1 = (float)(rt_x87_1(2, (double)(thf * t)) * (double)inv);
    }
    if (neg) w1 = -w1;
    rt_w32(G_MEM, a, fb(w0 * rf(a) + w1 * rf(b)));
    rt_w32(G_MEM, a + 4, fb(w1 * rf(b + 4) + w0 * rf(a + 4)));
    rt_w32(G_MEM, a + 8, fb(w1 * rf(b + 8) + w0 * rf(a + 8)));
    rt_w32(G_MEM, a + 12, fb(w1 * rf(b + 12) + w0 * rf(a + 12)));
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
    if (addr == 0x694970u) {
        uint32_t qa = ARG(0), qb = ARG(1), tb = ARG(2), before[4], mine[4]; float t; memcpy(&t, &tb, 4);
        int chk = check_on(); static int inside; if (inside) return 0;
        if (chk) for (int k = 0; k < 4; k++) before[k] = rt_r32(G_MEM, qa + 4u * (uint32_t)k);
        quat_slerp(qa, qb, t);
        if (chk) {
            static unsigned long calls, diffs;
            for (int k = 0; k < 4; k++) { mine[k] = rt_r32(G_MEM, qa + 4u * (uint32_t)k); rt_w32(G_MEM, qa + 4u * (uint32_t)k, before[k]); }
            uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp, args[3] = {qa, qb, tb};
            inside = 1; w32_callback(c, addr, 3, args); inside = 0;
            c->ebx = ebx; c->esi = esi; c->edi = edi; c->ebp = ebp; calls++;
            for (int k = 0; k < 4; k++) if (rt_r32(G_MEM, qa + 4u * (uint32_t)k) != mine[k] && diffs++ < 20)
                fprintf(stderr, "native: quat_slerp differs (fcw %04x) [%d] %08x, original %08x; t %08x\n", c->fcw, k, mine[k], rt_r32(G_MEM, qa + 4u * (uint32_t)k), tb);
            if (calls % 200000 == 0) fprintf(stderr, "native: quat_slerp %lu calls checked, %lu differences\n", calls, diffs);
        }
        c->esp += 4; return 1;
    }
    return 0;
}
