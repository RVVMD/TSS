#include <stdlib.h>
#include <string.h>

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

    if (!arr) {
        header = malloc(2 * sizeof(size_t) + newcap * elem_sz);
        header[0] = 0;
    } else {
        header = realloc((size_t *)arr - 2, 2 * sizeof(size_t) + newcap * elem_sz);
    }
    header[1] = newcap;
    header[0] += 1; /* increments length */
    return header + 2;
}
