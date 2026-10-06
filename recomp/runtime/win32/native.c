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

/* ---- vertex lighting: the two light loops in 0x695c99 (a mesh's lighting by one light; the largest part of the game
 * thread in big fights). For each of esi vertices: the light direction (ebp-0x24, -0x20, -0x1c) dotted with the vertex
 * normal (edi, 12 bytes apart); when that is above 0 it is scaled by the light's intensity (ebp-0xc; negated in the
 * second loop, a light that darkens), clamped to 1, times 255, stored as a float (ebp-0x28) and converted to an integer
 * (fistp, ebp-0x3c or -0x38); the light's colour ([[ebp+8]+4]) times that is added to (0x6789de) or taken from
 * (0x678a6a) the vertex colour ([ebp-4], 24 bytes apart), each channel saturating. ---- */
static uint32_t color_add_scaled(uint32_t d, uint32_t src, uint32_t s)   /* 0x6789de */
{
    uint32_t rb = (src & 0xff00ffu) * s + 0x800080u, g = ((src >> 8) & 0xffu) * s + 0x80u;
    uint32_t a = (((((rb >> 8) & 0xff00ffu) + rb) >> 8) & 0xff00ffu) + (d & 0xff00ffu);
    uint32_t gg = ((((g >> 8) & 0xffu) + g) & 0xff00u) + (d & 0xff00ff00u);
    if (a & 0xff000000u) a = (a & 0xffffu) | 0xff0000u;
    if (a & 0xff00u) a = (a & 0xff0000u) | 0xffu;
    if (gg & 0xff0000u) gg = (gg & 0xff00ff00u) | 0xff00u;
    return gg + a;
}
static uint32_t color_sub_scaled(uint32_t d, uint32_t src, uint32_t s)   /* 0x678a6a */
{
    uint32_t rb = (src & 0xff00ffu) * s + 0x800080u, g = ((src >> 8) & 0xffu) * s + 0x80u;
    uint32_t a = ((((rb >> 8) & 0xff00ffu) + rb) >> 8) & 0xff00ffu;
    uint32_t bl = (d & 0xffu) - (a & 0xffu), r = (d & 0xff0000u) - (a & 0xff0000u);
    uint32_t gr = (d & 0xff00u) - ((((g >> 8) & 0xffu) + g) & 0xff00u);
    if (r & 0xff00ffffu) r = 0;
    if (gr & 0xffff00ffu) gr = 0;
    if (bl & 0xffffff00u) bl = 0;
    return (bl + gr + r) | (d & 0xff000000u);
}
static struct { int pending, neg; uint32_t dst, n, edi, ebp; float t; uint32_t ti; int have; uint32_t col[4096]; } lchk;
static void light_loop(Ctx *c, int neg)
{
    uint32_t n = c->esi, ebp = c->ebp, fcw = c->fcw; if (!n) return;
    const uint8_t *M = G_MEM;
    double lx = rt_rf32(M, ebp - 0x24), ly = rt_rf32(M, ebp - 0x20), lz = rt_rf32(M, ebp - 0x1c), in = rt_rf32(M, ebp - 0xc);
    uint32_t src = rt_r32(M, rt_r32(M, ebp + 8) + 4), nrm = c->edx + 4, dst = c->ecx + 0xc;
    int check = check_on() && n <= 4096, have = 0, fast = (fcw & 0x300u) == 0;
    float last_t = 0; uint32_t last_i = 0, fsw_c = 0;
    float flx = (float)lx, fly = (float)ly, flz = (float)lz, fin = (float)in;
    #define OKF(x) ((x) == 0.0f || (__builtin_fabsf(x) >= 1.17549435e-38f && __builtin_fabsf(x) <= 3.40282347e+38f))
    for (uint32_t k = 0; k < n; k++, nrm += 12, dst += 0x18) {
        float t = 0; int lit = -1;                            /* -1: not decided by the float path */
        if (fast) {                                           /* single precision, normal results: float arithmetic is exact */
            float A = flx * rt_rf32(M, nrm - 4), B = flz * rt_rf32(M, nrm + 4), s1 = A + B, C = fly * rt_rf32(M, nrm), d = s1 + C;
            if (OKF(A) && OKF(B) && OKF(s1) && OKF(C) && OKF(d)) {
                if (!(d > 0.0f)) { lit = 0; fsw_c = d == 0.0f ? FSW_C3 : FSW_C0; }
                else {
                    float f = d * fin; if (neg) f = -f;
                    if (OKF(f)) {
                        float v = (f < 1.0f) ? f : 1.0f;      /* fcom 1.0; jb: below keeps f (an unordered f can't occur here: d and fin are finite) */
                        fsw_c = f < 1.0f ? FSW_C0 : f == 1.0f ? FSW_C3 : 0;
                        t = v * 255.0f; if (OKF(t)) lit = 1;
                    }
                }
            }
        }
        if (lit < 0) {                                        /* the exact x87 steps */
            double A = rt_pc(fcw, lx * (double)rt_rf32(M, nrm - 4)), B = rt_pc(fcw, lz * (double)rt_rf32(M, nrm + 4));
            double d = rt_pc(fcw, A + B); d = rt_pc(fcw, d + rt_pc(fcw, ly * (double)rt_rf32(M, nrm)));
            if (!(d > 0.0)) { lit = 0; fsw_c = d != d ? FSW_C0 | FSW_C2 | FSW_C3 : d == 0.0 ? FSW_C3 : FSW_C0; }   /* jbe: below, equal or unordered */
            else {
                double f = rt_pc(fcw, d * in); if (neg) f = -f;
                double v = (f < 1.0 || f != f) ? f : 1.0;     /* jb: below or unordered keeps f */
                fsw_c = f != f ? FSW_C0 | FSW_C2 | FSW_C3 : f < 1.0 ? FSW_C0 : f == 1.0 ? FSW_C3 : 0;
                t = rt_f32(fcw, rt_pc(fcw, v * 255.0)); lit = 1;
            }
        }
        if (!lit) { if (check) lchk.col[k] = rt_r32(M, dst); continue; }
        uint32_t i = rt_fist32(c, (double)t);                 /* fstp float, fld, fistp */
        last_t = t; last_i = i; have = 1;
        uint32_t d0 = rt_r32(M, dst), out = neg ? color_sub_scaled(d0, src, i) : color_add_scaled(d0, src, i);
        if (check) lchk.col[k] = out; else rt_w32(G_MEM, dst, out);
    }
    #undef OKF
    uint32_t end_edi = c->edx + 4 + 12 * n, end_dst = c->ecx + 0xc + 0x18 * n;
    if (check) {                                              /* the original runs now; compared at 0x6961ac */
        lchk.pending = 1; lchk.neg = neg; lchk.dst = c->ecx + 0xc; lchk.n = n; lchk.edi = end_edi; lchk.ebp = ebp;
        lchk.t = last_t; lchk.ti = last_i; lchk.have = have; return;
    }
    if (have) { rt_wf32(G_MEM, ebp - 0x28, last_t); rt_w32(G_MEM, ebp - (neg ? 0x38 : 0x3c), last_i); }
    rt_w32(G_MEM, ebp - 4, end_dst); c->edi = end_edi;
    c->fsw = (c->fsw & ~(uint32_t)(FSW_C0 | FSW_C2 | FSW_C3)) | fsw_c;   /* as the last compare left it */
    c->esi = 0;                                               /* the loop's own entry test (test esi, esi; jbe) now skips it */
}
static void light_check(Ctx *c)
{
    if (!lchk.pending) return;
    lchk.pending = 0;
    static long runs, bad, verts;
    int diff = 0; uint32_t first = 0;
    for (uint32_t k = 0; k < lchk.n; k++) if (rt_r32(G_MEM, lchk.dst + 0x18 * k) != lchk.col[k]) { if (!diff++) first = k; }
    if (lchk.have && (rt_r32(G_MEM, lchk.ebp - 0x28) != fb(lchk.t) || rt_r32(G_MEM, lchk.ebp - (lchk.neg ? 0x38 : 0x3c)) != lchk.ti)) diff++;
    if (c->edi != lchk.edi || rt_r32(G_MEM, lchk.ebp - 4) != lchk.dst + 0x18 * lchk.n) diff++;
    runs++; verts += lchk.n;
    if (diff && bad++ < 20)
        fprintf(stderr, "native: light loop differs (%s, %u vertices, first at %u: %08x, original %08x)\n", lchk.neg ? "darkening" : "lighting",
                lchk.n, first, lchk.col[first], rt_r32(G_MEM, lchk.dst + 0x18 * first));
    if (runs % 20000 == 0) fprintf(stderr, "native: light loop %ld runs (%ld vertices) checked, %ld differ\n", runs, verts, bad);
}
static void light_loop_timed(Ctx *c, int neg)
{
    static int prof = -1; if (prof < 0) prof = getenv("DS_NATIVE_PROF") != 0;
    if (!prof) { light_loop(c, neg); return; }
    static uint64_t t_sum, calls, verts, last; uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); uint32_t n = c->esi;
    light_loop(c, neg);
    uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); t_sum += t1 - t0; calls++; verts += n;
    if (!last) last = t1;
    if (t1 - last > 5000000000ull) { fprintf(stderr, "native: light loop %llu runs, %llu vertices, %.2f ms in 5 s\n", calls, verts, t_sum / 1e6); t_sum = calls = verts = 0; last = t1; }
}
/* lift.py HOOKS 0x6960b1 / 0x696139 (the loops' entry tests) and 0x6961ac (after them: the differential check) */
int native_hook(Ctx *c, uint32_t addr)
{
    if (!native_on()) return 0;
    switch (addr) {
    case 0x006960b1u: light_loop_timed(c, 0); return 1;
    case 0x00696139u: light_loop_timed(c, 1); return 1;
    case 0x006961acu: if (check_on()) light_check(c); return 1;
    }
    return 0;
}

/* 0x435d1d: FuBi's identity of a module file: its PE checksum and a crc of the file after that field; the multiplayer
 * sync digest adds the crc. Steam's DungeonSiege.exe is the same 1.11.1 build as GOG's but without GOG's small fixes,
 * so the two digests differ and Steam and GOG copies refuse each other's games. A Mac build made from Steam's file
 * reports GOG's two values for it, and so plays with GOG players (and PCs running the GOG executable); the header
 * checksum the game also advertises is set in the build's copy (tools/patch_exe.py). DS_STEAM_IDENTITY=steam keeps
 * Steam's own values (to play with unmodified Steam copies). DS_IDLOG=1 prints the values. */
int edition_identity(Ctx *c, uint32_t addr)
{
    static int inside; if (inside) return 0;
    uint32_t pck = ARG(0), pcrc = ARG(1), path = ARG(2), a[3] = {pck, pcrc, path};   /* path: the module handle */
    uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp;
    inside = 1; uint32_t ok = w32_callback(c, addr, 3, a) & 0xff; inside = 0;
    c->ebx = ebx; c->esi = esi; c->edi = edi; c->ebp = ebp; c->eax = ok;
    if (getenv("DS_IDLOG")) fprintf(stderr, "identity: module %08x ok %u checksum %08x crc %08x\n", path, ok, rt_r32(G_MEM, pck), rt_r32(G_MEM, pcrc));
    const char *keep = getenv("DS_STEAM_IDENTITY");
    if (ok && rt_r32(G_MEM, pck) == 0 && rt_r32(G_MEM, pcrc) == 0xfbc6a1f8u && !(keep && !strcmp(keep, "steam"))) {   /* Steam 1.11.1 */
        rt_w32(G_MEM, pck, 0x003b4d58u); rt_w32(G_MEM, pcrc, 0xd76d5528u);                                       /* GOG 1.11.1 */
        if (getenv("DS_IDLOG")) fprintf(stderr, "identity: Steam's executable presented as GOG's\n");
    }
    c->esp += 4; return 1;
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
