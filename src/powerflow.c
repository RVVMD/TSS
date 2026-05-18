#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <lapacke.h>

static double calc_P(int i, System *sys)
{
    double Vi = sys->bus[i].vm, ti = sys->bus[i].va, P = 0.0;
    for (int k = sys->colptr[i]; k < sys->colptr[i+1]; k++) {
        int j = sys->rowidx[k];
        double d = ti - sys->bus[j].va;
        P += Vi * sys->bus[j].vm * (sys->yval[2*k]*cos(d) + sys->yval[2*k+1]*sin(d));
    }
    return P;
}

static double calc_Q(int i, System *sys)
{
    double Vi = sys->bus[i].vm, ti = sys->bus[i].va, Q = 0.0;
    for (int k = sys->colptr[i]; k < sys->colptr[i+1]; k++) {
        int j = sys->rowidx[k];
        double d = ti - sys->bus[j].va;
        Q += Vi * sys->bus[j].vm * (sys->yval[2*k]*sin(d) - sys->yval[2*k+1]*cos(d));
    }
    return Q;
}

int powerflow_solve(System *sys, Arena *a)
{
    int n = sys->nbus, npv = 0, npq = 0, slack = -1;

    int *pq_idx = arena_alloc(a, n*sizeof(int));
    int *pv_idx = arena_alloc(a, n*sizeof(int));
    for (int i = 0; i < n; i++) {
        pq_idx[i] = pv_idx[i] = -1;
        if      (sys->bus[i].type == BUS_SLACK) slack = i;
        else if (sys->bus[i].type == BUS_PV)    pv_idx[i] = npv++;
        else                                     pq_idx[i] = npq++;
    }
    if (slack < 0) { log_error("no slack bus"); return -1; }

    int nu = npv + 2*npq;
    int *uq_bus = arena_alloc(a, nu*sizeof(int));
    int *uq_isV = arena_alloc(a, nu*sizeof(int));
    int kk = 0;
    for (int i = 0; i < n; i++) {
        if (i == slack) continue;
        if (sys->bus[i].type == BUS_PV) {
            uq_bus[kk] = i; uq_isV[kk] = 0; kk++;
        } else {
            uq_bus[kk] = i; uq_isV[kk] = 0; kk++;
            uq_bus[kk] = i; uq_isV[kk] = 1; kk++;
        }
    }

    double *J = arena_alloc(a, nu*nu*sizeof(double));
    double *F = arena_alloc(a, nu*sizeof(double));
    double *x = arena_alloc(a, nu*sizeof(double));

    double *Pc = arena_alloc(a, n*sizeof(double));
    double *Qc = arena_alloc(a, n*sizeof(double));

    int max_iter = 20, converged = 0;

    for (int iter = 0; iter < max_iter; iter++) {
        for (int i = 0; i < n; i++) { Pc[i] = calc_P(i, sys); Qc[i] = calc_Q(i, sys); }

        /* mismatch */
        for (int u = 0; u < nu; u++) {
            int i = uq_bus[u];
            double Ps = 0, Qs = 0;
            for (int g = 0; g < sys->ngen; g++)
                if (sys->gen[g].bus == i) { Ps += sys->gen[g].pg; Qs += sys->gen[g].qg; }
            F[u] = uq_isV[u] ? Qs - sys->bus[i].qd - Qc[i]
                              : Ps - sys->bus[i].pd - Pc[i];
        }

        double max_F = 0;
        for (int u = 0; u < nu; u++) if (fabs(F[u]) > max_F) max_F = fabs(F[u]);

        if (max_F < 1e-8) { converged = 1; break; }

        /* finite-difference Jacobian */
        double eps = 1e-7;
        memset(J, 0, nu*nu*sizeof(double));
        for (int v = 0; v < nu; v++) {
            double save;
            int i = uq_bus[v];
            if (uq_isV[v]) {
                save = sys->bus[i].vm;
                sys->bus[i].vm += eps;
            } else {
                save = sys->bus[i].va;
                sys->bus[i].va += eps;
            }
            for (int ii = 0; ii < n; ii++) { Pc[ii] = calc_P(ii, sys); Qc[ii] = calc_Q(ii, sys); }
            for (int u = 0; u < nu; u++) {
                int j = uq_bus[u];
                double Ps = 0, Qs = 0;
                for (int g = 0; g < sys->ngen; g++)
                    if (sys->gen[g].bus == j) { Ps += sys->gen[g].pg; Qs += sys->gen[g].qg; }
                double Fp = uq_isV[u] ? Qs - sys->bus[j].qd - Qc[j]
                                       : Ps - sys->bus[j].pd - Pc[j];
                J[u + v*nu] = (Fp - F[u]) / eps;  /* column-major */
            }
            if (uq_isV[v]) sys->bus[i].vm = save;
            else           sys->bus[i].va = save;
        }

        /* solve J * dx = -F via LAPACKE */
        for (int u = 0; u < nu; u++) x[u] = -F[u];
        int *ipiv = arena_alloc(a, nu*sizeof(int));
        int info = LAPACKE_dgesv(LAPACK_COL_MAJOR, nu, 1, J, nu, ipiv, x, nu);
        if (info != 0) { log_error("dgesv failed info=%d", info); return -1; }

        /* update */
        for (int u = 0; u < nu; u++) {
            int i = uq_bus[u];
            if (uq_isV[u]) { sys->bus[i].vm += x[u]; }
            else           { sys->bus[i].va += x[u]; }
        }
    }

    if (!converged) { log_error("PFLOW did not converge in %d iterations", max_iter); return -1; }
    log_info("PFLOW: converged");
    return 0;
}
