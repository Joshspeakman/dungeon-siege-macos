/* Action Mode (DS_ACTION=1, the launcher's Play Style row): Dungeon Siege played like an action RPG.
 *
 * - Steering: W/A/S/D or a controller's left stick walk the controlled hero in that direction, relative to the camera.
 *   The game only knows "move to a place", so while a direction is held the hero is given a place a little ahead of
 *   it, again and again (the engine's own move order, GoMind::RSMove, on the engine's own terrain).
 * - The controlled hero is the selected party member (the first one when several are). It no longer chases enemies
 *   on its own: you fight with it. The other party members keep their AI and guard it (the game's own Guard order:
 *   they follow it and fight what attacks it). Selecting someone else hands control over.
 * - The keys the game used for W, A, S and D (weapons panel, force attack, stop, free look) are taken over for
 *   steering while the game's keys are live (never while typing in the chat box).
 *
 * Positions: a SiegePos is {x, y, z, node}: a point in one terrain node's own space. The engine converts between
 * nodes with GetSiegeDifference (0x678e0e), which works in the frame of the node the camera follows ("world"), so
 * a camera direction is turned into the hero's node space through the node's own axes measured that way.
 *
 * With Action Mode off nothing here runs: every hook returns at once. */
#include "w32.h"
#include "ext.h"
#include <math.h>
#include <time.h>
#include <stdarg.h>

int action_active = -1;                     /* -1 until read from DS_ACTION */
volatile ActionPad action_pad;              /* the controller, written by the host (host/gamepad.m) */
volatile ActionHud action_hud;              /* what the host's overlay shows */

#define FX(name) ({ static uint32_t _a; if (!_a) _a = ext_export(name); _a; })
int party_members(Ctx *c, uint32_t *out, int max);                          /* loa.c */
uint32_t gpstr(Ctx *c, const char *text);                                   /* loa.c: a gpstring of the text (kept) */
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static int logging(void) { static int v = -1; if (v < 0) v = getenv("DS_ACTIONLOG") != 0; return v; }
#define LOG(...) do { if (logging()) fprintf(stderr, "action: " __VA_ARGS__); } while (0)
static int verbose(void) { static int v = -1; if (v < 0) v = getenv("DS_ACTIONLOG") && atoi(getenv("DS_ACTIONLOG")) >= 2; return v; }

int action_on(void)
{
    if (action_active < 0) { const char *e = getenv("DS_ACTION"); action_active = e && *e && strcmp(e, "0"); }
    return action_active;
}

/* ---------------------------------------------------------------- engine access */
static uint32_t enum_val(Ctx *c, const char *type, const char *name, uint32_t dflt);
static void hero_name(Ctx *c, uint32_t go, char *out, size_t cap);
static void toast(const char *fmt, ...);
static int weapon_kind(Ctx *c, uint32_t hero);
enum { K_MELEE, K_RANGED, K_MAGIC };
static uint32_t scratch(Ctx *c, uint32_t n) { c->esp -= (n + 15) & ~15u; return c->esp; }
#define go_comp(go, getter) ({ uint32_t _go = (go); _go ? ext_thiscall(c, FX(getter), _go, 0, 0) : 0; })
#define GO_MIND(go)      go_comp(go, "?GetMind@Go@@QAEPAVGoMind@@XZ")
#define GO_PLACEMENT(go) go_comp(go, "?GetPlacement@Go@@QAEPAVGoPlacement@@XZ")
#define GO_ASPECT(go)    go_comp(go, "?GetAspect@Go@@QAEPAVGoAspect@@XZ")
#define ENGINE()         rt_r32(G_MEM, 0x7a05fcu)                     /* siege::SiegeEngine */
static uint32_t go_pos(Ctx *c, uint32_t go)                        /* the Go's SiegePos (guest address) */
{
    uint32_t pl = GO_PLACEMENT(go);
    return pl ? ext_thiscall(c, FX("?GetPosition@GoPlacement@@QBEABUSiegePos@@XZ"), pl, 0, 0) : 0;
}
static int is_selected(Ctx *c, uint32_t go) { return go && (ext_thiscall(c, FX("?IsSelected@Go@@QBE_NXZ"), go, 0, 0) & 0xff); }
static int is_alive(Ctx *c, uint32_t go)
{
    uint32_t as = GO_ASPECT(go); if (!as) return 0;
    return ext_thiscall(c, FX("?GetLifeState@GoAspect@@QBE?AW4eLifeState@@XZ"), as, 0, 0) == enum_val(c, "eLifeState", "ls_alive_conscious", 1);
}
/* a FuBi enum constant by name (like loa.c fubi_enum, but safe for flag enums: their value range is huge and ToString
 * of a value that isn't a constant returns no string), looked up once */
int vm_committed(uint32_t addr, uint32_t size);
static int enum_lookup(Ctx *c, const char *type, const char *name, uint32_t *out)
{
    for (uint32_t sp = rt_r32(G_MEM, 0x79cf38u); sp; sp = rt_r32(G_MEM, sp + 0x28)) {
        uint32_t n = rt_r32(G_MEM, sp); if (!n || strcmp(GS(n), type)) continue;
        uint32_t to_string = rt_r32(G_MEM, sp + 0xc), b = rt_r32(G_MEM, sp + 0x1c), e = rt_r32(G_MEM, sp + 0x20);
        for (int pass = 0; pass < 2; pass++)
            for (uint32_t k = 0; k < (pass ? 32u : 512u); k++) {
                uint32_t v = pass ? 1u << k : b + k; if (!pass && v >= e) break;
                uint32_t r = w32_callback(c, to_string, 1, &v);
                if (r < 0x10000 || !vm_committed(r, 1) || !vm_committed(r + strlen(name), 1)) continue;
                if (!strcasecmp(GS(r), name)) { *out = v; return 1; }
            }
    }
    return 0;
}
static uint32_t enum_val(Ctx *c, const char *type, const char *name, uint32_t dflt)
{
    static struct { const char *type, *name; uint32_t v; int ok; } cache[32]; static int n;
    for (int i = 0; i < n; i++) if (cache[i].type == type && cache[i].name == name) return cache[i].ok ? cache[i].v : dflt;
    uint32_t v = 0; int ok = enum_lookup(c, type, name, &v);
    LOG("enum %s %s = %d (%s)\n", type, name, (int)v, ok ? "found" : "missing");
    if (n < 32) { cache[n].type = type; cache[n].name = name; cache[n].v = v; cache[n].ok = ok; n++; }
    return ok ? v : dflt;
}
/* the "world" position (frame of the node the camera follows) of a SiegePos */
typedef struct { float x, y, z; } V3;
static V3 world(Ctx *c, uint32_t sp)
{
    V3 r = {0, 0, 0}; uint32_t eng = ENGINE(); if (!eng || !sp) return r;
    uint32_t refp = rt_r32(G_MEM, eng + 0x38); if (!refp) return r;
    uint32_t esp = c->esp, buf = scratch(c, 32);                    /* ref SiegePos {0,0,0,refnode} at buf, out at buf+16 */
    rt_w32(G_MEM, buf, 0); rt_w32(G_MEM, buf + 4, 0); rt_w32(G_MEM, buf + 8, 0); rt_w32(G_MEM, buf + 12, rt_r32(G_MEM, refp));
    uint32_t a[3] = {buf + 16, buf, sp}; w32_callback(c, 0x678e0eu, 3, a);   /* GetSiegeDifference(out, a, b) = b - a */
    r.x = rt_rf32(G_MEM, buf + 16); r.y = rt_rf32(G_MEM, buf + 20); r.z = rt_rf32(G_MEM, buf + 24);
    c->esp = esp; return r;
}
static V3 world_of(Ctx *c, uint32_t node, float x, float y, float z)
{
    uint32_t esp = c->esp, sp = scratch(c, 16);
    rt_wf32(G_MEM, sp, x); rt_wf32(G_MEM, sp + 4, y); rt_wf32(G_MEM, sp + 8, z); rt_w32(G_MEM, sp + 12, node);
    V3 r = world(c, sp); c->esp = esp; return r;
}
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 sub(V3 a, V3 b) { V3 r = {a.x - b.x, a.y - b.y, a.z - b.z}; return r; }

/* the camera's forward direction on the ground, in world space */
static int camera_forward(Ctx *c, V3 *fwd)
{
    uint32_t eng = ENGINE(); if (!eng) return 0;
    uint32_t cam = ext_thiscall(c, FX("?GetCamera@SiegeEngine@siege@@QBEABVSiegeCamera@2@XZ"), eng, 0, 0); if (!cam) return 0;
    uint32_t cp = ext_thiscall(c, FX("?GetCameraSiegePos@SiegeCamera@siege@@QBEABUSiegePos@@XZ"), cam, 0, 0);
    uint32_t tp = ext_thiscall(c, FX("?GetTargetSiegePos@SiegeCamera@siege@@QBEABUSiegePos@@XZ"), cam, 0, 0);
    V3 f = sub(world(c, tp), world(c, cp)); f.y = 0;
    float l = sqrtf(f.x * f.x + f.z * f.z); if (l < 1e-4f) return 0;
    fwd->x = f.x / l; fwd->y = 0; fwd->z = f.z / l; return 1;
}

/* ---------------------------------------------------------------- input */
static uint32_t game_binder;                /* the in-game input binder (UIGame+8) */
static double steer_key_seen[256];          /* when the game's binder last saw that key go down: the keys are live */
static int steer_key(int vk)
{
    if (!(w32_keys[vk] & 0x80)) { steer_key_seen[vk] = 0; return 0; }
    return steer_key_seen[vk] > 0;
}
/* the keys Action Mode takes over (without Ctrl or Alt, which keep the game's own chords): W A S D walk, Space dodges,
 * 1-4 are the abilities (weapon configurations stay on 5-0), P pauses (the game's Space) */
static int stolen_key(uint32_t vk) { return vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' || vk == ' ' || (vk >= '1' && vk <= '4') || vk == 'P'; }

/* ---------------------------------------------------------------- the controlled hero and the party */
static uint32_t controlled, controlled_mind; /* the hero being steered (Go) and its GoMind */
static int steering;                        /* a direction was held last frame */
static Ctx *aim_ctx;                        /* the game thread's context while a frame runs (for aim) */
static double last_order; static float last_dx, last_dz;
static uint32_t leg; static float last_ix, last_iy;         /* the leg being walked (guest SiegePos) and the input that asked for it */
static double attack_ordered; static float attack_ix, attack_iy;   /* an attack you ordered, and the input held then */
static int companions_now;                  /* guard orders due at once (the hero just started walking) */
static uint32_t guarding[16];               /* party member -> the hero it was told to guard */

static uint32_t pick_controlled(Ctx *c, uint32_t *m, int n)
{
    for (int i = 0; i < n; i++) if (is_selected(c, m[i]) && is_alive(c, m[i])) return m[i];
    if (controlled) for (int i = 0; i < n; i++) if (m[i] == controlled && is_alive(c, m[i])) return controlled;
    for (int i = 0; i < n; i++) if (is_alive(c, m[i])) return m[i];
    return 0;
}
static void take_control(Ctx *c, uint32_t go)
{
    if (go == controlled) return;
    int switching = controlled != 0;
    controlled = go; controlled_mind = go ? GO_MIND(go) : 0;
    if (!go) return;
    if (switching && controlled_mind) {                              /* it was guarding the last one: that job ends now */
        uint32_t ao = enum_val(c, "eActionOrigin", "ao_human", 0);
        ext_thiscall(c, FX("?RSStop@GoMind@@QAEXW4eActionOrigin@@@Z"), controlled_mind, 1, &ao);
    }
    char nm[40]; hero_name(c, go, nm, sizeof nm); snprintf((char *)action_hud.hero, sizeof action_hud.hero, "%s", nm);
    static int first = 1; if (!first) toast("Playing as %s", nm); first = 0;
    LOG("controlling %08x (%s)\n", go, nm);
}
/* The hero you play acts only on your orders: the engine's own jobs for it that would walk it somewhere or start a fight
 * (its brain chasing an enemy it saw, wandering back to a spot, picking a target) are turned down at GoMind::SDoJob.
 * Its other jobs (dying, getting up, drinking potions, its auto-cast heals) go on as usual, and nothing about the hero
 * itself changes, so saves are the same as in Classic. */
static int unasked_job(Ctx *c, uint32_t mind, uint32_t req)
{
    if (!controlled_mind || !req || mind != controlled_mind) return 0;
    uint32_t origin = rt_r32(G_MEM, req + 0xc);                       /* JobReq {jat, jq, qp, origin, ...} */
    if (verbose()) LOG("job %u (origin %u) for the hero you play\n", rt_r32(G_MEM, req), origin);
    if (origin == enum_val(c, "eActionOrigin", "ao_human", 1)) {      /* yours: an attack holds the steering off (steer) */
        uint32_t jat = rt_r32(G_MEM, req);
        if (jat >= enum_val(c, "eJobAbstractType", "jat_attack_object", 2) && jat <= enum_val(c, "eJobAbstractType", "jat_attack_position_ranged", 7)) {
            attack_ordered = now_s(); attack_ix = last_ix; attack_iy = last_iy;
        }
    }
    if (origin != enum_val(c, "eActionOrigin", "ao_reflex", 0xfffffffeu)) return 0;   /* only what its own brain decides */
    static const char *const walks[] = {"jat_move", "jat_follow", "jat_patrol", "jat_flee_from_object", "jat_guard"};
    static const char *const attacks[] = {"jat_attack_object", "jat_attack_object_melee", "jat_attack_object_ranged", "jat_attack_position",
        "jat_attack_position_melee", "jat_attack_position_ranged"};
    uint32_t jat = rt_r32(G_MEM, req);
    for (size_t i = 0; i < sizeof walks / sizeof *walks; i++) if (jat == enum_val(c, "eJobAbstractType", walks[i], 0xfffffffeu)) return 1;
    for (size_t i = 0; i < sizeof attacks / sizeof *attacks; i++) if (jat == enum_val(c, "eJobAbstractType", attacks[i], 0xfffffffeu)) {
        /* it may defend itself where it stands: an attack on an enemy already in reach, while you aren't steering */
        if (steering) return 1;
        uint32_t goid = rt_r32(G_MEM, req + 0x1c), t = goid ? ext_thiscall(c, FX("?GetGo@Goid_@@ABEPAVGo@@XZ"), goid, 0, 0) : 0;
        if (!t) return 1;
        V3 h = world(c, go_pos(c, controlled)), p = world(c, go_pos(c, t));
        float d = sqrtf((p.x - h.x) * (p.x - h.x) + (p.z - h.z) * (p.z - h.z));
        int kind = weapon_kind(c, controlled); float reach = kind == K_MELEE ? 3.0f : kind == K_RANGED ? 18.0f : 14.0f;
        return d > reach;
    }
    return 0;
}
/* the other members guard the controlled hero; the order is given again when they finish it or drift away */
static void companions(Ctx *c, uint32_t *m, int n, double now)
{
    static double last; if (now - last < 0.75 && !companions_now) return; last = now; companions_now = 0;
    uint32_t hero = controlled; if (!hero) return;
    uint32_t qp = enum_val(c, "eQPlace", "qp_clear", 0), ao = enum_val(c, "eActionOrigin", "ao_human", 0);
    uint32_t jat_guard = enum_val(c, "eJobAbstractType", "jat_guard", 0xffffffffu);
    for (int i = 0; i < n && i < 16; i++) {
        uint32_t g = m[i]; if (g == hero || !is_alive(c, g)) { if (i < 16) guarding[i] = 0; continue; }
        uint32_t mind = GO_MIND(g); if (!mind) continue;
        if (is_selected(c, g)) continue;     /* the player is giving it orders too */
        uint32_t jat = ext_thiscall(c, FX("?GetFrontActionJat@GoMind@@QBE?AW4eJobAbstractType@@XZ"), mind, 0, 0);
        if (guarding[i] == hero && (jat == jat_guard || ext_thiscall(c, FX("?AmBusy@GoMind@@QBE_NXZ"), mind, 0, 0) & 0xff)) continue;
        uint32_t a[3] = {hero, qp, ao};
        ext_thiscall(c, FX("?RSGuard@GoMind@@QAEXPAVGo@@W4eQPlace@@W4eActionOrigin@@@Z"), mind, 3, a);
        guarding[i] = hero;
        LOG("member %08x guards %08x (front job %u)\n", g, hero, jat);
    }
}

/* ---------------------------------------------------------------- steering */
static int place_ahead(Ctx *c, uint32_t hero, V3 dir, float dist, uint32_t out)
{
    uint32_t hp = go_pos(c, hero); if (!hp) return 0;
    uint32_t node = rt_r32(G_MEM, hp + 12); float px = rt_rf32(G_MEM, hp), py = rt_rf32(G_MEM, hp + 4), pz = rt_rf32(G_MEM, hp + 8);
    V3 o = world_of(c, node, px, py, pz), ex = sub(world_of(c, node, px + 1, py, pz), o), ez = sub(world_of(c, node, px, py, pz + 1), o);
    float lx = dot(dir, ex), lz = dot(dir, ez), l = sqrtf(lx * lx + lz * lz); if (l < 1e-4f) return 0;
    lx /= l; lz /= l;
    for (float d = dist; d >= 0.6f; d *= 0.5f) {                     /* nearer when the far point has no ground */
        rt_wf32(G_MEM, out, px + lx * d); rt_wf32(G_MEM, out + 4, py); rt_wf32(G_MEM, out + 8, pz + lz * d); rt_w32(G_MEM, out + 12, node);
        uint32_t a[4] = {out, fbits(2.0f), 1, 0};
        if (ext_thiscall(c, 0x663232u, ENGINE(), 4, a) & 0xff) return 1;   /* SiegeEngine::AdjustPointToTerrain */
    }
    return 0;
}
static void steer(Ctx *c, double now)
{
    float x = 0, y = 0;                                               /* x right, y forward */
    if (steer_key('W')) y += 1; if (steer_key('S')) y -= 1; if (steer_key('D')) x += 1; if (steer_key('A')) x -= 1;
    float sx = action_pad.lx, sy = action_pad.ly, sm = sqrtf(sx * sx + sy * sy);
    if (action_pad.connected && sm > 0.22f) { x += sx; y += sy; }
    float m = sqrtf(x * x + y * y);
    uint32_t hero = controlled, mind = hero ? GO_MIND(hero) : 0;
    if (!mind) { steering = 0; return; }
    if (m < 0.01f) {
        if (steering) {                                              /* let go: stop where it is */
            uint32_t ao = enum_val(c, "eActionOrigin", "ao_human", 0);
            ext_thiscall(c, FX("?RSStop@GoMind@@QAEXW4eActionOrigin@@@Z"), mind, 1, &ao);
            LOG("stop\n");
        }
        steering = 0; return;
    }
    x /= m; y /= m;
    if (!steering) {                                                 /* starting to walk: you are this one hero now */
        uint32_t pm[16]; int np = party_members(c, pm, 16); uint32_t off = 0;
        for (int i = 0; i < np; i++) if (pm[i] != hero && is_selected(c, pm[i])) ext_thiscall(c, FX("?Select@Go@@QAE_N_N@Z"), pm[i], 1, &off);
        companions_now = 1;
    }
    V3 f; if (!camera_forward(c, &f)) return;
    V3 r = {-f.z, 0, f.x};                                           /* right of forward on screen (siege space is right-handed) */
    if (getenv("DS_ACTION_FLIPX")) { r.x = -r.x; r.z = -r.z; }
    V3 d = {f.x * y + r.x * x, 0, f.z * y + r.z * x};
    /* One leg of 4.5 m at a time, so the walk isn't interrupted: a new one when the direction asked for changes (in
     * input space: the world frame turns with the camera's node), when the leg is nearly walked, or every half second */
    if (!leg) leg = heap_alloc(w32_process_heap, 8, 16);
    if (steering && now - attack_ordered < 8) {                      /* an attack you ordered while walking: let it play */
        uint32_t jat = ext_thiscall(c, FX("?GetFrontActionJat@GoMind@@QBE?AW4eJobAbstractType@@XZ"), mind, 0, 0);
        int attacking = jat >= enum_val(c, "eJobAbstractType", "jat_attack_object", 2) && jat <= enum_val(c, "eJobAbstractType", "jat_attack_position_ranged", 7);
        if (attacking && x * attack_ix + y * attack_iy > 0.82f) return;   /* until you let go or turn away (about 35 degrees) */
        if (!attacking) attack_ordered = 0;
    }
    if (steering) {
        uint32_t hp = go_pos(c, hero), a[2] = {leg, hp};
        float left = hp ? (float)ext_call_f(c, 0x678deau, 2, a) : 0;          /* GetSiegeDistance */
        if (x * last_ix + y * last_iy > 0.985f && left > 2.2f && now - last_order < 0.5) return;
    }
    uint32_t esp = c->esp, tgt = scratch(c, 16);
    if (place_ahead(c, hero, d, 4.5f, tgt)) {
        memcpy(GP(leg), GP(tgt), 16); last_ix = x; last_iy = y;
        uint32_t qp = enum_val(c, "eQPlace", "qp_clear", 0), ao = enum_val(c, "eActionOrigin", "ao_human", 0);
        uint32_t a[3] = {tgt, qp, ao};
        ext_thiscall(c, FX("?RSMove@GoMind@@QAEXABUSiegePos@@W4eQPlace@@W4eActionOrigin@@@Z"), mind, 3, a);
        LOG("move (%.2f %.2f) -> %.2f %.2f %.2f node %08x\n", x, y, rt_rf32(G_MEM, tgt), rt_rf32(G_MEM, tgt + 4), rt_rf32(G_MEM, tgt + 8), rt_r32(G_MEM, tgt + 12));
        last_order = now; last_dx = d.x; last_dz = d.z; steering = 1;
    } else LOG("no ground ahead\n");
    c->esp = esp;
}

/* ---------------------------------------------------------------- abilities
 * Slot 0 is Dodge, slots 1-4 depend on what the hero holds (melee weapon, ranged weapon, or a spell):
 *   1 Cleave / Volley / Nova      hits several enemies at once (Nova costs mana)
 *   2 Second Wind                 heals the hero
 *   3 Charge / Power Shot / Chain Lightning   one hard hit (Chain Lightning jumps between enemies, costs mana)
 *   4 Rally                       heals and refills the whole party a little
 * Damage is the hero's own damage with the weapon or spell in hand (Rules::GetDamageRange), dealt through the game's
 * own Rules::DamageGo (experience, kills, flinching as usual); the effects are the game's own effect scripts. */
#define GO_GOID(go)      ((go) ? ext_thiscall(c, FX("?GetGoid@Go@@QBEPBUGoid_@@XZ"), (go), 0, 0) : 0)
#define GO_BODY(go)      go_comp(go, "?GetBody@Go@@QAEPAVGoBody@@XZ")
#define GO_INVENTORY(go) go_comp(go, "?GetInventory@Go@@QAEPAVGoInventory@@XZ")
#define GO_COMMON(go)    go_comp(go, "?GetCommon@Go@@QAEPAVGoCommon@@XZ")
#define SINGLETON(name)  w32_callback(c, FX("?FUBI_GetClassSingleton@" name "@@CAPAV1@XZ"), 0, 0)
static uint32_t gstr_perm(const char *s)                       /* a guest copy of a string, kept for good */
{
    static struct { const char *s; uint32_t g; } cache[64]; static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].s, s)) return cache[i].g;
    uint32_t g = heap_alloc(w32_process_heap, 8, (uint32_t)strlen(s) + 1); strcpy((char *)GP(g), s);
    if (n < 64) { cache[n].s = strdup(s); cache[n].g = g; n++; }
    return g;
}
static float aspect_f(Ctx *c, uint32_t go, uint32_t fn) { uint32_t as = GO_ASPECT(go); return as ? (float)ext_thiscall_f(c, fn, as, 0, 0) : 0; }
#define LIFE(go)     aspect_f(c, go, FX("?GetCurrentLife@GoAspect@@QBEMXZ"))
#define MAX_LIFE(go) aspect_f(c, go, FX("?GetMaxLife@GoAspect@@QBEMXZ"))
#define MANA(go)     aspect_f(c, go, FX("?GetCurrentMana@GoAspect@@QBEMXZ"))
#define MAX_MANA(go) aspect_f(c, go, FX("?GetMaxMana@GoAspect@@QBEMXZ"))
static void change_life(Ctx *c, uint32_t go, float d) { uint32_t r = SINGLETON("Rules"), a[2] = {GO_GOID(go), fbits(d)}; if (r && a[0]) ext_thiscall(c, FX("?ChangeLife@Rules@@QAEXPBUGoid_@@M@Z"), r, 2, a); }
static void change_mana(Ctx *c, uint32_t go, float d) { uint32_t r = SINGLETON("Rules"), a[2] = {GO_GOID(go), fbits(d)}; if (r && a[0]) ext_thiscall(c, FX("?ChangeMana@Rules@@QAEXPBUGoid_@@M@Z"), r, 2, a); }
static void effect(Ctx *c, const char *name, uint32_t target, uint32_t source)
{
    uint32_t fx = w32_callback(c, FX("?FUBI_GetClassSingleton@WorldFx@@KAPAV1@XZ"), 0, 0); if (!fx || !target) return;
    uint32_t src = GO_GOID(source ? source : target);
    uint32_t a[6] = {gstr_perm(name), GO_GOID(target), src, gstr_perm(""), src, enum_val(c, "eWorldEvent", "we_req_activate", 0)};
    ext_thiscall(c, FX("?SRunScript@WorldFx@@QAEPBUSFxSID_@@PBDPBUGoid_@@101W4eWorldEvent@@@Z"), fx, 6, a);
}
static int weapon_kind(Ctx *c, uint32_t hero)
{
    static int forced = -2; if (forced == -2) { const char *e = getenv("DS_TEST") ? getenv("DS_ACTION_TESTKIND") : 0;   /* test mode: melee/ranged/magic */
        forced = !e ? -1 : !strcmp(e, "magic") ? K_MAGIC : !strcmp(e, "ranged") ? K_RANGED : K_MELEE; }
    if (forced >= 0) return forced;
    uint32_t inv = GO_INVENTORY(hero); if (!inv) return K_MELEE;
    if (ext_thiscall(c, FX("?IsSpellSelected@GoInventory@@QBE_NXZ"), inv, 0, 0) & 0xff) return K_MAGIC;
    if (ext_thiscall(c, FX("?IsRangedWeaponSelected@GoInventory@@QBE_NXZ"), inv, 0, 0) & 0xff) return K_RANGED;
    return K_MELEE;
}
static uint32_t held_weapon(Ctx *c, uint32_t hero)          /* the weapon or spell in hand (Go), or 0 for fists */
{
    uint32_t inv = GO_INVENTORY(hero); if (!inv) return 0;
    if (ext_thiscall(c, FX("?IsSpellSelected@GoInventory@@QBE_NXZ"), inv, 0, 0) & 0xff) return ext_thiscall(c, FX("?GetSelectedSpell@GoInventory@@QBEPAVGo@@XZ"), inv, 0, 0);
    return ext_thiscall(c, FX("?GetSelectedWeapon@GoInventory@@QBEPAVGo@@XZ"), inv, 0, 0);
}
static float roll_damage(Ctx *c, uint32_t hero)
{
    uint32_t rules = SINGLETON("Rules"), w = held_weapon(c, hero); float lo = 0, hi = 0;
    if (rules) {
        uint32_t esp = c->esp, t = scratch(c, 16); rt_wf32(G_MEM, t, 0); rt_wf32(G_MEM, t + 4, 0);
        uint32_t a[4] = {GO_GOID(hero), GO_GOID(w), t, t + 4}; ext_thiscall(c, FX("?GetDamageRange@Rules@@QAE_NPBUGoid_@@0AAM1@Z"), rules, 4, a);
        lo = rt_rf32(G_MEM, t); hi = rt_rf32(G_MEM, t + 4); c->esp = esp;
    }
    if (!(hi > 0) || hi > 100000) { lo = 3 + 0.05f * MAX_LIFE(hero); hi = lo * 1.5f; }   /* no damage of its own (a healing spell): from the hero's strength */
    return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}
static void hit(Ctx *c, uint32_t hero, uint32_t victim, float mult, const char *fx)
{
    uint32_t rules = SINGLETON("Rules"); if (!rules || !victim) return;
    float d = roll_damage(c, hero) * mult;
    uint32_t a[6] = {GO_GOID(victim), GO_GOID(hero), GO_GOID(held_weapon(c, hero)), fbits(d), 0, 0};
    ext_thiscall(c, FX("?DamageGo@Rules@@QAE_NPBUGoid_@@00M_N1@Z"), rules, 6, a);
    if (fx) effect(c, fx, victim, hero);
    LOG("hit %08x for %.1f\n", victim, d);
}
/* living enemies of the hero within a radius, nearest first, with their direction and distance from the hero */
typedef struct { uint32_t go; float dist, dx, dz; } Foe;
static int foes(Ctx *c, uint32_t hero, float radius, Foe *out, int max)
{
    uint32_t mind = GO_MIND(hero), aiq = SINGLETON("AIQuery"); if (!mind || !aiq) return 0;
    uint32_t coll = ext_thiscall(c, FX("?GetTempGopColl2@AIQuery@@QAEAAUGopColl@@XZ"), aiq, 0, 0); if (!coll) return 0;
    ext_thiscall(c, FX("?Clear@GopColl@@AAEXXZ"), coll, 0, 0);
    uint32_t a[2] = {fbits(radius), coll}; ext_thiscall(c, FX("?GetEnemiesInSphere@GoMind@@QBE_NMPAUGopColl@@@Z"), mind, 2, a);
    int n = (int)ext_thiscall(c, FX("?Size@GopColl@@ABEHXZ"), coll, 0, 0), m = 0; uint32_t got[64]; int ng = 0;
    for (int i = 0; i < n && ng < 64; i++) { uint32_t k = (uint32_t)i; got[ng++] = ext_thiscall(c, FX("?Get@GopColl@@ABEPAVGo@@H@Z"), coll, 1, &k); }
    ext_thiscall(c, FX("?Clear@GopColl@@AAEXXZ"), coll, 0, 0);
    V3 h = world(c, go_pos(c, hero));
    for (int i = 0; i < ng && m < max; i++) {
        uint32_t g = got[i]; if (!g || g == hero || !is_alive(c, g)) continue;
        V3 p = world(c, go_pos(c, g)); float dx = p.x - h.x, dz = p.z - h.z, d = sqrtf(dx * dx + dz * dz);
        if (d > radius) continue;
        Foe f = {g, d, d > 1e-3f ? dx / d : 0, d > 1e-3f ? dz / d : 0}; int j = m++;
        while (j > 0 && out[j - 1].dist > d) { out[j] = out[j - 1]; j--; }
        out[j] = f;
    }
    return m;
}
/* the way the player is facing the action: the direction last steered (for a moment after), else none (any way) */
static int aim(V3 *d, double now)
{
    if (now - last_order > 1.2 || !controlled) return 0;
    /* in this frame's world: towards the leg being walked, else the last input turned by the camera as it is now (the
     * world frame follows the camera's node, so a direction stored earlier may have turned) */
    Ctx *c = aim_ctx; V3 h = world(c, go_pos(c, controlled)), t = leg ? world(c, leg) : h;
    float dx = t.x - h.x, dz = t.z - h.z, l = sqrtf(dx * dx + dz * dz);
    if (l > 0.3f) { d->x = dx / l; d->y = 0; d->z = dz / l; return 1; }
    V3 f; if (!camera_forward(c, &f)) return 0;
    V3 r = {-f.z, 0, f.x}; d->x = f.x * last_iy + r.x * last_ix; d->y = 0; d->z = f.z * last_iy + r.z * last_ix;
    l = sqrtf(d->x * d->x + d->z * d->z); if (l < 1e-3f) return 0; d->x /= l; d->z /= l; return 1;
}
static int in_front(const Foe *f, V3 d, int have, float cos_half) { return !have || f->dx * d.x + f->dz * d.z >= cos_half; }
static void attack(Ctx *c, uint32_t hero, uint32_t target)
{
    uint32_t mind = GO_MIND(hero); if (!mind || !target) return;
    uint32_t a[5] = {enum_val(c, "eJobAbstractType", "jat_attack_object", 0), enum_val(c, "eJobQ", "jq_action", 0),
                     enum_val(c, "eQPlace", "qp_clear", 0), enum_val(c, "eActionOrigin", "ao_human", 0), GO_GOID(target)};
    uint32_t req = w32_callback(c, FX("?MakeJobReq@@YAAAUJobReq@@W4eJobAbstractType@@W4eJobQ@@W4eQPlace@@W4eActionOrigin@@PBUGoid_@@@Z"), 5, a);
    if (req) ext_thiscall(c, FX("?RSDoJob@GoMind@@QAEXABUJobReq@@@Z"), mind, 1, &req);
}
static void toast(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf((char *)action_hud.toast, sizeof action_hud.toast, fmt, ap); va_end(ap);
    LOG("message: %s\n", (const char *)action_hud.toast);
    action_hud.toast_until = now_s() + 2.2; action_hud.seq++;
}
static double cd_until[ACTION_SLOTS], cd_len[ACTION_SLOTS];
/* Dodge: a quick step out of the way, and for 0.6 s nothing hurts the hero (the damage is turned down where every hit
 * lands, Rules 0x5a97b8). The engine's own speed settings don't make anyone faster (measured: walking pace either way),
 * so the step is a walk; the safety is what makes it a dodge. */
static double untouchable_until; static uint32_t untouchable_goid;
static const char *slot_name(int slot, int kind)
{
    static const char *const names[ACTION_SLOTS][3] = {
        {"Dodge", "Dodge", "Dodge"}, {"Cleave", "Volley", "Nova"}, {"Second Wind", "Second Wind", "Second Wind"},
        {"Charge", "Power Shot", "Chain Lightning"}, {"Rally", "Rally", "Rally"}, {"", "", ""}};
    return names[slot][kind];
}
static float slot_cost(Ctx *c, uint32_t hero, int slot, int kind)  /* mana */
{
    float mm = MAX_MANA(hero);
    if (kind == K_MAGIC && slot == 1) return fmaxf(4, mm * 0.12f);
    if (kind == K_MAGIC && slot == 3) return fmaxf(6, mm * 0.18f);
    return 0;
}
static double slot_cooldown(int slot, int kind)
{
    static const double cd[ACTION_SLOTS][3] = {{0.9, 0.9, 0.9}, {3.0, 4.0, 4.5}, {30, 30, 30}, {7, 6, 7}, {60, 60, 60}, {0, 0, 0}};
    return cd[slot][kind];
}
static int use_slot(Ctx *c, uint32_t hero, int slot, double now)
{
    int kind = weapon_kind(c, hero); const char *name = slot_name(slot, kind);
    if (!*name) return 0;
    if (now < cd_until[slot]) return 0;
    if (slot && !(w32_callback(c, 0x51ed95u, 0, 0) & 0xff)) { toast("Abilities work in single player and for the host"); return 0; }   /* IsServerLocal: damage is the host's */
    float cost = slot_cost(c, hero, slot, kind);
    if (cost > 0 && MANA(hero) < cost) { toast("Not enough mana for %s", name); return 0; }
    Foe f[24]; V3 d; int have = aim(&d, now), n, used = 0;
    switch (slot) {
    case 0: {                                                        /* Dodge: a step where you're steering (or away from the nearest foe) */
        V3 dir = d;
        if (!have) { n = foes(c, hero, 12, f, 1); if (n) { dir.x = -f[0].dx; dir.z = -f[0].dz; } else if (!camera_forward(c, &dir)) return 0; }
        uint32_t mind = GO_MIND(hero); if (!mind) return 0;
        uint32_t esp = c->esp, tgt = scratch(c, 16);
        if (place_ahead(c, hero, dir, 4.5f, tgt)) {
            uint32_t a[3] = {tgt, enum_val(c, "eQPlace", "qp_clear", 0), enum_val(c, "eActionOrigin", "ao_human", 0)};
            ext_thiscall(c, FX("?RSMove@GoMind@@QAEXABUSiegePos@@W4eQPlace@@W4eActionOrigin@@@Z"), mind, 3, a);
            untouchable_goid = GO_GOID(hero); untouchable_until = now + 0.6;
            if (!leg) leg = heap_alloc(w32_process_heap, 8, 16);
            memcpy(GP(leg), GP(tgt), 16);                            /* abilities right after aim the way it went */
            effect(c, "dust_explosion_small", hero, hero);
            last_order = now; used = 1;
        }
        c->esp = esp; break;
    }
    case 1:                                                          /* several enemies at once */
        if (kind == K_MELEE) {                                       /* Cleave: everything in an arc within reach */
            n = foes(c, hero, 4.0f, f, 24);
            for (int i = 0; i < n; i++) if (in_front(&f[i], d, have, 0.2f)) { hit(c, hero, f[i].go, 1.1f, "triple_strike_hit"); if (!used) attack(c, hero, f[i].go); used++; }
        } else if (kind == K_RANGED) {                               /* Volley: up to five enemies ahead, in range */
            n = foes(c, hero, 22.0f, f, 24);
            for (int i = 0, k = 0; i < n && k < 5; i++) if (in_front(&f[i], d, have, 0.5f)) { hit(c, hero, f[i].go, 0.85f, "hit_sunburst_10_lye"); if (!used) attack(c, hero, f[i].go); used++; k++; }
        } else {                                                     /* Nova: a burst around the hero */
            n = foes(c, hero, 7.0f, f, 24);
            effect(c, "shock_wave", hero, hero);
            for (int i = 0; i < n; i++) { hit(c, hero, f[i].go, 1.0f, "hit_sunburst_10_lbl"); used++; }
            used = used ? used : 1;                                  /* the burst goes off even with nobody near */
        }
        if (!used) toast("No enemies in reach");
        break;
    case 2: {                                                        /* Second Wind */
        float max = MAX_LIFE(hero), cur = LIFE(hero);
        if (cur >= max - 0.5f) { toast("Already at full health"); break; }
        change_life(c, hero, max * 0.35f); effect(c, "heal", hero, hero); used = 1; break;
    }
    case 3:
        n = foes(c, hero, kind == K_MELEE ? 12.0f : kind == K_RANGED ? 24.0f : 16.0f, f, 24);
        if (kind == K_MAGIC) {                                       /* Chain Lightning: up to six enemies, nearest first */
            for (int i = 0, k = 0; i < n && k < 6; i++) if (in_front(&f[i], d, have, 0.0f)) { hit(c, hero, f[i].go, 0.9f, "lightning_bolt"); used++; k++; }
        } else {                                                     /* Charge / Power Shot: the first enemy ahead, hard */
            for (int i = 0; i < n; i++) if (in_front(&f[i], d, have, 0.6f)) {
                attack(c, hero, f[i].go); hit(c, hero, f[i].go, kind == K_MELEE ? 1.8f : 2.4f, kind == K_MELEE ? "rock_fist_dust" : "hit_beam_15_lor"); used = 1; break;
            }
        }
        if (!used) toast("No enemies in reach");
        break;
    case 4: {                                                        /* Rally: the whole party */
        uint32_t m[16]; int np = party_members(c, m, 16);
        for (int i = 0; i < np; i++) if (is_alive(c, m[i])) { change_life(c, m[i], MAX_LIFE(m[i]) * 0.25f); change_mana(c, m[i], MAX_MANA(m[i]) * 0.20f); effect(c, "heal", m[i], hero); }
        used = 1; break;
    }
    }
    if (!used) return 0;
    if (cost > 0) change_mana(c, hero, -cost);
    cd_len[slot] = slot_cooldown(slot, kind); cd_until[slot] = now + cd_len[slot];
    LOG("%s (slot %d)\n", name, slot);
    return 1;
}
/* the controller's A: attack the nearest enemy ahead, else talk to / use the nearest thing ahead */
static void interact(Ctx *c, uint32_t hero, double now)
{
    Foe f[8]; V3 d; int have = aim(&d, now), n = foes(c, hero, 14.0f, f, 8);
    for (int i = 0; i < n; i++) if (in_front(&f[i], d, have, 0.3f)) { attack(c, hero, f[i].go); LOG("attack %08x\n", f[i].go); return; }
    uint32_t mind = GO_MIND(hero), aiq = SINGLETON("AIQuery"); if (!mind || !aiq) return;
    uint32_t coll = ext_thiscall(c, FX("?GetTempGopColl2@AIQuery@@QAEAAUGopColl@@XZ"), aiq, 0, 0);
    ext_thiscall(c, FX("?Clear@GopColl@@AAEXXZ"), coll, 0, 0);
    uint32_t a[2] = {fbits(4.0f), coll}; ext_thiscall(c, FX("?GetOccupantsInSphere@GoMind@@QBE_NMAAUGopColl@@@Z"), mind, 2, a);
    int k = (int)ext_thiscall(c, FX("?Size@GopColl@@ABEHXZ"), coll, 0, 0); uint32_t best = 0; float bd = 1e9f; int item = 0;
    V3 h = world(c, go_pos(c, hero)); uint32_t got[64]; int ng = 0;
    for (int i = 0; i < k && ng < 64; i++) { uint32_t j = (uint32_t)i; got[ng++] = ext_thiscall(c, FX("?Get@GopColl@@ABEPAVGo@@H@Z"), coll, 1, &j); }
    ext_thiscall(c, FX("?Clear@GopColl@@AAEXXZ"), coll, 0, 0);
    uint32_t m[16]; int np = party_members(c, m, 16);
    for (int i = 0; i < ng; i++) {
        uint32_t g = got[i]; int mine = 0; for (int j = 0; j < np; j++) if (m[j] == g) mine = 1;
        if (!g || mine) continue;
        int is_item = ext_thiscall(c, FX("?IsItem@Go@@QBE_NXZ"), g, 0, 0) & 0xff, usable = ext_thiscall(c, FX("?IsUsable@Go@@QBE_NXZ"), g, 0, 0) & 0xff;
        int actor = ext_thiscall(c, FX("?IsActor@Go@@QBE_NXZ"), g, 0, 0) & 0xff;
        if (!is_item && !usable && !actor) continue;
        V3 p = world(c, go_pos(c, g)); float dx = p.x - h.x, dz = p.z - h.z, dd = sqrtf(dx * dx + dz * dz);
        if (have && dd > 0.5f && (dx * d.x + dz * d.z) / dd < 0.2f) continue;
        if (dd < bd) { bd = dd; best = g; item = is_item; }
    }
    if (!best) return;
    uint32_t b[3] = {best, enum_val(c, "eQPlace", "qp_clear", 0), enum_val(c, "eActionOrigin", "ao_human", 0)};
    ext_thiscall(c, item ? FX("?RSGet@GoMind@@QAEXPAVGo@@W4eQPlace@@W4eActionOrigin@@@Z") : FX("?RSUse@GoMind@@QAEXPAVGo@@W4eQPlace@@W4eActionOrigin@@@Z"), mind, 3, b);
    LOG("%s %08x\n", item ? "get" : "use", best);
}
/* gold and potions on the ground near the hero you play are picked up when it stands idle with no enemy close, as in
 * Diablo (everything else stays for you, or Z to collect all loot) */
static uint32_t pickup_failed[64];
static void pick_up_gold(Ctx *c, uint32_t hero, double now)
{
    static double last; if (now - last < 0.5 || steering || now - last_order < 0.6) return; last = now;
    uint32_t mind = GO_MIND(hero), aiq = SINGLETON("AIQuery"); if (!mind || !aiq) return;
    if (ext_thiscall(c, FX("?AmBusy@GoMind@@QBE_NXZ"), mind, 0, 0) & 0xff) return;
    Foe f[1]; if (foes(c, hero, 12.0f, f, 1)) return;
    uint32_t coll = ext_thiscall(c, FX("?GetTempGopColl2@AIQuery@@QAEAAUGopColl@@XZ"), aiq, 0, 0);
    ext_thiscall(c, FX("?Clear@GopColl@@AAEXXZ"), coll, 0, 0);
    uint32_t a[2] = {fbits(5.0f), coll}; ext_thiscall(c, FX("?GetOccupantsInSphere@GoMind@@QBE_NMAAUGopColl@@@Z"), mind, 2, a);
    int k = (int)ext_thiscall(c, FX("?Size@GopColl@@ABEHXZ"), coll, 0, 0); uint32_t best = 0;
    for (int i = 0; i < k && !best; i++) {
        uint32_t j = (uint32_t)i, g = ext_thiscall(c, FX("?Get@GopColl@@ABEPAVGo@@H@Z"), coll, 1, &j); if (!g) continue;
        if (!(ext_thiscall(c, FX("?IsItem@Go@@QBE_NXZ"), g, 0, 0) & 0xff)) continue;
        if (ext_thiscall(c, FX("?GetParent@Go@@QBEPAV1@XZ"), g, 0, 0)) continue;                  /* carried by someone */
        int skip = 0; for (int b = 0; b < 64; b++) if (pickup_failed[b] == g) skip = 1;
        if (skip) continue;
        if ((ext_thiscall(c, FX("?IsGold@Go@@QBE_NXZ"), g, 0, 0) & 0xff) || (ext_thiscall(c, FX("?IsPotion@Go@@QBE_NXZ"), g, 0, 0) & 0xff)) best = g;
    }
    /* one that is still lying there after a try (the pack is full, it can't be reached) is left alone from then on */
    static uint32_t tried; static double tried_at; static int nfailed;
    if (best && best == tried && now - tried_at < 3.0) best = 0;               /* still on its way */
    else if (best && best == tried) { pickup_failed[nfailed++ % 64] = best; LOG("leaving %08x where it is\n", best); best = 0; }
    ext_thiscall(c, FX("?Clear@GopColl@@AAEXXZ"), coll, 0, 0);
    if (!best) return;
    uint32_t b[3] = {best, enum_val(c, "eQPlace", "qp_clear", 0), enum_val(c, "eActionOrigin", "ao_human", 0)};
    ext_thiscall(c, FX("?RSGet@GoMind@@QAEXPAVGo@@W4eQPlace@@W4eActionOrigin@@@Z"), mind, 3, b);
    tried = best; tried_at = now;
    LOG("picking up %08x\n", best);
}
/* ---------------------------------------------------------------- pace: drops and packs
 * Fights come faster and more often, the way an action RPG's do, with the game's own things only (so a save made in
 * Action Mode is an ordinary save):
 * - a fallen enemy sometimes leaves a small health potion (18%) or mana potion (8%) where it fell;
 * - a group of two or more of the same monster gets one more of its kind half of the time (DS_ACTION_PACKS=0: never).
 *   A lone monster (a boss, a quest character) is never copied. */
typedef struct { uint32_t goid; uint8_t pos[16]; uint32_t tmpl_hash; int seen; } Watched;
static Watched watched[96]; static int nwatched;
static uint32_t packed[512]; static int npacked;                 /* goids already counted for packs (the copies too) */
static uint32_t str_hash(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
static int was_packed(uint32_t goid) { for (int i = 0; i < npacked; i++) if (packed[i] == goid) return 1; return 0; }
static void mark_packed(uint32_t goid) { if (npacked < 512) packed[npacked++] = goid; else packed[(uint32_t)rand() % 512] = goid; }
static uint32_t spawn(Ctx *c, const char *tmpl, const uint8_t *pos, float dx, float dz)
{
    uint32_t godb = SINGLETON("GoDb"); if (!godb) return 0;
    uint32_t esp = c->esp, sp = scratch(c, 16); memcpy(GP(sp), pos, 16);
    if (dx || dz) {                                                  /* a step aside, on the ground */
        rt_wf32(G_MEM, sp, rt_rf32(G_MEM, sp) + dx); rt_wf32(G_MEM, sp + 8, rt_rf32(G_MEM, sp + 8) + dz);
        uint32_t a[4] = {sp, fbits(2.0f), 1, 0};
        if (!(ext_thiscall(c, 0x663232u, ENGINE(), 4, a) & 0xff)) memcpy(GP(sp), pos, 16);
    }
    uint32_t t = gstr_perm(tmpl), req = w32_callback(c, FX("?MakeGoCloneReq@@YAAAUGoCloneReq@@PBD@Z"), 1, &t);
    ext_thiscall(c, FX("?SetStartingPos@GoCloneReq@@QAEXABUSiegePos@@@Z"), req, 1, &sp);
    uint32_t goid = ext_thiscall(c, FX("?SCloneGo@GoDb@@QAEPBUGoid_@@ABUGoCloneReq@@@Z"), godb, 1, &req);
    c->esp = esp; return goid;
}
static void pace(Ctx *c, uint32_t hero, double now)
{
    static double last; if (now - last < 0.25) return; last = now;
    if (!(w32_callback(c, 0x51ed95u, 0, 0) & 0xff)) return;          /* the host's world only */
    Foe f[48]; int n = foes(c, hero, 26.0f, f, 48);
    for (int i = 0; i < nwatched; i++) watched[i].seen = 0;
    for (int i = 0; i < n; i++) {
        uint32_t goid = GO_GOID(f[i].go), pos = go_pos(c, f[i].go); if (!goid || !pos) continue;
        int k = 0; while (k < nwatched && watched[k].goid != goid) k++;
        if (k == nwatched) { if (nwatched == 96) continue; nwatched++; watched[k].goid = goid;
                             watched[k].tmpl_hash = str_hash(GS(ext_thiscall(c, FX("?GetTemplateName@Go@@QBEPBDXZ"), f[i].go, 0, 0))); }
        memcpy(watched[k].pos, GP(pos), 16); watched[k].seen = 1;
    }
    for (int i = 0; i < nwatched; i++) {
        if (watched[i].seen) continue;
        uint32_t go = ext_thiscall(c, FX("?GetGo@Goid_@@ABEPAVGo@@XZ"), watched[i].goid, 0, 0);
        uint32_t as = go ? GO_ASPECT(go) : 0, ls = as ? ext_thiscall(c, FX("?GetLifeState@GoAspect@@QBE?AW4eLifeState@@XZ"), as, 0, 0) : 0;
        if (go && as && ls != enum_val(c, "eLifeState", "ls_alive_conscious", 1) && ls != enum_val(c, "eLifeState", "ls_alive_unconscious", 2)) {
            int r = rand() % 100; const char *drop = r < 18 ? "potion_health_small" : r < 26 ? "potion_mana_small" : 0;
            if (drop) { uint32_t g = spawn(c, drop, watched[i].pos, 0.4f, 0.4f); LOG("%s dropped (%08x)\n", drop, g); }
        }
        watched[i--] = watched[--nwatched];                         /* dead, or out of sight: no longer watched */
    }
    /* packs: a new group of two or more of the same monster, near each other */
    static int packs = -1; if (packs < 0) { const char *e = getenv("DS_ACTION_PACKS"); packs = !e || strcmp(e, "0"); }
    if (!packs) return;
    for (int i = 0; i < n; i++) {
        uint32_t gi = GO_GOID(f[i].go); if (!gi || was_packed(gi)) continue;
        const char *tn = GS(ext_thiscall(c, FX("?GetTemplateName@Go@@QBEPBDXZ"), f[i].go, 0, 0));
        int same = 0; uint32_t members[16]; int nm = 0;
        V3 a = world(c, go_pos(c, f[i].go));
        for (int j = 0; j < n && nm < 16; j++) {
            if (strcmp(GS(ext_thiscall(c, FX("?GetTemplateName@Go@@QBEPBDXZ"), f[j].go, 0, 0)), tn)) continue;
            V3 b = world(c, go_pos(c, f[j].go)); if ((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z) > 14.0f * 14.0f) continue;
            members[nm++] = GO_GOID(f[j].go); same++;
        }
        int counted = 0; for (int j = 0; j < nm; j++) if (members[j] != gi && was_packed(members[j])) counted = 1;   /* the group was counted already */
        for (int j = 0; j < nm && !counted; j++) {                   /* or it has a copy in it (no content id): grown before a reload */
            uint32_t g = ext_thiscall(c, FX("?GetGo@Goid_@@ABEPAVGo@@XZ"), members[j], 0, 0), scid = g ? ext_thiscall(c, FX("?GetScid@Go@@QBEPBUScid_@@XZ"), g, 0, 0) : 1;
            if (verbose()) LOG("pack member %08x scid %08x\n", members[j], scid);
            if (!scid || scid >= 0xfffffff0u) counted = 1;              /* copies read 0xfffffffe (made at run time) */
        }
        for (int j = 0; j < nm; j++) mark_packed(members[j]);
        static int always = -1; if (always < 0) always = getenv("DS_TEST") && getenv("DS_ACTION_PACKTEST");   /* test mode: every group */
        if (counted || same < 2 || n > 24 || (!always && rand() % 2)) continue;
        char tcopy[96]; snprintf(tcopy, sizeof tcopy, "%s", tn);
        uint32_t esp = c->esp, pos = go_pos(c, f[i].go); uint8_t p16[16]; memcpy(p16, GP(pos), 16); c->esp = esp;
        uint32_t g = spawn(c, tcopy, p16, 1.3f, -1.1f); if (g) mark_packed(g);
        uint32_t cg = g ? ext_thiscall(c, FX("?GetGo@Goid_@@ABEPAVGo@@XZ"), g, 0, 0) : 0, hm = GO_MIND(hero);
        LOG("pack of %d %s: one more (%08x: %s, %s, scid %08x)\n", same, tcopy, g, cg && is_alive(c, cg) ? "alive" : "not alive",
            cg && hm && (ext_thiscall(c, FX("?IsEnemy@GoMind@@QBE_NPBVGo@@@Z"), hm, 1, &cg) & 0xff) ? "an enemy" : "not an enemy",
            cg ? ext_thiscall(c, FX("?GetScid@Go@@QBEPBUScid_@@XZ"), cg, 0, 0) : 0);
    }
}

static int slot_request[ACTION_SLOTS];                               /* from the keyboard (binder) */
static int interact_request;
static const char *slot_key(int slot) { static const char *const k[ACTION_SLOTS] = {"Space", "1", "2", "3", "4", ""}; return k[slot]; }
static void hud_fill(Ctx *c, uint32_t hero, double now)
{
    int kind = weapon_kind(c, hero), changed = 0; float mana = MANA(hero);
    for (int i = 0; i < ACTION_SLOTS; i++) {
        const char *nm = slot_name(i, kind); volatile typeof(action_hud.slot[0]) *s = &action_hud.slot[i];
        if (strcmp((const char *)s->name, nm)) { snprintf((char *)s->name, sizeof s->name, "%s", nm); snprintf((char *)s->key, sizeof s->key, "%s", slot_key(i)); changed = 1; }
        double left = cd_until[i] - now; s->cooldown = left > 0 && cd_len[i] > 0 ? (float)(left / cd_len[i]) : 0;
        float cost = *nm ? slot_cost(c, hero, i, kind) : 0; int ready = left <= 0 && mana >= cost;
        if (s->ready != ready) { s->ready = ready; changed = 1; }
        s->cost = cost;
    }
    action_hud.life = LIFE(hero); action_hud.life_max = MAX_LIFE(hero); action_hud.mana = mana; action_hud.mana_max = MAX_MANA(hero);
    if (changed) action_hud.seq++;
}
/* a game window is open (the inventory, a shop, a conversation, the options...): steering and abilities pause and the
 * controller works the pointer. Some interfaces hold always-shown parts too (the inventory's), so for those one window
 * that only shows with the screen open is asked instead. */
static int window_visible(Ctx *c, uint32_t shell, const char *window, const char *iface)
{
    uint32_t a[2] = {gstr_perm(window), gstr_perm(iface)}, w = ext_thiscall(c, 0x6e06d3u, shell, 2, a);   /* UIShell::FindUIWindow */
    return w && (ext_thiscall(c, FX("?GetVisible@UIWindow@@QBE_NXZ"), w, 0, 0) & 0xff);
}
static int menu_open(Ctx *c)
{
    /* {window, interface}: a window that only shows while that screen is open (the interfaces themselves stay loaded) */
    static const char *const windows[][2] = {
        {"button_inventory_exit", "inventory"}, {"pack_mule_button_inventory_exit", "inventory"}, {"character_paperdoll", "character"},
        {"button_spellbook_close", "spell"}, {"button_gold_human_1", "multi_inventory"}, {"button_cancel", "store"},
        {"dialogue_main_bg", "dialogue_box"}, {"exit_game_button", "in_game_menu"}, {"button_x", "journal"}, {"button_save_cancel", "loadsave_game"},
        {"button_ok", "options_audio"}, {"button_ok", "options_game"}, {"button_ok", "options_video"}, {"button_ok", "options_input"},
        {"button_ok", "options_bindings"}, {"background", "backend_dialog"}, {"button_confirm", "gold_transfer"}, {"defeat_background", "defeat_menu"},
        {"dialog_box_eg_bg", "end_game"}, {"button_close", "lore_book"}, {"button_stat_okay", "hire_stats"}};
    uint32_t shell = rt_r32(G_MEM, 0x7a065cu); if (!shell) return 0;
    const char *open = 0;
    for (size_t i = 0; i < sizeof windows / sizeof *windows && !open; i++) {
        uint32_t n = gpstr(c, windows[i][1]);
        if ((ext_thiscall(c, FX("?IsInterfaceVisible@UIShell@@QAE_NABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@@Z"), shell, 1, &n) & 0xff)
            && window_visible(c, shell, windows[i][0], windows[i][1])) open = windows[i][1];
    }
    static const char *was; if (was != open) { was = open; LOG("window open: %s\n", open ? open : "(none)"); }
    return open != 0;
}
static void hero_name(Ctx *c, uint32_t go, char *out, size_t cap)
{
    uint32_t cm = GO_COMMON(go); snprintf(out, cap, "%s", "");
    if (cm) {
        uint32_t esp = c->esp, g = scratch(c, 16); ext_thiscall(c, 0x42aea8u, g, 0, 0);          /* gpstring::gpstring() */
        ext_thiscall(c, FX("?GetScreenName@GoCommon@@QBEXAAV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@@Z"), cm, 1, &g);
        uint32_t t = rt_r32(G_MEM, g); if (t) snprintf(out, cap, "%s", GS(t));
        ext_thiscall(c, 0x48899au, g, 0, 0); c->esp = esp;                                     /* ~gpstring */
    }
    if (!*out) snprintf(out, cap, "%s", GS(ext_thiscall(c, FX("?GetTemplateName@Go@@QBEPBDXZ"), go, 0, 0)));
}

/* test mode: DS_ACTION_TESTPARTY=gyorn,ulora clones those characters next to the hero and has them join the party */
static void test_party(Ctx *c, uint32_t hero)
{
    const char *e = getenv("DS_ACTION_TESTPARTY"); if (!e || !getenv("DS_TEST") || !hero) return;
    char list[256]; snprintf(list, sizeof list, "%s", e);
    uint32_t server = SINGLETON("Server"), party = server ? ext_thiscall(c, FX("?GetScreenParty@Server@@QAEPAVGo@@XZ"), server, 0, 0) : 0;
    uint32_t gp = party ? go_comp(party, "?GetParty@Go@@QAEPAVGoParty@@XZ") : 0, godb = SINGLETON("GoDb"); if (!gp || !godb) return;
    for (char *save, *t = strtok_r(list, ",", &save); t; t = strtok_r(0, ",", &save)) {
        uint32_t a[1] = {gstr_perm(t)}, req = w32_callback(c, FX("?MakeGoCloneReq@@YAAAUGoCloneReq@@PBD@Z"), 1, a), pos = go_pos(c, hero);
        ext_thiscall(c, FX("?SetStartingPos@GoCloneReq@@QAEXABUSiegePos@@@Z"), req, 1, &pos);
        uint32_t goid = ext_thiscall(c, FX("?SCloneGo@GoDb@@QAEPBUGoid_@@ABUGoCloneReq@@@Z"), godb, 1, &req);
        uint32_t go = goid ? ext_thiscall(c, FX("?GetGo@Goid_@@ABEPAVGo@@XZ"), goid, 0, 0) : 0;
        if (go) ext_thiscall(c, FX("?RSAddMemberNow@GoParty@@QAEXPAVGo@@@Z"), gp, 1, &go);
        LOG("test party: %s -> %08x\n", t, go);
    }
}

/* the world was left or is being reloaded: every Go pointer from it is gone */
static void forget_world(void)
{
    controlled = controlled_mind = 0; steering = 0; untouchable_goid = 0; last_order = 0; attack_ordered = 0; memset(guarding, 0, sizeof guarding);
    memset(slot_request, 0, sizeof slot_request); interact_request = 0; nwatched = 0; npacked = 0; memset(pickup_failed, 0, sizeof pickup_failed);
}
/* DS_ACTIONLOG=2: how evenly the hero walks while steered (per-frame distance, GetSiegeDistance), summed up at each stop */
static void speed_probe(Ctx *c, double now)
{
    static uint32_t prev; static int have; static double t0, last, sum, sum2; static int n, slow; static float speeds[2048];
    if (!verbose() || !controlled) return;
    if (!prev) prev = heap_alloc(w32_process_heap, 8, 16);
    uint32_t p = go_pos(c, controlled); if (!p) return;
    static int clickwalk; static double still_since;                  /* a walk on a click (one long order), for comparison */
    if (!steering && !have && now > last) {
        uint32_t a[2] = {prev, p}; float d = (float)ext_call_f(c, 0x678deau, 2, a), v = d / (float)(now - last);
        if (v > 0.8f) { if (!clickwalk) { clickwalk = 1; t0 = now; n = 0; sum = sum2 = 0; slow = 0; } still_since = 0;
                        if (now - t0 > 0.4 && n < 2048) { speeds[n++] = v; sum += v; sum2 += v * v; } }
        else if (clickwalk) {
            if (!still_since) still_since = now;
            else if (now - still_since > 0.3) {
                clickwalk = 0; still_since = 0;
                if (n > 10) { double mean = sum / n, sd = sqrt(fmax(0, sum2 / n - mean * mean)); slow = 0; for (int i = 0; i < n; i++) if (speeds[i] < mean * 0.3) slow++;
                              LOG("click walk: %d frames, %.2f m/s mean, %.2f sd, %d frames under 30%% of it\n", n, mean, sd, slow); }
            } else if (n < 2048) { speeds[n++] = v; sum += v; sum2 += v * v; }
        }
    }
    if (steering && have && now > last) {
        uint32_t a[2] = {prev, p}; float d = (float)ext_call_f(c, 0x678deau, 2, a), v = d / (float)(now - last);
        if (now - t0 > 0.4 && n < 2048) { speeds[n++] = v; sum += v; sum2 += v * v; }
    }
    if (steering && !have) { t0 = now; n = 0; sum = sum2 = 0; slow = 0; }
    if (!steering && have && n > 10) {
        double mean = sum / n, sd = sqrt(fmax(0, sum2 / n - mean * mean)); for (int i = 0; i < n; i++) if (speeds[i] < mean * 0.3) slow++;
        LOG("walk: %d frames, %.2f m/s mean, %.2f sd, %d frames under 30%% of it\n", n, mean, sd, slow);
    }
    have = steering; memcpy(GP(prev), GP(p), 16); last = now;
}

/* ---------------------------------------------------------------- per frame */
static void frame(Ctx *c)
{
    uint32_t ws = rt_r32(G_MEM, rt_r32(G_MEM, 0x7a05c0u) + 0x20);
    if (!(w32_callback(c, FX("?IsInGame@@YA_NW4eWorldState@@@Z"), 1, &ws) & 0xff)) { forget_world(); if (action_hud.in_game) { action_hud.in_game = 0; action_hud.seq++; } return; }
    double now = now_s(); aim_ctx = c;
    if (ws == 27) {                                                   /* ws_sp_nis: a cutscene plays: nothing of ours, no overlay */
        steering = 0; if (action_hud.in_game) { action_hud.in_game = 0; action_hud.seq++; } return;
    }
    static int once; if (!once) { once = 1; LOG("in game (world state %u)\n", ws); }
    uint32_t m[16]; int n = party_members(c, m, 16); if (!n) return;
    if (once == 1) { once = 2; LOG("party of %d\n", n); }
    uint32_t want = pick_controlled(c, m, n);
    if (want && !is_selected(c, want)) {                             /* the one you played is down: the new one is selected (camera, portraits) */
        int any = 0; for (int i = 0; i < n; i++) if (is_selected(c, m[i]) && is_alive(c, m[i])) any = 1;
        if (!any) { uint32_t on = 1; ext_thiscall(c, FX("?Select@Go@@QAE_N_N@Z"), want, 1, &on);
                    uint32_t off = 0; for (int i = 0; i < n; i++) if (m[i] != want && is_selected(c, m[i])) ext_thiscall(c, FX("?Select@Go@@QAE_N_N@Z"), m[i], 1, &off); }
    }
    if (once == 2) { once = 3; LOG("picked %08x\n", want); }
    if (want != controlled) { take_control(c, want); memset(guarding, 0, sizeof guarding); }
    static int tested; if (!tested && controlled) { tested = 1; test_party(c, controlled); }
    if (logging()) {                                                  /* where the hero is, once a second */
        static double t; if (now - t > 1.0 && controlled) { t = now; uint32_t p = go_pos(c, controlled); V3 w = world(c, p), f = {0, 0, 0}; camera_forward(c, &f);
            LOG("hero at %.2f %.2f %.2f, camera forward %.2f %.2f\n", w.x, w.y, w.z, f.x, f.z); }
    }
    static double menu_t; static int menu; if (now - menu_t > 0.1) { menu_t = now; menu = menu_open(c); }
    if (action_hud.menu_open != menu) { action_hud.menu_open = menu; action_hud.seq++; }
    for (int vk = '1'; vk <= '4'; vk++) if (!(w32_keys[vk] & 0x80)) steer_key_seen[vk] = 0;
    if (!(w32_keys[' '] & 0x80)) steer_key_seen[' '] = 0;
    uint32_t app = rt_r32(G_MEM, 0x79cf74u); int paused = app && G_MEM[app + 0x7c];   /* AppModule::IsUserPaused */
    if (!menu && !paused) {
        steer(c, now);
        speed_probe(c, now);
        static uint32_t prev; uint32_t b = action_pad.buttons, pressed = b & ~prev; prev = b;
        static const uint32_t pad_slot[ACTION_SLOTS] = {PAD_B, PAD_X, PAD_Y, PAD_RB, PAD_RT, 0};
        for (int i = 0; i < ACTION_SLOTS; i++) if ((pressed & pad_slot[i]) || slot_request[i]) { slot_request[i] = 0; if (controlled) use_slot(c, controlled, i, now); }
        if ((pressed & PAD_A) || interact_request) { interact_request = 0; if (controlled) interact(c, controlled, now); }
        if (controlled && w32_callback(c, 0x51ed95u, 0, 0) & 0xff) pick_up_gold(c, controlled, now);
        if (controlled) pace(c, controlled, now);
    } else memset(slot_request, 0, sizeof slot_request);
    companions(c, m, n, now);
    if (controlled) hud_fill(c, controlled, now);
    if (!action_hud.in_game) { action_hud.in_game = 1; action_hud.seq++; }
}

/* lift.py OVERRIDES (dispatched first by hooks.c rt_override): return 1 when the call was taken over */
int action_override(Ctx *c, uint32_t addr)
{
    if (!action_on()) return 0;
    switch (addr) {
    case 0x4e72f3:                                    /* UIGame publishes its key commands: the game's input binder */
        game_binder = rt_r32(G_MEM, c->ecx + 8); return 0;
    case 0x43b01e: {                                  /* an input binder handles a key event {flags, key, ...} */
        if (!game_binder || c->ecx != game_binder) return 0;
        uint32_t ev = ARG(0), vk = rt_r32(G_MEM, ev + 4);
        if (vk >= 256 || !stolen_key(vk)) return 0;
        uint32_t ws = rt_r32(G_MEM, rt_r32(G_MEM, 0x7a05c0u) + 0x20), ecx = c->ecx;
        int in_game = w32_callback(c, FX("?IsInGame@@YA_NW4eWorldState@@@Z"), 1, &ws) & 0xff; c->ecx = ecx;
        if (!in_game) return 0;
        if ((w32_keys[0x11] | w32_keys[0x12]) & 0x80) return 0;   /* Ctrl/Alt chords stay the game's */
        if (vk == 'P') { rt_w32(G_MEM, ev + 4, 0x13); return 0; }     /* P: the game's Pause key */
        if (!(w32_keys[vk] & 0x80)) steer_key_seen[vk] = 0;           /* let go: the next press is a new one */
        else {
            if (!steer_key_seen[vk]) {                                 /* pressed now (not a repeat) */
                if (vk == ' ') slot_request[0] = 1;
                if (vk >= '1' && vk <= '4') slot_request[vk - '0'] = 1;
            }
            steer_key_seen[vk] = now_s();
        }
        c->eax = 1; c->esp += 4 + 4; return 1;        /* handled: the game's own commands on these keys don't run */
    }
    case 0x5d281f: {                                  /* GoMind::SDoJob(const JobReq&) (ecx: the mind) */
        uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, top = c->top;
        int drop = unasked_job(c, ecx, ARG(0));
        c->eax = eax; c->ecx = ecx; c->edx = edx; c->top = top;
        if (!drop) return 0;
        { static double t; double n = now_s(); if (n - t > 2) { t = n; LOG("turned down job %u for the hero you play\n", rt_r32(G_MEM, ARG(0))); } }
        c->eax = 0; c->esp += 4 + 4; return 1;        /* no job */
    }
    case 0x5a97b8: {                                  /* Rules: damage (Goid victim, Goid attacker, Goid weapon, float, bool, bool, int) */
        if (!untouchable_goid || ARG(0) != untouchable_goid || ARG(1) == untouchable_goid || now_s() > untouchable_until) return 0;
        { static double t; double n = now_s(); if (n - t > 0.3) { t = n; LOG("dodged %.1f damage\n", ARGF(3)); } }
        c->eax = 0; c->esp += 4 + 28; return 1;        /* no damage while dodging */
    }
    case 0x57470d: {                                  /* WorldTime::Update: once a frame on the game's thread */
        uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, top = c->top;   /* the caller's registers: `this` is in ecx */
        double t0 = verbose() ? now_s() : 0;
        frame(c); c->eax = eax; c->ecx = ecx; c->edx = edx; c->top = top;
        if (verbose()) {                                              /* DS_ACTIONLOG=2: what Action Mode costs a frame */
            static double sum, worst, since; static int n; double t = now_s() - t0; sum += t; n++; if (t > worst) worst = t;
            if (!since) since = t0; if (t0 - since > 10) { LOG("cost: %.3f ms a frame on average, %.2f ms at most (%d frames)\n", sum / n * 1000, worst * 1000, n); sum = worst = 0; n = 0; since = t0; }
        }
        return 0;
    }
    }
    return 0;
}
