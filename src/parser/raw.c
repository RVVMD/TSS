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
} LineBuf;

static LineBuf lb_new(Arena *a)
{
    LineBuf lb;
    lb.cap = 4096;
    lb.data = arena_alloc(a, lb.cap);
    lb.len = 0;
    return lb;
}

static int lb_getline(LineBuf *lb, FILE *fp)
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

static int parse_int(const char *s)   { return (int)strtol(s, NULL, 10); }
static double parse_dbl(const char *s) { return strtod(s, NULL); }

/* tokenize a comma-delimited line into up to nmax fields; returns count.
 * Handles quoted fields (single-quote only) and strips surrounding whitespace. */
static int tokenize(char *line, char **tokens, int nmax)
{
    int n = 0;
    char *p = line;
    while (*p && n < nmax) {
        /* skip leading whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        int quoted = 0;
        if (*p == '\'') {
            quoted = 1;
            p++; /* skip opening quote */
        }

        tokens[n++] = p;

        if (quoted) {
            while (*p && *p != '\'') p++;
            if (*p == '\'') { *p = '\0'; p++; }
        } else {
            /* find next comma or end */
            while (*p && *p != ',') p++;
        }
        if (*p == ',') { *p = '\0'; p++; }

        /* strip trailing whitespace from token */
        if (!quoted) {
            char *end = tokens[n-1] + strlen(tokens[n-1]) - 1;
            while (end >= tokens[n-1] && isspace((unsigned char)*end)) {
                *end = '\0'; end--;
            }
        }
    }
    return n;
}

/* detect section terminator: line that starts with "0" (possibly followed by comma or whitespace) */
static int is_term(const char *line)
{
    while (isspace((unsigned char)*line)) line++;
    if (line[0] != '0') return 0;
    line++;
    while (*line && (isspace((unsigned char)*line) || *line == ',')) line++;
    return *line == '\0' || *line == '/'; /* also treat '0 / ...' comments as terminator */
}

/* ================================================================
 * RAW parser
 * ================================================================ */
int raw_parse(const char *filename, System *sys, Arena *a)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) { log_error("cannot open %s", filename); return -1; }

    LineBuf lb = lb_new(a);
    char *tok[40];
    int nt;

    memset(sys, 0, sizeof(*sys));

    /* --- Line 1: IC, SBASE, REV, ... --- */
    if (lb_getline(&lb, fp) < 0) { fclose(fp); return -1; }
    nt = tokenize(lb.data, tok, 40);
    if (nt >= 2) sys->base_mva = parse_dbl(tok[1]);
    else sys->base_mva = 100.0;

    /* --- Line 2: comment --- */
    if (lb_getline(&lb, fp) < 0) { fclose(fp); return -1; }

    /* --- Line 3: comment --- */
    if (lb_getline(&lb, fp) < 0) { fclose(fp); return -1; }

    /* --- Bus data --- */
    while (lb_getline(&lb, fp) == 0) {
        if (is_term(lb.data)) break;
        nt = tokenize(lb.data, tok, 40);
        if (nt < 3) continue;

        Bus b;
        memset(&b, 0, sizeof(b));
        b.id    = parse_int(tok[0]);

        /* name may be quoted */
        char *name = tok[1];
        if (name[0] == '\'') { name++; name[strlen(name)-1] = '\0'; }
        strncpy(b.name, name, sizeof(b.name) - 1);
        b.name[sizeof(b.name) - 1] = '\0';

        if (nt >= 3) b.base_kv = parse_dbl(tok[2]);
        if (nt >= 4) {
            int typ = parse_int(tok[3]);
            if (typ == 1)      b.type = BUS_PQ;
            else if (typ == 2) b.type = BUS_PV;
            else if (typ == 3) b.type = BUS_SLACK;
            else               b.type = BUS_ISOLATED;
        }
        /* fields 4-6: area, zone, owner */
        if (nt >= 8) b.vm = parse_dbl(tok[7]);
        if (nt >= 9) b.va = parse_dbl(tok[8]) * M_PI / 180.0;
        if (nt >= 10) b.pd = parse_dbl(tok[9]) / sys->base_mva;
        if (nt >= 11) b.qd = parse_dbl(tok[10]) / sys->base_mva;
        if (nt >= 12) b.vmax = parse_dbl(tok[11]);
        if (nt >= 13) b.vmin = parse_dbl(tok[12]);

        arrpush(sys->bus, b);
    }
    sys->nbus = arrlen(sys->bus);

    /* --- Load data (optional) --- */
    while (lb_getline(&lb, fp) == 0) {
        if (is_term(lb.data)) break;
        nt = tokenize(lb.data, tok, 40);
        if (nt < 3) continue;
        int busid = parse_int(tok[0]);
        if (busid == 0) break;

        Load ld;
        memset(&ld, 0, sizeof(ld));
        ld.id  = parse_int(tok[1]);
        ld.bus = busid - 1;
        ld.p   = parse_dbl(tok[5]) / sys->base_mva;
        ld.q   = parse_dbl(tok[6]) / sys->base_mva;

        arrpush(sys->load, ld);
    }
    sys->nload = arrlen(sys->load);

    /* --- Generator data --- */
    while (lb_getline(&lb, fp) == 0) {
        if (is_term(lb.data)) break;
        nt = tokenize(lb.data, tok, 40);
        if (nt < 3) continue;
        int busid = parse_int(tok[0]);
        if (busid == 0) break;

        Gen g;
        memset(&g, 0, sizeof(g));
        g.id   = parse_int(tok[1]);
        g.bus  = busid - 1;
        if (nt >= 3) g.pg = parse_dbl(tok[2]) / sys->base_mva;
        if (nt >= 4) g.qg = parse_dbl(tok[3]) / sys->base_mva;
        if (nt >= 10) g.mbase = parse_dbl(tok[9]);
        /* if mbase not given, default to machine base */
        if (g.mbase <= 0) g.mbase = 100.0;
        if (nt >= 11) g.vsched = parse_dbl(tok[10]);
        g.machine_idx = -1;

        arrpush(sys->gen, g);
    }
    sys->ngen = arrlen(sys->gen);

    /* --- Branch data --- */
    while (lb_getline(&lb, fp) == 0) {
        if (is_term(lb.data)) break;
        nt = tokenize(lb.data, tok, 40);
        if (nt < 6) continue;
        int bus_from = parse_int(tok[0]);
        if (bus_from == 0) break;

        Branch br;
        memset(&br, 0, sizeof(br));
        br.tap = 1.0;   /* default: no transformer */
        br.from  = bus_from - 1;
        br.to    = parse_int(tok[1]) - 1;
        if (nt >= 3) {
            strncpy(br.ckt, tok[2], sizeof(br.ckt));
            br.ckt[sizeof(br.ckt) - 1] = '\0';
        }
        if (nt >= 4) br.r = parse_dbl(tok[3]);
        if (nt >= 5) br.x = parse_dbl(tok[4]);
        if (nt >= 6) br.b = parse_dbl(tok[5]);
        if (nt >= 7) br.rate_a = parse_dbl(tok[6]);
        if (nt >= 8) br.rate_b = parse_dbl(tok[7]);
        if (nt >= 9) br.rate_c = parse_dbl(tok[8]);
        /* transformer parameters at fields 12, 13 (0-based: 11, 12) */
        if (nt >= 12) {
            double ratio = parse_dbl(tok[11]);
            if (ratio > 0.0) br.tap = ratio;
        }
        if (nt >= 13) br.shift = parse_dbl(tok[12]);
        br.status = 1;

        arrpush(sys->branch, br);
    }
    sys->nbranch = arrlen(sys->branch);

    fclose(fp);

    log_info("RAW: %d buses, %d branches, %d gens, %d loads (Sbase=%.1f MVA)",
             sys->nbus, sys->nbranch, sys->ngen, sys->nload, sys->base_mva);
    return 0;
}

int raw_write(const char *filename, const System *sys)
{
    FILE *fp = fopen(filename, "w");
    if (!fp) { log_error("cannot open %s for writing", filename); return -1; }
    fprintf(fp, "0, %.2f\n", sys->base_mva);
    fprintf(fp, "TSS exported RAW file\n");
    fprintf(fp, "\n");
    for (int i = 0; i < sys->nbus; i++) {
        Bus *b = &sys->bus[i];
        int typ = 1;
        switch (b->type) {
            case BUS_PQ:      typ = 1; break;
            case BUS_PV:      typ = 2; break;
            case BUS_SLACK:   typ = 3; break;
            default:          typ = 4; break;
        }
        fprintf(fp, "%d, '%s', %.4f, %d, 1, 1, 1, %.6f, %.4f, %.6f, %.6f, %.6f, %.4f\n",
            b->id, b->name, b->base_kv, typ,
            b->vm, b->va * 180.0 / M_PI,
            b->pd * sys->base_mva, b->qd * sys->base_mva,
            b->vmax, b->vmin);
    }
    fprintf(fp, "0\n");
    for (int i = 0; i < sys->nload; i++) {
        Load *ld = &sys->load[i];
        int bus_id = sys->bus[ld->bus].id;
        fprintf(fp, "%d, %d, 1, 1, 1, %.6f, %.6f\n",
            bus_id, ld->id, ld->p * sys->base_mva, ld->q * sys->base_mva);
    }
    fprintf(fp, "0\n");
    for (int i = 0; i < sys->ngen; i++) {
        Gen *g = &sys->gen[i];
        int bus_id = sys->bus[g->bus].id;
        double mbase = g->mbase > 0.0 ? g->mbase : 100.0;
        double vsched = g->vsched > 0.0 ? g->vsched : sys->bus[g->bus].vm;
        fprintf(fp, "%d, %d, %.6f, %.6f, 0.0, 0.0, %.4f, 1, %.2f, %.4f\n",
            bus_id, g->id, g->pg * sys->base_mva, g->qg * sys->base_mva,
            vsched, mbase, vsched);
    }
    fprintf(fp, "0\n");
    for (int i = 0; i < sys->nbranch; i++) {
        Branch *br = &sys->branch[i];
        int from_id = sys->bus[br->from].id;
        int to_id = sys->bus[br->to].id;
        fprintf(fp, "%d, %d, '%s', %.6f, %.6f, %.6f, %.2f, %.2f, %.2f, 0.0, 0.0, %.6f, %.2f\n",
            from_id, to_id, br->ckt, br->r, br->x, br->b,
            br->rate_a, br->rate_b, br->rate_c,
            br->tap > 0.0 ? br->tap : 1.0, br->shift);
    }
    fprintf(fp, "0\n0\n");
    fclose(fp);
    log_info("RAW: saved to %s (%d buses, %d branches, %d gens, %d loads)",
             filename, sys->nbus, sys->nbranch, sys->ngen, sys->nload);
    return 0;
}
