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
#include "ext.h"

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
    case 0x0051c8b9:     /* the hardware profiles' shadow_tex_size has just been read (eax) */
    case 0x0064e928: {   /* the renderer's constructor has just set its shadow size ([esi+0x690]) to 64 */
        /* DS_SHADOW_RESOLUTION replaces both, so the engine's allocation, projection and copy rectangles all agree; the
         * 2002 profiles match no Mac GPU, so on a Mac the constructor's value is the one that stays */
        const char *v = getenv("DS_SHADOW_RESOLUTION"); char *end; unsigned long n = v ? strtoul(v, &end, 10) : 0;
        if (!(v && *v && !*end && (n == 64 || n == 128 || n == 256 || n == 512 || n == 1024))) break;
        if (addr == 0x0051c8b9) c->eax = (uint32_t)n; else rt_w32(G_MEM, c->esi + 0x690u, (uint32_t)n);
        if (getenv("DS_HOOKLOG")) fprintf(stderr, "hook: shadow size %lu (%s %08x)\n", n, addr == 0x0051c8b9 ? "profile" : "renderer", c->esi);
        break;
    }
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
    if (addr == 0x4b7d69u) {                      /* the default Shadows setting (config/options.gas), every startup */
        /* The game's shipped default is complex_party (complex shadows for the party only, simple for everything else),
         * chosen for 2002 hardware, and its hardware table (system_detail.gas) would have left a card it doesn't know at
         * Simple anyway. Every Mac draws all complex shadows easily, so the default here is All Complex; a choice saved
         * in the preferences is applied over it right after, as before. DS_STOCK_SHADOWS=1 keeps the game's default. */
        uint32_t world = rt_r32(G_MEM, 0x7a05fcu), sh = world ? rt_r32(G_MEM, world + 0xa8u) : 0;
        uint32_t rend = rt_r32(G_MEM, 0x7a0644u); rend = rend ? rt_r32(G_MEM, rend + 0x1e0u) : 0;
        if (getenv("DS_STOCK_SHADOWS") || !sh || !rend || !rt_r8(G_MEM, rend + 0x61bu)) return 0;   /* +0x61b: full rendering */
        rt_w8(G_MEM, sh + 0x09u, 1); rt_w8(G_MEM, sh + 0x0au, 1);          /* shadows on */
        rt_w8(G_MEM, sh + 0x13u, 1); rt_w8(G_MEM, sh + 0x14u, 0);          /* complex for everything (not the party only) */
        uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp;
        ext_thiscall(c, 0x53e6bau, rt_r32(G_MEM, 0x7a05c8u), 0, 0);        /* what the original does after a change */
        c->ebx = ebx; c->esi = esi; c->edi = edi; c->ebp = ebp; c->esp += 4; return 1;
    }
    if (addr == 0x5338e9u || addr == 0x694970u) { int native_override(Ctx *, uint32_t); if (native_override(c, addr)) return 1; }   /* native maths */
    if (addr == 0x5d2679u || addr == 0x5fbb3cu || addr == 0x5faf98u || addr == 0x574789u || addr == 0x5d281fu || addr == 0x5fae2eu || addr == 0x565464u || addr == 0x57470du) { int mpfeel_override(Ctx *, uint32_t); if (mpfeel_override(c, addr)) return 1; }   /* multiplayer feel */
    if (addr == 0x435d1du) {                      /* a module's PE checksum and file crc (cdecl: &checksum, &crc, path) */
        int edition_identity(Ctx *, uint32_t); return edition_identity(c, addr);
    }
    if (addr == 0x42cda8u) {                      /* DS_REPORTLOG: the engine's warnings/asserts formatter (fmt, ...) */
        if (!getenv("DS_REPORTLOG")) return 0;
        uint32_t f = ARG(0); const char *fmt = f ? (const char *)GP(f) : ""; char out[2048]; size_t o = 0; int ai = 1;
        for (const char *q = fmt; *q && o < sizeof out - 300; q++) {
            if (*q != '%') { out[o++] = *q; continue; }
            q++; while (*q && strchr("0123456789.-#l", *q)) q++;
            if (*q == '%') { out[o++] = '%'; continue; }
            uint32_t v = ARG(ai++);
            if (*q == 's') o += (size_t)snprintf(out + o, 256, "%s", v ? (const char *)GP(v) : "(null)");
            else o += (size_t)snprintf(out + o, 24, "0x%x", v);
        }
        out[o] = 0; fprintf(stderr, "warning: %s\n", out); return 0;
    }
    if (addr == 0x517017u) {                      /* the inventory paper doll's camera for the screen size (thiscall, no arguments) */
        /* ui/config/paperdoll_positions lists the doll's screen position (x, y), distance and offsets per resolution; the
         * game takes the entry whose width and height match the screen exactly and otherwise keeps what it had (the
         * 640x480 values at first), which leaves the doll outside its box at the window sizes a Mac uses. Without an
         * exact entry the values are worked out from the screen size: the doll's box is fixed in pixels and the stock and
         * ResolutionFix-mod entries all follow these within a few pixels (horizontal FOV fixed, so the size goes with the
         * width). An exact entry is used as before (the last one, as in the original loop). */
        uint32_t me = c->ecx, scr = rt_r32(G_MEM, 0x79cf74u);
        if (me && scr) {
            int32_t w = (int32_t)(rt_r32(G_MEM, scr + 0xc8u) - rt_r32(G_MEM, scr + 0xc0u)), h = (int32_t)(rt_r32(G_MEM, scr + 0xccu) - rt_r32(G_MEM, scr + 0xc4u));
            float v[6]; int found = 0;                  /* x, y, x_dockbar_offset, y_dockbar_offset, distance, store_offset */
            uint32_t e = rt_r32(G_MEM, me + 0x9cu), end = rt_r32(G_MEM, me + 0xa0u);
            for (int k = 0; e && e != end && k < 256; e += 0x20u, k++) {
                float ew, eh; memcpy(&ew, G_MEM + e + 0x14u, 4); memcpy(&eh, G_MEM + e + 0x18u, 4);
                if (ew != (float)w || eh != (float)h) continue;
                memcpy(&v[0], G_MEM + e, 8); memcpy(&v[2], G_MEM + e + 0x0cu, 8); memcpy(&v[4], G_MEM + e + 0x08u, 4); memcpy(&v[5], G_MEM + e + 0x1cu, 4); found = 1;
            }
            if (!found && w >= 320 && h >= 240) {
                v[0] = -1.1547f + 440.0f / (float)w; v[1] = 1.0f - 840.0f / (float)h;
                v[2] = 0; v[3] = -54.0f / (float)h; v[4] = 9.0f * (float)w / 1024.0f; v[5] = 0.1f; found = 1;
            }
            if (found) memcpy(G_MEM + me + 0xa8u, v, sizeof v);   /* +0xa8 x, +0xac y, +0xb0/+0xb4 dockbar offsets, +0xb8 distance, +0xbc store */
        }
        c->esp += 4; return 1;
    }
    if (addr == 0x412d12u) return loa_override(c, addr);       /* DS_REPORTLOG: the engine's reports, in any game */
    return loa_active ? loa_override(c, addr) : 0;
}
