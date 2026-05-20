#include "types.h"
#include <stdlib.h>
#include <math.h>

int machine_init(System *sys)
{
    if (sys->nmachines == 0) return -1;

    sys->machine_states = malloc(2 * sys->nmachines * sizeof(double));
    if (!sys->machine_states) return -1;

    for (int m = 0; m < sys->nmachines; m++) {
        Machine *mc = &sys->machine[m];
        Gen    *gen = &sys->gen[mc->gen_idx];
        Bus    *bus = &sys->bus[gen->bus];

        double Vr = bus->vm * cos(bus->va);
        double Vi = bus->vm * sin(bus->va);
        double V = bus->vm;
        double P = gen->pg;
        double Q = gen->qg;

        double Vsq = V * V;
        if (Vsq < 1e-20) Vsq = 1e-20;
        double Ir = (P * Vr + Q * Vi) / Vsq;
        double Ii = (P * Vi - Q * Vr) / Vsq;

        double Er = Vr - mc->xdp * Ii;
        double Ei = Vi + mc->xdp * Ir;

        mc->Ep = sqrt(Er * Er + Ei * Ei);

        sys->machine_states[2 * m]     = atan2(Ei, Er);
        sys->machine_states[2 * m + 1] = 1.0;
    }

    return 0;
}
