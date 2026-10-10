/* Deleting colliding entries must not replace a still-live recursive mutex on the next lookup. */
#ifdef OLD_CS
#include OLD_CS
static void cs_delete(uint32_t a) { CS *s = cs_find(a, 0); if (s) { pthread_mutex_destroy(&s->m); s->addr = 1; } }
#else
#include "../runtime/win32/critical_sections.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <sched.h>

static void *contend(void *p)
{
    CS *s = cs_find(0x2000, 1); assert(s == p);
    assert(pthread_mutex_trylock(&s->m) != 0); return 0;
}
static void *churn(void *p)
{
    uint32_t a = 0x500000 + (uint32_t)(uintptr_t)p * 4096;
    for (int i = 0; i < 1000; i++) {
        CS *s = cs_find(a, 1); assert(s);
        assert(!pthread_mutex_lock(&s->m)); sched_yield();
        assert(cs_find(a, 0) == s);
        assert(!pthread_mutex_unlock(&s->m)); cs_delete(a);
    }
    return 0;
}
int main(void)
{
    CS *a = cs_find(0x1000, 1), *b = cs_find(0x2000, 1); assert(a && b && a != b);
    assert(!pthread_mutex_lock(&b->m)); assert(!pthread_mutex_lock(&b->m));
    cs_delete(0x1000);
    assert(cs_find(0x2000, 0) == b); // Old lookup stopped at the preceding tombstone.
    assert(cs_find(0x2000, 1) == b); // Old creation replaced the live mutex with a different one.
    pthread_t t; assert(!pthread_create(&t, 0, contend, b)); pthread_join(t, 0);
    CS *c = cs_find(0x3000, 1); assert(c == a);
    assert(!pthread_mutex_unlock(&b->m)); assert(!pthread_mutex_unlock(&b->m));
    cs_delete(0x2000); cs_delete(0x3000);
    for (uint32_t k = 1; k <= NCS; k++) assert(cs_find(k * 4096, 1));
    assert(!cs_find((NCS + 1) * 4096, 1));
    cs_delete(0x1000);
    for (uint32_t k = 2; k <= NCS; k++) assert(cs_find(k * 4096, 0));
    assert(cs_find((NCS + 1) * 4096, 1));
    for (uint32_t k = 2; k <= NCS + 1; k++) cs_delete(k * 4096);
    pthread_t workers[4];
    for (uintptr_t k = 0; k < 4; k++) assert(!pthread_create(&workers[k], 0, churn, (void *)k));
    for (int k = 0; k < 4; k++) pthread_join(workers[k], 0);
    puts("Critical sections: collisions, recursive ownership, full table and concurrent reuse passed");
    return 0;
}
