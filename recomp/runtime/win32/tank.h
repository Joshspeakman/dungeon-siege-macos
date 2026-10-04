/* Native access to Dungeon Siege Tank archives and .gas text (runtime/win32/tank.c). */
#ifndef TANK_H
#define TANK_H
#include <stdint.h>
#include <stddef.h>
int tank_read(const char *archive, const char *path, uint8_t **out, size_t *len);   /* malloc'd; 0 = found */

#define GAS_MAX_KEYS 32
#define GAS_MAX_CHILD 512
typedef struct GasBlock {
    char name[64]; int nkey, nchild;
    char key[GAS_MAX_KEYS][48]; char val[GAS_MAX_KEYS][128];
    struct GasBlock *child[GAS_MAX_CHILD];
} GasBlock;
GasBlock *gas_parse(const char *text);
void gas_free(GasBlock *b);
GasBlock *gas_child(GasBlock *b, const char *name);
const char *gas_get(GasBlock *b, const char *key, const char *dflt);
#endif
