#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void *_arrgrow(void *arr, size_t elem_sz)
{
    size_t *header;
    size_t cap, newcap;

    if (!arr) {
        cap = 0;
    } else {
        header = (size_t *)arr - 2;
        cap = header[1];
    }

    newcap = cap ? cap * 2 : 8;
    if (newcap > SIZE_MAX / elem_sz) return NULL;

    size_t alloc_sz = 2 * sizeof(size_t) + newcap * elem_sz;
    if (alloc_sz < 2 * sizeof(size_t)) return NULL; /* overflow */

    if (!arr) {
        header = malloc(alloc_sz);
        if (!header) return NULL;
        header[0] = 0;
    } else {
        header = realloc((size_t *)arr - 2, alloc_sz);
        if (!header) return NULL;
    }
    header[1] = newcap;
    header[0] += 1;
    return header + 2;
}
