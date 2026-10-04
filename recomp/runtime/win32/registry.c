/* ADVAPI32 registry: an in-memory tree (empty at start; the GOG game keeps its settings in INI files). */
#include "w32.h"

typedef struct RVal { char name[128]; uint32_t type, size; uint8_t *data; } RVal;
typedef struct RKey { char path[512]; RVal *v; int nv; } RKey;
static RKey *keys[512]; static int nkeys;
static pthread_mutex_t rlock = PTHREAD_MUTEX_INITIALIZER;
typedef struct RHandle { RKey *k; } RHandle;

static const char *root_name(uint32_t h)
{
    switch (h) { case 0x80000000u: return "hkcr"; case 0x80000001u: return "hkcu"; case 0x80000002u: return "hklm"; case 0x80000003u: return "hku"; }
    return 0;
}
static RKey *key_of(uint32_t h);
static int full_path(uint32_t parent, const char *sub, char *out, size_t cap)
{
    const char *r = root_name(parent); RKey *pk = r ? 0 : key_of(parent);
    if (!r && !pk) return -1;
    snprintf(out, cap, "%s%s%s", r ? r : pk->path, sub && *sub ? "\\" : "", sub ? sub : "");
    for (char *s = out; *s; s++) { if (*s >= 'A' && *s <= 'Z') *s += 32; if (*s == '/') *s = '\\'; }
    size_t l = strlen(out); while (l && out[l - 1] == '\\') out[--l] = 0;
    return 0;
}
static RKey *lookup(const char *path, int create)
{
    for (int k = 0; k < nkeys; k++) if (!strcmp(keys[k]->path, path)) return keys[k];
    if (!create || nkeys == 512) return 0;
    RKey *k = calloc(1, sizeof *k); snprintf(k->path, sizeof k->path, "%s", path); keys[nkeys++] = k;
    return k;
}
#define HREG_BASE 0x00a00000u
static RKey *rh[256];
static RKey *key_of(uint32_t h) { uint32_t i = (h - HREG_BASE) / 4; return (h >= HREG_BASE && i < 256) ? rh[i] : 0; }
static uint32_t new_handle(RKey *k) { for (int i = 1; i < 256; i++) if (!rh[i]) { rh[i] = k; return HREG_BASE + 4 * (uint32_t)i; } return 0; }

IMPL(advapi32, RegOpenKeyExA)
{
    char p[1024]; uint32_t out = ARG(4);
    pthread_mutex_lock(&rlock);
    RKey *k = full_path(ARG(0), ARG(1) ? GS(ARG(1)) : "", p, sizeof p) ? 0 : lookup(p, 0);
    uint32_t h = k ? new_handle(k) : 0;
    pthread_mutex_unlock(&rlock);
    if (!k) RET(2, 5);                                          /* ERROR_FILE_NOT_FOUND */
    rt_w32(G_MEM, out, h); RET(0, 5);
}
IMPL(advapi32, RegCreateKeyExA)
{
    char p[1024]; uint32_t out = ARG(7), disp = ARG(8);
    pthread_mutex_lock(&rlock);
    int existed = 0; RKey *k = 0;
    if (!full_path(ARG(0), GS(ARG(1)), p, sizeof p)) { existed = lookup(p, 0) != 0; k = lookup(p, 1); }
    uint32_t h = k ? new_handle(k) : 0;
    pthread_mutex_unlock(&rlock);
    if (!k) RET(2, 9);
    rt_w32(G_MEM, out, h); if (disp) rt_w32(G_MEM, disp, existed ? 2 : 1);
    RET(0, 9);
}
IMPL(advapi32, RegCloseKey) { uint32_t i = (ARG(0) - HREG_BASE) / 4; if (ARG(0) >= HREG_BASE && i < 256) rh[i] = 0; RET(0, 1); }
IMPL(advapi32, RegQueryValueExA)
{
    uint32_t name = ARG(1), type = ARG(3), data = ARG(4), szp = ARG(5);
    RKey *k = key_of(ARG(0)); if (!k) RET(6, 6);
    const char *n = name ? GS(name) : "";
    for (int j = 0; j < k->nv; j++) if (!strcasecmp(k->v[j].name, n)) {
        RVal *v = &k->v[j];
        if (type) rt_w32(G_MEM, type, v->type);
        if (data) {
            if (!szp || rt_r32(G_MEM, szp) < v->size) { if (szp) rt_w32(G_MEM, szp, v->size); RET(234, 6); }   /* ERROR_MORE_DATA */
            memcpy(GP(data), v->data, v->size);
        }
        if (szp) rt_w32(G_MEM, szp, v->size);
        RET(0, 6);
    }
    RET(2, 6);
}
IMPL(advapi32, RegSetValueExA)
{
    uint32_t name = ARG(1), type = ARG(3), data = ARG(4), size = ARG(5);
    RKey *k = key_of(ARG(0)); if (!k) RET(6, 6);
    const char *n = name ? GS(name) : "";
    pthread_mutex_lock(&rlock);
    RVal *v = 0;
    for (int j = 0; j < k->nv; j++) if (!strcasecmp(k->v[j].name, n)) v = &k->v[j];
    if (!v) { k->v = realloc(k->v, (size_t)(k->nv + 1) * sizeof *k->v); v = &k->v[k->nv++]; memset(v, 0, sizeof *v); snprintf(v->name, sizeof v->name, "%s", n); }
    free(v->data); v->type = type; v->size = size; v->data = malloc(size ? size : 1); memcpy(v->data, GP(data), size);
    pthread_mutex_unlock(&rlock);
    RET(0, 6);
}
