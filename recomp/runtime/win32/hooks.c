/* Native features added to the recompiled game at fixed x86 addresses (tools/lift.py HOOKS). Each hook runs before the
 * instruction at its address with the guest registers spilled to the context; at default settings none changes anything.
 *
 * Draw distance (DS_DRAW_DISTANCE=<percent>, set by the launch window): the mood loader (0x58fdca) reads each area's
 * fog and frustum from world/global/moods into a stack record; at 0x59000f all of it is in place:
 *   [ebp-0x10f] frustum given, [ebp-0x10c] width, [ebp-0x108] height
 *   [ebp-0x104] fog given, [ebp-0x100] near, [ebp-0xfc] far, [ebp-0xf8] low-detail near, [ebp-0xf4] low-detail far
 * Fog distances scale with the setting, and every mood gets a world frustum of the game's default 45 x 60 m times the setting (the
 * frustum is the part of the world kept loaded and drawn; the SeeFar mod uses 72 x 96 m, about 160%). */
#include "w32.h"

float w32_draw_distance = 1.0f;

static float gf(uint32_t a) { float v; uint32_t u = rt_r32(G_MEM, a); memcpy(&v, &u, 4); return v; }
static void sf(uint32_t a, float v) { uint32_t u; memcpy(&u, &v, 4); rt_w32(G_MEM, a, u); }

static void mood_loaded(Ctx *c)
{
    float f = w32_draw_distance; uint32_t bp = c->ebp;
    if (getenv("DS_HOOKLOG")) fprintf(stderr, "hook: mood fog %d %.1f/%.1f frustum %d %.1f x %.1f (x%.2f)\n", G_MEM[bp - 0x104], gf(bp - 0x100), gf(bp - 0xfc),
                                      G_MEM[bp - 0x10f], gf(bp - 0x10c), gf(bp - 0x108), f);
    if (f <= 1.001f) return;
    if (G_MEM[bp - 0x104]) for (uint32_t o = 0x100; o >= 0xf4; o -= 4) sf(bp - o, gf(bp - o) * f);
    if (G_MEM[bp - 0x10f]) { sf(bp - 0x10c, gf(bp - 0x10c) * f); sf(bp - 0x108, gf(bp - 0x108) * f); }
    else { G_MEM[bp - 0x10f] = 1; sf(bp - 0x10c, 45.0f * f); sf(bp - 0x108, 60.0f * f); }   /* the game's default frustum, scaled */
}

int vm_committed(uint32_t addr, uint32_t size);
static int readable(uint32_t p) { return p >= 0x10000 && vm_committed(p, 1); }
/* printf of a guest format string with guest cdecl arguments starting at stack address ap (%s %d %i %u %x %c %f %%) */
static void guest_format(char *out, size_t cap, uint32_t fmt, uint32_t ap)
{
    size_t n = 0; const char *f = (const char *)GP(fmt);
    for (; *f && n + 1 < cap; f++) {
        if (*f != '%') { out[n++] = *f; continue; }
        char spec[16] = "%"; int k = 1; f++;
        while (*f && strchr("-+ #0123456789.l", *f) && k < 12) spec[k++] = *f++;
        if (!*f) break;
        spec[k++] = *f; spec[k] = 0; int w = 0;
        switch (*f) {
        case 's': { uint32_t p = rt_r32(G_MEM, ap); ap += 4; w = snprintf(out + n, cap - n, spec, readable(p) ? (const char *)GP(p) : "(?)"); break; }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'c': w = snprintf(out + n, cap - n, spec, rt_r32(G_MEM, ap)); ap += 4; break;
        case 'f': case 'g': { double v; memcpy(&v, GP(ap), 8); ap += 8; w = snprintf(out + n, cap - n, spec, v); break; }
        case '%': out[n] = '%'; w = 1; break;
        default: w = 0;
        }
        if (w > 0) n += (size_t)w < cap - n ? (size_t)w : cap - n - 1;
    }
    out[n] = 0;
}
/* the Skrit compiler's message routine: retail builds discard these; DS_SKRITLOG=1 prints them */
static void skrit_message(Ctx *c)
{
    if (!getenv("DS_SKRITLOG")) return;
    uint32_t fmt = rt_r32(G_MEM, c->esp + 12);
    if (fmt < 0x720000 || fmt >= 0x7ac000) return;          /* the messages are string constants in the exe (.rdata/.data) */
    char msg[1024]; guest_format(msg, sizeof msg, rt_r32(G_MEM, c->esp + 12), c->esp + 16);
    uint32_t level = rt_r32(G_MEM, c->esp + 8), self = rt_r32(G_MEM, c->esp + 4);
    /* the file being compiled: a string field of the compiler object (found once by looking for a .skrit path) */
    static int name_off = -1; const char *file = "";
    for (int pass = 0; pass < 2 && !*file; pass++)
        for (int off = pass ? 0 : (name_off < 0 ? 0 : name_off); off < (pass ? 0x400 : (name_off < 0 ? 0 : name_off + 4)); off += 4) {
            uint32_t p = rt_r32(G_MEM, self + (uint32_t)off);
            if (p >= 0xffff0000u || !readable(p) || !readable(p + 259)) continue;
            const char *t = (const char *)GP(p); size_t n = strnlen(t, 260);
            if (n > 6 && n < 260 && (strstr(t, ".skrit") || strstr(t, "skrit"))) { file = t; name_off = off; break; }
        }
    fprintf(stderr, "skrit %s: %s%s%s\n", level >= 2 ? "error" : level == 1 ? "warning" : "note", *file ? file : "", *file ? ": " : "", msg);
}

void rt_hook(Ctx *c, uint32_t addr)
{
    switch (addr) {
    case 0x0059000f: mood_loaded(c); break;
    case 0x0061d06c: skrit_message(c); break;
    case 0x004acb46: case 0x005d1fcf: case 0x004036a8: case 0x005a39a6: { extern int loa_active; void loa_hook(Ctx *, uint32_t); if (loa_active) loa_hook(c, addr); break; }
    default:   /* development trace hooks (tools/lift.py DS_TRACE_HOOKS) */
        if (getenv("DS_HOOKTRACE")) fprintf(stderr, "hook %08x: eax %08x ecx %08x edx %08x ebx %08x esi %08x edi %08x | ret %08x args %08x %08x %08x %08x\n",
                                            addr, c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi, rt_r32(G_MEM, c->esp), rt_r32(G_MEM, c->esp + 4),
                                            rt_r32(G_MEM, c->esp + 8), rt_r32(G_MEM, c->esp + 12), rt_r32(G_MEM, c->esp + 16));
        if (getenv("DS_HOOKTRACE") && atoi(getenv("DS_HOOKTRACE")) >= 2)   /* and 80 bytes at the first two stack arguments */
            for (int k = 1; k <= 2; k++) {
                uint32_t p = rt_r32(G_MEM, c->esp + 4 * (uint32_t)k); if (p < 0x10000) continue;
                fprintf(stderr, "   arg%d:", k); for (uint32_t j = 0; j < 80; j++) fprintf(stderr, " %02x", G_MEM[p + j]); fprintf(stderr, "\n");
            }
    }
}

/* lift.py OVERRIDES: the expansion's engine changes take over a few functions when Legends of Aranna is played */
int rt_override(Ctx *c, uint32_t addr)
{
    extern int loa_active; int loa_override(Ctx *, uint32_t);
    if (addr == 0x41aea9u) {                      /* W32_CRCLOG=<path>: the checksums FuBi's sync digest is built from */
        static FILE *f; static int init; if (!init) { init = 1; const char *p = getenv("W32_CRCLOG"); if (p) f = fopen(p, "w"); }
        if (!f) return 0;
        uint32_t ret = rt_r32(G_MEM, c->esp), seed = rt_r32(G_MEM, c->esp + 4), ptr = rt_r32(G_MEM, c->esp + 8), len = rt_r32(G_MEM, c->esp + 12);
        if ((ret >= 0x452000u && ret < 0x45c000u) || ret == 0x46f7a7u) {   /* FuBi digest; content schemas */
            fprintf(f, "%06x %08x %u ", ret, seed, len);
            for (uint32_t i = 0; i < len && i < (ret == 0x46f7a7u ? 4096u : 300u); i++) { uint8_t b = rt_r8(G_MEM, ptr + i); if (len == 8 || len == 4 || ret == 0x46f7a7u) fprintf(f, "%02x", b); else fputc(b >= 32 && b < 127 ? b : '.', f); }
            fputc('\n', f); fflush(f);
        }
        return 0;
    }
    if (addr == 0x412d12u) return loa_override(c, addr);       /* DS_REPORTLOG: the engine's reports, in any game */
    return loa_active ? loa_override(c, addr) : 0;
}
