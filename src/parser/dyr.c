#include "types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} LineBuf2;

static LineBuf2 lb2_new(Arena *a)
{
    LineBuf2 lb;
    lb.cap = 4096;
    lb.data = arena_alloc(a, lb.cap);
    lb.len = 0;
    return lb;
}

static int lb2_getline(LineBuf2 *lb, FILE *fp)
{
    lb->len = 0;
    int c;
    while ((c = fgetc(fp)) != EOF && c != '\n') {
        if (lb->len + 1 >= lb->cap) return -1;
        lb->data[lb->len++] = (char)c;
    }
    lb->data[lb->len] = '\0';
    while (lb->len > 0 && (lb->data[lb->len-1] == '\r' || lb->data[lb->len-1] == '\n'))
        lb->data[--lb->len] = '\0';
    return (lb->len > 0 || c == '\n') ? 0 : -1;
}

static double parse_dbl(const char *s) { return strtod(s, NULL); }

static int tokenize_dyr(char *line, char **tokens, int nmax)
{
    int n = 0;
    char *p = line;
    while (*p && n < nmax) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        int quoted = 0;
        if (*p == '\'') { quoted = 1; p++; }

        tokens[n++] = p;

        if (quoted) {
            while (*p && *p != '\'') p++;
            if (*p == '\'') { *p = '\0'; p++; }
        } else {
            while (*p && *p != ',') p++;
        }
        if (*p == ',') { *p = '\0'; p++; }

        if (!quoted) {
            char *end = tokens[n-1] + strlen(tokens[n-1]) - 1;
            while (end >= tokens[n-1] && isspace((unsigned char)*end)) {
                *end = '\0'; end--;
            }
        }
    }
    return n;
}

int dyr_parse(const char *filename, System *sys, Arena *a)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) { log_error("cannot open %s", filename); return -1; }

    LineBuf2 lb = lb2_new(a);
    char *tok[20];
    int nt;
    int nmachines = 0;

    while (lb2_getline(&lb, fp) == 0) {
        if (lb.data[0] == '\0') continue;
        char *trimmed = lb.data;
        while (isspace((unsigned char)*trimmed)) trimmed++;
        if (trimmed[0] == '\0') continue;

        nt = tokenize_dyr(trimmed, tok, 20);
        if (nt < 4) continue;

        int bus_id = (int)strtol(tok[0], NULL, 10);

        for (int i = 0; i < sys->ngen; i++) {
            if (sys->bus[sys->gen[i].bus].id == bus_id) {
                Machine m;
                memset(&m, 0, sizeof(m));
                m.gen_idx = i;
                if (nt >= 4) m.h   = parse_dbl(tok[3]);   /* H */
                if (nt >= 5) m.d   = parse_dbl(tok[4]);   /* DAMP */
                if (nt >= 6) m.xdp = parse_dbl(tok[5]);   /* X'd */

                arrpush(sys->machine, m);
                sys->gen[i].machine_idx = nmachines;
                nmachines++;
                break;
            }
        }
    }

    sys->nmachines = nmachines;
    fclose(fp);

    if (nmachines == 0) {
        log_warn("DYR: no machines found, using defaults");
        /* fallback: assign GENCLS to all generators with defaults */
        for (int i = 0; i < sys->ngen; i++) {
            Machine m;
            memset(&m, 0, sizeof(m));
            m.gen_idx = i;
            m.h   = 5.0;
            m.d   = 0.0;
            m.xdp = 0.1;
            arrpush(sys->machine, m);
            sys->gen[i].machine_idx = i;
        }
        sys->nmachines = sys->ngen;
    }

    log_info("DYR: %d machines", sys->nmachines);
    return 0;
}

int dyr_write(const char *filename, const System *sys)
{
    FILE *fp = fopen(filename, "w");
    if (!fp) { log_error("cannot open %s for writing", filename); return -1; }
    for (int i = 0; i < sys->nmachines; i++) {
        Machine *m = &sys->machine[i];
        int gen_idx = m->gen_idx;
        int bus_id = sys->bus[sys->gen[gen_idx].bus].id;
        int gen_id = sys->gen[gen_idx].id;
        fprintf(fp, "%d, %d, GENCLS, %.4f, %.4f, %.4f\n",
            bus_id, gen_id, m->h, m->d, m->xdp);
    }
    fclose(fp);
    log_info("DYR: saved to %s (%d machines)", filename, sys->nmachines);
    return 0;
}
