#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <nvector/nvector_serial.h>

#define Y_G(k)  (dae->sys->yval[2*(k)])
#define Y_B(k)  (dae->sys->yval[2*(k)+1])
#define YDIFF(m)  (2*(m))
#define YOMEGA(m) (2*(m)+1)
#define YVR(i)  (dae->ndiff + 2*(i))
#define YVI(i)  (dae->ndiff + 2*(i) + 1)

int dae_init(DAE *dae, System *sys, Arena *a)
{
    (void)a;
    dae->sys = sys;
    dae->ndiff = 2 * sys->nmachines;
    dae->nalg  = 2 * sys->nbus;
    dae->neq   = dae->ndiff + dae->nalg;
    dae->jcolptr = NULL;
    dae->jrowidx = NULL;
    dae->jval = NULL;
    dae->jnnz = 0;

    sys->fault_bus = -1;
    sys->fault_Y_r = 0;
    sys->fault_Y_i = 0;

    return 0;
}

void dae_free(DAE *dae) { (void)dae; }

int dae_residual(double t, N_Vector yy, N_Vector yp,
                  N_Vector rr, void *user_data)
{
    DAE *dae = (DAE *)user_data;
    System *sys = dae->sys;
    double *y = N_VGetArrayPointer(yy);
    double *ydot = N_VGetArrayPointer(yp);
    double *r = N_VGetArrayPointer(rr);
    double ws = 2.0 * M_PI * 60.0;

    (void)t; (void)ydot;

    for (int m = 0; m < sys->nmachines; m++) {
        int ridx = YDIFF(m);
        double delta = y[ridx], omega = y[YOMEGA(m)];
        Machine *mc = &sys->machine[m];
        Gen *gen = &sys->gen[mc->gen_idx];
        int bus_i = gen->bus;
        double Vr = y[YVR(bus_i)], Vi = y[YVI(bus_i)];
        double Ep = mc->Ep, Er = Ep*cos(delta), Ei = Ep*sin(delta);
        double dVr = Er-Vr, dVi = Ei-Vi;
        double Pe = Vr*(dVi/mc->xdp) + Vi*(-dVr/mc->xdp);
        double TwoH = 2.0*mc->h;
        r[ridx]       = ydot[ridx] - ws*(omega-1.0);
        r[YOMEGA(m)] = TwoH/ws*ydot[YOMEGA(m)] - (gen->pg - Pe - mc->d*(omega-1.0));
    }

    int slack = 0;
    for (int i = 0; i < sys->nbus; i++)
        if (sys->bus[i].type == BUS_SLACK) { slack = i; break; }

    for (int i = 0; i < sys->nbus; i++) {
        double Ir = 0.0, Ii = 0.0;
        double Vr = y[YVR(i)], Vi = y[YVI(i)];

        for (int m = 0; m < sys->nmachines; m++) {
            Gen *gen = &sys->gen[sys->machine[m].gen_idx];
            if (gen->bus != i) continue;
            double delta = y[YDIFF(m)], Ep = sys->machine[m].Ep;
            double dVr = Ep*cos(delta)-Vr, dVi = Ep*sin(delta)-Vi;
            Ir +=  dVi/sys->machine[m].xdp;
            Ii += -dVr/sys->machine[m].xdp;
        }

        Bus *bus = &sys->bus[i];
        if (fabs(bus->gl) > 1e-10 || fabs(bus->bl) > 1e-10) {
            /* constant-Z load: Y = gl + j*bl, pre-computed from power-flow V */
            Ir -= (bus->gl * Vr - bus->bl * Vi);
            Ii -= (bus->bl * Vr + bus->gl * Vi);
        }

        double YVr = 0.0, YVi = 0.0;
        for (int k = sys->colptr[i]; k < sys->colptr[i+1]; k++) {
            int j = sys->rowidx[k];
            double G = Y_G(k), B = Y_B(k);
            double Vjr = y[YVR(j)], Vji = y[YVI(j)];
            YVr += G*Vjr - B*Vji;
            YVi += B*Vjr + G*Vji;
        }

        r[YVR(i)] = YVr - Ir;
        if (i == slack)
            r[YVI(i)] = y[YVI(i)];
        else
            r[YVI(i)] = YVi - Ii;
    }

    return 0;
}
