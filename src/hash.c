#include "types.h"
#include <stdlib.h>
#include <string.h>

#define HM_INIT_CAP 16
#define HM_LOAD    0.75

static unsigned int hash_int(int key)
{
    unsigned int x = (unsigned int)key;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x;
}

HashMap *hashmap_new(void)
{
    HashMap *m = malloc(sizeof(HashMap));
    m->cap = HM_INIT_CAP;
    m->len = 0;
    m->buckets = calloc(m->cap, sizeof(HMEntry));
    return m;
}

void hashmap_free(HashMap *m)
{
    if (!m) return;
    free(m->buckets);
    free(m);
}

static HMEntry *hm_find(HMEntry *buckets, size_t cap, int key)
{
    unsigned int h = hash_int(key);
    size_t idx = h & (cap - 1);
    while (buckets[idx].occupied) {
        if (buckets[idx].key == key) return &buckets[idx];
        idx = (idx + 1) & (cap - 1);
    }
    return &buckets[idx];
}

static void hm_resize(HashMap *m, size_t newcap)
{
    HMEntry *old = m->buckets;
    size_t oldcap = m->cap;
    m->buckets = calloc(newcap, sizeof(HMEntry));
    m->cap = newcap;
    for (size_t i = 0; i < oldcap; i++) {
        if (old[i].occupied) {
            HMEntry *dst = hm_find(m->buckets, newcap, old[i].key);
            *dst = old[i];
        }
    }
    free(old);
}

void *hashmap_get(HashMap *m, int key)
{
    HMEntry *e = hm_find(m->buckets, m->cap, key);
    return e->occupied ? e->val : NULL;
}

void hashmap_put(HashMap *m, int key, void *val)
{
    if ((double)m->len / m->cap > HM_LOAD)
        hm_resize(m, m->cap * 2);
    HMEntry *e = hm_find(m->buckets, m->cap, key);
    if (!e->occupied) m->len++;
    e->key = key;
    e->val = val;
    e->occupied = 1;
}

void hashmap_remove(HashMap *m, int key)
{
    HMEntry *e = hm_find(m->buckets, m->cap, key);
    if (e->occupied) {
        e->occupied = 0;
        m->len--;
    }
}
