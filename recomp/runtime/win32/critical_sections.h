/* Host mutexes keyed by guest critical-section addresses. A deleted entry is a tombstone, not the end of a
 * collision chain. Keep looking for an existing address before reusing that entry. Guest callers must still
 * obey critical-section lifetime rules; this table does not make concurrent deletion of an owned lock valid. */
#ifndef W32_CRITICAL_SECTIONS_H
#define W32_CRITICAL_SECTIONS_H
#include <stdint.h>
#include <pthread.h>
typedef struct CS { uint32_t addr; pthread_mutex_t m; } CS;
#define NCS 4096
static CS cstab[NCS]; static pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;
static CS *cs_find_locked(uint32_t a, int create)
{
    if (a <= 1) return 0;
    uint32_t k = (a * 2654435761u) % NCS; CS *free_slot = 0;
    for (int n = 0; n < NCS; n++, k = (k + 1) % NCS) {
        CS *s = &cstab[k];
        if (s->addr == a) return s;
        if (s->addr <= 1 && !free_slot) free_slot = s;
        if (!s->addr) break;
    }
    if (!create || !free_slot) return 0;
    pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    int err = pthread_mutex_init(&free_slot->m, &at); pthread_mutexattr_destroy(&at);
    if (err) return 0;
    free_slot->addr = a; return free_slot;
}
static CS *cs_find(uint32_t a, int create)
{
    pthread_mutex_lock(&cs_lock);
    CS *s = cs_find_locked(a, create);
    pthread_mutex_unlock(&cs_lock); return s;
}
static void cs_delete(uint32_t a)
{
    pthread_mutex_lock(&cs_lock);
    CS *s = cs_find_locked(a, 0);
    if (s && !pthread_mutex_destroy(&s->m)) s->addr = 1;
    pthread_mutex_unlock(&cs_lock);
}
#endif
