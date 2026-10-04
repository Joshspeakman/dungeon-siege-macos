/* Legends of Aranna on the base engine: the engine functions the expansion's scripts call, implemented for the GOG
 * 1.11.1 engine (whose executable the build verifies by checksum, so its internal addresses below are fixed).
 * Registered by loa_register() when the expansion's data is active (DS_EXPANSION); see ext.c for how they reach FuBi.
 * Names are the C++ decorated names the scripts' compiler binds to; the signatures follow the calls in the
 * expansion's scripts. */
#include "ext.h"
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
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
/* scratch memory on the guest stack for out-parameters (released by scratch_end) */
static uint32_t scratch(Ctx *c, uint32_t n) { c->esp -= (n + 15) & ~15u; return c->esp; }
static void scratch_end(Ctx *c, uint32_t esp) { c->esp = esp; }
static uint32_t goid_go(Ctx *c, uint32_t goid) { return goid ? ext_thiscall(c, FX("?GetGo@Goid_@@ABEPAVGo@@XZ"), goid, 0, 0) : 0; }
static uint32_t go_comp(Ctx *c, uint32_t go, const char *getter) { return go ? ext_thiscall(c, ext_export(getter), go, 0, 0) : 0; }
#define GO_ASPECT(go)    go_comp(c, go, "?GetAspect@Go@@QAEPAVGoAspect@@XZ")
#define GO_MIND(go)      go_comp(c, go, "?GetMind@Go@@QAEPAVGoMind@@XZ")
#define GO_ACTOR(go)     go_comp(c, go, "?GetActor@Go@@QAEPAVGoActor@@XZ")
#define GO_MAGIC(go)     go_comp(c, go, "?GetMagic@Go@@QAEPAVGoMagic@@XZ")
#define GO_COMMON(go)    go_comp(c, go, "?GetCommon@Go@@QAEPAVGoCommon@@XZ")
#define GO_PLACEMENT(go) go_comp(c, go, "?GetPlacement@Go@@QAEPAVGoPlacement@@XZ")

/* per-object state the base engine has no field for, keyed by object address or Goid */
typedef struct { uint32_t key, kind; uint32_t v; } Side;
static Side side[16384]; static pthread_mutex_t side_lock = PTHREAD_MUTEX_INITIALIZER;
enum { S_ALLOW_MOVE = 1, S_LODFI, S_PCONTENT_INV, S_DAMAGE_TAKER, S_SPELLBOOK, S_IS_SET_ITEM, S_SET_COUNT, S_REAL_MINUTES, S_COPY_INV };
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
static void GoActor_RSCopySkills(Ctx *c)             /* (GoActor const& from, float multiplier): the attributes */
{
    uint32_t from = ARG(0); float k = ARGF(1); uint32_t get = FX("?GetSkillLevel@GoActor@@QBEMPBD@Z");
    static uint32_t names; if (!names) { names = heap_alloc(w32_process_heap, 8, 64); strcpy((char *)GP(names), "strength"); strcpy((char *)GP(names + 16), "dexterity"); strcpy((char *)GP(names + 32), "intelligence"); }
    uint32_t v[3];
    for (int i = 0; i < 3; i++) { uint32_t n = names + 16 * (uint32_t)i; v[i] = fbits((float)ext_thiscall_f(c, get, from, 1, &n) * k); }
    ext_thiscall(c, FX("?RCSetSkillLevels@GoActor@@QAEXMMM@Z"), THIS, 3, v);
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
static void GoAspect_GetNumSubTextures(Ctx *c) { RET(1, 0); }
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
static void GoInventory_IsNonAggressivePack(Ctx *c) { RET(ext_thiscall(c, FX("?IsPackOnly@GoInventory@@QBE_NXZ"), THIS, 0, 0) & 0xff, 0); }
static void GoInventory_SSetDirtySetItem(Ctx *c) { RET(0, 2); }
static void GoInventory_TestGet1(Ctx *c) { uint32_t a = ARG(0); RET(ext_thiscall(c, FX("?TestGet@GoInventory@@QBE_NPBUGoid_@@@Z"), THIS, 1, &a) & 0xff, 1); }
static void GoInventory_TestGet2(Ctx *c) { uint32_t a[2] = {ARG(0), ARG(1)}; RET(ext_thiscall(c, FX("?TestGet@GoInventory@@QBE_NPBUGoid_@@_N@Z"), THIS, 2, a) & 0xff, 2); }
static void GoInventory_GetGridbox(Ctx *c) { RET(ext_thiscall(c, FX("?GetGridbox@GoInventory@@QBEPAVUIGridbox@@XZ"), THIS, 0, 0), 0); }

/* ---- singletons and the rest ---- */
static uint32_t singleton(uint32_t *g) { if (!*g) *g = heap_alloc(w32_process_heap, 8, 64); return *g; }
static uint32_t g_transform, g_overhead, g_empty_string;
static void TransformationManager_Singleton(Ctx *c) { RETC(singleton(&g_transform)); }
static void OverheadMap_Singleton(Ctx *c) { RETC(singleton(&g_overhead)); }
static void TransformationManager_STransformMe(Ctx *c) { RET(0, 3); }
static void TransformationManager_SUnTransformMe(Ctx *c) { RET(0, 1); }
static void TransformationManager_GetNewTemplateName(Ctx *c) { RET(singleton(&g_empty_string), 1); }   /* an empty string */
static void OverheadMap_RS2(Ctx *c) { RET(0, 2); }
static void OverheadMap_RS3(Ctx *c) { RET(0, 3); }
static void Player_GetParty(Ctx *c)
{
    uint32_t server = w32_callback(c, FX("?FUBI_GetClassSingleton@Server@@CAPAV1@XZ"), 0, 0);
    RET(server ? ext_thiscall(c, FX("?GetScreenParty@Server@@QAEPAVGo@@XZ"), server, 0, 0) : 0, 0);
}
static void TimeOfDay_SetRealMinutesModifier(Ctx *c) { side_set(THIS, S_REAL_MINUTES, ARG(0)); RET(0, 1); }
static void UIGame_SetGameInputBinderActive(Ctx *c) { RET(0, 1); }
static void WorldMap_GetUsingPlayerJournal(Ctx *c) { RET(0, 0); }
static void UIPartyManager_RedistributePotions(Ctx *c) { RET(0, 2); }
static void GoCommon_SToggleTriggeredEffects(Ctx *c) { RET(0, 2); }
static void GoDb_SRemoveEnchantments(Ctx *c) { RET(0, 3); }
static void Rules_RSSetNaturalSkillLevel(Ctx *c) { RET(0, 3); }

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
#define UISHELL (rt_r32(G_MEM, 0x7a065cu))
#define UI_FIND_WINDOW 0x6e06d3u      /* UIWindow* UIShell::FindUIWindow(const char* name, const char* interface) */
static int ui_wrapping;
static void ui_hide_window(Ctx *c, const char *name)
{
    uint32_t esp = c->esp, n = scratch(c, 64); snprintf((char *)GP(n), 64, "%s", name);
    uint32_t a[2] = {n, 0}, w = ext_thiscall(c, UI_FIND_WINDOW, UISHELL, 2, a);
    if (w && G_MEM[w + 0x108]) { uint32_t off = 0; ext_thiscall(c, rt_r32(G_MEM, rt_r32(G_MEM, w) + 0x48), w, 1, &off); }   /* SetVisible(false) */
    scratch_end(c, esp);
}
static void ui_hide_group(Ctx *c, const char *group)
{
    uint32_t esp = c->esp, n = scratch(c, 64); snprintf((char *)GP(n), 64, "%s", group);
    uint32_t a[4] = {n, 0, 0, 0}; ext_thiscall(c, 0x6dee75u, UISHELL, 4, a);
    scratch_end(c, esp);
}
static void ui_expansion_fixups(Ctx *c)
{
    char n[64];
    for (int i = 1; i <= 8; i++) {
        snprintf(n, sizeof n, "awp_transformed_portrait_%d", i); ui_hide_window(c, n);
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

int loa_override(Ctx *c, uint32_t addr)
{
    switch (addr) {
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
    ext_add("?RSSetCurrentBackground@OverheadMap@@QAEX" GPS "PBUGoid_@@@Z", OverheadMap_RS2);
    ext_add("?RSUpdateCurrentAreaMarker@OverheadMap@@QAEX" GPS "PBUGoid_@@@Z", OverheadMap_RS2);
    ext_add("?RSUpdateMapPieceVisibility@OverheadMap@@QAEX" GPS "_NPBUGoid_@@@Z", OverheadMap_RS3);
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
