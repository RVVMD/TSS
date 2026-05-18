#include "transient.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mpi.h>
#include <nvector/nvector_serial.h>
#include <ida/ida.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>

static void usage(const char *prog)
{
    printf(
        "Usage: %s [OPTIONS]\n"
        "  --raw FILE      PSS/E RAW file (required)\n"
        "  --dyr FILE      PSS/E DYR file (required)\n"
        "  --t-end SEC     Simulation time (default: 10.0)\n"
        "  --t-step SEC    Output interval (default: 0.01)\n"
        "  --events FILE   Event script (INI format)\n"
        "  --output DIR    Output directory (default: ./results)\n"
        "  --osc BUS       3-phase oscillogram for bus ID (1 kHz sampled)\n"
        "  --plot          Generate gnuplot PNGs\n"
        "  --help          Show this help\n", prog);
}

static FILE *open_out(const char *dir, const char *name)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/%s", dir, name);
    return fopen(buf, "w");
}

int main(int argc, char **argv)
{
    MPI_Init(NULL, NULL);

    const char *raw_file = NULL, *dyr_file = NULL, *events_file = NULL;
    const char *output_dir = "results";
    double t_end = 10.0, t_step = 0.01;
    int do_plot = 0, osc_bus = -1;

    static struct option long_opts[] = {
        {"raw",    required_argument, 0, 'r'},
        {"dyr",    required_argument, 0, 'd'},
        {"t-end",  required_argument, 0, 'T'},
        {"t-step", required_argument, 0, 's'},
        {"events", required_argument, 0, 'e'},
        {"output", required_argument, 0, 'o'},
        {"osc",    required_argument, 0, 'O'},
        {"plot",   no_argument,       0, 'p'},
        {"help",   no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "r:d:T:s:e:o:O:ph", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'r': raw_file    = optarg; break;
        case 'd': dyr_file    = optarg; break;
        case 'T': t_end       = atof(optarg); break;
        case 's': t_step      = atof(optarg); break;
        case 'e': events_file = optarg; break;
        case 'o': output_dir  = optarg; break;
        case 'O': osc_bus     = atoi(optarg); break;
        case 'p': do_plot     = 1; break;
        case 'h': usage(argv[0]); MPI_Finalize(); return 0;
        default:  usage(argv[0]); MPI_Finalize(); return 1;
        }
    }

    if (!raw_file || !dyr_file) {
        fprintf(stderr, "Error: --raw and --dyr required\n");
        usage(argv[0]); MPI_Finalize(); return 1;
    }

    Arena a = arena_new(1 << 24);
    System sys;
    memset(&sys, 0, sizeof(sys));

    if (raw_parse(raw_file, &sys, &a) != 0) { MPI_Finalize(); return 1; }
    if (dyr_parse(dyr_file, &sys, &a) != 0) { MPI_Finalize(); return 1; }
    if (ybus_build(&sys, &a) != 0)    { MPI_Finalize(); return 1; }
    if (powerflow_solve(&sys, &a) != 0) { /* non-critical */ }
    if (machine_init(&sys) != 0)       { MPI_Finalize(); return 1; }

    /* constant-Z load shunts from power-flow V */
    for (int i = 0; i < sys.nbus; i++) {
        Bus *bus = &sys.bus[i];
        double Vm2 = bus->vm * bus->vm;
        if (Vm2 > 1e-10) {
            bus->gl =  bus->pd / Vm2;
            bus->bl = -bus->qd / Vm2;
        } else {
            bus->gl = 0; bus->bl = 0;
        }
    }

    Event *events = NULL;
    int n_events = 0;
    if (events_file) {
        if (events_parse(events_file, &events, &a) != 0) {
            MPI_Finalize(); return 1;
        }
        n_events = (int)arrlen(events);
    }

    DAE dae;
    memset(&dae, 0, sizeof(dae));
    if (dae_init(&dae, &sys, &a) != 0) { MPI_Finalize(); return 1; }

    Integrator itg;
    memset(&itg, 0, sizeof(itg));
    if (integrator_init(&itg, &dae, 0.0) != 0) { MPI_Finalize(); return 1; }

    mkdir(output_dir, 0755);

    FILE *dfile = open_out(output_dir, "delta.csv");
    FILE *vfile = open_out(output_dir, "voltage.csv");

    if (dfile) {
        fprintf(dfile, "time");
        for (int m = 0; m < sys.nmachines; m++)
            fprintf(dfile, ",delta_%d,omega_%d", m, m);
        fprintf(dfile, "\n");
    }
    if (vfile) {
        fprintf(vfile, "time");
        for (int i = 0; i < sys.nbus; i++)
            fprintf(vfile, ",V_%d", sys.bus[i].id);
        fprintf(vfile, "\n");
    }

    /* oscillogram output */
    FILE *ofile = NULL;
    int osc_idx = -1;
    if (osc_bus > 0) {
        for (int i = 0; i < sys.nbus; i++)
            if (sys.bus[i].id == osc_bus) { osc_idx = i; break; }
        if (osc_idx >= 0) {
            char oname[256];
            snprintf(oname, sizeof(oname), "%s/osc_bus%d.csv", output_dir, osc_bus);
            ofile = fopen(oname, "w");
            if (ofile) fprintf(ofile, "time,Va,Vb,Vc\n");
        } else {
            log_warn("osc bus %d not found", osc_bus);
        }
    }

    double t = 0.0, t_next = t_step, t_prev = 0.0;
    double Vr_prev = 0, Vi_prev = 0;
    int step = 0, ev_idx = 0;
    int ndiff = dae.ndiff, neq = dae.neq;

    /* capture initial V for osc interpolation */
    if (osc_idx >= 0) {
        double *y0 = N_VGetArrayPointer(itg.nvec_y);
        Vr_prev = y0[ndiff + 2*osc_idx];
        Vi_prev = y0[ndiff + 2*osc_idx + 1];
    }

    log_info("Simulating: t_end=%.1f t_step=%.3f", t_end, t_step);

    while (t < t_end - 1e-10) {
        double tout = t_next;
        if (tout > t_end) tout = t_end;

        for (int ei = ev_idx; ei < n_events; ei++) {
            if (events[ei].time > t + 1e-10 &&
                events[ei].time < tout + 1e-10) {
                tout = events[ei].time;
                break;
            }
        }

        double tret;
        int ret = IDASolve(itg.ida_mem, tout, &tret,
                           itg.nvec_y, itg.nvec_ydot, IDA_NORMAL);
        if (ret < 0) {
            log_error("IDA failed at t=%.6f, tout=%.6f (ret=%d)", t, tout, ret);
            break;
        }
        t = tret;

        int events_fired = 0;
        while (ev_idx < n_events && events[ev_idx].time <= t + 1e-10) {
            events_apply(&sys, &events[ev_idx], t);
            ev_idx++;
            events_fired = 1;
        }

        if (events_fired) {
            ybus_build(&sys, &a);

            double *ydata = N_VGetArrayPointer(itg.nvec_y);
            double *ypd   = N_VGetArrayPointer(itg.nvec_ydot);
            double *Vtmp  = malloc((size_t)(2 * sys.nbus) * sizeof(double));

            if (events_post_state(&sys, ydata, Vtmp) == 0) {
                for (int i = 0; i < sys.nbus; i++) {
                    ydata[ndiff + 2*i]     = Vtmp[2*i];
                    ydata[ndiff + 2*i + 1] = Vtmp[2*i + 1];
                }

                memset(ypd, 0, (size_t)neq * sizeof(double));
                double ws = 2.0 * M_PI * 60.0;
                for (int m = 0; m < sys.nmachines; m++) {
                    Machine *mc = &sys.machine[m];
                    Gen *gen = &sys.gen[mc->gen_idx];
                    int bi = gen->bus;
                    double Vr = ydata[ndiff + 2*bi], Vi = ydata[ndiff + 2*bi + 1];
                    double d = ydata[2*m], Ep = mc->Ep, xdp = mc->xdp;
                    double Pe = Vr*(Ep*sin(d)-Vi)/xdp + Vi*(-(Ep*cos(d)-Vr))/xdp;
                    double TwoH = 2.0 * mc->h;
                    double om = ydata[2*m+1];
                    if (om < 0.1 || om > 2.0) om = 1.0;
                    ypd[2*m]   = ws * (om - 1.0);
                    ypd[2*m+1] = ws / TwoH * (gen->pg - Pe - mc->d * (om - 1.0));
                }

                /* save a clean copy of y before IDACalcIC */
                double *y_save = malloc((size_t)neq * sizeof(double));
                memcpy(y_save, ydata, (size_t)neq * sizeof(double));

                int icr = IDACalcIC(itg.ida_mem, IDA_Y_INIT, 0.01);
                if (icr != 0) {
                    /* restore clean post-solve state */
                    memcpy(ydata, y_save, (size_t)neq * sizeof(double));
                    memset(ypd, 0, (size_t)neq * sizeof(double));
                    for (int m = 0; m < sys.nmachines; m++) {
                        Machine *mc = &sys.machine[m];
                        Gen *gen = &sys.gen[mc->gen_idx];
                        int bi = gen->bus;
                        double Vr = ydata[ndiff + 2*bi], Vi = ydata[ndiff + 2*bi + 1];
                        double d = ydata[2*m], Ep = mc->Ep, xdp = mc->xdp;
                        double Pe = Vr*(Ep*sin(d)-Vi)/xdp + Vi*(-(Ep*cos(d)-Vr))/xdp;
                        double TwoH = 2.0 * mc->h;
                        double om = ydata[2*m+1];
                        if (om < 0.1 || om > 2.0) om = 1.0;
                        ypd[2*m]   = ws * (om - 1.0);
                        ypd[2*m+1] = ws / TwoH * (gen->pg - Pe - mc->d * (om - 1.0));
                    }
                    log_warn("EVENT: IDACalcIC failed (%d), restored clean state", icr);
                }
                free(y_save);

                IDASetMaxOrd(itg.ida_mem, 1);
                int rrc = IDAReInit(itg.ida_mem, t, itg.nvec_y, itg.nvec_ydot);
                IDASStolerances(itg.ida_mem, 1e-4, 1e-6);
                if (rrc == 0)
                    log_info("EVENT: IDA re-initialized at t=%.4f", t);
                else
                    log_warn("EVENT: IDAReInit returned %d", rrc);
            }
            free(Vtmp);
        }

        {
            double *y = N_VGetArrayPointer(itg.nvec_y);
            if (dfile) {
                fprintf(dfile, "%.6f", t);
                for (int m = 0; m < sys.nmachines; m++)
                    fprintf(dfile, ",%.12g,%.12g", y[2*m], y[2*m+1]);
                fprintf(dfile, "\n");
            }
            if (vfile) {
                fprintf(vfile, "%.6f", t);
                for (int i = 0; i < sys.nbus; i++) {
                    double Vr = y[ndiff + 2*i], Vi = y[ndiff + 2*i+1];
                    fprintf(vfile, ",%.12g", sqrt(Vr*Vr + Vi*Vi));
                }
                fprintf(vfile, "\n");
            }

            /* 3-phase oscillogram at 1 kHz */
            if (ofile) {
                double wr = 2.0 * M_PI * 60.0;
                double Vr_n = y[ndiff + 2*osc_idx];
                double Vi_n = y[ndiff + 2*osc_idx + 1];

                double dt_osc = 0.001;
                for (double ts = t_prev; ts < t - 1e-10; ts += dt_osc) {
                    double alpha = (ts - t_prev) / (t - t_prev + 1e-20);
                    double Vr = Vr_prev * (1.0 - alpha) + Vr_n * alpha;
                    double Vi = Vi_prev * (1.0 - alpha) + Vi_n * alpha;
                    double va = Vr * cos(wr * ts) - Vi * sin(wr * ts);
                    double vb = Vr * cos(wr * ts - 2.0943951024) - Vi * sin(wr * ts - 2.0943951024);
                    double vc = Vr * cos(wr * ts + 2.0943951024) - Vi * sin(wr * ts + 2.0943951024);
                    fprintf(ofile, "%.6f,%.6f,%.6f,%.6f\n", ts, va, vb, vc);
                }
                Vr_prev = Vr_n;
                Vi_prev = Vi_n;
                t_prev = t;
            }
        }

        step++;
        while (t_next <= t + 1e-10) t_next += t_step;

        if (step % 200 == 0)
            log_info("t=%.3f/%.1f steps=%d", t, t_end, step);
    }

    log_info("Done: t=%.4f steps=%d", t, step);

    if (dfile) fclose(dfile);
    if (vfile) fclose(vfile);
    if (ofile) fclose(ofile);

    if (do_plot) {
        char buf[2048];
        FILE *gp;
        snprintf(buf, sizeof(buf), "%s/delta.gp", output_dir);
        gp = fopen(buf, "w");
        if (gp) {
            fprintf(gp, "set terminal pngcairo size 1200,800\n"
                "set datafile separator ','\n"
                "set output '%s/delta.png'\n"
                "set xlabel 'Time (s)'\nset ylabel 'Rotor Angle (rad)'\n"
                "set title 'Generator Rotor Angles'\n"
                "plot '%s/delta.csv' using 1:2 with lines title 'Gen 1'",
                output_dir, output_dir);
            for (int m = 1; m < sys.nmachines; m++)
                fprintf(gp, ", '' using 1:%d with lines title 'Gen %d'", 2*m+2, m+1);
            fprintf(gp, "\n"); fclose(gp);
        }
        snprintf(buf, sizeof(buf), "%s/voltage.gp", output_dir);
        gp = fopen(buf, "w");
        if (gp) {
            fprintf(gp, "set terminal pngcairo size 1200,800\n"
                "set datafile separator ','\n"
                "set output '%s/voltage.png'\n"
                "set xlabel 'Time (s)'\nset ylabel 'Voltage Magnitude (pu)'\n"
                "set title 'Bus Voltages'\n"
                "plot '%s/voltage.csv' using 1:2 with lines title 'Bus 1'",
                output_dir, output_dir);
            for (int i = 1; i < sys.nbus; i++)
                fprintf(gp, ", '' using 1:%d with lines title 'Bus %d'", i+2, i+1);
            fprintf(gp, "\n"); fclose(gp);
        }
        snprintf(buf, sizeof(buf), "gnuplot %s/delta.gp %s/voltage.gp 2>/dev/null",
                 output_dir, output_dir);
        system(buf);
    }

    integrator_free(&itg);
    dae_free(&dae);
    free(sys.machine_states);
    free(sys.colptr); free(sys.rowidx); free(sys.yval);
    arrfree(events); arrfree(sys.machine); arrfree(sys.bus);
    arrfree(sys.branch); arrfree(sys.gen); arrfree(sys.load);
    arena_free(&a);

    MPI_Finalize();
    return 0;
}
