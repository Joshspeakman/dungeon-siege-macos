// shadows_test: the renderer's character-shadow handling (renderer.m), driven with DSR commands as the game sends them:
//  1. a 1024 silhouette drawn while the back buffer is 800x600 comes out whole (its own scratch target, no clipping);
//  2. DSR_SHADOW_FILTER softens a silhouette's edges where its receiver pass samples it, and leaves an identical texture
//     that is not a silhouette untouched.
// Build and run from src/renderer (the renderer reads shaders.metal from the working directory):
//   clang -fobjc-arc -framework Metal -framework QuartzCore -framework Foundation -I ../dsr ../../recomp/tests/shadows_test.m renderer.m -o /tmp/shadows_test
//   (cd src/renderer && /tmp/shadows_test)
#import <Metal/Metal.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "renderer.h"
#include "dsr_proto.h"

static DSRRenderer *r;
static uint32_t *rb_area; static volatile uint32_t rb_done; static uint32_t cookie;
static void cmd(uint32_t op, const void *p, uint32_t n) { dsr_renderer_exec(r, op, p, n); }
static void state(uint32_t k, uint32_t v) { uint32_t p[] = {k, v}; cmd(DSR_RENDER_STATE, p, sizeof p); }
static void stage(uint32_t k, uint32_t v) { uint32_t p[] = {0, k, v}; cmd(DSR_TSS, p, sizeof p); }
static void binding(uint32_t t) { uint32_t p[] = {0, t}; cmd(DSR_TEXTURE, p, sizeof p); }
static void surface(uint32_t id, uint32_t w, uint32_t h, uint32_t fmt) { uint32_t p[] = {id, w, h, 1, fmt, 0, 0, 1}; cmd(DSR_SURFACE_CREATE, p, sizeof p); }
static void viewport(uint32_t w, uint32_t h) { uint32_t p[] = {0, 0, w, h, 0, 0x3f800000}; cmd(DSR_VIEWPORT, p, sizeof p); }
static void clear(void) { uint32_t p[] = {3, 0xffeeeeee, 0x3f800000, 0, 0}; cmd(DSR_CLEAR, p, sizeof p); }
static void read_surface(uint32_t id, uint32_t w, uint32_t h, uint32_t *pixels)
{
    uint32_t p[] = {id, 0, 0, w, h, ++cookie}; cmd(DSR_READBACK, p, sizeof p);
    assert(rb_done == cookie); memcpy(pixels, rb_area, (size_t)w * h * 4);
}
static void quad(const float *pos, uint32_t color)
{
    struct { uint32_t head[4]; struct { float p[3]; uint32_t color; float uv[2]; } v[4]; } p = {.head = {5, 0x142, 4, 0}};
    for (int k = 0; k < 4; k++) { memcpy(p.v[k].p, pos + 3 * k, 12); p.v[k].color = color; p.v[k].uv[0] = k & 1; p.v[k].uv[1] = k >> 1; }
    cmd(DSR_DRAW, &p, sizeof p);
}
static const float square[] = {-1, -1, .5, 1, -1, .5, -1, 1, .5, 1, 1, .5};
static void init(uint32_t w, uint32_t h, const char *filter)
{
    setenv("DSR_SHADOW_FILTER", filter, 1);
    r = dsr_renderer_create(MTLCreateSystemDefaultDevice()); assert(r);
    dsr_renderer_set_readback(r, rb_area, 1024 * 1024 * 4, &rb_done);
    surface(1, w, h, 1); surface(4, w, h, 3);
    uint32_t target[] = {1, 4}; cmd(DSR_SET_RT, target, sizeof target); viewport(w, h);
    state(22, 1); state(7, 0); state(14, 0); stage(1, 2); stage(2, 0); stage(4, 1);
}
static void silhouette(uint32_t n)   /* the game's sequence: white square on the back buffer, the silhouette, its copy */
{
    surface(2, n, n, 1); viewport(n, n); state(7, 0); state(14, 0);
    uint32_t fill[] = {1, 0, 0, n, n, 0, 0, 0, 0, 0, 0, 0xffffffff}; cmd(DSR_BLT, fill, sizeof fill);
    const float mesh[] = {-.5, -.5, .5, .5, -.5, .5, -.5, .5, .5, .5, .5, .5}; quad(mesh, 0xff000000);
    uint32_t copy[] = {2, 0, 0, n, n, 1, 0, 0, n, n, 0, 0}; cmd(DSR_BLT, copy, sizeof copy);
}
static void silhouette_check(void)
{
    init(800, 600, "off"); silhouette(1024);
    uint32_t *px = malloc(1024 * 1024 * 4); assert(px); read_surface(2, 1024, 1024, px);
    assert((px[700 * 1024 + 512] & 0xffffff) == 0);          /* below the 600-line back buffer: still drawn */
    assert((px[900 * 1024 + 512] & 0xffffff) == 0xffffff);
    assert((px[512 * 1024 + 100] & 0xffffff) == 0xffffff);
    free(px); puts("1024 silhouette with an 800x600 back buffer is whole: passed");
}
static void filtered_scene(const char *filter, int tagged, uint32_t *px)
{
    init(256, 256, filter); silhouette(1024);
    if (!tagged) { surface(3, 1024, 1024, 1); uint32_t copy[] = {3, 0, 0, 1024, 1024, 2, 0, 0, 1024, 1024, 0, 0}; cmd(DSR_BLT, copy, sizeof copy); }
    viewport(256, 256); clear(); stage(1, 2); stage(2, 2); stage(16, 2); stage(17, 2);
    binding(tagged ? 2 : 3); state(27, 1); state(19, 1); state(20, 3);   /* the receiver: multiplied in (ZERO, SRCCOLOR) */
    float shifted[12]; memcpy(shifted, square, sizeof shifted); for (int k = 0; k < 4; k++) shifted[k * 3] += .5f / 256;
    quad(shifted, 0xffffffff);
    read_surface(1, 256, 256, px);
}
static void filter_check(void)
{
    uint32_t *orig = malloc(256 * 256 * 4), *soft = malloc(256 * 256 * 4), *other = malloc(256 * 256 * 4); assert(orig && soft && other);
    filtered_scene("off", 1, orig); filtered_scene("soft", 1, soft); filtered_scene("soft", 0, other);
    assert(!memcmp(orig, other, 256 * 256 * 4));           /* not a silhouette: unfiltered */
    unsigned changed = 0;
    for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) if (orig[y * 256 + x] != soft[y * 256 + x]) {
        changed++;
        assert(((abs(x - 64) < 3 || abs(x - 192) < 3) && y >= 62 && y <= 194) || ((abs(y - 64) < 3 || abs(y - 192) < 3) && x >= 62 && x <= 194));
    }
    printf("filtered changes: %u; edge %08x/%08x centre %08x/%08x\n", changed, orig[128 * 256 + 64], soft[128 * 256 + 64], orig[128 * 256 + 128], soft[128 * 256 + 128]);
    assert(changed > 100);
    free(orig); free(soft); free(other);
    puts("the filter changes silhouette edges only; an identical texture that is not one is untouched: passed");
}
int main(void)
{
    @autoreleasepool {
        setbuf(stdout, NULL);
        assert(!posix_memalign((void **)&rb_area, 16384, 1024 * 1024 * 4));
        silhouette_check(); filter_check();
    }
    return 0;
}
