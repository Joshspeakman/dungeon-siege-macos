/* Legends of Aranna on the base engine: the engine functions the expansion's scripts call, implemented for the GOG
 * 1.11.1 engine (whose executable the build verifies by checksum, so its internal addresses below are fixed).
 * Registered by loa_register() when the expansion's data is active (DS_EXPANSION); see ext.c for how they reach FuBi.
 * Names are the C++ decorated names the scripts' compiler binds to; the signatures follow the calls in the
 * expansion's scripts. */
#include "ext.h"

/* ---- base engine internals (GOG 1.11.1) ---- */
#define GO_SET_MODIFIERS_DIRTY  0x5af628u    /* void Go::SetModifiersDirty(bool) */
#define COMP_GO(comp)           rt_r32(G_MEM, (comp) + 4)      /* GoComponent::m_Go */
#define GO_GOID(go)             rt_r32(G_MEM, (go) + 0x4c)

static void modifiers_dirty(Ctx *c, uint32_t go) { uint32_t a = 1; if (go) ext_thiscall(c, GO_SET_MODIFIERS_DIRTY, go, 1, &a); }

/* ---- Math ---- */
static void Math_LShift(Ctx *c) { RETC((uint32_t)((int32_t)ARG(0) << (ARG(1) & 31))); }
static void Math_RShift(Ctx *c) { RETC((uint32_t)((int32_t)ARG(0) >> (ARG(1) & 31))); }

/* ---- GoAspect: life recovery (the base engine keeps natural/current pairs: unit +0x74/+0x78, period +0x7c/+0x80) ---- */
static void GoAspect_SetNaturalLifeRecoveryUnit(Ctx *c)
{
    rt_wf32(G_MEM, THIS + 0x74, ARGF(0)); modifiers_dirty(c, COMP_GO(THIS)); RET(0, 1);
}
static void GoAspect_SetLifeRecoveryUnit(Ctx *c) { rt_wf32(G_MEM, THIS + 0x78, ARGF(0)); RET(0, 1); }

void loa_register(void)
{
    ext_add("?LShift@Math@@YAHHH@Z", Math_LShift);
    ext_add("?RShift@Math@@YAHHH@Z", Math_RShift);
    ext_add("?SetNaturalLifeRecoveryUnit@GoAspect@@QAEXM@Z", GoAspect_SetNaturalLifeRecoveryUnit);
    ext_add("?SetLifeRecoveryUnit@GoAspect@@QAEXM@Z", GoAspect_SetLifeRecoveryUnit);
}
