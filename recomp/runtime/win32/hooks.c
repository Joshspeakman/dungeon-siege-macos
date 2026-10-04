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

void rt_hook(Ctx *c, uint32_t addr)
{
    switch (addr) {
    case 0x0059000f: mood_loaded(c); break;
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
