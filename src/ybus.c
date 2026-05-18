#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

typedef struct { int r, c; double re, im; } Trip;

int ybus_build(System *sys, Arena *a)
{
    (void)a;
    int n = sys->nbus;

    Trip *trips = NULL;

    for (int i = 0; i < n; i++) {
        Trip t = {i, i, sys->bus[i].gs, sys->bus[i].bs};
        arrpush(trips, t);
    }

    /* add fault shunt if active */
    if (sys->fault_bus >= 0) {
        Trip ft = {sys->fault_bus, sys->fault_bus, sys->fault_Y_r, sys->fault_Y_i};
        arrpush(trips, ft);
    }

    for (int b = 0; b < sys->nbranch; b++) {
        Branch *br = &sys->branch[b];
        if (!br->status) continue;
        int f = br->from, t = br->to;
        if (f >= n || t >= n) continue;

        double r = br->r, x = br->x;
        double denom = r * r + x * x;
        double g, bval;
        if (denom < 1e-20) { g = 1e8; bval = 0.0; }
        else                { g = r/denom; bval = -x/denom; }
        double bc = br->b;

        if (fabs(br->tap - 1.0) > 1e-6 || fabs(br->shift) > 1e-6) {
            double tap = br->tap, phi = br->shift * M_PI / 180.0;
            double tr = tap * cos(phi), ti = tap * sin(phi);
            double t2 = tr*tr + ti*ti;

            /* tap on FROM side: series + shunt both divided by tap^2 */
            double yf_re =  g/t2;
            double yf_im = (bval + bc/2.0) / t2;
            Trip tf = {f, f, yf_re, yf_im};
            arrpush(trips, tf);

            if (f != t) {
                Trip tt = {t, t, g, bval + bc/2.0};
                arrpush(trips, tt);
                double yft_r = -(g*tr - bval*ti) / t2;
                double yft_i = -(g*ti + bval*tr) / t2;
                Trip tft = {t, f, yft_r, yft_i};
                arrpush(trips, tft);
                double ytf_r = -(g*tr + bval*ti) / t2;
                double ytf_i = -(bval*tr - g*ti) / t2;
                Trip ttf = {f, t, ytf_r, ytf_i};
                arrpush(trips, ttf);
            }
        } else {
            Trip tf = {f, f, g, bval + bc/2.0};
            arrpush(trips, tf);
            if (f != t) {
                Trip tt = {t, t, g, bval + bc/2.0};
                arrpush(trips, tt);
                Trip tft = {t, f, -g, -bval};
                arrpush(trips, tft);
                Trip ttf = {f, t, -g, -bval};
                arrpush(trips, ttf);
            }
        }
    }

    int ntrips = arrlen(trips);

    for (int i = 0; i < ntrips; i++) {
        for (int j = i + 1; j < ntrips; j++) {
            if (trips[j].c < trips[i].c ||
                (trips[j].c == trips[i].c && trips[j].r < trips[i].r)) {
                Trip tmp = trips[i]; trips[i] = trips[j]; trips[j] = tmp;
            }
        }
    }

    int nunique = 0;
    for (int i = 0; i < ntrips; i++) {
        if (nunique > 0 && trips[i].r == trips[nunique-1].r && trips[i].c == trips[nunique-1].c) {
            trips[nunique-1].re += trips[i].re;
            trips[nunique-1].im += trips[i].im;
        } else {
            trips[nunique] = trips[i];
            nunique++;
        }
    }

    /* free old arrays if they exist */
    free(sys->colptr); free(sys->rowidx); free(sys->yval);

    int *colptr = calloc(n + 1, sizeof(int));
    int *rowidx = calloc(nunique, sizeof(int));
    double *yval = calloc(2 * nunique, sizeof(double));

    memset(colptr, 0, (n + 1) * sizeof(int));
    for (int i = 0; i < nunique; i++) colptr[trips[i].c + 1]++;
    for (int i = 1; i <= n; i++) colptr[i] += colptr[i - 1];

    int *colfill = calloc(n, sizeof(int));
    memset(colfill, 0, n * sizeof(int));
    for (int i = 0; i < nunique; i++) {
        int c = trips[i].c, off = colptr[c] + colfill[c];
        rowidx[off] = trips[i].r;
        yval[2*off] = trips[i].re;
        yval[2*off+1] = trips[i].im;
        colfill[c]++;
    }

    free(colfill);

    sys->colptr = colptr;
    sys->rowidx = rowidx;
    sys->yval   = yval;
    sys->nnz    = nunique;

    arrfree(trips);
    log_info("YBUS: %d x %d, %d non-zeros", n, n, nunique);
    return 0;
}
