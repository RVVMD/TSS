#include "transient.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <mpi.h>

#include <nvector/nvector_serial.h>
#include <sunlinsol/sunlinsol_dense.h>
#include <sunmatrix/sunmatrix_dense.h>

int integrator_init(Integrator *itg, DAE *dae, double t0)
{
    int neq = dae->neq;
    int ret;

    itg->dae = dae;
    itg->neq = neq;

    ret = SUNContext_Create(MPI_COMM_WORLD, &itg->sunctx);
    if (ret) { log_error("SUNContext_Create failed"); return -1; }

    itg->nvec_y = N_VNew_Serial(neq, itg->sunctx);
    itg->nvec_ydot = N_VNew_Serial(neq, itg->sunctx);
    if (!itg->nvec_y || !itg->nvec_ydot) {
        log_error("N_VNew_Serial failed"); return -1;
    }

    double *ydata = N_VGetArrayPointer(itg->nvec_y);
    double *ypdata = N_VGetArrayPointer(itg->nvec_ydot);

    for (int m = 0; m < dae->sys->nmachines; m++) {
        ydata[2*m]     = dae->sys->machine_states[2*m];
        ydata[2*m + 1] = dae->sys->machine_states[2*m + 1];
        ypdata[2*m]     = 0.0;
        ypdata[2*m + 1] = 0.0;
    }

    int ndiff = dae->ndiff;
    int nbus = dae->sys->nbus;
    for (int i = 0; i < nbus; i++) {
        double Vm = dae->sys->bus[i].vm;
        double Va = dae->sys->bus[i].va;
        ydata[ndiff + 2*i]     = Vm * cos(Va);
        ydata[ndiff + 2*i + 1] = Vm * sin(Va);
        ypdata[ndiff + 2*i]     = 0.0;
        ypdata[ndiff + 2*i + 1] = 0.0;
    }

    itg->ida_mem = IDACreate(itg->sunctx);
    if (!itg->ida_mem) { log_error("IDACreate failed"); return -1; }

    IDASetMaxOrd(itg->ida_mem, 1);

    ret = IDAInit(itg->ida_mem, dae_residual, t0,
                  itg->nvec_y, itg->nvec_ydot);
    if (ret) { log_error("IDAInit failed (ret=%d)", ret); return -1; }

    IDASetUserData(itg->ida_mem, dae);
    IDASStolerances(itg->ida_mem, 1e-4, 1e-6);
    IDASetMaxNumSteps(itg->ida_mem, 500000);

    N_Vector id = N_VNew_Serial(neq, itg->sunctx);
    double *idd = N_VGetArrayPointer(id);
    for (int i = 0; i < ndiff; i++) idd[i] = 1.0;
    for (int i = ndiff; i < neq; i++) idd[i] = 0.0;
    IDASetId(itg->ida_mem, id);
    N_VDestroy(id);

    itg->sunmat_J = SUNDenseMatrix(neq, neq, itg->sunctx);
    if (!itg->sunmat_J) { log_error("SUNDenseMatrix failed"); return -1; }

    itg->sunls = SUNLinSol_Dense(itg->nvec_y, itg->sunmat_J, itg->sunctx);
    if (!itg->sunls) { log_error("SUNLinSol_Dense failed"); return -1; }

    ret = IDASetLinearSolver(itg->ida_mem, itg->sunls, itg->sunmat_J);
    if (ret) { log_error("IDASetLinearSolver failed"); return -1; }

    ret = IDACalcIC(itg->ida_mem, IDA_Y_INIT, 0.01);
    if (ret) {
        log_warn("IDACalcIC had issues (ret=%d), proceeding", ret);
    } else {
        log_info("IDACalcIC: initial condition corrected");
    }

    log_info("IDA: init OK (DENSE), neq=%d (diff=%d, alg=%d)",
             neq, dae->ndiff, dae->nalg);
    return 0;
}

int integrator_step(Integrator *itg, double tout, double *tret)
{
    return IDASolve(itg->ida_mem, tout, tret,
                    itg->nvec_y, itg->nvec_ydot, IDA_NORMAL);
}

void integrator_free(Integrator *itg)
{
    if (itg->nvec_y)    N_VDestroy(itg->nvec_y);
    if (itg->nvec_ydot) N_VDestroy(itg->nvec_ydot);
    if (itg->sunls)     SUNLinSolFree(itg->sunls);
    if (itg->sunmat_J)  SUNMatDestroy(itg->sunmat_J);
    if (itg->ida_mem)   IDAFree(&itg->ida_mem);
    if (itg->sunctx)    SUNContext_Free(&itg->sunctx);
}
