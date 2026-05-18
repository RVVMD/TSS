#include "types.h"
#include <string.h>
#include <math.h>
#include <lapacke.h>

int events_apply(System *sys, Event *ev, double t)
{
    switch (ev->type) {
    case FAULT: {
        int bi = -1;
        for (int i = 0; i < sys->nbus; i++) {
            if (sys->bus[i].id == ev->bus) { bi = i; break; }
        }
        if (bi < 0) { log_warn("fault: bus %d not found", ev->bus); return -1; }
        double zr = ev->fault_r, zx = ev->fault_x;
        if (fabs(zr) + fabs(zx) < 1e-10) {
            zr = 1e-6; zx = 1e-6;
        }
        double denom = zr*zr + zx*zx;
        sys->fault_bus  = bi;
        sys->fault_Y_r  =  zr / denom;
        sys->fault_Y_i  = -zx / denom;
        log_info("EVENT: 3ph fault bus %d (Y=%.1f+j%.1f) t=%.3f",
                 ev->bus, sys->fault_Y_r, sys->fault_Y_i, t);
        break;
    }
    case FAULT_CLEAR:
        log_info("EVENT: fault cleared bus %d", ev->bus);
        sys->fault_bus = -1;
        break;
    case LINE_OPEN: {
        log_info("EVENT: line %d-%d opened", ev->from, ev->to);
        for (int b = 0; b < sys->nbranch; b++) {
            if ((sys->bus[sys->branch[b].from].id == ev->from &&
                 sys->bus[sys->branch[b].to].id   == ev->to) ||
                (sys->bus[sys->branch[b].from].id == ev->to &&
                 sys->bus[sys->branch[b].to].id   == ev->from)) {
                sys->branch[b].status = 0;
                break;
            }
        }
        break;
    }
    case LINE_CLOSE:
    case GEN_TRIP:
    case LOAD_SHED:
        log_warn("event type %d not yet implemented", ev->type);
        break;
    default:
        break;
    }
    return 0;
}

int events_post_state(const System *sys, const double *delta, double *V_out)
{
    int n = sys->nbus;
    int N2 = 2 * n;

    int slack = -1;
    for (int i = 0; i < n; i++)
        if (sys->bus[i].type == BUS_SLACK) { slack = i; break; }

    double *G = calloc((size_t)n * n, sizeof(double));
    double *B = calloc((size_t)n * n, sizeof(double));

    for (int i = 0; i < n; i++) {
        for (int k = sys->colptr[i]; k < sys->colptr[i+1]; k++) {
            int j = sys->rowidx[k];
            G[j * n + i] = sys->yval[2 * k];
            B[j * n + i] = sys->yval[2 * k + 1];
        }
    }

    for (int m = 0; m < sys->nmachines; m++) {
        int bi = sys->gen[sys->machine[m].gen_idx].bus;
        B[bi * n + bi] += -1.0 / sys->machine[m].xdp;
    }

    for (int i = 0; i < n; i++) {
        Bus *bus = &sys->bus[i];
        G[i * n + i] += bus->gl;
        B[i * n + i] += bus->bl;
    }

    double *A = calloc((size_t)N2 * N2, sizeof(double));

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            double g = G[i * n + j];
            double b = B[i * n + j];
            A[2*i     * N2 + 2*j]     =  g;
            A[2*i     * N2 + 2*j + 1] = -b;
            A[(2*i+1) * N2 + 2*j]     =  b;
            A[(2*i+1) * N2 + 2*j+1]   =  g;
        }
    }
    free(G); free(B);

    double *rhs = calloc(N2, sizeof(double));

    for (int m = 0; m < sys->nmachines; m++) {
        int bi = sys->gen[sys->machine[m].gen_idx].bus;
        double d = delta[2 * m];
        double Ep = sys->machine[m].Ep;
        double xdp = sys->machine[m].xdp;
        rhs[2*bi]     +=  Ep * sin(d) / xdp;
        rhs[2*bi + 1] += -Ep * cos(d) / xdp;
    }

    if (slack >= 0) {
        for (int j = 0; j < N2; j++)
            A[(2*slack+1) * N2 + j] = 0.0;
        A[(2*slack+1) * N2 + (2*slack+1)] = 1.0;
        rhs[2*slack+1] = 0.0;
    }

    int *ipiv = malloc((size_t)N2 * sizeof(int));
    int info = LAPACKE_dgesv(LAPACK_ROW_MAJOR, N2, 1, A, N2, ipiv, rhs, 1);

    memcpy(V_out, rhs, (size_t)N2 * sizeof(double));
    free(rhs); free(ipiv); free(A);
    return info;
}
