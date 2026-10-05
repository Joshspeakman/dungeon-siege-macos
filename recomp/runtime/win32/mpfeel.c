/* Multiplayer feel for a Mac that joins someone else's game (docs/NETCODE-PLAN.md). Nothing here changes what goes on
 * the wire: Windows players stay compatible.
 *
 * Dungeon Siege's joiner sends a move order to the host (GoMind::RSDoJob packs a job request) and its hero only moves
 * when the host's movement plan comes back and its start time arrives on the joiner's clock (GoFollower playback).
 *
 * DS_MPFEEL=1 measures that: the time from a move order on a joiner to the hero's position first changing, per order
 * and as a running average, split into when the host's first plan segment arrived and how far ahead of the joiner's
 * clock it was set to start. */
#include "w32.h"
#include "ext.h"
#include <math.h>

enum { RSDOJOB = 0x5d2679, FOLLOWER_UPDATE = 0x5fbb3c, FOLLOWER_UNPACK = 0x5faf98, SET_SERVER_TIME = 0x574789, IS_SERVER_LOCAL = 0x51ed95, GO_GET_PLACEMENT = 0x473e05, PLACEMENT_GET_POSITION = 0x537075 };

static uint64_t now_us(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000; }
static int feel_on(void) { static int on = -1; if (on < 0) on = getenv("DS_MPFEEL") != 0; return on; }

static int is_joiner(Ctx *c) { return !(w32_callback(c, IS_SERVER_LOCAL, 0, 0) & 0xff); }
/* a Go's position: x, y, z within its terrain node, and the node */
static int go_pos(Ctx *c, uint32_t go, float p[3], uint32_t *node)
{
    uint32_t pl = go ? ext_thiscall(c, GO_GET_PLACEMENT, go, 0, 0) : 0; if (!pl) return 0;
    uint32_t sp = ext_thiscall(c, PLACEMENT_GET_POSITION, pl, 0, 0); if (!sp) return 0;
    for (int i = 0; i < 3; i++) { uint32_t b = rt_r32(G_MEM, sp + 4 * (uint32_t)i); memcpy(&p[i], &b, 4); }
    *node = rt_r32(G_MEM, sp + 12);
    return 1;
}

static double rd(uint32_t a) { double d; uint64_t v = (uint64_t)rt_r32(G_MEM, a) | (uint64_t)rt_r32(G_MEM, a + 4) << 32; memcpy(&d, &v, 8); return d; }
static double joiner_clock(void) { uint32_t wt = rt_r32(G_MEM, 0x7a05ccu); return wt ? rd(wt + 0x10) : 0; }   /* WorldTime: seconds */
static struct { int armed, seg; uint32_t go, node, job; uint64_t t0, t_seg; float p[3]; double lead, sum; int n; } probe;

static int feel_impl(Ctx *c, uint32_t addr)
{
    if (addr == RSDOJOB) {                                  /* GoMind::RSDoJob(const JobReq&): ecx = the mind */
        if (!feel_on() || !is_joiner(c)) return 0;
        uint32_t go = rt_r32(G_MEM, c->ecx + 4), req = ARG(0);
        probe.armed = go_pos(c, go, probe.p, &probe.node); probe.go = go; probe.t0 = now_us(); probe.job = req ? rt_r32(G_MEM, req) : 0; probe.seg = 0;
        fprintf(stderr, "mpfeel: order (job %u)%s\n", probe.job, probe.armed ? "" : ": no position, not measured");
        return 0;
    }
    if (addr == SET_SERVER_TIME) {                          /* WorldTime::RCSetServerTime(double): the host's clock, once a second */
        if (!feel_on()) return 0;
        double host = rd(c->esp + 4), local = rd(c->ecx + 0x10);
        fprintf(stderr, "mpfeel: host time %.3f, this clock %.3f: behind by %.0f ms\n", host, local, (host - local) * 1000);
        return 0;
    }
    if (addr == FOLLOWER_UNPACK) {                          /* GoFollower: one unpacked plan update (ARG 0; +8 its time) */
        if (!probe.armed || probe.seg || rt_r32(G_MEM, c->ecx + 4) != probe.go) return 0;
        uint32_t u = ARG(0); if (!(rt_r8(G_MEM, u) & 1)) return 0;                     /* a timed segment */
        probe.seg = 1; probe.t_seg = now_us(); probe.lead = rd(u + 8) - joiner_clock();
        return 0;
    }
    if (addr == FOLLOWER_UPDATE) {                          /* GoFollower::Update(): ecx = the follower, +4 its Go */
        if (!probe.armed || rt_r32(G_MEM, c->ecx + 4) != probe.go) return 0;
        float p[3]; uint32_t node;
        if (!go_pos(c, probe.go, p, &node)) return 0;
        float d = node != probe.node ? 1.0f : sqrtf((p[0] - probe.p[0]) * (p[0] - probe.p[0]) + (p[1] - probe.p[1]) * (p[1] - probe.p[1]) + (p[2] - probe.p[2]) * (p[2] - probe.p[2]));
        if (d < 0.02f) {
            if (now_us() - probe.t0 > 5000000) probe.armed = 0;   /* an order that never moved the hero */
            return 0;
        }
        double ms = (now_us() - probe.t0) / 1000.0; probe.armed = 0; probe.sum += ms; probe.n++;
        fprintf(stderr, "mpfeel: order (job %u) to movement %.0f ms (average %.0f ms over %d); first segment arrived after %.0f ms, set %.0f ms ahead of this clock\n",
                probe.job, ms, probe.sum / probe.n, probe.n, probe.seg ? (probe.t_seg - probe.t0) / 1000.0 : -1.0, probe.seg ? probe.lead * 1000 : 0.0);
        return 0;
    }
    return 0;
}
int mpfeel_override(Ctx *c, uint32_t addr)
{
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp;
    if (feel_impl(c, addr)) return 1;
    c->eax = eax; c->ecx = ecx; c->edx = edx; c->ebx = ebx; c->esi = esi; c->edi = edi; c->ebp = ebp;
    return 0;
}
