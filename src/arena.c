#include "types.h"
#include <stdlib.h>
#include <string.h>

Arena arena_new(size_t cap)
{
    Arena a;
    a.mem = malloc(cap);
    a.cap = cap;
    a.off = 0;
    return a;
}

void *arena_alloc(Arena *a, size_t n)
{
    n = (n + 7) & ~7UL;
    if (a->off + n > a->cap) return NULL;
    void *p = a->mem + a->off;
    a->off += n;
    return p;
}

void arena_free(Arena *a)
{
    free(a->mem);
    a->mem = NULL;
    a->cap = 0;
    a->off = 0;
}
