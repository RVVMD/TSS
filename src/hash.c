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
    if (!m) return NULL;
    m->cap = HM_INIT_CAP;
    m->len = 0;
    m->buckets = calloc(m->cap, sizeof(HMEntry));
    if (!m->buckets) { free(m); return NULL; }
    return m;
}

void hashmap_free(HashMap *m)
{
    if (!m) return;
    free(m->buckets);
    free(m);
}

static void hm_resize(HashMap *m, size_t newcap);

/* find slot for key, or return first empty/tombstone slot */
static HMEntry *hm_find_slot(HMEntry *buckets, size_t cap, int key, int for_insert)
{
    unsigned int h = hash_int(key);
    size_t idx = h & (cap - 1);
    HMEntry *tombstone = NULL;
    for (size_t i = 0; i < cap; i++) {
        HMEntry *e = &buckets[(idx + i) & (cap - 1)];
        if (!e->occupied) {
            /* empty slot — nothing further in chain */
            if (for_insert && tombstone) return tombstone;
            return e;
        }
        if (e->tombstone) {
            if (!tombstone) tombstone = e;
            continue;
        }
        if (e->key == key) return e;
        /* collided with different key, keep probing */
    }
    /* table full (shouldn't happen with load factor) — return first tombstone or last slot */
    return tombstone ? tombstone : &buckets[idx];
}

void *hashmap_get(HashMap *m, int key)
{
    if (!m || !m->buckets) return NULL;
    HMEntry *e = hm_find_slot(m->buckets, m->cap, key, 0);
    if (e->occupied && !e->tombstone && e->key == key)
        return e->val;
    return NULL;
}

void hashmap_put(HashMap *m, int key, void *val)
{
    if (!m) return;
    if ((double)m->len / m->cap > HM_LOAD)
        hm_resize(m, m->cap * 2);
    HMEntry *e = hm_find_slot(m->buckets, m->cap, key, 1);
    if (!e->occupied) {
        m->len++;
    } else if (e->tombstone) {
        /* reusing tombstone slot */
        m->len++;
    }
    e->key = key;
    e->val = val;
    e->occupied = 1;
    e->tombstone = 0;
}

void hashmap_remove(HashMap *m, int key)
{
    if (!m || !m->buckets) return;
    HMEntry *e = hm_find_slot(m->buckets, m->cap, key, 0);
    if (e->occupied && !e->tombstone && e->key == key) {
        e->tombstone = 1;
        m->len--;
    }
}

static void hm_resize(HashMap *m, size_t newcap)
{
    HMEntry *old = m->buckets;
    size_t oldcap = m->cap;
    m->buckets = calloc(newcap, sizeof(HMEntry));
    if (!m->buckets) {
        /* allocation failed — restore old buckets */
        m->buckets = old;
        return;
    }
    m->cap = newcap;
    m->len = 0;
    for (size_t i = 0; i < oldcap; i++) {
        if (old[i].occupied && !old[i].tombstone) {
            HMEntry *dst = hm_find_slot(m->buckets, newcap, old[i].key, 1);
            *dst = old[i];
            dst->tombstone = 0;
            m->len++;
        }
    }
    free(old);
}
