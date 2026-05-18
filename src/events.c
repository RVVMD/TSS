#include "types.h"
#include <string.h>
#include <math.h>
#include <lapacke.h>

/* compute Thevenin impedance at bus bi from Y-bus augmented with gens and loads */
static void thevenin(const System *sys, int bi,
                     double *Zth_r, double *Zth_i,
                     double *Vth_r, double *Vth_i)
{
    /* extract diagonal of augmented Y: Y_ii = Y_bus_ii + Y_gen_shunt + Y_load */
    double Gr = 0, Gi = 0;
    for (int k = sys->colptr[bi]; k < sys->colptr[bi+1]; k++) {
        if (sys->rowidx[k] == bi) {
            Gr = sys->yval[2*k];
            Gi = sys->yval[2*k+1];
            break;
        }
    }
    /* add generator shunt: 1/(j*x'd) = -j/x'd */
    for (int m = 0; m < sys->nmachines; m++) {
        if (sys->gen[sys->machine[m].gen_idx].bus == bi) {
            Gi += -1.0 / sys->machine[m].xdp;
        }
    }
    /* add load shunt */
    Gi += sys->bus[bi].bl;
    Gr += sys->bus[bi].gl;

    /* Zth = 1 / Y_ii */
    double denom = Gr*Gr + Gi*Gi;
    if (denom < 1e-20) { *Zth_r = 1e8; *Zth_i = 0; }
    else {
        *Zth_r =  Gr / denom;
        *Zth_i = -Gi / denom;
    }

    /* Thevenin voltage: estimated as 1.0 + j0 (nominal) */
    *Vth_r = 1.0;
    *Vth_i = 0.0;
}

/* compute effective positive-sequence fault shunt admittance */
static void fault_shunt(double Zth_r, double Zth_i, double Zf_r, double Zf_i,
                        int ftype, double *Yr, double *Yi)
{
    double Zeq_r = Zf_r, Zeq_i = Zf_i;

    switch (ftype) {
    case 0: /* 3-phase balanced */
        Zeq_r = Zf_r;
        Zeq_i = Zf_i;
        break;
    case 1: /* SLG: Z_eq = Z2 + Z0 + 3Zf ≈ 2*Zth + 3Zf */
        Zeq_r = 2.0*Zth_r + 3.0*Zf_r;
        Zeq_i = 2.0*Zth_i + 3.0*Zf_i;
        break;
    case 2: /* LL: Z_eq = Z2 + Zf ≈ Zth + Zf */
        Zeq_r = Zth_r + Zf_r;
        Zeq_i = Zth_i + Zf_i;
        break;
    case 3: /* DLG: Z_eq = Z2||(Z0+3Zf) + Zf ≈ Zth||(Zth+3Zf) + Zf */
        {
            double Z0_r = Zth_r + 3.0*Zf_r;
            double Z0_i = Zth_i + 3.0*Zf_i;
            /* Z2 || Z0 */
            double num_r = Zth_r*Z0_r - Zth_i*Z0_i;
            double num_i = Zth_r*Z0_i + Zth_i*Z0_r;
            double den_r = Zth_r + Z0_r;
            double den_i = Zth_i + Z0_i;
            double d2 = den_r*den_r + den_i*den_i;
            Zeq_r = (num_r*den_r + num_i*den_i)/d2 + Zf_r;
            Zeq_i = (num_i*den_r - num_r*den_i)/d2 + Zf_i;
        }
        break;
    default: break;
    }

    /* Y = 1 / Zeq */
    double d = Zeq_r*Zeq_r + Zeq_i*Zeq_i;
    if (d < 1e-20) { *Yr = 1e8; *Yi = 0; }
    else {
        *Yr =  Zeq_r / d;
        *Yi = -Zeq_i / d;
    }
}

int events_apply(System *sys, Event *ev, double t)
{
    switch (ev->type) {
    case FAULT:
    case FAULT_SLG:
    case FAULT_LL:
    case FAULT_DLG: {
        int bi = -1;
        for (int i = 0; i < sys->nbus; i++) {
            if (sys->bus[i].id == ev->bus) { bi = i; break; }
        }
        if (bi < 0) { log_warn("fault: bus %d not found", ev->bus); return -1; }

        double zr = ev->fault_r, zx = ev->fault_x;
        if (fabs(zr) + fabs(zx) < 1e-10) { zr = 1e-6; zx = 1e-6; }

        sys->fault_bus = bi;

        /* compute Thevenin at fault bus */
        double Zth_r, Zth_i, Vth_r, Vth_i;
        thevenin(sys, bi, &Zth_r, &Zth_i, &Vth_r, &Vth_i);

        /* map event type to internal fault_type */
        int ftype = 0;
        const char *ftname = "3ph";
        if (ev->type == FAULT_SLG) { ftype = 1; ftname = "SLG"; }
        else if (ev->type == FAULT_LL) { ftype = 2; ftname = "LL"; }
        else if (ev->type == FAULT_DLG) { ftype = 3; ftname = "DLG"; }

        sys->fault_type = ftype;
        sys->fault_Zth_r = Zth_r;
        sys->fault_Zth_i = Zth_i;
        sys->fault_Vth_r = Vth_r;
        sys->fault_Vth_i = Vth_i;

        /* effective positive-sequence shunt admittance */
        fault_shunt(Zth_r, Zth_i, zr, zx, ftype,
                    &sys->fault_Y_r, &sys->fault_Y_i);

        log_info("EVENT: %s fault bus %d (Yeff=%.1f+j%.1f Zth=%.4f+j%.4f) t=%.3f",
                 ftname, ev->bus, sys->fault_Y_r, sys->fault_Y_i,
                 Zth_r, Zth_i, t);
        break;
    }
    case FAULT_CLEAR:
        log_info("EVENT: fault cleared bus %d", ev->bus);
        sys->fault_bus = -1;
        sys->fault_type = 0;
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
