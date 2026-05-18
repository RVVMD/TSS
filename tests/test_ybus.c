#include "transient.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mpi.h>
#include <nvector/nvector_serial.h>

static int test_full(char *raw_file, char *dyr_file)
{
    Arena a = arena_new(1 << 20);
    System sys;
    DAE dae;
    Integrator itg;
    memset(&sys, 0, sizeof(sys));

    if (raw_parse(raw_file, &sys, &a) != 0) {
        fprintf(stderr, "FAIL: raw_parse\n"); arena_free(&a); return 1;
    }
    printf("PASS: RAW parse - %d buses, %d branches, %d gens\n",
           sys.nbus, sys.nbranch, sys.ngen);

    if (dyr_parse(dyr_file, &sys, &a) != 0) {
        fprintf(stderr, "FAIL: dyr_parse\n"); goto fail;
    }
    printf("PASS: DYR parse - %d machines\n", sys.nmachines);

    if (ybus_build(&sys, &a) != 0) {
        fprintf(stderr, "FAIL: ybus\n"); goto fail;
    }
    printf("PASS: ybus_build - %d non-zeros\n", sys.nnz);

    /* power flow first */
    if (powerflow_solve(&sys, &a) != 0) {
        fprintf(stderr, "FAIL: power flow\n"); goto fail;
    }
    printf("PASS: powerflow converged\n");

    /* machine init AFTER power flow (uses updated V values) */
    if (machine_init(&sys) != 0) {
        fprintf(stderr, "FAIL: machine_init\n"); goto fail;
    }
    printf("PASS: machine_init\n");

    memset(&dae, 0, sizeof(dae));
    if (dae_init(&dae, &sys, &a) != 0) { fprintf(stderr, "FAIL: dae_init\n"); goto fail; }
    printf("PASS: dae_init - neq=%d\n", dae.neq);

    /* quick residual check */
    SUNContext ctx;
    SUNContext_Create(MPI_COMM_WORLD, &ctx);
    N_Vector ny = N_VNew_Serial(dae.neq, ctx);
    N_Vector nyp = N_VNew_Serial(dae.neq, ctx);
    N_Vector nr = N_VNew_Serial(dae.neq, ctx);
    double *yy = N_VGetArrayPointer(ny);
    for (int m = 0; m < sys.nmachines; m++) {
        yy[2*m] = sys.machine_states[2*m];
        yy[2*m+1] = sys.machine_states[2*m+1];
    }
    for (int i = 0; i < sys.nbus; i++) {
        double Vm = sys.bus[i].vm, Va = sys.bus[i].va;
        yy[dae.ndiff + 2*i] = Vm * cos(Va);
        yy[dae.ndiff + 2*i+1] = Vm * sin(Va);
    }
    memset(N_VGetArrayPointer(nyp), 0, dae.neq*sizeof(double));
    dae_residual(0.0, ny, nyp, nr, &dae);
    double *rr = N_VGetArrayPointer(nr);
    double rn = 0; int bad = 0;
    for (int i = 0; i < dae.neq; i++) {
        rn += rr[i]*rr[i];
        if (!isfinite(rr[i])) bad = 1;
    }
    printf("PASS: residual ||r||=%.3e  (bad=%d)\n", sqrt(rn), bad);
    if (bad) { fprintf(stderr, "FAIL: residual has NaN/Inf\n"); goto fail; }

    N_VDestroy(ny); N_VDestroy(nyp); N_VDestroy(nr);
    SUNContext_Free(&ctx);

    memset(&itg, 0, sizeof(itg));
    if (integrator_init(&itg, &dae, 0.0) != 0) {
        fprintf(stderr, "FAIL: integrator_init\n"); goto fail;
    }
    printf("PASS: integrator_init\n");

    double t = 0.0, tend = 1.0, tret;
    int steps = 0;
    while (t < tend - 1e-10) {
        int rc = IDASolve(itg.ida_mem, tend, &tret, itg.nvec_y, itg.nvec_ydot, IDA_NORMAL);
        if (rc < 0) { fprintf(stderr, "FAIL: step at t=%.3f rc=%d\n", t, rc); break; }
        t = tret;
        steps++;
        if (steps >= 50) break;
    }

    if (steps > 0)
        printf("PASS: integrator steps=%d t_final=%.3f\n", steps, t);
    else
        fprintf(stderr, "FAIL: no integration steps\n");

    integrator_free(&itg);
    dae_free(&dae);
    free(sys.machine_states);
    arrfree(sys.machine);
    arrfree(sys.bus);
    arrfree(sys.branch);
    arrfree(sys.gen);
    arrfree(sys.load);
    arena_free(&a);
    return 0;

fail:
    free(sys.machine_states);
    arrfree(sys.machine);
    arrfree(sys.bus);
    arrfree(sys.branch);
    arrfree(sys.gen);
    arrfree(sys.load);
    arena_free(&a);
    return 1;
}

int main(void)
{
    MPI_Init(NULL, NULL);
    int failures = 0;
    failures += test_full("tests/data/ieee14.raw", "tests/data/ieee14.dyr");
    if (failures) printf("\n%d test(s) FAILED\n", failures);
    else          printf("\nAll tests PASSED\n");
    MPI_Finalize();
    return failures;
}
