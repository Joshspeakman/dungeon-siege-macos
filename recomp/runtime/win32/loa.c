/* Legends of Aranna on the base engine: the engine functions the expansion's scripts call, implemented for the GOG
 * 1.11.1 engine (whose executable the build verifies by checksum, so its internal addresses below are fixed).
 * Registered by loa_register() when the expansion's data is active (DS_EXPANSION); see ext.c for how they reach FuBi.
 * Names are the C++ decorated names the scripts' compiler binds to; the signatures follow the calls in the
 * expansion's scripts. */
#include "ext.h"
#include "tank.h"
#include <dirent.h>
#include <math.h>

/* ---- base engine internals (GOG 1.11.1) ---- */
#define GO_SET_MODIFIERS_DIRTY  0x5af628u    /* void Go::SetModifiersDirty(bool) */
#define COMP_GO(comp)           rt_r32(G_MEM, (comp) + 4)      /* GoComponent::m_Go */
#define GO_GOID(go)             rt_r32(G_MEM, (go) + 0x4c)

static void modifiers_dirty(Ctx *c, uint32_t go) { uint32_t a = 1; if (go) ext_thiscall(c, GO_SET_MODIFIERS_DIRTY, go, 1, &a); }

/* ---- helpers ---- */
/* base-engine exports by decorated name, resolved once per call site */
#define FX(name) ({ static uint32_t _a; if (!_a) _a = ext_export(name); _a; })
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
#define GPSTR_CTOR 0x42aea8u                             /* gpstring::gpstring() */
static uint32_t gstr(const char *s);
static uint32_t gpstr(Ctx *c, const char *text);
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
/* scratch memory on the guest stack for out-parameters (released by scratch_end) */
static uint32_t scratch(Ctx *c, uint32_t n) { c->esp -= (n + 15) & ~15u; return c->esp; }
static void scratch_end(Ctx *c, uint32_t esp) { c->esp = esp; }
static uint32_t goid_go(Ctx *c, uint32_t goid) { return goid ? ext_thiscall(c, FX("?GetGo@Goid_@@ABEPAVGo@@XZ"), goid, 0, 0) : 0; }
/* the nodes of an MSVC std::map in order (map: allocator, then the head node pointer; nodes {left, parent, right, key,
 * value}; leaves point at a shared nil node, the first node's left) */
static int map_nodes(uint32_t map, uint32_t *node, int max)
{
    uint32_t head = rt_r32(G_MEM, map + 4), n = head ? rt_r32(G_MEM, head) : 0, m = 0;   /* head->left: the first node */
    if (!head || n == head) return 0;
    uint32_t nil = rt_r32(G_MEM, n);                                               /* the first node's left is nil */
    while (n != head && m < (uint32_t)max) {
        node[m++] = n;
        uint32_t r = rt_r32(G_MEM, n + 8);
        if (r != nil) { n = r; for (int g = 0; g < 64 && rt_r32(G_MEM, n) != nil; g++) n = rt_r32(G_MEM, n); }
        else {
            uint32_t p = rt_r32(G_MEM, n + 4); int g = 0;
            while (n == rt_r32(G_MEM, p + 8) && g++ < 64) { n = p; p = rt_r32(G_MEM, p + 4); }
            if (rt_r32(G_MEM, n + 8) != p) n = p;
        }
    }
    return (int)m;
}
#define go_comp(c, go, getter) ({ uint32_t _go = (go); _go ? ext_thiscall(c, FX(getter), _go, 0, 0) : 0; })   /* a component (getter: literal) */
#define GO_ASPECT(go)    go_comp(c, go, "?GetAspect@Go@@QAEPAVGoAspect@@XZ")
#define GO_MIND(go)      go_comp(c, go, "?GetMind@Go@@QAEPAVGoMind@@XZ")
#define GO_BODY(go)      go_comp(c, go, "?GetBody@Go@@QAEPAVGoBody@@XZ")
#define GO_ACTOR(go)     go_comp(c, go, "?GetActor@Go@@QAEPAVGoActor@@XZ")
#define GO_MAGIC(go)     go_comp(c, go, "?GetMagic@Go@@QAEPAVGoMagic@@XZ")
#define GO_COMMON(go)    go_comp(c, go, "?GetCommon@Go@@QAEPAVGoCommon@@XZ")
#define GO_PLACEMENT(go) go_comp(c, go, "?GetPlacement@Go@@QAEPAVGoPlacement@@XZ")

/* per-object state the base engine has no field for, keyed by object address or Goid */
typedef struct { uint32_t key, kind; uint32_t v; } Side;
static Side side[16384]; static pthread_mutex_t side_lock = PTHREAD_MUTEX_INITIALIZER;
enum { S_ALLOW_MOVE = 1, S_LODFI, S_PCONTENT_INV, S_DAMAGE_TAKER, S_SPELLBOOK, S_IS_SET_ITEM, S_SET_COUNT, S_REAL_MINUTES, S_COPY_INV,
       S_TRANSFORM, S_OWN_ASPECT, S_OWN_SCALE, S_CREATURE_ASPECT };
static uint32_t side_get(uint32_t key, uint32_t kind, uint32_t dflt)
{
    pthread_mutex_lock(&side_lock); uint32_t h = (key * 2654435761u ^ kind * 40503u) & 16383, r = dflt;
    for (int i = 0; i < 16384; i++) { Side *e = &side[(h + (uint32_t)i) & 16383]; if (!e->kind) break; if (e->key == key && e->kind == kind) { r = e->v; break; } }
    pthread_mutex_unlock(&side_lock); return r;
}
static void side_set(uint32_t key, uint32_t kind, uint32_t v)
{
    pthread_mutex_lock(&side_lock); uint32_t h = (key * 2654435761u ^ kind * 40503u) & 16383;
    for (int i = 0; i < 16384; i++) { Side *e = &side[(h + (uint32_t)i) & 16383]; if (!e->kind || (e->key == key && e->kind == kind)) { e->key = key; e->kind = kind; e->v = v; break; } }
    pthread_mutex_unlock(&side_lock);
}

/* ---- Math ---- */
static void Math_LShift(Ctx *c) { RETC((uint32_t)((int32_t)ARG(0) << (ARG(1) & 31))); }
static void Math_RShift(Ctx *c) { RETC((uint32_t)((int32_t)ARG(0) >> (ARG(1) & 31))); }

/* ---- GoAspect: life recovery (the base engine keeps natural/current pairs: unit +0x74/+0x78, period +0x7c/+0x80) ---- */
static void GoAspect_SetNaturalLifeRecoveryUnit(Ctx *c)
{
    rt_wf32(G_MEM, THIS + 0x74, ARGF(0)); modifiers_dirty(c, COMP_GO(THIS)); RET(0, 1);
}
static void GoAspect_SetLifeRecoveryUnit(Ctx *c) { rt_wf32(G_MEM, THIS + 0x78, ARGF(0)); RET(0, 1); }

/* ---- Victory: the expansion's quest calls name the player whose quest log is meant (multiplayer); the base engine keeps
 * one log, which is the player's in single player ---- */
#define GPS "ABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@"
static uint32_t fn_is_completed, fn_is_active, fn_order, fn_activate, fn_deactivate, fn_post_data, fn_post_scid_data, fn_data1;
static void Victory_IsQuestCompleted(Ctx *c) { uint32_t a = ARG(0); RET(ext_thiscall(c, fn_is_completed, THIS, 1, &a) & 0xff, 2); }
static void Victory_IsQuestActive(Ctx *c) { uint32_t a = ARG(0); RET(ext_thiscall(c, fn_is_active, THIS, 1, &a) & 0xff, 2); }
static void Victory_GetQuestOrder(Ctx *c) { uint32_t a = ARG(0); RET(ext_thiscall(c, fn_order, THIS, 1, &a), 2); }
static void Victory_RSActivateQuest(Ctx *c) { uint32_t a[2] = {ARG(0), ARG(1)}; ext_thiscall(c, fn_activate, THIS, 2, a); RET(0, 3); }
static void Victory_RSDeactivateQuest(Ctx *c) { uint32_t a = ARG(0); ext_thiscall(c, fn_deactivate, THIS, 1, &a); RET(0, 2); }

/* ---- world messages: the "activator" travels in the message's data word (WorldMessage::GetData1) ---- */
static void PostWorldMessage_activator(Ctx *c)        /* (event, Goid from, Scid to, Goid activator, float delay) */
{
    uint32_t a[5] = {ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)}; w32_callback(c, fn_post_scid_data, 5, a); RETC(0);
}
static void SPostWorldMessage_goid_data(Ctx *c)       /* (event, Goid from, Goid to, DWORD data, float delay) */
{
    uint32_t a[5] = {ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)}; w32_callback(c, fn_post_data, 5, a); RETC(0);
}
static void SPostWorldMessage_goid(Ctx *c)            /* (event, Goid from, Goid to, float delay) */
{
    uint32_t a[5] = {ARG(0), ARG(1), ARG(2), 0, ARG(3)}; w32_callback(c, fn_post_data, 5, a); RETC(0);
}
static void WorldMessage_GetActivator(Ctx *c) { RET(ext_thiscall(c, fn_data1, THIS, 0, 0), 0); }

/* ---- GoMind ---- */
static void GoMind_SetAllowNewMovementJobs(Ctx *c) { side_set(THIS, S_ALLOW_MOVE, ARG(0) & 0xff); RET(0, 1); }
static void GoMind_GetAllowNewMovementJobs(Ctx *c) { RET(side_get(THIS, S_ALLOW_MOVE, 1), 0); }
static void GoMind_GetBestSupportEnemy(Ctx *c)       /* the first enemy one of the given party members is fighting */
{
    uint32_t coll = ARG(0), best = FX("?GetBestFocusEnemy@GoMind@@QBEPAVGo@@XZ"), r = 0;
    int n = coll ? (int)ext_thiscall(c, FX("?Size@GopColl@@ABEHXZ"), coll, 0, 0) : 0;
    for (int i = 0; i < n && !r; i++) {
        uint32_t k = (uint32_t)i, m = ext_thiscall(c, FX("?Get@GopColl@@ABEPAVGo@@H@Z"), coll, 1, &k), mind = GO_MIND(m);
        if (mind) r = ext_thiscall(c, best, mind, 0, 0);
    }
    if (!r) r = ext_thiscall(c, best, THIS, 0, 0);
    RET(r, 1);
}
static void GoMind_RSApproach(Ctx *c)                /* (Go target, float distance, eQPlace, eActionOrigin, bool) */
{
    uint32_t target = ARG(0), pl = GO_PLACEMENT(target);
    if (pl) {
        uint32_t pos = ext_thiscall(c, FX("?GetPosition@GoPlacement@@QBEABUSiegePos@@XZ"), pl, 0, 0);
        uint32_t a[3] = {pos, ARG(2), ARG(3)}; ext_thiscall(c, FX("?RSMove@GoMind@@QAEXABUSiegePos@@W4eQPlace@@W4eActionOrigin@@@Z"), THIS, 3, a);
    }
    RET(0, 5);
}
static void GoMind_RSCopyMind(Ctx *c) { RET(0, 1); }        /* minds keep their own template settings */

/* ---- AIQuery ---- */
static void AIQuery_GetEnemiesOfGoInSphere(Ctx *c)   /* (SiegePos const& centre, float radius, Go const* of, GopColl& out) */
{
    uint32_t out = ARG(3), mind = GO_MIND(ARG(2));
    uint32_t a[3] = {ARG(0), ARG(1), out}; ext_thiscall(c, FX("?GetOccupantsInSphere@AIQuery@@QAE_NABUSiegePos@@MAAUGopColl@@@Z"), THIS, 3, a);
    int n = (int)ext_thiscall(c, FX("?Size@GopColl@@ABEHXZ"), out, 0, 0); uint32_t found[512]; int m = 0;
    for (int i = 0; i < n && m < 512; i++) {
        uint32_t k = (uint32_t)i, o = ext_thiscall(c, FX("?Get@GopColl@@ABEPAVGo@@H@Z"), out, 1, &k);
        if (mind && (ext_thiscall(c, FX("?IsEnemy@GoMind@@QBE_NPBVGo@@@Z"), mind, 1, &o) & 0xff)) found[m++] = o;
    }
    ext_thiscall(c, FX("?Clear@GopColl@@AAEXXZ"), out, 0, 0);
    for (int i = 0; i < m; i++) ext_thiscall(c, FX("?Add@GopColl@@AAEXPAVGo@@@Z"), out, 1, &found[i]);
    RET(m > 0, 4);
}

/* ---- GoActor ---- */
static void GoActor_SAddGenericState_int(Ctx *c)     /* (name, description, int duration, Goid caster) */
{
    uint32_t a[6] = {ARG(0), ARG(1), fbits((float)(int32_t)ARG(2)), ARG(3), ARG(3), fbits(1.0f)};
    ext_thiscall(c, FX("?SAddGenericState@GoActor@@QAEXPBD0MPBUGoid_@@1M@Z"), THIS, 6, a); RET(0, 4);
}
/* generic states: a map at +0x48 (end node at +0x4c) of {description, caster, spell, level, duration} at node+0x14 */
static void GoActor_GetGenericStateCasterGoid(Ctx *c)
{
    uint32_t esp = c->esp, t = scratch(c, 16); rt_w32(G_MEM, t, ARG(0));
    uint32_t a[2] = {t + 4, t}; uint32_t it = ext_thiscall(c, 0x5b90d5u, THIS + 0x48, 2, a);
    uint32_t node = rt_r32(G_MEM, it), r = node == rt_r32(G_MEM, THIS + 0x4c) ? rt_r32(G_MEM, 0x7a1324u) : rt_r32(G_MEM, node + 0x1c);
    scratch_end(c, esp); RET(r, 1);
}
/* skills: a vector of 0x48-byte entries at GoActor +0x18/+0x1c: name (string, its text pointer first), natural level
 * +0x34, bonus from modifiers +0x38 */
static uint32_t skill_entry(uint32_t actor, const char *name)
{
    for (uint32_t e = rt_r32(G_MEM, actor + 0x18), end = rt_r32(G_MEM, actor + 0x1c); e && e < end; e += 0x48) {
        uint32_t p = rt_r32(G_MEM, e); if (p && !strcasecmp(GS(p), name)) return e;
    }
    return 0;
}
static void GoActor_RSCopySkills(Ctx *c)             /* (GoActor const& from, float multiplier): every skill's natural level */
{
    uint32_t from = ARG(0); float k = ARGF(1);
    for (uint32_t e = rt_r32(G_MEM, from + 0x18), end = rt_r32(G_MEM, from + 0x1c); e && e < end; e += 0x48) {
        uint32_t p = rt_r32(G_MEM, e); if (!p) continue;
        uint32_t d = skill_entry(THIS, GS(p)); if (d) rt_wf32(G_MEM, d + 0x34, rt_rf32(G_MEM, e + 0x34) * k);
    }
    modifiers_dirty(c, COMP_GO(THIS));
    RET(0, 2);
}

/* ---- Go ---- */
static void Go_SetLodfi(Ctx *c) { side_set(THIS, S_LODFI, ARG(0) & 0xff); RET(0, 1); }
static void Go_CheckModifierRecalc(Ctx *c) { modifiers_dirty(c, THIS); RET(1, 0); }
static void Go_IsImbuedItem(Ctx *c) { RET(0, 0); }
static void Go_GetIsPContentInventory(Ctx *c) { RET(side_get(THIS, S_PCONTENT_INV, 0), 0); }
static void Go_SetIsPContentInventory(Ctx *c) { side_set(THIS, S_PCONTENT_INV, ARG(0) & 0xff); RET(0, 1); }
static void Go_RSSetDoppelgangerStats(Ctx *c)        /* (Go from, float multiplier): life, mana and damage scaled */
{
    uint32_t src = GO_ASPECT(ARG(0)), dst = GO_ASPECT(THIS); float k = ARGF(1);
    if (src && dst) {
        uint32_t life = fbits((float)ext_thiscall_f(c, FX("?GetMaxLife@GoAspect@@QBEMXZ"), src, 0, 0) * k);
        uint32_t mana = fbits((float)ext_thiscall_f(c, FX("?GetMaxMana@GoAspect@@QBEMXZ"), src, 0, 0) * k);
        ext_thiscall(c, FX("?SSetNaturalMaxLife@GoAspect@@QAEXM@Z"), dst, 1, &life); ext_thiscall(c, FX("?SSetMaxLife@GoAspect@@QAEXM@Z"), dst, 1, &life);
        ext_thiscall(c, FX("?SSetCurrentLife@GoAspect@@QAEXM@Z"), dst, 1, &life);
        ext_thiscall(c, FX("?SSetNaturalMaxMana@GoAspect@@QAEXM@Z"), dst, 1, &mana); ext_thiscall(c, FX("?SSetMaxMana@GoAspect@@QAEXM@Z"), dst, 1, &mana);
        ext_thiscall(c, FX("?SSetCurrentMana@GoAspect@@QAEXM@Z"), dst, 1, &mana);
    }
    uint32_t sa = go_comp(c, ARG(0), "?GetAttack@Go@@QAEPAVGoAttack@@XZ"), da = go_comp(c, THIS, "?GetAttack@Go@@QAEPAVGoAttack@@XZ");
    if (sa && da) {
        uint32_t mn = fbits((float)ext_thiscall_f(c, FX("?GetDamageMin@GoAttack@@QBEMXZ"), sa, 0, 0) * k);
        uint32_t mx = fbits((float)ext_thiscall_f(c, FX("?GetDamageMax@GoAttack@@QBEMXZ"), sa, 0, 0) * k);
        ext_thiscall(c, FX("?SetDamageMinNatural@GoAttack@@QAEXM@Z"), da, 1, &mn); ext_thiscall(c, FX("?SetDamageMaxNatural@GoAttack@@QAEXM@Z"), da, 1, &mx);
    }
    modifiers_dirty(c, THIS);
    RET(0, 2);
}
static void GoCloneReq_SetCopyInventory(Ctx *c) { side_set(THIS, S_COPY_INV, ARG(0) & 0xff); RET(0, 1); }

/* ---- Rules ---- */
static void rules_damage(Ctx *c, int max)            /* (Goid attacker, Goid weapon or spell) with the attacker's bonuses */
{
    uint32_t esp = c->esp, t = scratch(c, 16); rt_wf32(G_MEM, t, 0); rt_wf32(G_MEM, t + 4, 0);
    uint32_t a[4] = {ARG(0), ARG(1), t, t + 4}; ext_thiscall(c, FX("?GetDamageRange@Rules@@QAE_NPBUGoid_@@0AAM1@Z"), THIS, 4, a);
    float v = rt_rf32(G_MEM, t + (max ? 4 : 0)); scratch_end(c, esp); RETF(v, 2);
}
static void Rules_GetDamageMin(Ctx *c) { rules_damage(c, 0); }
static void Rules_GetDamageMax(Ctx *c) { rules_damage(c, 1); }
static void Rules_IsMemberofGroup(Ctx *c)            /* (Goid, const gpbstring& group): the object's membership */
{
    uint32_t common = GO_COMMON(goid_go(c, ARG(0))), r = 0;
    if (common) {
        uint32_t m = ext_thiscall(c, FX("?GetMembership@GoCommon@@QBEABVMembership@@XZ"), common, 0, 0), s = ARG(1);
        r = ext_thiscall(c, FX("?Contains@Membership@@QBE_N" GPS "@Z"), m, 1, &s) & 0xff;
    }
    RET(r, 2);
}

/* ---- GoPlacement ---- */
static void GoPlacement_AdjustPosition(Ctx *c)       /* (dx, dy, dz, bool) within the current node */
{
    uint32_t esp = c->esp, t = scratch(c, 32), pos = ext_thiscall(c, FX("?GetPosition@GoPlacement@@QBEABUSiegePos@@XZ"), THIS, 0, 0);
    memcpy(GP(t), GP(pos), 16);
    for (int i = 0; i < 3; i++) rt_wf32(G_MEM, t + 4 * (uint32_t)i, rt_rf32(G_MEM, t + 4 * (uint32_t)i) + ARGF(i));
    uint32_t a[2] = {t, ARG(3) & 0xff}; ext_thiscall(c, FX("?SSetPosition@GoPlacement@@QAEXABUSiegePos@@_N@Z"), THIS, 2, a);
    scratch_end(c, esp); RET(0, 4);
}
static void GoPlacement_OrientToPosition(Ctx *c)     /* turn (about the vertical) to face a position in the same node */
{
    uint32_t pos = ext_thiscall(c, FX("?GetPosition@GoPlacement@@QBEABUSiegePos@@XZ"), THIS, 0, 0), to = ARG(0);
    if (rt_r32(G_MEM, pos + 12) == rt_r32(G_MEM, to + 12)) {
        float dx = rt_rf32(G_MEM, to) - rt_rf32(G_MEM, pos), dz = rt_rf32(G_MEM, to + 8) - rt_rf32(G_MEM, pos + 8);
        if (dx * dx + dz * dz > 1e-6f) {
            float yaw = atan2f(dx, dz) * 0.5f; uint32_t esp = c->esp, q = scratch(c, 16);
            rt_wf32(G_MEM, q, 0); rt_wf32(G_MEM, q + 4, sinf(yaw)); rt_wf32(G_MEM, q + 8, 0); rt_wf32(G_MEM, q + 12, cosf(yaw));
            ext_thiscall(c, FX("?SSetOrientation@GoPlacement@@QAEXABUQuat@@@Z"), THIS, 1, &q); scratch_end(c, esp);
        }
    }
    RET(0, 1);
}

/* ---- GoBody, GoPhysics ---- */
static void GoBody_SetMaxMoveVelocity(Ctx *c) { rt_wf32(G_MEM, THIS + 0x2c, ARGF(0)); RET(0, 1); }
static void GoBody_GetTerrainMovementPermissions(Ctx *c) { RET(rt_r32(G_MEM, THIS + 0x30), 0); }   /* terrain_movement_permissions */
static void GoPhysics_GetSimDuration(Ctx *c) { RETF(ext_thiscall_f(c, 0x5ffeccu, THIS, 0, 0), 0); }   /* the template's sim_duration */
static void WorldFx_GetPosition(Ctx *c) { RET(0, 2); }      /* (SFx script, SiegePos& out): not tracked, out unchanged */

/* ---- GoAspect ---- */
static void GoAspect_SetRenderScaleMultiplier(Ctx *c) { rt_wf32(G_MEM, THIS + 0x1c, ARGF(0)); RET(0, 1); }   /* scale_multiplier */
static void GoAspect_HasAspectHandle(Ctx *c) { RET(rt_r32(G_MEM, THIS + 0x34) != 0, 0); }
static void GoAspect_GetNumSubTextures(Ctx *c)        /* the model's texture count (aspect +0x38 -> model -> +0x3c) */
{
    uint32_t inst = rt_r32(G_MEM, THIS + 0x38), shared = inst ? rt_r32(G_MEM, inst) : 0;
    RET(shared ? rt_r32(G_MEM, shared + 0x3c) : 1, 0);
}
static void GoAspect_SetDamageTaker(Ctx *c) { side_set(THIS, S_DAMAGE_TAKER, ARG(0)); RET(0, 1); }
static void GoAspect_ClearDamageTaker(Ctx *c) { side_set(THIS, S_DAMAGE_TAKER, 0); RET(0, 0); }
static void GoAspect_SSetIsSetItem(Ctx *c) { side_set(THIS, S_IS_SET_ITEM, ARG(0) & 0xff); RET(0, 1); }
static void GoAspect_SSetItemNumbers(Ctx *c) { side_set(THIS, S_SET_COUNT, ARG(0)); RET(0, 1); }
static void GoAspect_SSetItemSetGroup(Ctx *c) { RET(0, 2); }
static void GoAspect_SSetItemSetModName(Ctx *c) { RET(0, 1); }
static void GoAspect_SInitISModifierData(Ctx *c) { RET(0, 0); }

/* ---- nema (the renderer's model instances): alpha and ambience live in the instance's diffuse colour ---- */
static void Aspect_InitializeLighting(Ctx *c) { rt_w32(G_MEM, THIS + 0xa8, (ARG(1) << 24) | (ARG(0) & 0xffffff)); RET(0, 2); }
static void Aspect_GetAmbience(Ctx *c) { RET(rt_r32(G_MEM, THIS + 0xa8) & 0xffffff, 0); }
static void Aspect_GetAlpha(Ctx *c) { RET(rt_r32(G_MEM, THIS + 0xa8) >> 24, 0); }
static void Blender_ChangeSpeedModifier(Ctx *c) { RET(0, 1); }

/* ---- GoMagic: potions keep their remaining (+0x68) and full (+0x6c) amounts in their potion enchantment ---- */
static uint32_t potion_enchantment(uint32_t magic)
{
    uint32_t st = rt_r32(G_MEM, magic + 0x2c); if (!st) return 0;
    for (uint32_t it = rt_r32(G_MEM, st + 4), end = rt_r32(G_MEM, st + 8); it && it != end; it += 4) {
        uint32_t e = rt_r32(G_MEM, it); if (e && G_MEM[e + 0x94]) return e;
    }
    return 0;
}
static void GoMagic_GetPotionAmount(Ctx *c) { uint32_t e = potion_enchantment(THIS); RETF(e ? rt_rf32(G_MEM, e + ((ARG(0) & 0xff) ? 0x6c : 0x68)) : 0.0f, 1); }
static void GoMagic_SSetPotionAmountReally(Ctx *c)
{
    uint32_t e = potion_enchantment(THIS); if (e) { rt_wf32(G_MEM, e + 0x68, ARGF(0)); rt_wf32(G_MEM, e + 0x6c, ARGF(0)); }
    RET(0, 1);
}
static int potion_kind(Ctx *c, const char *what)
{
    if (!(ext_thiscall(c, FX("?IsPotion@GoMagic@@QBE_NXZ"), THIS, 0, 0) & 0xff)) return 0;
    uint32_t name = ext_thiscall(c, FX("?GetTemplateName@Go@@QBEPBDXZ"), COMP_GO(THIS), 0, 0);
    return name && strcasestr((const char *)GP(name), what) != 0;
}
static void GoMagic_IsHealthPotion(Ctx *c) { RET(potion_kind(c, "health"), 0); }
static void GoMagic_IsManaPotion(Ctx *c) { RET(potion_kind(c, "mana"), 0); }

/* ---- GoInventory ---- */
static void GoInventory_GetActiveSpellBook(Ctx *c)    /* one set by SetActiveSpellBook, else the equipped spellbook */
{
    uint32_t r = side_get(THIS, S_SPELLBOOK, 0), slot = 7;   /* es_spellbook */
    if (!r) r = ext_thiscall(c, FX("?GetEquipped@GoInventory@@QBEPAVGo@@W4eEquipSlot@@@Z"), THIS, 1, &slot);
    RET(r, 0);
}
static void GoInventory_SetActiveSpellBook(Ctx *c) { side_set(THIS, S_SPELLBOOK, ARG(0)); RET(0, 1); }
/* template fields the base engine does not use itself, read by name from the object's template */
static int go_template_bool(Ctx *c, uint32_t go, const char *comp, const char *field, int dflt)
{
    if (!go) return dflt;
    uint32_t esp = c->esp, t = scratch(c, 96); snprintf((char *)GP(t), 32, "%s", comp); snprintf((char *)GP(t + 32), 64, "%s", field);
    uint32_t a[3] = {t, t + 32, (uint32_t)dflt}; int r = ext_thiscall(c, FX("?GetComponentBool@Go@@QAE_NPBD0_N@Z"), go, 3, a) & 0xff;
    scratch_end(c, esp); return r;
}
static void GoInventory_IsNonAggressivePack(Ctx *c)    /* pack mules don't fight; the expansion's pack animals do */
{
    int pack = ext_thiscall(c, FX("?IsPackOnly@GoInventory@@QBE_NXZ"), THIS, 0, 0) & 0xff;
    RET(pack && go_template_bool(c, COMP_GO(THIS), "inventory", "is_nonaggressive_pack", 1), 0);
}
static void GoInventory_SSetDirtySetItem(Ctx *c) { RET(0, 2); }
static void GoInventory_TestGet1(Ctx *c) { uint32_t a = ARG(0); RET(ext_thiscall(c, FX("?TestGet@GoInventory@@QBE_NPBUGoid_@@@Z"), THIS, 1, &a) & 0xff, 1); }
static void GoInventory_TestGet2(Ctx *c) { uint32_t a[2] = {ARG(0), ARG(1)}; RET(ext_thiscall(c, FX("?TestGet@GoInventory@@QBE_NPBUGoid_@@_N@Z"), THIS, 2, a) & 0xff, 2); }
static void GoInventory_GetGridbox(Ctx *c) { RET(ext_thiscall(c, FX("?GetGridbox@GoInventory@@QBEPAVUIGridbox@@XZ"), THIS, 0, 0), 0); }

/* ---- singletons and the rest ---- */
static uint32_t singleton(uint32_t *g) { if (!*g) *g = heap_alloc(w32_process_heap, 8, 64); return *g; }
static uint32_t g_transform, g_overhead, g_empty_string;
static void TransformationManager_Singleton(Ctx *c) { RETC(singleton(&g_transform)); }
static void OverheadMap_Singleton(Ctx *c) { RETC(singleton(&g_overhead)); }
/* ---- transformation: the expansion's Transform spells (and its doppelgangers) turn a character into a creature for a
 * while. The base engine can already show a Go with another model than its template's: GoAspect holds the shown
 * ("current") nema aspect at +0x34 (its pointer cached at +0x38) and keeps the template's own at +0x3c while another is
 * shown (body armour is worn this way). The creature's model becomes the current aspect and the body takes the
 * creature's animations (the chore dictionary and bone translator of the creature template's [body]); the character's
 * own aspect, animations and scale come back when it ends. The spell's enchantments give the creature's strength. */
#define CONTENTDB            rt_r32(G_MEM, 0x7a05c4u)
#define ASPECT_STORAGE       rt_r32(G_MEM, 0x7a0664u)   /* nema aspect handles: load, release */
#define ASPECT_OBJECTS       rt_r32(G_MEM, 0x7a05dcu)   /* handle -> nema::Aspect* */
#define NAMING_KEY           rt_r32(G_MEM, 0x7a0464u)   /* model name -> file */
#define TEMPLATE_FIND        0x5252edu   /* ContentDb: GoDataTemplate* by name, loaded (templates load on first use; kept) */
#define TEMPLATE_COMPONENT   0x5f8ea9u   /* GoDataTemplate::FindComponentByName(const char*) -> GoDataComponent* */
#define NAMING_RESOLVE       0x6388aeu   /* bool (const char* name, gpstring& file) */
#define ASPECT_LOAD          0x68d0e0u   /* handle& (handle& out, const char* file, const char* name) */
#define ASPECT_RELEASE       0x68cff5u   /* (handle, bool) */
#define ASPECT_PTR           0x47abebu   /* nema::Aspect* (handle) */
#define ASPECT_TAKE_OVER     0x68ff99u   /* new->(old aspect, visible): placement and state of the one it replaces */
#define ASPECT_TEXTURE       0x532008u   /* GoAspect: (slot, const char* texture or 0 for the model's own) */
#define BODY_LOAD_CHORES     0x5ba227u   /* GoBody: (re)load its animations from its [body] data onto the current aspect */
#define UISHELL (rt_r32(G_MEM, 0x7a065cu))
static void ui_expansion_fixups(Ctx *c);
static uint32_t sstr(Ctx *c, const char *s) { uint32_t n = (uint32_t)strlen(s) + 1, g = scratch(c, n); memcpy(GP(g), s, n); return g; }
static uint32_t template_data(Ctx *c, const char *tmpl, const char *component)
{
    uint32_t esp = c->esp, t = sstr(c, tmpl), r = CONTENTDB ? ext_thiscall(c, TEMPLATE_FIND, CONTENTDB, 1, &t) : 0;
    if (r) { uint32_t n = sstr(c, component); r = ext_thiscall(c, TEMPLATE_COMPONENT, r, 1, &n); }
    scratch_end(c, esp); return r;
}
static int template_string(Ctx *c, const char *path, char *out, size_t cap)   /* "template:component:field" */
{
    uint32_t esp = c->esp, p = sstr(c, path);
    uint32_t g = ext_thiscall(c, FX("?GetTemplateString@ContentDb@@QAEABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@PBD@Z"), CONTENTDB, 1, &p);
    uint32_t t = g ? rt_r32(G_MEM, g) : 0; snprintf(out, cap, "%s", t ? GS(t) : ""); scratch_end(c, esp); return *out != 0;
}
static float template_float(Ctx *c, const char *path, float dflt)
{
    uint32_t esp = c->esp, a[2] = {sstr(c, path), fbits(dflt)};
    float v = (float)ext_thiscall_f(c, FX("?GetTemplateFloat@ContentDb@@QAEMPBDM@Z"), CONTENTDB, 2, a); scratch_end(c, esp); return v;
}
static void aspect_show(Ctx *c, uint32_t go, uint32_t aspect, uint32_t h)     /* make h the shown aspect */
{
    uint32_t np = ext_thiscall(c, ASPECT_PTR, ASPECT_OBJECTS, 1, &h), old = rt_r32(G_MEM, aspect + 0x38);
    uint32_t vis = ext_thiscall(c, FX("?IsInAnyScreenWorldFrustum@Go@@QBE_NXZ"), go, 0, 0) & 0xff;
    if (np && old) { uint32_t a[2] = {old, vis}; ext_thiscall(c, ASPECT_TAKE_OVER, np, 2, a); }
    rt_w32(G_MEM, aspect + 0x34, h); rt_w32(G_MEM, aspect + 0x38, np);
    if (np) rt_w32(G_MEM, np + 0x10, GO_GOID(go));
}
static void body_chores(Ctx *c, uint32_t body, uint32_t data)               /* animations from a [body] (0: its own) */
{
    uint32_t own = rt_r32(G_MEM, body + 8), no = 0;
    if (data) rt_w32(G_MEM, body + 8, data);
    ext_thiscall(c, BODY_LOAD_CHORES, body, 1, &no);
    rt_w32(G_MEM, body + 8, own);
}
static int transform_go(Ctx *c, uint32_t go, const char *tmpl)
{
    uint32_t aspect = GO_ASPECT(go), body = GO_BODY(go);
    if (!tmpl || !*tmpl || !aspect || !body || side_get(go, S_TRANSFORM, 0)) return 0;
    char path[256], model[128], file[512] = ""; snprintf(path, sizeof path, "%s:aspect:model", tmpl);
    uint32_t data = template_data(c, tmpl, "body");
    int got = template_string(c, path, model, sizeof model);
    if (!data || !got) { fprintf(stderr, "loa: cannot transform into '%s' (body %08x, model '%s')\n", tmpl, data, model); return 0; }
    uint32_t esp = c->esp, g = scratch(c, 16), out = g + 8;
    ext_thiscall(c, GPSTR_CTOR, g, 0, 0);
    uint32_t a[3] = {sstr(c, model), g};
    if (ext_thiscall(c, NAMING_RESOLVE, NAMING_KEY, 2, a) & 0xff) { uint32_t t = rt_r32(G_MEM, g); snprintf(file, sizeof file, "%s", t ? GS(t) : ""); }
    uint32_t h = 0;
    if (*file) { uint32_t b[3] = {out, sstr(c, file), sstr(c, model)}; h = rt_r32(G_MEM, ext_thiscall(c, ASPECT_LOAD, ASPECT_STORAGE, 3, b)); }
    ext_thiscall(c, 0x48899au, g, 0, 0);                                     /* ~gpstring */
    scratch_end(c, esp);
    if (!h) { fprintf(stderr, "loa: no model %s for '%s'\n", model, tmpl); return 0; }
    side_set(go, S_OWN_ASPECT, rt_r32(G_MEM, aspect + 0x34));                 /* kept (with its reference) until the end */
    side_set(go, S_OWN_SCALE, rt_r32(G_MEM, aspect + 0x18));
    aspect_show(c, go, aspect, h);
    uint32_t np = rt_r32(G_MEM, aspect + 0x38), shared = np ? rt_r32(G_MEM, np) : 0, nt = shared ? rt_r32(G_MEM, shared + 0x3c) : 1;
    for (uint32_t i = 0; i < nt && i < 16; i++) { uint32_t a2[2] = {i, 0}; ext_thiscall(c, ASPECT_TEXTURE, aspect, 2, a2); }   /* its own skins */
    body_chores(c, body, data);
    snprintf(path, sizeof path, "%s:aspect:scale_base", tmpl);
    rt_wf32(G_MEM, aspect + 0x18, template_float(c, path, 1.0f));
    side_set(go, S_TRANSFORM, gstr(tmpl)); side_set(go, S_CREATURE_ASPECT, h);
    if (UISHELL) ui_expansion_fixups(c);
    if (getenv("DS_EXTLOG")) fprintf(stderr, "loa: %08x transformed into %s (model %s)\n", go, tmpl, model);
    return 1;
}
static int untransform_go(Ctx *c, uint32_t go)
{
    uint32_t aspect = GO_ASPECT(go), body = GO_BODY(go), t = side_get(go, S_TRANSFORM, 0), own = side_get(go, S_OWN_ASPECT, 0);
    if (!t || !aspect || !body) return 0;
    uint32_t h = rt_r32(G_MEM, aspect + 0x34);
    if (h == side_get(go, S_CREATURE_ASPECT, 0)) {
        aspect_show(c, go, aspect, own);
        uint32_t r[2] = {h, 1}; ext_thiscall(c, ASPECT_RELEASE, ASPECT_STORAGE, 2, r);
    } else {                     /* the engine changed the model meanwhile (armour put on): it released the creature's */
        uint32_t r[2] = {own, 1}; ext_thiscall(c, ASPECT_RELEASE, ASPECT_STORAGE, 2, r);
        fprintf(stderr, "loa: %08x changed its model while transformed\n", go);
    }
    body_chores(c, body, 0);
    rt_w32(G_MEM, aspect + 0x18, side_get(go, S_OWN_SCALE, rt_r32(G_MEM, aspect + 0x18)));
    side_set(go, S_TRANSFORM, 0); side_set(go, S_OWN_ASPECT, 0); side_set(go, S_CREATURE_ASPECT, 0);
    if (UISHELL) ui_expansion_fixups(c);
    if (getenv("DS_EXTLOG")) fprintf(stderr, "loa: %08x back from %s\n", go, GS(t));
    return 1;
}
/* transformed: by a spell this session (the side table is per object, so it is checked against the actor's state) */
static int is_transformed(Ctx *c, uint32_t go)
{
    if (!go || !side_get(go, S_TRANSFORM, 0)) return 0;
    if (getenv("DS_LOA_TRANSFORMTEST")) return 1;
    uint32_t actor = GO_ACTOR(go); static uint32_t name; if (!name) name = gstr("transformed");
    return actor && (ext_thiscall(c, FX("?HasGenericState@GoActor@@QBE_NPBD@Z"), actor, 1, &name) & 0xff);
}
static void TransformationManager_STransformMe(Ctx *c)          /* (Go*, gpstring& creature template, gpstring& spell) */
{
    uint32_t t = rt_r32(G_MEM, ARG(1)); transform_go(c, ARG(0), t ? GS(t) : ""); RET(0, 3);
}
static void TransformationManager_SUnTransformMe(Ctx *c) { untransform_go(c, ARG(0)); RET(0, 1); }
static void TransformationManager_GetNewTemplateName(Ctx *c)   /* the creature a Go is transformed into, or "" */
{
    uint32_t t = side_get(ARG(0), S_TRANSFORM, 0);
    RET(t ? gpstr(c, GS(t)) : singleton(&g_empty_string), 1);
}
static void Player_GetParty(Ctx *c)
{
    uint32_t server = w32_callback(c, FX("?FUBI_GetClassSingleton@Server@@CAPAV1@XZ"), 0, 0);
    RET(server ? ext_thiscall(c, FX("?GetScreenParty@Server@@QAEPAVGo@@XZ"), server, 0, 0) : 0, 0);
}
/* TimeOfDay +0: real seconds per game minute (Update adds real time at +4); the modifier scales the template's rate */
static void TimeOfDay_SetRealMinutesModifier(Ctx *c)
{
    uint32_t base = side_get(THIS, S_REAL_MINUTES, 0);
    if (!base) { base = rt_r32(G_MEM, THIS); side_set(THIS, S_REAL_MINUTES, base); }
    float m = ARGF(0); if (m > 0.01f) rt_wf32(G_MEM, THIS, bitsf(base) * m);
    RET(0, 1);
}
static void UIGame_SetGameInputBinderActive(Ctx *c) { RET(0, 1); }
static void WorldMap_GetUsingPlayerJournal(Ctx *c) { RET(0, 0); }
/* a FuBi enum constant by name: the specs FuBi registers form a list (head 0x79cf38, next +0x28) of
 * {name, ..., ToString +0xc, ..., begin +0x1c, end +0x20} */
static int fubi_enum(Ctx *c, const char *type, const char *name, uint32_t *out)
{
    for (uint32_t sp = rt_r32(G_MEM, 0x79cf38u); sp; sp = rt_r32(G_MEM, sp + 0x28)) {
        uint32_t n = rt_r32(G_MEM, sp); if (!n || strcmp(GS(n), type)) continue;
        uint32_t to_string = rt_r32(G_MEM, sp + 0xc), b = rt_r32(G_MEM, sp + 0x1c), e = rt_r32(G_MEM, sp + 0x20);
        for (uint32_t v = b; v < e; v++) {
            uint32_t r = w32_callback(c, to_string, 1, &v);
            if (r && !strcasecmp(GS(r), name)) { *out = v; return 1; }
        }
    }
    return 0;
}

/* UIPartyManager::RedistributePotions(bool, Goid except): health and mana potions shared evenly among the conscious
 * party members (pack mules hand theirs out; the given member is left out) */
static int redistribute_potions(Ctx *c, uint32_t except);
static void UIPartyManager_RedistributePotions(Ctx *c) { RET(redistribute_potions(c, ARG(1)), 2); }
static int redistribute_potions(Ctx *c, uint32_t except)
{
    static uint32_t il_main, ao_reflex, ls_ok; static int init;
    if (!init) { init = fubi_enum(c, "eInventoryLocation", "il_main", &il_main) && fubi_enum(c, "eActionOrigin", "ao_reflex", &ao_reflex) &&
                        fubi_enum(c, "eLifeState", "ls_alive_conscious", &ls_ok) ? 1 : -1; }
    uint32_t server = w32_callback(c, FX("?FUBI_GetClassSingleton@Server@@CAPAV1@XZ"), 0, 0);
    uint32_t party = server ? ext_thiscall(c, FX("?GetScreenParty@Server@@QAEPAVGo@@XZ"), server, 0, 0) : 0;
    if (init < 0 || !party) return 0;
    uint32_t kids = ext_thiscall(c, FX("?GetChildren@Go@@QBEABUGopColl@@XZ"), party, 0, 0), size = FX("?Size@GopColl@@ABEHXZ"), get = FX("?Get@GopColl@@ABEPAVGo@@H@Z");
    int n = (int)ext_thiscall(c, size, kids, 0, 0); if (n > 16) n = 16;
    uint32_t member[16], inv[16]; int receiver[16], m = 0;
    for (int i = 0; i < n; i++) {
        uint32_t k = (uint32_t)i, g = ext_thiscall(c, get, kids, 1, &k), iv = go_comp(c, g, "?GetInventory@Go@@QAEPAVGoInventory@@XZ"), as = GO_ASPECT(g);
        if (!g || !iv) continue;
        int ok = as && ext_thiscall(c, FX("?GetLifeState@GoAspect@@QBE?AW4eLifeState@@XZ"), as, 0, 0) == ls_ok;
        ok = ok && !(ext_thiscall(c, FX("?IsPackOnly@GoInventory@@QBE_NXZ"), iv, 0, 0) & 0xff) && GO_GOID(g) != except;
        member[m] = g; inv[m] = iv; receiver[m] = ok; m++;
    }
    int nrecv = 0; for (int i = 0; i < m; i++) nrecv += receiver[i];
    if (!nrecv) return 0;
    uint32_t coll = ext_thiscall(c, FX("?GetTempGopColl2@AIQuery@@QAEAAUGopColl@@XZ"), w32_callback(c, FX("?FUBI_GetClassSingleton@AIQuery@@CAPAV1@XZ"), 0, 0), 0, 0);
    for (int kind = 0; kind < 2; kind++) {                 /* health, then mana */
        uint32_t pot[256], own[256]; int np = 0, have[16] = {0};
        for (int i = 0; i < m; i++) {
            ext_thiscall(c, FX("?Clear@GopColl@@AAEXXZ"), coll, 0, 0);
            uint32_t a[2] = {il_main, coll}; ext_thiscall(c, FX("?ListItems@GoInventory@@QBE_NW4eInventoryLocation@@AAUGopColl@@@Z"), inv[i], 2, a);
            int ni = (int)ext_thiscall(c, size, coll, 0, 0);
            for (int j = 0; j < ni && np < 256; j++) {
                uint32_t k = (uint32_t)j, it = ext_thiscall(c, get, coll, 1, &k);
                if (!it || !(ext_thiscall(c, FX("?IsPotion@Go@@QBE_NXZ"), it, 0, 0) & 0xff)) continue;
                uint32_t nm = ext_thiscall(c, FX("?GetTemplateName@Go@@QBEPBDXZ"), it, 0, 0);
                if (!nm || !strcasestr(GS(nm), kind ? "mana" : "health")) continue;
                pot[np] = it; own[np] = (uint32_t)i; np++; have[i]++;
            }
        }
        int share = np / nrecv, extra = np % nrecv, want[16];
        for (int i = 0; i < m; i++) { want[i] = receiver[i] ? share + (extra > 0) : 0; if (receiver[i] && extra > 0) extra--; }
        for (int p = 0; p < np; p++) {                    /* move surplus potions to whoever is short */
            int from = (int)own[p]; if (have[from] <= want[from]) continue;
            for (int to = 0; to < m; to++) {
                if (have[to] >= want[to]) continue;
                uint32_t ca[3] = {pot[p], il_main, 0};
                if (!(ext_thiscall(c, FX("?CanAdd@GoInventory@@QBE_NPBVGo@@W4eInventoryLocation@@_N@Z"), inv[to], 3, ca) & 0xff)) continue;
                uint32_t ta[5] = {pot[p], member[to], il_main, ao_reflex, 0};
                ext_thiscall(c, FX("?RSTransfer@GoInventory@@QAEPAUCookie__@FuBi@@PAVGo@@0W4eInventoryLocation@@W4eActionOrigin@@_N@Z"), inv[from], 5, ta);
                have[from]--; have[to]++; break;
            }
        }
    }
    return 1;
}
static void GoCommon_SToggleTriggeredEffects(Ctx *c) { RET(0, 2); }
static void GoDb_SRemoveEnchantments(Ctx *c) { RET(0, 3); }
static void Rules_RSSetNaturalSkillLevel(Ctx *c)    /* (Goid, const char* skill, float level) */
{
    uint32_t go = goid_go(c, ARG(0)), actor = GO_ACTOR(go), e = actor ? skill_entry(actor, GS(ARG(1))) : 0;
    if (e) { rt_wf32(G_MEM, e + 0x34, ARGF(2)); modifiers_dirty(c, go); }
    RET(0, 3);
}

/* ---- jat_approach: the expansion's new AI job ("move near an object"), job type 33 after the base engine's 33 ----
 * The job types' names come from ToString/FromString (overridden for 33), FuBi learns the constants from the enum's
 * registration (count raised to 34 when it is constructed), and every mind loads its job scripts from its template in
 * a loop to 33 (a hook at the loop's end adds jat_approach). Job flags: those of jat_follow. */
#define JAT_APPROACH 33u
#define JAT_FOLLOW   19u
int loa_active;
static uint32_t gstr(const char *s) { uint32_t g = heap_alloc(w32_process_heap, 8, (uint32_t)strlen(s) + 1); strcpy((char *)GP(g), s); return g; }
static uint32_t s_jat_approach, s_jat_none, s_qt_underattack;
#define QT_UNDERATTACK 49u            /* the expansion's query trait: someone is fighting the object */

/* ---- interface: the expansion's screens have parts the base engine does not manage (pack-animal inventories, the
 * transformed-hero portrait overlay); after the engine shows an interface or a group they are put back as the
 * expansion's engine would have them: hidden unless their feature is in use ---- */
#define UI_FIND_WINDOW 0x6e06d3u      /* UIWindow* UIShell::FindUIWindow(const char* name, const char* interface) */
static int ui_wrapping;
static void ui_show_window(Ctx *c, const char *name, int show)
{
    uint32_t esp = c->esp, n = scratch(c, 64); snprintf((char *)GP(n), 64, "%s", name);
    uint32_t a[2] = {n, 0}, w = ext_thiscall(c, UI_FIND_WINDOW, UISHELL, 2, a);
    if (w && !G_MEM[w + 0x108] != !show) { uint32_t v = !!show; ext_thiscall(c, rt_r32(G_MEM, rt_r32(G_MEM, w) + 0x48), w, 1, &v); }   /* SetVisible */
    scratch_end(c, esp);
}
#define ui_hide_window(c, name) ui_show_window(c, name, 0)
static void ui_hide_group(Ctx *c, const char *group)
{
    uint32_t esp = c->esp, n = scratch(c, 64); snprintf((char *)GP(n), 64, "%s", group);
    uint32_t a[4] = {n, 0, 0, 0}; ext_thiscall(c, 0x6dee75u, UISHELL, 4, a);
    scratch_end(c, esp);
}
static int is_transformed(Ctx *c, uint32_t go);
static int party_members(Ctx *c, uint32_t *out, int max);
static void ui_expansion_fixups(Ctx *c)
{
    char n[64]; uint32_t m[16]; int np = party_members(c, m, 16);
    for (int i = 1; i <= 8; i++) {   /* the "transformed" mark on the portraits of transformed party members */
        snprintf(n, sizeof n, "awp_transformed_portrait_%d", i); ui_show_window(c, n, i <= np && is_transformed(c, m[i - 1]));
        snprintf(n, sizeof n, "multi_inventory_dsx_pack_animal_%d", i); ui_hide_group(c, n);
    }
    ui_hide_group(c, "dsx_pack_animal_inventory");
}
/* run the original function (the override steps aside while it runs), then the fix-ups; nargs: its stack arguments */
static int ui_wrap(Ctx *c, uint32_t fn, int nargs)
{
    if (ui_wrapping || !UISHELL) return 0;
    ui_wrapping = 1;
    uint32_t a[4]; for (int i = 0; i < nargs; i++) a[i] = ARG(i);
    ext_thiscall(c, fn, c->ecx, nargs, a);
    ui_expansion_fixups(c);
    ui_wrapping = 0;
    c->esp += 4 + 4 * (uint32_t)nargs; return 1;
}

static void world_map_open(Ctx *c);
static void world_map_close(Ctx *c);
static int end_party_spells(Ctx *c, int transforms);
static void publish_keys(Ctx *c, uint32_t uigame);
static int override_impl(Ctx *c, uint32_t addr);
/* an override either performs the whole call or returns 0 to let the original run; calls made while deciding (into the
 * game) clobber the registers the original expects at its entry, so they are put back */
int loa_override(Ctx *c, uint32_t addr)
{
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp;
    if (override_impl(c, addr)) return 1;
    c->eax = eax; c->ecx = ecx; c->edx = edx; c->ebx = ebx; c->esi = esi; c->edi = edi; c->ebp = ebp;
    return 0;
}
static int override_impl(Ctx *c, uint32_t addr)
{
    switch (addr) {
    case 0x5abf1e: {                                  /* Rules::ChangeLife(Goid, float delta, DWORD): damage transference */
        static int inside; if (inside || bitsf(ARG(1)) >= 0) return 0;
        uint32_t aspect = GO_ASPECT(goid_go(c, ARG(0))), taker = aspect ? side_get(aspect, S_DAMAGE_TAKER, 0) : 0;
        if (!taker || !goid_go(c, taker)) return 0;
        inside = 1; uint32_t a[3] = {taker, ARG(1), ARG(2)}; ext_thiscall(c, addr, c->ecx, 3, a); inside = 0;
        c->esp += 4 + 12; return 1;
    }
    case 0x5d281f: {                                  /* Job* GoMind::SDoJob(const JobReq&): frozen minds take no movement jobs */
        uint32_t jat = rt_r32(G_MEM, ARG(0));         /* JobReq::m_Jat */
        static uint32_t jat_get, jat_talk; if (!jat_get) { fubi_enum(c, "eJobAbstractType", "jat_get", &jat_get); fubi_enum(c, "eJobAbstractType", "jat_talk", &jat_talk); }
        if ((jat == jat_get || jat == jat_talk) && is_transformed(c, COMP_GO(c->ecx))) { c->eax = 0; c->esp += 8; return 1; }   /* a creature can neither pick up nor talk */
        if (side_get(c->ecx, S_ALLOW_MOVE, 1)) return 0;
        if (jat != 25 && jat != 19 && jat != 26 && jat != 18 && jat != JAT_APPROACH) return 0;   /* move, follow, patrol, flee, approach */
        c->eax = 0; c->esp += 8; return 1;
    }
    case 0x4f0b34: {                                  /* the in-game interface's commands (UI "notify" messages) */
        uint32_t p = rt_r32(G_MEM, ARG(0)); const char *m = p ? GS(p) : "";
        if (!strcmp(m, "redistribute_potions")) redistribute_potions(c, 0);
        else if (!strcmp(m, "activate_world_map")) world_map_open(c);
        else if (!strcmp(m, "exit_world_map")) world_map_close(c);
        else if (!strcmp(m, "unsummon_creatures")) end_party_spells(c, 0);
        else if (!strcmp(m, "eg_close")) {            /* closing the end-of-game dialog: ours, then the base game's own handling */
            uint32_t n = gpstr(c, "dsx_end_game");
            ext_thiscall(c, FX("?MarkInterfaceForDeactivation@UIShell@@QAEXABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@@Z"), UISHELL, 1, &n);
            return 0;
        }
        else if (!strcmp(m, "untransform_characters")) end_party_spells(c, 1);
        else return 0;                                /* the base engine's own commands */
        c->esp += 4 + 8; return 1;
    }
    case 0x4e72f3: {                                  /* UIGame: publish the in-game key commands */
        static int inside; if (inside) return 0;
        inside = 1; ext_thiscall(c, addr, c->ecx, 0, 0); publish_keys(c, c->ecx); inside = 0;
        c->esp += 4; return 1;
    }
    case 0x533eb3: {                                  /* GoAspect::Xfer (saving, cloning): a transformed character as itself */
        static int inside; uint32_t a = c->ecx, go = COMP_GO(a);
        if (inside || !is_transformed(c, go)) return 0;
        uint32_t cur = rt_r32(G_MEM, a + 0x34), ptr = rt_r32(G_MEM, a + 0x38), scale = rt_r32(G_MEM, a + 0x18), own = side_get(go, S_OWN_ASPECT, 0);
        rt_w32(G_MEM, a + 0x34, own); rt_w32(G_MEM, a + 0x38, ext_thiscall(c, ASPECT_PTR, ASPECT_OBJECTS, 1, &own));
        rt_w32(G_MEM, a + 0x18, side_get(go, S_OWN_SCALE, scale));
        uint32_t arg = ARG(0); inside = 1; ext_thiscall(c, addr, a, 1, &arg); inside = 0;
        rt_w32(G_MEM, a + 0x34, cur); rt_w32(G_MEM, a + 0x38, ptr); rt_w32(G_MEM, a + 0x18, scale);
        c->esp += 8; return 1;
    }
    case 0x4997d7: {                                  /* the campaign is won: the expansion's own end-of-game dialog */
        uint32_t a[2] = {gpstr(c, "ui:interfaces:backend:dsx_end_game"), 1};
        ext_thiscall(c, FX("?ActivateInterface@UIShell@@QAEXABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@_N@Z"), UISHELL, 2, a);
        static uint32_t iface, mp, sp; if (!iface) { iface = gstr("dsx_end_game"); mp = gstr("text_box_eg_mp"); sp = gstr("text_box_eg"); }
        for (int k = 0; k < 2; k++) {                 /* as the base game: the "continue or start anew" text */
            uint32_t f[2] = {k ? sp : mp, iface}, w = ext_thiscall(c, UI_FIND_WINDOW, UISHELL, 2, f), v = !k;
            if (w) ext_thiscall(c, rt_r32(G_MEM, rt_r32(G_MEM, w) + 0x48), w, 1, &v);
        }
        c->esp += 4; return 1;
    }
    case 0x6dec5c: return ui_wrap(c, addr, 1);        /* ShowInterface(const gpstring&) */
    case 0x6dee75: return ui_wrap(c, addr, 4);        /* ShowGroup(group, show, ..., interface) */
    case 0x5cfa0d:                                    /* const char* ToString(eJobAbstractType) */
        if (ARG(0) != JAT_APPROACH) return 0;
        c->eax = s_jat_approach; c->esp += 4; return 1;
    case 0x5cfa1e:                                    /* bool FromString(const char*, eJobAbstractType&) */
        if (strcasecmp(GS(ARG(0)), "jat_approach")) return 0;
        rt_w32(G_MEM, ARG(1), JAT_APPROACH); c->eax = 1; c->esp += 4; return 1;
    case 0x51f739:                                    /* const char* ToString(eQueryTrait) */
        if (ARG(0) != QT_UNDERATTACK) return 0;
        c->eax = s_qt_underattack; c->esp += 4; return 1;
    case 0x5e179b: {                                  /* bool AIQuery::Is(Go const* from, Go const* target, eQueryTrait) */
        if (ARG(2) != QT_UNDERATTACK) return 0;
        uint32_t mind = GO_MIND(ARG(1)), r = 0;
        if (mind) {
            uint32_t coll = ext_thiscall(c, FX("?GetTempGopColl3@AIQuery@@QAEAAUGopColl@@XZ"), c->ecx, 0, 0);
            ext_thiscall(c, FX("?Clear@GopColl@@AAEXXZ"), coll, 0, 0);
            ext_thiscall(c, FX("?GetEngagedMeEnemies@GoMind@@QBE_NAAUGopColl@@@Z"), mind, 1, &coll);
            r = ext_thiscall(c, FX("?Size@GopColl@@ABEHXZ"), coll, 0, 0) > 0;
        }
        c->eax = r; c->esp += 4 + 12; return 1;
    }
    case 0x5cfa33: {                                  /* DWORD job type flags */
        if (ARG(0) != JAT_APPROACH) return 0;
        uint32_t follow = JAT_FOLLOW; c->eax = w32_callback(c, 0x5cfa33, 1, &follow); c->esp += 4; return 1;
    }
    }
    return 0;
}

void loa_hook(Ctx *c, uint32_t addr)
{
    switch (addr) {
    case 0x4acb46:                                    /* a FuBi enum spec was constructed (this in eax) */
        if (c->eax == 0x7a9fa0u) rt_w32(G_MEM, 0x7a9fa0u + 0x20, JAT_APPROACH + 1);   /* eJobAbstractType: 34 values */
        break;
    case 0x4036a8:                                    /* the same, second copy of the constructor */
        if (c->eax == 0x7a7980u) rt_w32(G_MEM, 0x7a7980u + 0x20, QT_UNDERATTACK + 1);  /* eQueryTrait: 50 values */
        break;
    case 0x5d1fcf: {                                  /* GoMind: template jobs loaded (ebx: the mind's data, esi: GoMind) */
        uint32_t esp = c->esp, t = scratch(c, 64), str = t, out = t + 0x20, pair = t + 0x28;
        uint32_t a[3];
        ext_thiscall(c, 0x42aea8u, str, 0, 0);                                    /* gpstring() */
        a[0] = s_jat_approach; a[1] = str; ext_thiscall(c, 0x5da61eu, c->ebx, 2, a);  /* the template's jat_approach */
        int empty = ext_thiscall(c, 0x495958u, str, 0, 0) & 0xff;
        a[0] = s_jat_none; a[1] = rt_r32(G_MEM, 0x721418u);
        int none = !empty && (ext_thiscall(c, 0x495925u, str, 2, a) & 0xff);
        if (!empty && !none) {
            uint32_t interior = ext_thiscall(c, 0x474107u, c->ebx, 0, 0) & 0xff;
            uint32_t ai = w32_callback(c, 0x47338au, 0, 0);                        /* AIAction singleton */
            a[0] = out; a[1] = str; a[2] = interior;
            uint32_t r = ext_thiscall(c, 0x5ea61bu, ai, 3, a);                     /* the job's AI action */
            ext_thiscall(c, 0x5d207eu, pair, 0, 0);
            rt_w32(G_MEM, pair, JAT_APPROACH); rt_w32(G_MEM, pair + 4, rt_r32(G_MEM, r));
            a[0] = pair; ext_thiscall(c, 0x5d908du, c->esi + 0x20c, 1, a);         /* the mind's job map */
        }
        ext_thiscall(c, 0x48899au, str, 0, 0);                                    /* ~gpstring() */
        scratch_end(c, esp);
        break;
    }
    }
}

/* ---- the overhead world map (Legends of Aranna): the map's info/overheadmap.gas lists the pieces of the world
 * image revealed as the party explores (256x256 textures at x,y on a 1024x768 map) and the named area markers. Which
 * pieces are revealed and which marker is current are kept in the game's quest database (saved with the game). ---- */
static uint32_t gpstr(Ctx *c, const char *text)          /* a heap gpstring with the given text (kept) */
{
    uint32_t g = heap_alloc(w32_process_heap, 8, 16); ext_thiscall(c, GPSTR_CTOR, g, 0, 0);
    static uint32_t fmt; if (!fmt) fmt = gstr("%s");
    uint32_t t = gstr(text), a[3] = {g, fmt, t};
    w32_callback(c, FX("?AssignF@String@@CAAAV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@AAV2@PBDZZ"), 3, a);
    return g;
}
static GasBlock *omap; static char omap_for[128];
static GasBlock *wmap_settings(void)                     /* ui:config:worldmap_settings (font, colours, marker texture) */
{
    extern char w32_game_layer[1024]; static GasBlock *g; static int tried;
    if (!tried) {
        tried = 1; char dir[1100], path[1400]; snprintf(dir, sizeof dir, "%s/Resources", w32_game_layer);
        DIR *d = opendir(dir); struct dirent *e;
        while (d && !g && (e = readdir(d))) {
            if (!strcasestr(e->d_name, ".dsres")) continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name); uint8_t *data; size_t n;
            if (!tank_read(path, "ui/config/worldmap_settings/worldmap_settings.gas", &data, &n)) {
                char *t = malloc(n + 1); memcpy(t, data, n); t[n] = 0; free(data); g = gas_child(gas_parse(t), "worldmap_settings"); free(t);
            }
        }
        if (d) closedir(d);
    }
    return g;
}
static GasBlock *overheadmap(Ctx *c)                      /* the current map's overhead map description */
{
    extern char w32_game_layer[1024];
    uint32_t wm = w32_callback(c, FX("?FUBI_GetClassSingleton@WorldMap@@CAPAV1@XZ"), 0, 0);
    uint32_t np = wm ? rt_r32(G_MEM, wm + 4) : 0; const char *map = np ? GS(np) : "";
    if (omap && !strcmp(map, omap_for)) return omap;
    gas_free(omap); omap = 0; snprintf(omap_for, sizeof omap_for, "%s", map);
    char dir[1100], path[1400], inside[256]; snprintf(dir, sizeof dir, "%s/Maps", w32_game_layer);
    snprintf(inside, sizeof inside, "world/maps/%s/info/overheadmap.gas", map);
    DIR *d = opendir(dir); struct dirent *e;
    while (d && !omap && (e = readdir(d))) {
        if (!strcasestr(e->d_name, ".dsmap")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        uint8_t *data; size_t n;
        if (!tank_read(path, inside, &data, &n)) { char *t = malloc(n + 1); memcpy(t, data, n); t[n] = 0; free(data); omap = gas_child(gas_parse(t), "overheadmap"); free(t); }
    }
    if (d) closedir(d);
    return omap;
}
static void quest_set(Ctx *c, const char *key, int value, int is_bool)
{
    uint32_t godb = w32_callback(c, FX("?FUBI_GetClassSingleton@GoDb@@CAPAV1@XZ"), 0, 0), any = w32_callback(c, FX("?GetAnyGoid@Goid_@@CAPBU1@XZ"), 0, 0);
    static uint32_t cat; if (!cat) cat = gstr("dsx_overheadmap");
    uint32_t a[4] = {any, cat, gstr(key), (uint32_t)value};
    ext_thiscall(c, is_bool ? FX("?SSetQuestBool@GoDb@@QAEXPBUGoid_@@PBD1_N@Z") : FX("?SSetQuestInt@GoDb@@QAEXPBUGoid_@@PBD1H@Z"), godb, 4, a);
}
static int quest_get(Ctx *c, const char *key, int is_bool)    /* written for every player (AnyGoid), read for the screen hero */
{
    uint32_t godb = w32_callback(c, FX("?FUBI_GetClassSingleton@GoDb@@CAPAV1@XZ"), 0, 0);
    uint32_t server = w32_callback(c, FX("?FUBI_GetClassSingleton@Server@@CAPAV1@XZ"), 0, 0);
    uint32_t hero = server ? ext_thiscall(c, FX("?GetScreenHero@Server@@QAEPAVGo@@XZ"), server, 0, 0) : 0, any = hero ? GO_GOID(hero) : 0;
    if (!godb || !any) return 0;
    static uint32_t cat; if (!cat) cat = gstr("dsx_overheadmap");
    uint32_t esp = c->esp, t = scratch(c, 64); snprintf((char *)GP(t), 64, "%s", key);
    uint32_t a[3] = {any, cat, t};
    int r = (int)ext_thiscall(c, is_bool ? FX("?GetQuestBool@GoDb@@QBE_NPBUGoid_@@PBD1@Z") : FX("?GetQuestInt@GoDb@@QBEHPBUGoid_@@PBD1@Z"), godb, 3, a);
    scratch_end(c, esp); return is_bool ? (r & 0xff) : r;
}
static const char *gpstr_text(uint32_t g) { uint32_t p = g ? rt_r32(G_MEM, g) : 0; return p ? GS(p) : ""; }
static void OverheadMap_RSSetCurrentBackground(Ctx *c) { RET(0, 2); }       /* one background (mainland) */
static void OverheadMap_RSUpdateCurrentAreaMarker(Ctx *c)                    /* (const gpstring& marker, Goid) */
{
    GasBlock *m = gas_child(overheadmap(c), "markers"); const char *name = gpstr_text(ARG(0));
    for (int i = 0; m && i < m->nchild; i++) if (!strcasecmp(m->child[i]->name, name)) quest_set(c, "current_marker", i + 1, 0);
    RET(0, 2);
}
static void OverheadMap_RSUpdateMapPieceVisibility(Ctx *c)                   /* (const gpstring& piece, bool visible, Goid) */
{
    char key[96]; snprintf(key, sizeof key, "piece_%s", gpstr_text(ARG(0))); quest_set(c, key, ARG(1) & 0xff, 1);
    RET(0, 3);
}
/* a window on the world map interface: texture (or text) at a rectangle given in the map's 1024x768 coordinates */
static uint32_t wmap_window(Ctx *c, const char *type, const char *name, int x0, int y0, int x1, int y1)
{
    uint32_t sh = UISHELL, tp = gpstr(c, type);
    uint32_t w = ext_thiscall(c, FX("?CreateDefaultWindowOfType@UIShell@@QAEPAVUIWindow@@ABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@@Z"), sh, 1, &tp);
    if (!w) return 0;
    uint32_t nm = gpstr(c, name); ext_thiscall(c, FX("?SetName@UIWindow@@QAEXABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@@Z"), w, 1, &nm);
    int W = (int)ext_thiscall(c, FX("?GetScreenWidth@UIShell@@QBEHXZ"), sh, 0, 0), H = (int)ext_thiscall(c, FX("?GetScreenHeight@UIShell@@QBEHXZ"), sh, 0, 0);
    uint32_t r = ext_thiscall(c, FX("?GetRect@UIWindow@@QAEAAUGRect@@XZ"), w, 0, 0);
    rt_w32(G_MEM, r, (uint32_t)(x0 * W / 1024)); rt_w32(G_MEM, r + 4, (uint32_t)(y0 * H / 768));
    rt_w32(G_MEM, r + 8, (uint32_t)(x1 * W / 1024)); rt_w32(G_MEM, r + 12, (uint32_t)(y1 * H / 768));
    uint32_t order = 5; ext_thiscall(c, FX("?SetDrawOrder@UIWindow@@QAEXH@Z"), w, 1, &order);
    uint32_t a[3] = {w, gpstr(c, "world_map"), 1};
    ext_thiscall(c, FX("?AddWindowToInterface@UIShell@@QAEXPAVUIWindow@@ABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@_N@Z"), sh, 3, a);
    uint32_t on = 1; ext_thiscall(c, rt_r32(G_MEM, rt_r32(G_MEM, w) + 0x48), w, 1, &on);
    return w;
}
static void wmap_texture(Ctx *c, uint32_t w, const char *tex)
{
    uint32_t on = 1; ext_thiscall(c, FX("?SetHasTexture@UIWindow@@QAEX_N@Z"), w, 1, &on);
    uint32_t uv = ext_thiscall(c, FX("?GetUVRect@UIWindow@@QBEABVUINormalizedRect@@XZ"), w, 0, 0);   /* the whole texture */
    rt_wf64(G_MEM, uv, 0); rt_wf64(G_MEM, uv + 8, 1); rt_wf64(G_MEM, uv + 16, 0); rt_wf64(G_MEM, uv + 24, 1);   /* left, right, top, bottom (doubles) */
    uint32_t a[2] = {gpstr(c, tex), 0}; ext_thiscall(c, FX("?FUBI_RENAME_LoadTexture@UIWindow@@QAEXABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@_N@Z"), w, 2, a);
    uint32_t one = fbits(1.0f), off = 0;
    ext_thiscall(c, FX("?SetAlpha@UIWindow@@QAEXM@Z"), w, 1, &one);
    ext_thiscall(c, FX("?SetBackgroundFill@UIWindow@@QAEX_N@Z"), w, 1, &off);
    uint32_t white = 0xffffffffu; ext_thiscall(c, FX("?SetBackgroundColor@UIWindow@@QAEXI@Z"), w, 1, &white);
    if (getenv("DS_EXTLOG")) fprintf(stderr, "loa: texture %s -> index %u\n", tex, ext_thiscall(c, FX("?GetTextureIndex@UIWindow@@QBEIXZ"), w, 0, 0));
}
static void world_map_open(Ctx *c)
{
    GasBlock *om = overheadmap(c); if (!om || !UISHELL) return;
    if (getenv("DS_LOA_MAPTEST")) { quest_set(c, "piece_arhok", 1, 1); quest_set(c, "piece_beach", 1, 1); quest_set(c, "current_marker", 1, 0); }   /* development */
    uint32_t a[2] = {gpstr(c, "ui:interfaces:backend:world_map"), 1};
    ext_thiscall(c, FX("?ActivateInterface@UIShell@@QAEXABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@_N@Z"), UISHELL, 2, a);
    GasBlock *pieces = gas_child(om, "pieces"), *markers = gas_child(om, "markers");
    for (int i = 0; pieces && i < pieces->nchild; i++) {
        GasBlock *p = pieces->child[i]; char key[96], nm[96]; snprintf(key, sizeof key, "piece_%s", p->name);
        if (!quest_get(c, key, 1)) continue;
        int x = atoi(gas_get(p, "x", "0")), y = atoi(gas_get(p, "y", "0"));
        snprintf(nm, sizeof nm, "dsx_wmap_piece_%s", p->name);
        uint32_t w = wmap_window(c, "window", nm, x, y, x + 256, y + 256);
        if (w) wmap_texture(c, w, gas_get(p, "texture", ""));
        if (getenv("DS_EXTLOG")) fprintf(stderr, "loa: world map piece %s at %d,%d -> window %08x\n", p->name, x, y, w);
    }
    int cur = quest_get(c, "current_marker", 0);
    if (getenv("DS_EXTLOG")) fprintf(stderr, "loa: world map for '%s': %d pieces, %d markers, current %d\n", omap_for, pieces ? pieces->nchild : -1, markers ? markers->nchild : -1, cur);
    if (markers && cur > 0 && cur <= markers->nchild) {
        GasBlock *m = markers->child[cur - 1]; int x = atoi(gas_get(m, "x", "0")), y = atoi(gas_get(m, "y", "0"));
        uint32_t w = wmap_window(c, "window", "dsx_wmap_marker", x - 16, y - 16, x + 16, y + 16);
        GasBlock *st = wmap_settings();
        if (w) wmap_texture(c, w, gas_get(st, "default_marker_texture", "b_gui_ig_mnu_wmap_marker"));
        uint32_t t = wmap_window(c, "text", "dsx_wmap_marker_name", x - 300, y + 16, x + 300, y + 66);
        if (t) {
            uint32_t f = gpstr(c, gas_get(st, "font", "b_gui_fnt_16p_copperplate-light"));
            uint32_t col = (uint32_t)strtoul(gas_get(st, "default_text_color", "0xFFEDE6C7"), 0, 16), just = 2;   /* justify_center */
            ext_thiscall(c, FX("?SetFont@UIText@@QAEXABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@@Z"), t, 1, &f);
            ext_thiscall(c, FX("?SetColor@UIText@@QAEXI@Z"), t, 1, &col);
            ext_thiscall(c, FX("?SetJustification@UIText@@QAEXW4JUSTIFICATION@@@Z"), t, 1, &just);
            uint32_t ta[2] = {gpstr(c, gas_get(m, "screen_name", m->name)), 0};
            ext_thiscall(c, FX("?SetText@UIText@@AAEXABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@_N@Z"), t, 2, ta);
        }
    }
}
static void world_map_close(Ctx *c)
{
    uint32_t n = gpstr(c, "world_map");
    /* deferred: this runs while the map's own button is handling the click */
    ext_thiscall(c, FX("?MarkInterfaceForDeactivation@UIShell@@QAEXABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@@Z"), UISHELL, 1, &n);
}

/* ---- the party's spells: dismissing summoned creatures and ending transformations. Both kinds of spell keep a
 * generic state on the party member naming the controlling spell object, which ends the effect on WE_REQ_DEACTIVATE
 * (the expansion's own buttons do the same). The states are a std::map (MSVC: head node at map+4, nodes
 * {left, parent, right, key gpstring, value}, leaves point at a shared nil node). */
static int party_members(Ctx *c, uint32_t *out, int max)
{
    uint32_t server = w32_callback(c, FX("?FUBI_GetClassSingleton@Server@@CAPAV1@XZ"), 0, 0);
    uint32_t party = server ? ext_thiscall(c, FX("?GetScreenParty@Server@@QAEPAVGo@@XZ"), server, 0, 0) : 0;
    if (!party) return 0;
    uint32_t kids = ext_thiscall(c, FX("?GetChildren@Go@@QBEABUGopColl@@XZ"), party, 0, 0);
    int n = (int)ext_thiscall(c, FX("?Size@GopColl@@ABEHXZ"), kids, 0, 0), m = 0;
    for (int i = 0; i < n && m < max; i++) { uint32_t k = (uint32_t)i, g = ext_thiscall(c, FX("?Get@GopColl@@ABEPAVGo@@H@Z"), kids, 1, &k); if (g) out[m++] = g; }
    return m;
}
static int generic_states(uint32_t actor, uint32_t *node, int max) { return map_nodes(actor + 0x48, node, max); }
#define STATE_NAME(node)  rt_r32(G_MEM, (node) + 0xc)
#define STATE_SPELL(node) rt_r32(G_MEM, (node) + 0x20)
static int end_party_spells(Ctx *c, int transforms)
{
    static uint32_t we_deactivate; if (!we_deactivate && !fubi_enum(c, "eWorldEvent", "we_req_deactivate", &we_deactivate)) return 0;
    static const char *const summon[] = {"spell_summon", "spell_summon_clone", "spell_summon_multiple", "spell_summon_random"};
    uint32_t member[16], node[64]; int n = party_members(c, member, 16), done = 0;
    for (int i = 0; i < n; i++) {
        uint32_t actor = GO_ACTOR(member[i]); if (!actor) continue;
        int k = generic_states(actor, node, 64);
        for (int j = 0; j < k; j++) {
            uint32_t nm = STATE_NAME(node[j]), spell = STATE_SPELL(node[j]), sg = goid_go(c, spell); int hit = 0;
            if (!sg) continue;
            if (transforms) hit = nm && !strcasecmp(GS(nm), "transformed");
            else for (int q = 0; q < 4 && !hit; q++) {
                static uint32_t cn[4]; if (!cn[q]) cn[q] = gstr(summon[q]);
                hit = ext_thiscall(c, FX("?HasComponent@Go@@QBE_NPBD@Z"), sg, 1, &cn[q]) & 0xff;
            }
            if (!hit) continue;
            uint32_t a[5] = {we_deactivate, GO_GOID(member[i]), spell, GO_GOID(member[i]), 0};
            w32_callback(c, fn_post_data, 5, a); done++;
        }
    }
    return done;
}

/* ---- the expansion's key commands, published to the game's input binder next to the base game's (the bindings
 * themselves come from the expansion's config/input_bindings.gas) ---- */
static int world_map_visible(Ctx *c) { uint32_t n = gpstr(c, "world_map"); return UISHELL && (ext_thiscall(c, FX("?IsInterfaceVisible@UIShell@@QAE_NABV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@@Z"), UISHELL, 1, &n) & 0xff); }
static void key_toggle_world_map(Ctx *c) { if (world_map_visible(c)) world_map_close(c); else world_map_open(c); RET(1, 0); }
static void key_redistribute_potions(Ctx *c) { redistribute_potions(c, 0); RET(1, 0); }
static void key_unsummon(Ctx *c) { end_party_spells(c, 0); RET(1, 0); }
static void key_untransform(Ctx *c)
{
    const char *test = getenv("DS_LOA_TRANSFORMTEST");      /* development: Y turns the first party member into <template> and back */
    const char *spell = getenv("DS_LOA_SPELLTEST");         /* development: Y casts spell <template> from the first member on itself */
    if (getenv("DS_LOA_ENDTEST")) { w32_callback(c, 0x4997d7u, 0, 0); RET(1, 0); }   /* development: Y shows the end-of-game dialog */
    uint32_t m[16];
    if (test && party_members(c, m, 16)) { if (!untransform_go(c, m[0])) transform_go(c, m[0], test); }
    else if (spell && party_members(c, m, 16) && is_transformed(c, m[0])) end_party_spells(c, 1);
    else if (spell && party_members(c, m, 16)) {
        static uint32_t we_cast; if (!we_cast) fubi_enum(c, "eWorldEvent", getenv("DS_LOA_SPELLEVENT") ? getenv("DS_LOA_SPELLEVENT") : "we_req_cast", &we_cast);
        uint32_t esp = c->esp, hero = GO_GOID(m[0]), a[2] = {hero, sstr(c, spell)};
        uint32_t req = w32_callback(c, FX("?MakeGoCloneReq@@YAAAUGoCloneReq@@PBUGoid_@@PBD@Z"), 2, a);
        uint32_t godb = w32_callback(c, FX("?FUBI_GetClassSingleton@GoDb@@CAPAV1@XZ"), 0, 0);
        uint32_t sp = ext_thiscall(c, FX("?SCloneGo@GoDb@@QAEPBUGoid_@@ABUGoCloneReq@@@Z"), godb, 1, &req);
        scratch_end(c, esp);
        uint32_t p[5] = {we_cast, hero, sp, hero, 0}; w32_callback(c, fn_post_data, 5, p);
        fprintf(stderr, "loa: test cast of %s (spell %08x) by %08x\n", spell, sp, hero);
    }
    else end_party_spells(c, 1);
    RET(1, 0);
}
static void publish_keys(Ctx *c, uint32_t uigame)
{
    static const struct { const char *name; void (*fn)(Ctx *); } keys[] = {
        {"toggle_world_map", key_toggle_world_map}, {"redistribute_potions", key_redistribute_potions},
        {"unsummon_critter", key_unsummon}, {"untransform_actor", key_untransform},
    };
    uint32_t binder = rt_r32(G_MEM, uigame + 8);                  /* the in-game input binder */
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) {
        static uint32_t thunk[8]; if (!thunk[i]) thunk[i] = w32_thunk_register(keys[i].name, keys[i].fn);
        uint32_t a[5] = {gpstr(c, keys[i].name), thunk[i], 0, uigame, 0x4e1adau};   /* name; functor {method, adjust, object, invoker} */
        ext_thiscall(c, 0x43b0fau, binder, 5, a);
    }
}

void loa_register(void)
{
    loa_active = 1;
    s_jat_approach = gstr("jat_approach"); s_jat_none = gstr("jat_none"); s_qt_underattack = gstr("qt_underattack");
    fn_is_completed = ext_export("?IsQuestCompleted@Victory@@QAE_N" GPS "@Z");
    fn_is_active = ext_export("?IsQuestActive@Victory@@QAE_N" GPS "@Z");
    fn_order = ext_export("?GetQuestOrder@Victory@@QAEH" GPS "@Z");
    fn_activate = ext_export("?RSActivateQuest@Victory@@QAEXPBDH@Z");
    fn_deactivate = ext_export("?RSDeactivateQuest@Victory@@QAEXPBD@Z");
    fn_post_data = ext_export("?PostWorldMessage@@YAXW4eWorldEvent@@PBUGoid_@@1KM@Z");
    fn_post_scid_data = ext_export("?PostWorldMessage@@YAXW4eWorldEvent@@PBUGoid_@@PBUScid_@@KM@Z");
    fn_data1 = ext_export("?FUBI_RENAME_GetData1@WorldMessage@@QBEHXZ");
    ext_add("?IsQuestCompleted@Victory@@QAE_N" GPS "PBUGoid_@@@Z", Victory_IsQuestCompleted);
    ext_add("?IsQuestActive@Victory@@QAE_N" GPS "PBUGoid_@@@Z", Victory_IsQuestActive);
    ext_add("?GetQuestOrder@Victory@@QAEH" GPS "PBUGoid_@@@Z", Victory_GetQuestOrder);
    ext_add("?RSActivateQuest@Victory@@QAEXPBDHPBUGoid_@@@Z", Victory_RSActivateQuest);
    ext_add("?RSDeactivateQuest@Victory@@QAEXPBDPBUGoid_@@@Z", Victory_RSDeactivateQuest);
    ext_add("?PostWorldMessage@@YAXW4eWorldEvent@@PBUGoid_@@PBUScid_@@1M@Z", PostWorldMessage_activator);
    ext_add("?SPostWorldMessage@@YAXW4eWorldEvent@@PBUGoid_@@1KM@Z", SPostWorldMessage_goid_data);
    ext_add("?SPostWorldMessage@@YAXW4eWorldEvent@@PBUGoid_@@1M@Z", SPostWorldMessage_goid);
    ext_add("?GetActivator@WorldMessage@@QBEPBUGoid_@@XZ", WorldMessage_GetActivator);
#define GPS_ "AAV?$gpbstring@DU?$char_traits@D@std@@V?$allocator@D@2@@@"
    ext_add("?SetAllowNewMovementJobs@GoMind@@QAEX_N@Z", GoMind_SetAllowNewMovementJobs);
    ext_add("?GetAllowNewMovementJobs@GoMind@@QBE_NXZ", GoMind_GetAllowNewMovementJobs);
    ext_add("?GetBestSupportEnemy@GoMind@@QBEPAVGo@@PAUGopColl@@@Z", GoMind_GetBestSupportEnemy);
    ext_add("?RSApproach@GoMind@@QAEXPAVGo@@MW4eQPlace@@W4eActionOrigin@@_N@Z", GoMind_RSApproach);
    ext_add("?RSCopyMind@GoMind@@QAEXABV1@@Z", GoMind_RSCopyMind);
    ext_add("?GetEnemiesOfGoInSphere@AIQuery@@QAE_NABUSiegePos@@MPBVGo@@AAUGopColl@@@Z", AIQuery_GetEnemiesOfGoInSphere);
    ext_add("?SAddGenericState@GoActor@@QAEXPBD0HPBUGoid_@@@Z", GoActor_SAddGenericState_int);
    ext_add("?GetGenericStateCasterGoid@GoActor@@QBEPBUGoid_@@PBD@Z", GoActor_GetGenericStateCasterGoid);
    ext_add("?RSCopySkills@GoActor@@QAEXABV1@M@Z", GoActor_RSCopySkills);
    ext_add("?SetLodfi@Go@@QAEX_N@Z", Go_SetLodfi);
    ext_add("?CheckModifierRecalc@Go@@QAE_NXZ", Go_CheckModifierRecalc);
    ext_add("?IsImbuedItem@Go@@QBE_NXZ", Go_IsImbuedItem);
    ext_add("?GetIsPContentInventory@Go@@QBE_NXZ", Go_GetIsPContentInventory);
    ext_add("?SetIsPContentInventory@Go@@QAEX_N@Z", Go_SetIsPContentInventory);
    ext_add("?RSSetDoppelgangerStats@Go@@QAEXPAV1@M@Z", Go_RSSetDoppelgangerStats);
    ext_add("?SetCopyInventory@GoCloneReq@@QAEX_N@Z", GoCloneReq_SetCopyInventory);
    ext_add("?GetDamageMin@Rules@@QAEMPBUGoid_@@0@Z", Rules_GetDamageMin);
    ext_add("?GetDamageMax@Rules@@QAEMPBUGoid_@@0@Z", Rules_GetDamageMax);
    ext_add("?IsMemberofGroup@Rules@@QAE_NPBUGoid_@@" GPS "@Z", Rules_IsMemberofGroup);
    ext_add("?RSSetNaturalSkillLevel@Rules@@QAEXPBUGoid_@@PBDM@Z", Rules_RSSetNaturalSkillLevel);
    ext_add("?AdjustPosition@GoPlacement@@QAEXMMM_N@Z", GoPlacement_AdjustPosition);
    ext_add("?OrientToPosition@GoPlacement@@QAEXABUSiegePos@@@Z", GoPlacement_OrientToPosition);
    ext_add("?SetMaxMoveVelocity@GoBody@@QAEXM@Z", GoBody_SetMaxMoveVelocity);
    ext_add("?GetTerrainMovementPermissions@GoBody@@QBE?AW4eLogicalNodeFlags@siege@@XZ", GoBody_GetTerrainMovementPermissions);
    ext_add("?GetSimDuration@GoPhysics@@QBEMXZ", GoPhysics_GetSimDuration);
    ext_add("?GetPosition@WorldFx@@QAE_NHAAUSiegePos@@@Z", WorldFx_GetPosition);
    ext_add("?SetRenderScaleMultiplier@GoAspect@@QAEXM@Z", GoAspect_SetRenderScaleMultiplier);
    ext_add("?HasAspectHandle@GoAspect@@QBE_NXZ", GoAspect_HasAspectHandle);
    ext_add("?GetNumSubTextures@GoAspect@@QAEHXZ", GoAspect_GetNumSubTextures);
    ext_add("?SetDamageTaker@GoAspect@@QAEXPBUGoid_@@@Z", GoAspect_SetDamageTaker);
    ext_add("?ClearDamageTaker@GoAspect@@QAEXXZ", GoAspect_ClearDamageTaker);
    ext_add("?SSetIsSetItem@GoAspect@@QAEX_N@Z", GoAspect_SSetIsSetItem);
    ext_add("?SSetItemNumbers@GoAspect@@QAEXH@Z", GoAspect_SSetItemNumbers);
    ext_add("?SSetItemSetGroup@GoAspect@@QAEX" GPS_ "0@Z", GoAspect_SSetItemSetGroup);
    ext_add("?SSetItemSetModName@GoAspect@@QAEX" GPS_ "@Z", GoAspect_SSetItemSetModName);
    ext_add("?SInitISModifierData@GoAspect@@QAEXXZ", GoAspect_SInitISModifierData);
    ext_add("?InitializeLighting@Aspect@nema@@QAEXKK@Z", Aspect_InitializeLighting);
    ext_add("?GetAmbience@Aspect@nema@@QBEKXZ", Aspect_GetAmbience);
    ext_add("?GetAlpha@Aspect@nema@@QBEKXZ", Aspect_GetAlpha);
    ext_add("?ChangeSpeedModifier@Blender@nema@@QAEXM@Z", Blender_ChangeSpeedModifier);
    ext_add("?GetPotionAmount@GoMagic@@QBEM_N@Z", GoMagic_GetPotionAmount);
    ext_add("?SSetPotionAmountReally@GoMagic@@QAEXM@Z", GoMagic_SSetPotionAmountReally);
    ext_add("?IsHealthPotion@GoMagic@@QBE_NXZ", GoMagic_IsHealthPotion);
    ext_add("?IsManaPotion@GoMagic@@QBE_NXZ", GoMagic_IsManaPotion);
    ext_add("?GetActiveSpellBook@GoInventory@@QBEPAVGo@@XZ", GoInventory_GetActiveSpellBook);
    ext_add("?SetActiveSpellBook@GoInventory@@QAEXPAVGo@@@Z", GoInventory_SetActiveSpellBook);
    ext_add("?IsNonAggressivePack@GoInventory@@QBE_NXZ", GoInventory_IsNonAggressivePack);
    ext_add("?SSetDirtySetItem@GoInventory@@QAEXPAVGo@@K@Z", GoInventory_SSetDirtySetItem);
    ext_add("?TestGet@GoInventory@@QAE_NPBUGoid_@@@Z", GoInventory_TestGet1);
    ext_add("?TestGet@GoInventory@@QAE_NPBUGoid_@@_N@Z", GoInventory_TestGet2);
    ext_add("?GetGridbox@GoInventory@@QAEPAVUIGridbox@@XZ", GoInventory_GetGridbox);
    ext_add("?FUBI_GetClassSingleton@TransformationManager@@CAPAV1@XZ", TransformationManager_Singleton);
    ext_add("?STransformMe@TransformationManager@@QAEXPAVGo@@" GPS_ "1@Z", TransformationManager_STransformMe);
    ext_add("?SUnTransformMe@TransformationManager@@QAEXPAVGo@@@Z", TransformationManager_SUnTransformMe);
    ext_add("?GetNewTemplateName@TransformationManager@@QAE" GPS_ "PAVGo@@@Z", TransformationManager_GetNewTemplateName);
    ext_add("?FUBI_GetClassSingleton@OverheadMap@@CAPAV1@XZ", OverheadMap_Singleton);
    ext_add("?RSSetCurrentBackground@OverheadMap@@QAEX" GPS "PBUGoid_@@@Z", OverheadMap_RSSetCurrentBackground);
    ext_add("?RSUpdateCurrentAreaMarker@OverheadMap@@QAEX" GPS "PBUGoid_@@@Z", OverheadMap_RSUpdateCurrentAreaMarker);
    ext_add("?RSUpdateMapPieceVisibility@OverheadMap@@QAEX" GPS "_NPBUGoid_@@@Z", OverheadMap_RSUpdateMapPieceVisibility);
    ext_add("?GetParty@Player@@QAEPAVGo@@XZ", Player_GetParty);
    ext_add("?SetRealMinutesModifier@TimeOfDay@@QAEXM@Z", TimeOfDay_SetRealMinutesModifier);
    ext_add("?SetGameInputBinderActive@UIGame@@QAEX_N@Z", UIGame_SetGameInputBinderActive);
    ext_add("?GetUsingPlayerJournal@WorldMap@@QBE_NXZ", WorldMap_GetUsingPlayerJournal);
    ext_add("?RedistributePotions@UIPartyManager@@QAE_N_NPBUGoid_@@@Z", UIPartyManager_RedistributePotions);
    ext_add("?SToggleTriggeredEffects@GoCommon@@QAEX" GPS_ "_N@Z", GoCommon_SToggleTriggeredEffects);
    ext_add("?SRemoveEnchantments@GoDb@@QAEXPBUGoid_@@0_N@Z", GoDb_SRemoveEnchantments);
    ext_add("?LShift@Math@@YAHHH@Z", Math_LShift);
    ext_add("?RShift@Math@@YAHHH@Z", Math_RShift);
    ext_add("?SetNaturalLifeRecoveryUnit@GoAspect@@QAEXM@Z", GoAspect_SetNaturalLifeRecoveryUnit);
    ext_add("?SetLifeRecoveryUnit@GoAspect@@QAEXM@Z", GoAspect_SetLifeRecoveryUnit);
}
