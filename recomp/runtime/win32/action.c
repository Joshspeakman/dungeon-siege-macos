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

int action_on(void)
{
    if (action_active < 0) { const char *e = getenv("DS_ACTION"); action_active = e && *e && strcmp(e, "0"); }
    return action_active;
}

/* ---------------------------------------------------------------- engine access */
static uint32_t enum_val(Ctx *c, const char *type, const char *name, uint32_t dflt);
static void hero_name(Ctx *c, uint32_t go, char *out, size_t cap);
static void toast(const char *fmt, ...);
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
static uint32_t controlled;                 /* the hero being steered (Go) */
static uint32_t guarding[16];               /* party member -> the hero it was told to guard */
static uint32_t saved_move_orders[16][2];   /* {go, orders + 1}: the controlled hero's own movement orders, given back */

static uint32_t pick_controlled(Ctx *c, uint32_t *m, int n)
{
    for (int i = 0; i < n; i++) if (is_selected(c, m[i]) && is_alive(c, m[i])) return m[i];
    if (controlled) for (int i = 0; i < n; i++) if (m[i] == controlled && is_alive(c, m[i])) return controlled;
    for (int i = 0; i < n; i++) if (is_alive(c, m[i])) return m[i];
    return 0;
}
static void set_move_orders(Ctx *c, uint32_t go, const char *orders)
{
    uint32_t mind = GO_MIND(go); if (!mind) return;
    uint32_t v = enum_val(c, "eMovementOrders", orders, 0xffffffffu); if (v == 0xffffffffu) return;
    uint32_t cur = ext_thiscall(c, FX("?GetMovementOrders@GoMind@@QBE?AW4eMovementOrders@@XZ"), mind, 0, 0);
    if (cur == v) return;
    ext_thiscall(c, FX("?RSSetMovementOrders@GoMind@@QAEXW4eMovementOrders@@@Z"), mind, 1, &v);
}
static void take_control(Ctx *c, uint32_t go)
{
    if (go == controlled) return;
    if (controlled) {                        /* the old hero gets its own movement orders back */
        for (int i = 0; i < 16; i++) if (saved_move_orders[i][0] == controlled) {
            uint32_t mind = GO_MIND(controlled), v = saved_move_orders[i][1] - 1;
            if (mind) ext_thiscall(c, FX("?RSSetMovementOrders@GoMind@@QAEXW4eMovementOrders@@@Z"), mind, 1, &v);
            saved_move_orders[i][0] = 0;
        }
    }
    controlled = go;
    if (!go) return;
    uint32_t mind = GO_MIND(go);
    if (mind) {
        int k = 0; while (k < 16 && saved_move_orders[k][0]) k++;
        if (k < 16) { saved_move_orders[k][0] = go; saved_move_orders[k][1] = ext_thiscall(c, FX("?GetMovementOrders@GoMind@@QBE?AW4eMovementOrders@@XZ"), mind, 0, 0) + 1; }
    }
    set_move_orders(c, go, "mo_hold");      /* it stays where you put it: no chasing on its own */
    char nm[40]; hero_name(c, go, nm, sizeof nm); snprintf((char *)action_hud.hero, sizeof action_hud.hero, "%s", nm);
    static int first = 1; if (!first) toast("Playing as %s", nm); first = 0;
    LOG("controlling %08x (%s)\n", go, nm);
}
/* the other members guard the controlled hero; the order is given again when they finish it or drift away */
static void companions(Ctx *c, uint32_t *m, int n, double now)
{
    static double last; if (now - last < 0.75) return; last = now;
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
static int steering;                        /* a direction was held last frame */
static double last_order; static float last_dx, last_dz;
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
    V3 f; if (!camera_forward(c, &f)) return;
    V3 r = {-f.z, 0, f.x};                                           /* right of forward on screen (siege space is right-handed) */
    if (getenv("DS_ACTION_FLIPX")) { r.x = -r.x; r.z = -r.z; }
    V3 d = {f.x * y + r.x * x, 0, f.z * y + r.z * x};
    float turn = d.x * last_dx + d.z * last_dz;                      /* cos of the change since the last order */
    if (steering && now - last_order < 0.15 && turn > 0.95f) return;
    uint32_t esp = c->esp, tgt = scratch(c, 16);
    if (place_ahead(c, hero, d, 2.5f, tgt)) {
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
enum { K_MELEE, K_RANGED, K_MAGIC };
static int weapon_kind(Ctx *c, uint32_t hero)
{
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
static int aim(V3 *d, double now) { if (now - last_order > 1.2) return 0; d->x = last_dx; d->y = 0; d->z = last_dz; return 1; }
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
    action_hud.toast_until = now_s() + 2.2; action_hud.seq++;
}
static double cd_until[ACTION_SLOTS], cd_len[ACTION_SLOTS];
static double dash_until; static uint32_t dash_body; static float dash_saved, dash_set;
static void dash_end(Ctx *c)
{
    if (!dash_body) return;
    if (rt_rf32(G_MEM, dash_body + 0x2c) == dash_set) rt_wf32(G_MEM, dash_body + 0x2c, dash_saved);   /* GoBody max move velocity */
    dash_body = 0; dash_until = 0; (void)c;
}
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
    float cost = slot_cost(c, hero, slot, kind);
    if (cost > 0 && MANA(hero) < cost) { toast("Not enough mana for %s", name); return 0; }
    Foe f[24]; V3 d; int have = aim(&d, now), n, used = 0;
    switch (slot) {
    case 0: {                                                        /* Dodge: a quick dash where you're steering (or away from the nearest foe) */
        V3 dir = d;
        if (!have) { n = foes(c, hero, 12, f, 1); if (n) { dir.x = -f[0].dx; dir.z = -f[0].dz; } else if (!camera_forward(c, &dir)) return 0; }
        uint32_t body = GO_BODY(hero), mind = GO_MIND(hero); if (!body || !mind) return 0;
        uint32_t esp = c->esp, tgt = scratch(c, 16);
        if (place_ahead(c, hero, dir, 4.5f, tgt)) {
            dash_end(c);
            dash_body = body; dash_saved = rt_rf32(G_MEM, body + 0x2c); dash_set = dash_saved * 2.6f; rt_wf32(G_MEM, body + 0x2c, dash_set);
            dash_until = now + 0.45;
            uint32_t a[3] = {tgt, enum_val(c, "eQPlace", "qp_clear", 0), enum_val(c, "eActionOrigin", "ao_human", 0)};
            ext_thiscall(c, FX("?RSMove@GoMind@@QAEXABUSiegePos@@W4eQPlace@@W4eActionOrigin@@@Z"), mind, 3, a);
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
            effect(c, "hock_wave", hero, hero);
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
                if (kind == K_MELEE && f[i].dist > 2.5f) {           /* rush in */
                    uint32_t body = GO_BODY(hero);
                    if (body) { dash_end(c); dash_body = body; dash_saved = rt_rf32(G_MEM, body + 0x2c); dash_set = dash_saved * 2.4f; rt_wf32(G_MEM, body + 0x2c, dash_set); dash_until = now + 0.9; }
                }
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
static int menu_open(Ctx *c)
{
    static const char *const ifaces[] = {"inventory", "multi_inventory", "character", "spell", "store", "dialogue_box", "in_game_menu", "journal",
        "loadsave_game", "options_audio", "options_bindings", "options_game", "options_input", "options_video", "backend_dialog", "gold_transfer",
        "defeat_menu", "end_game", "lore_book", "hire_stats", "field_commands", "game_console", "world_map"};
    uint32_t shell = rt_r32(G_MEM, 0x7a065cu); if (!shell) return 0;
    for (size_t i = 0; i < sizeof ifaces / sizeof *ifaces; i++) {
        uint32_t n = gpstr(c, ifaces[i]);
        if (ext_thiscall(c, FX("?IsInterfaceVisible@UIShell@@QAE_NABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@@Z"), shell, 1, &n) & 0xff) {
            static const char *was; if (was != ifaces[i]) { was = ifaces[i]; LOG("window open: %s\n", ifaces[i]); }
            return 1;
        }
    }
    return 0;
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

/* ---------------------------------------------------------------- per frame */
static void frame(Ctx *c)
{
    uint32_t ws = rt_r32(G_MEM, rt_r32(G_MEM, 0x7a05c0u) + 0x20);
    if (!(w32_callback(c, FX("?IsInGame@@YA_NW4eWorldState@@@Z"), 1, &ws) & 0xff)) { controlled = 0; steering = 0; if (action_hud.in_game) { action_hud.in_game = 0; action_hud.seq++; } return; }
    double now = now_s();
    static int once; if (!once) { once = 1; LOG("in game (world state %u)\n", ws); }
    uint32_t m[16]; int n = party_members(c, m, 16); if (!n) return;
    if (once == 1) { once = 2; LOG("party of %d\n", n); }
    uint32_t want = pick_controlled(c, m, n);
    if (once == 2) { once = 3; LOG("picked %08x\n", want); }
    if (want != controlled) { take_control(c, want); memset(guarding, 0, sizeof guarding); }
    if (logging()) {                                                  /* where the hero is, once a second */
        static double t; if (now - t > 1.0 && controlled) { t = now; uint32_t p = go_pos(c, controlled); V3 w = world(c, p), f = {0, 0, 0}; camera_forward(c, &f);
            LOG("hero at %.2f %.2f %.2f, camera forward %.2f %.2f\n", w.x, w.y, w.z, f.x, f.z); }
    }
    static double menu_t; static int menu; if (now - menu_t > 0.1) { menu_t = now; menu = menu_open(c); }
    if (action_hud.menu_open != menu) { action_hud.menu_open = menu; action_hud.seq++; }
    if (dash_until && now > dash_until) dash_end(c);
    if (!menu) {
        steer(c, now);
        static uint32_t prev; uint32_t b = action_pad.buttons, pressed = b & ~prev; prev = b;
        static const uint32_t pad_slot[ACTION_SLOTS] = {PAD_B, PAD_X, PAD_Y, PAD_RB, PAD_RT, 0};
        for (int i = 0; i < ACTION_SLOTS; i++) if ((pressed & pad_slot[i]) || slot_request[i]) { slot_request[i] = 0; if (controlled) use_slot(c, controlled, i, now); }
        if ((pressed & PAD_A) || interact_request) { interact_request = 0; if (controlled) interact(c, controlled, now); }
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
        if (w32_keys[vk] & 0x80) {
            if (!steer_key_seen[vk]) {                                 /* pressed now (not a repeat) */
                if (vk == ' ') slot_request[0] = 1;
                if (vk >= '1' && vk <= '4') slot_request[vk - '0'] = 1;
            }
            steer_key_seen[vk] = now_s();
        }
        c->eax = 1; c->esp += 4 + 4; return 1;        /* handled: the game's own commands on these keys don't run */
    }
    case 0x57470d: {                                  /* WorldTime::Update: once a frame on the game's thread */
        uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, top = c->top;   /* the caller's registers: `this` is in ecx */
        frame(c); c->eax = eax; c->ecx = ecx; c->edx = edx; c->top = top;
        return 0;
    }
    }
    return 0;
}
