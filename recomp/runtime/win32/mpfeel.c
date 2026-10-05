/* Multiplayer feel for a Mac that joins someone else's game (docs/NETCODE-PLAN.md). Nothing here changes what goes on
 * the wire: Windows players stay compatible.
 *
 * Dungeon Siege's joiner sends a move order to the host (GoMind::RSDoJob packs a job request) and its hero only moves
 * when the host's movement plan comes back and its start time arrives on the joiner's clock (GoFollower playback).
 *
 * Own heroes play the host's plans as they arrive: the host schedules each plan a little ahead (its "planner lag", from
 * the latency it measured, up to half a second, and it varies), and a joiner otherwise waits for that start time on its
 * own clock. Each hero this machine gives orders to is played on its own clock, advanced by the lead of the plan that
 * started it moving: when a plan arrives for a hero standing still (nothing queued), its advance becomes that plan's
 * lead, so it starts at once (a standing hero can't visibly jump); while it moves the advance stays. Everything else
 * plays as before. DS_NO_OWN_LEAD=1 turns it off.
 *
 * When this Mac hosts, the planner's lead comes from the network's real round trip. The game takes it from its own ping
 * of each player, which also counts how long that player's game takes to send its reply (some 250 ms more with a
 * Windows player, on an 18 ms network), and raises it at once on any slow reply, so every joiner waited 110-225 ms
 * longer than needed for every move. Here it is half the largest round trip to a player plus 60 ms for the joiners'
 * clock error, at most half a second as before. DS_GAME_LAG=1 keeps the game's own.
 *
 * A joiner's clock follows the host's smoothly. The game sets it from the host's time once a second without allowing
 * for the network delay, then jumps a third of the averaged error at once (so the world jolts by up to a sixth of a
 * second); here each sync counts half the round trip as the delay and the error is folded into the following frames,
 * the clock running at most 10% fast or slow. Errors over 2 s are still set at once. DS_GAME_CLOCK=1 keeps the game's.
 *
 * DS_MPFEEL=1 measures that: the time from a move order on a joiner to the hero's position first changing, per order
 * and as a running average, split into when the host's first plan segment arrived and how far ahead of the joiner's
 * clock it was set to start. On a host it logs, for each job, the time until the first plan for that Go was sent to the
 * other machines, and the planner's lag. */
#include "w32.h"
#include "ext.h"
#include <math.h>
#include <sys/time.h>
#include <time.h>

enum { RSDOJOB = 0x5d2679, FOLLOWER_UPDATE = 0x5fbb3c, FOLLOWER_UNPACK = 0x5faf98, SET_SERVER_TIME = 0x574789, SDOJOB = 0x5d281f, SEND_UPDATE = 0x5fae2e, SERVER_LAG = 0x565464, WORLD_UPDATE = 0x57470d, IS_SERVER_LOCAL = 0x51ed95, GO_GET_PLACEMENT = 0x473e05, PLACEMENT_GET_POSITION = 0x537075 };

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
/* own heroes and the lead they are played with */
static uint32_t own[16]; static double own_adv[16]; static int nown;
static int own_slot(uint32_t go) { for (int k = 0; k < nown; k++) if (own[k] == go) return k; return -1; }
static int is_own(uint32_t go) { return own_slot(go) >= 0; }
static double clock_pending;                                /* seconds the joiner's clock still has to move */
static int clock_on(void) { static int on = -1; if (on < 0) on = !getenv("DS_GAME_CLOCK"); return on; }
static int lead_on(void) { static int on = -1; if (on < 0) on = !getenv("DS_NO_OWN_LEAD"); return on; }
static struct { int armed, seg; uint32_t go, node, job; uint64_t t0, t_seg; float p[3]; double lead, sum; int n; } probe;

static void feel_moved(Ctx *c, uint32_t go);
static int feel_impl(Ctx *c, uint32_t addr)
{
    if (addr == RSDOJOB) {                                  /* GoMind::RSDoJob(const JobReq&): ecx = the mind */
        if (!is_joiner(c)) return 0;
        uint32_t go = rt_r32(G_MEM, c->ecx + 4), req = ARG(0);
        if (go && !is_own(go)) { int k = nown < 16 ? nown++ : (int)(go % 16); own[k] = go; own_adv[k] = 0; }
        if (!feel_on()) return 0;
        probe.go = go; probe.seg = 0; probe.t0 = now_us();
        probe.armed = go_pos(c, go, probe.p, &probe.node); probe.job = req ? rt_r32(G_MEM, req) : 0;
        fprintf(stderr, "mpfeel: order (job %u)%s\n", probe.job, probe.armed ? "" : ": no position, not measured");
        return 0;
    }
    if (addr == SERVER_LAG) {                               /* Server: the planner lag (float), the most any player needs */
        static int own = -1; if (own < 0) own = !getenv("DS_GAME_LAG");
        int dpnet_host_max_rtt(void); int rtt = own ? dpnet_host_max_rtt() : -1;
        if (rtt < 0) return 0;
        double lag = rtt / 2000.0 + 0.06; if (lag > 0.5) lag = 0.5;
        FPUSH((double)(float)lag); c->esp += 4; return 1;
    }
    if (addr == SDOJOB || addr == SEND_UPDATE) {            /* host: GoMind::SDoJob (ecx mind) / GoFollower::RCSendPackedUpdateToFollowers */
        static struct { uint32_t go; uint64_t t; } jobs[64];
        if (!feel_on() || is_joiner(c)) return 0;
        uint32_t go = rt_r32(G_MEM, c->ecx + 4); uint64_t now = now_us();
        if (addr == SDOJOB) { int k = 0; while (k < 63 && jobs[k].go && jobs[k].go != go) k++; jobs[k].go = go; jobs[k].t = now; return 0; }
        for (int k = 0; k < 64; k++) if (jobs[k].go == go && jobs[k].t) {
            uint32_t wo = rt_r32(G_MEM, 0x7a0650u), lagb = wo ? rt_r32(G_MEM, wo + 0x10) : 0; float lag; memcpy(&lag, &lagb, 4);
            if (now - jobs[k].t < 3000000) {
                struct timeval tv; gettimeofday(&tv, 0); struct tm tm; localtime_r(&tv.tv_sec, &tm); uint64_t ago = now - jobs[k].t;
                long ms = (long)(tv.tv_usec / 1000) - (long)(ago / 1000); int sec = tm.tm_sec; while (ms < 0) { ms += 1000; sec--; }
                fprintf(stderr, "mpfeel: host: job at %02d:%02d:%02d.%03ld, first plan sent %.0f ms later (Go %08x); planner lag %.0f ms\n",
                        tm.tm_hour, tm.tm_min, sec, ms, ago / 1000.0, go, lag * 1000);
            }
            jobs[k].t = 0;
        }
        return 0;
    }
    if (addr == SET_SERVER_TIME) {                          /* WorldTime::RCSetServerTime(double): the host's clock, once a second */
        if (!is_joiner(c)) return 0;
        double host = rd(c->esp + 4), local = rd(c->ecx + 0x10);
        int dpnet_client_rtt(void); int rtt = dpnet_client_rtt(); double owd = rtt > 0 ? rtt / 2000.0 : 0;
        if (feel_on()) fprintf(stderr, "mpfeel: host time %.3f (+%.0f ms on the way), this clock %.3f: behind by %.0f ms\n", host, owd * 1000, local, (host + owd - local) * 1000);
        if (!clock_on()) return 0;
        double err = host + owd - local;
        if (fabs(err) > 2.0) { uint64_t v; double t = host + owd; memcpy(&v, &t, 8); rt_w32(G_MEM, c->ecx + 0x10, (uint32_t)v); rt_w32(G_MEM, c->ecx + 0x14, (uint32_t)(v >> 32)); clock_pending = 0; }
        else clock_pending = fabs(err) > 0.5 ? err : err * 0.5;
        c->esp += 4 + 8; return 1;
    }
    if (addr == WORLD_UPDATE) {                             /* WorldTime::Update(float dt): fold the pending correction in */
        if (clock_pending == 0 || !clock_on()) return 0;
        uint32_t b = rt_r32(G_MEM, c->esp + 4); float dt; memcpy(&dt, &b, 4);
        if (!(dt > 0) || dt > 1) return 0;
        double step = clock_pending, cap = dt * 0.1; if (step > cap) step = cap; if (step < -cap) step = -cap;
        clock_pending -= step; float ndt = (float)(dt + step); memcpy(&b, &ndt, 4); rt_w32(G_MEM, c->esp + 4, b);
        return 0;
    }
    if (addr == FOLLOWER_UNPACK) {                          /* GoFollower: one unpacked plan update (ARG 0; +8 its time) */
        uint32_t go = rt_r32(G_MEM, c->ecx + 4), u = ARG(0); int k = own_slot(go);
        if (!(rt_r8(G_MEM, u) & 1)) return 0;                                            /* timed waypoints only */
        double lead = rd(u + 8) - joiner_clock();
        if (feel_on() && is_joiner(c)) {                    /* late waypoints snap: how often, by how much (every 10 s) */
            static uint64_t t0; static int n, late, nown_, lown; static double worst, worst_own; uint64_t t = now_us(); if (!t0) t0 = t;
            double eff = k >= 0 ? lead - own_adv[k] : lead;
            if (k >= 0) { nown_++; if (eff < 0) { lown++; if (-eff > worst_own) worst_own = -eff; } }
            else { n++; if (eff < 0) { late++; if (-eff > worst) worst = -eff; } }
            if (t - t0 >= 10000000) {
                fprintf(stderr, "mpfeel: waypoints in 10 s: others %d (late %d, worst %.0f ms), own heroes %d (late %d, worst %.0f ms)\n", n, late, worst * 1000, nown_, lown, worst_own * 1000);
                t0 = t; n = late = nown_ = lown = 0; worst = worst_own = 0;
            }
        }
        if (k < 0) return 0;
        if (rt_r32(G_MEM, c->ecx + 0x54) == 0) own_adv[k] = lead < 0 ? 0 : lead > 0.5 ? 0.5 : lead;   /* standing still */
        if (probe.armed && !probe.seg && go == probe.go) { probe.seg = 1; probe.t_seg = now_us(); probe.lead = lead; }
        return 0;
    }
    if (addr == FOLLOWER_UPDATE) {                          /* GoFollower::Update(float): ecx = the follower, +4 its Go */
        uint32_t go = rt_r32(G_MEM, c->ecx + 4);
        int k = own_slot(go);
        if (k >= 0 && own_adv[k] > 0 && lead_on() && is_joiner(c)) {   /* play it on its advanced clock */
            static int inside; uint32_t wt = rt_r32(G_MEM, 0x7a05ccu);
            if (!inside && wt) {
                uint32_t lo = rt_r32(G_MEM, wt + 0x10), hi = rt_r32(G_MEM, wt + 0x14), a = ARG(0);
                double t = rd(wt + 0x10) + own_adv[k]; uint64_t v; memcpy(&v, &t, 8);
                rt_w32(G_MEM, wt + 0x10, (uint32_t)v); rt_w32(G_MEM, wt + 0x14, (uint32_t)(v >> 32));
                inside = 1; ext_thiscall(c, addr, c->ecx, 1, &a); inside = 0;
                rt_w32(G_MEM, wt + 0x10, lo); rt_w32(G_MEM, wt + 0x14, hi);
                feel_moved(c, go);
                c->esp += 4 + 4; return 1;
            }
        }
        feel_moved(c, go);
        return 0;
    }
    return 0;
}
static void feel_moved(Ctx *c, uint32_t go)            /* DS_MPFEEL: has the ordered hero started moving? */
{
    if (!feel_on() || !probe.armed || go != probe.go) return;
    float p[3]; uint32_t node;
    if (!go_pos(c, probe.go, p, &node)) return;
    float d = node != probe.node ? 1.0f : sqrtf((p[0] - probe.p[0]) * (p[0] - probe.p[0]) + (p[1] - probe.p[1]) * (p[1] - probe.p[1]) + (p[2] - probe.p[2]) * (p[2] - probe.p[2]));
    if (d < 0.02f) {
        if (now_us() - probe.t0 > 5000000) probe.armed = 0;   /* an order that never moved the hero */
        return;
    }
    double ms = (now_us() - probe.t0) / 1000.0; probe.armed = 0; probe.sum += ms; probe.n++;
    fprintf(stderr, "mpfeel: order (job %u) to movement %.0f ms (average %.0f ms over %d); first segment arrived after %.0f ms, set %.0f ms ahead of this clock; own heroes played %.0f ms ahead\n",
            probe.job, ms, probe.sum / probe.n, probe.n, probe.seg ? (probe.t_seg - probe.t0) / 1000.0 : -1.0, probe.seg ? probe.lead * 1000 : 0.0, lead_on() && own_slot(probe.go) >= 0 ? own_adv[own_slot(probe.go)] * 1000 : 0.0);
}
int mpfeel_override(Ctx *c, uint32_t addr)
{
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp;
    if (feel_impl(c, addr)) return 1;
    c->eax = eax; c->ecx = ecx; c->edx = edx; c->ebx = ebx; c->esi = esi; c->edi = edi; c->ebp = ebp;
    return 0;
}
