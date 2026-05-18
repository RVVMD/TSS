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
        "TSS — Transient Stability Simulator\n"
        "\n"
        "Usage: %s --raw <FILE> --dyr <FILE> [OPTIONS]\n"
        "\n"
        "=== Input ===\n"
        "  --raw FILE        PSS/E RAW file (required)\n"
        "  --dyr FILE        PSS/E DYR file (required)\n"
        "  --events FILE     Event script in INI format\n"
        "\n"
        "=== Simulation ===\n"
        "  --t-end SEC       Simulation duration (default: 10.0)\n"
        "  --t-step SEC      Output interval (default: 0.01)\n"
        "\n"
        "=== Output ===\n"
        "  --output DIR      Output directory (default: ./results)\n"
        "  --osc BUS         3-phase oscillogram at 1 kHz\n"
        "  --plot            Generate gnuplot PNGs\n"
        "\n"
        "=== Inspection (no simulation) ===\n"
        "  --info            Print network topology report\n"
        "  --topo FILE       Export Graphviz DOT topology graph\n"
        "\n"
        "=== Examples ===\n"
        "  # Inspect a case\n"
        "  %s --raw ieee14.raw --dyr ieee14.dyr --info\n"
        "\n"
        "  # Run 3-phase fault simulation\n"
        "  %s --raw ieee14.raw --dyr ieee14.dyr \\\n"
        "      --events fault.ini --t-end 3.0 --plot\n"
        "\n"
        "  # Generate oscillogram for bus 4\n"
        "  %s --raw ieee14.raw --dyr ieee14.dyr \\\n"
        "      --events fault.ini --osc 4 --plot\n"
        "\n"
        "  # Export topology graph\n"
        "  %s --raw ieee14.raw --dyr ieee14.dyr \\\n"
        "      --topo network.dot\n"
        "\n", prog, prog, prog, prog, prog, prog);
}

static FILE *open_out(const char *dir, const char *name)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/%s", dir, name);
    return fopen(buf, "w");
}

/* tracking for post-sim summary */
typedef struct {
    double v_min, v_max, v_sum;
    double d_min, d_max, w_min, w_max;
    int samples;
} BusStats;

typedef struct {
    double d_min, d_max, w_min, w_max;
    double pe_min, pe_max;
    int samples;
} GenStats;

int main(int argc, char **argv)
{
    MPI_Init(NULL, NULL);

    const char *raw_file = NULL, *dyr_file = NULL, *events_file = NULL;
    const char *output_dir = "results", *topo_file = NULL;
    double t_end = 10.0, t_step = 0.01;
    int do_plot = 0, osc_bus = -1, do_info = 0;

    static struct option long_opts[] = {
        {"raw",    required_argument, 0, 'r'},
        {"dyr",    required_argument, 0, 'd'},
        {"t-end",  required_argument, 0, 'T'},
        {"t-step", required_argument, 0, 's'},
        {"events", required_argument, 0, 'e'},
        {"output", required_argument, 0, 'o'},
        {"osc",    required_argument, 0, 'O'},
        {"plot",   no_argument,       0, 'p'},
        {"info",   no_argument,       0, 'I'},
        {"topo",   required_argument, 0, 'G'},
        {"help",   no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "r:d:T:s:e:o:O:I:G:ph", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'r': raw_file    = optarg; break;
        case 'd': dyr_file    = optarg; break;
        case 'T': t_end       = atof(optarg); break;
        case 's': t_step      = atof(optarg); break;
        case 'e': events_file = optarg; break;
        case 'o': output_dir  = optarg; break;
        case 'O': osc_bus     = atoi(optarg); break;
        case 'I': do_info     = 1; break;
        case 'G': topo_file   = optarg; break;
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

    /* --info: print topology and exit */
    if (do_info) { print_network_info(&sys); MPI_Finalize(); return 0; }

    /* --topo: export DOT and exit */
    if (topo_file) { print_network_dot(&sys, topo_file); MPI_Finalize(); return 0; }

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
        } else { bus->gl = 0; bus->bl = 0; }
    }

    Event *events = NULL;
    int n_events = 0;
    if (events_file) {
        if (events_parse(events_file, &events, &a) != 0) { MPI_Finalize(); return 1; }
        n_events = (int)arrlen(events);
    }

    DAE dae;
    memset(&dae, 0, sizeof(dae));
    if (dae_init(&dae, &sys, &a) != 0) { MPI_Finalize(); return 1; }

    Integrator itg;
    memset(&itg, 0, sizeof(itg));
    if (integrator_init(&itg, &dae, 0.0) != 0) { MPI_Finalize(); return 1; }

    mkdir(output_dir, 0755);

    /* CSV headers via CSVWriter API */
    const char *dheaders[11] = {"d0","w0","Pe0","Pm0","Vt0","d1","w1","Pe1","Pm1","Vt1",NULL};
    int ncols_d = 2 * sys.nmachines + 1; /* 2 per machine (d, w) */
    /* We'll use manual fprintf for flexibility */

    FILE *dfile = open_out(output_dir, "delta.csv");
    FILE *vfile = open_out(output_dir, "voltage.csv");

    if (dfile) {
        fprintf(dfile, "time");
        for (int m = 0; m < sys.nmachines; m++) {
            int bid = sys.bus[sys.gen[sys.machine[m].gen_idx].bus].id;
            fprintf(dfile, ",d_g%d,Pe_g%d,Vt_g%d", bid, bid, bid);
        }
        fprintf(dfile, "\n");
    }
    if (vfile) {
        fprintf(vfile, "time");
        for (int i = 0; i < sys.nbus; i++)
            fprintf(vfile, ",V_%d,ang_%d", sys.bus[i].id, sys.bus[i].id);
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
        } else log_warn("osc bus %d not found", osc_bus);
    }

    double t = 0.0, t_next = t_step, t_prev = 0.0;
    double Vr_prev = 0, Vi_prev = 0;
    double Va_pr = 0, Va_pi = 0, Vb_pr = 0, Vb_pi = 0, Vc_pr = 0, Vc_pi = 0;
    double Vdc_a = 0, Vdc_b = 0, Vdc_c = 0;
    int step = 0, ev_idx = 0;
    int ndiff = dae.ndiff, neq = dae.neq;

    /* post-sim stats */
    BusStats *bstat = calloc((size_t)sys.nbus, sizeof(BusStats));
    GenStats *gstat = calloc((size_t)sys.nmachines, sizeof(GenStats));
    for (int i = 0; i < sys.nbus; i++)
        bstat[i].v_min = 1e30;
    for (int m = 0; m < sys.nmachines; m++) {
        gstat[m].d_min = 1e30; gstat[m].w_min = 1e30;
        gstat[m].pe_min = 1e30;
    }

    if (osc_idx >= 0) {
        double *y0 = N_VGetArrayPointer(itg.nvec_y);
        Vr_prev = y0[ndiff + 2*osc_idx];
        Vi_prev = y0[ndiff + 2*osc_idx + 1];
        double a2r=-0.5, a2i=-0.86602540378, ar=-0.5, ai=0.86602540378;
        Va_pr = Vr_prev; Va_pi = Vi_prev;
        Vb_pr = a2r*Vr_prev - a2i*Vi_prev; Vb_pi = a2r*Vi_prev + a2i*Vr_prev;
        Vc_pr = ar*Vr_prev - ai*Vi_prev;   Vc_pi = ar*Vi_prev + ai*Vr_prev;
    }

    log_info("Simulating: t_end=%.1f t_step=%.3f", t_end, t_step);

    while (t < t_end - 1e-10) {
        double tout = t_next;
        if (tout > t_end) tout = t_end;

        for (int ei = ev_idx; ei < n_events; ei++) {
            if (events[ei].time > t + 1e-10 && events[ei].time < tout + 1e-10) {
                tout = events[ei].time; break;
            }
        }

        double tret;
        int ret = IDASolve(itg.ida_mem, tout, &tret,
                           itg.nvec_y, itg.nvec_ydot, IDA_NORMAL);
        if (ret < 0) { log_error("IDA failed at t=%.6f (ret=%d)", t, ret); break; }
        t = tret;

        int events_fired = 0;
        while (ev_idx < n_events && events[ev_idx].time <= t + 1e-10) {
            if (ofile && (events[ev_idx].type == FAULT || events[ev_idx].type == FAULT_SLG
                || events[ev_idx].type == FAULT_LL || events[ev_idx].type == FAULT_DLG)) {
                double *ypre = N_VGetArrayPointer(itg.nvec_y);
                double Vr = ypre[ndiff + 2*osc_idx], Vi = ypre[ndiff + 2*osc_idx + 1];
                double wr = 2.0 * M_PI * 60.0;
                Vdc_a = Vr * cos(wr * t) - Vi * sin(wr * t);
                Vdc_b = Vr * cos(wr * t - 2.0943951024) - Vi * sin(wr * t - 2.0943951024);
                Vdc_c = Vr * cos(wr * t + 2.0943951024) - Vi * sin(wr * t + 2.0943951024);
            }
            if (ofile && events[ev_idx].type == FAULT_CLEAR)
                { Vdc_a = 0; Vdc_b = 0; Vdc_c = 0; }
            events_apply(&sys, &events[ev_idx], t);
            ev_idx++; events_fired = 1;
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
                    double Vr = ydata[ndiff + 2*bi], Vi = ydata[ndiff + 2*bi+1];
                    double d = ydata[2*m], Ep = mc->Ep, xdp = mc->xdp;
                    double Pe = Vr*(Ep*sin(d)-Vi)/xdp + Vi*(-(Ep*cos(d)-Vr))/xdp;
                    double TwoH = 2.0 * mc->h;
                    double om = ydata[2*m+1];
                    if (om < 0.1 || om > 2.0) om = 1.0;
                    ypd[2*m]   = ws * (om - 1.0);
                    ypd[2*m+1] = ws / TwoH * (gen->pg - Pe - mc->d*(om-1.0));
                }
                double *y_save = malloc((size_t)neq * sizeof(double));
                memcpy(y_save, ydata, (size_t)neq * sizeof(double));
                int icr = IDACalcIC(itg.ida_mem, IDA_Y_INIT, 0.01);
                if (icr != 0) {
                    memcpy(ydata, y_save, (size_t)neq * sizeof(double));
                    memset(ypd, 0, (size_t)neq * sizeof(double));
                    for (int m = 0; m < sys.nmachines; m++) {
                        Machine *mc = &sys.machine[m];
                        Gen *gen = &sys.gen[mc->gen_idx];
                        int bi = gen->bus;
                        double Vr = ydata[ndiff + 2*bi], Vi = ydata[ndiff + 2*bi+1];
                        double d = ydata[2*m], Ep = mc->Ep, xdp = mc->xdp;
                        double Pe = Vr*(Ep*sin(d)-Vi)/xdp + Vi*(-(Ep*cos(d)-Vr))/xdp;
                        double TwoH = 2.0 * mc->h;
                        double om = ydata[2*m+1];
                        if (om < 0.1 || om > 2.0) om = 1.0;
                        ypd[2*m]   = ws * (om - 1.0);
                        ypd[2*m+1] = ws / TwoH * (gen->pg - Pe - mc->d*(om-1.0));
                    }
                    log_warn("EVENT: IDACalcIC failed (%d), restored clean state", icr);
                }
                free(y_save);
                IDASetMaxOrd(itg.ida_mem, 1);
                int rrc = IDAReInit(itg.ida_mem, t, itg.nvec_y, itg.nvec_ydot);
                IDASStolerances(itg.ida_mem, 1e-4, 1e-6);
                if (rrc == 0) log_info("EVENT: IDA re-initialized at t=%.4f", t);
                else          log_warn("EVENT: IDAReInit returned %d", rrc);
            }
            free(Vtmp);
        }

        /* output */
        {
            double *y = N_VGetArrayPointer(itg.nvec_y);
            double ws = 2.0 * M_PI * 60.0;

            if (vfile) {
                fprintf(vfile, "%.6f", t);
                for (int i = 0; i < sys.nbus; i++) {
                    double Vr = y[ndiff + 2*i], Vi = y[ndiff + 2*i + 1];
                    double Vm = sqrt(Vr*Vr + Vi*Vi);
                    double Va = atan2(Vi, Vr) * 180.0 / M_PI;
                    fprintf(vfile, ",%.12g,%.12g", Vm, Va);
                    if (Vm < bstat[i].v_min) bstat[i].v_min = Vm;
                    if (Vm > bstat[i].v_max) bstat[i].v_max = Vm;
                    bstat[i].v_sum += Vm; bstat[i].samples++;
                }
                fprintf(vfile, "\n");
            }
            if (dfile) {
                fprintf(dfile, "%.6f", t);
                for (int m = 0; m < sys.nmachines; m++) {
                    Machine *mc = &sys.machine[m];
                    Gen *gen = &sys.gen[mc->gen_idx];
                    int bi = gen->bus;
                    double d = y[2*m], om = y[2*m+1];
                    double Vr = y[ndiff + 2*bi], Vi = y[ndiff + 2*bi + 1];
                    double Ep = mc->Ep, xdp = mc->xdp;
                    double Pe = Vr*(Ep*sin(d)-Vi)/xdp + Vi*(-(Ep*cos(d)-Vr))/xdp;
                    double Pm = gen->pg;
                    double Vt = sqrt(Vr*Vr + Vi*Vi);
                    fprintf(dfile, ",%.12g,%.12g,%.12g", d, Pe, Vt);
                    if (d < gstat[m].d_min) gstat[m].d_min = d;
                    if (d > gstat[m].d_max) gstat[m].d_max = d;
                    if (om < gstat[m].w_min) gstat[m].w_min = om;
                    if (om > gstat[m].w_max) gstat[m].w_max = om;
                    if (Pe < gstat[m].pe_min) gstat[m].pe_min = Pe;
                    if (Pe > gstat[m].pe_max) gstat[m].pe_max = Pe;
                    gstat[m].samples++;
                }
                fprintf(dfile, "\n");
            }

            /* 3-phase oscillogram at 1 kHz */
            if (ofile) {
                double wr = 2.0 * M_PI * 60.0;
                double Vr_n = y[ndiff + 2*osc_idx];
                double Vi_n = y[ndiff + 2*osc_idx + 1];
                double a2r=-0.5, a2i=-0.86602540378, ar=-0.5, ai=0.86602540378;

                double Var = Vr_n, Vai = Vi_n;
                double Vbr = a2r*Vr_n - a2i*Vi_n, Vbi = a2r*Vi_n + a2i*Vr_n;
                double Vcr = ar*Vr_n - ai*Vi_n, Vci = ar*Vi_n + ai*Vr_n;

                if (sys.fault_bus >= 0 && sys.fault_type > 0 && osc_idx == sys.fault_bus) {
                    double dVr = sys.fault_Vth_r - Vr_n;
                    double dVi = sys.fault_Vth_i - Vi_n;
                    double Ztr = sys.fault_Zth_r, Zti = sys.fault_Zth_i;
                    double den = Ztr*Ztr + Zti*Zti;
                    if (den > 1e-20) {
                        double I1r = (dVr*Ztr + dVi*Zti)/den, I1i = (dVi*Ztr - dVr*Zti)/den;
                        double I2r=0,I2i=0,I0r=0,I0i=0;
                        if (sys.fault_type == 1) { I2r=I1r; I2i=I1i; I0r=I1r; I0i=I1i; }
                        else if (sys.fault_type == 2) { I2r=-I1r; I2i=-I1i; }
                        else if (sys.fault_type == 3) { I2r=-0.5*I1r; I2i=-0.5*I1i; I0r=-0.5*I1r; I0i=-0.5*I1i; }
                        double V2r = -(Ztr*I2r-Zti*I2i), V2i = -(Ztr*I2i+Zti*I2r);
                        double V0r = -(Ztr*I0r-Zti*I0i), V0i = -(Ztr*I0i+Zti*I0r);
                        Var=Vr_n+V2r+V0r; Vai=Vi_n+V2i+V0i;
                        Vbr=(a2r*Vr_n-a2i*Vi_n)+(ar*V2r-ai*V2i)+V0r; Vbi=(a2r*Vi_n+a2i*Vr_n)+(ar*V2i+ai*V2r)+V0i;
                        Vcr=(ar*Vr_n-ai*Vi_n)+(a2r*V2r-a2i*V2i)+V0r; Vci=(ar*Vi_n+ai*Vr_n)+(a2r*V2i+a2i*V2r)+V0i;
                    }
                }

                for (double ts = t_prev; ts < t - 1e-10; ts += 0.001) {
                    double alpha = (ts - t_prev) / (t - t_prev + 1e-20);
                    double var = Va_pr*(1-alpha) + Var*alpha, vai = Va_pi*(1-alpha) + Vai*alpha;
                    double vbr = Vb_pr*(1-alpha) + Vbr*alpha, vbi = Vb_pi*(1-alpha) + Vbi*alpha;
                    double vcr = Vc_pr*(1-alpha) + Vcr*alpha, vci = Vc_pi*(1-alpha) + Vci*alpha;

                    double va = var*cos(wr*ts) - vai*sin(wr*ts);
                    double vb = vbr*cos(wr*ts) - vbi*sin(wr*ts);
                    double vc = vcr*cos(wr*ts) - vci*sin(wr*ts);

                    if (sys.fault_t0 > 0 && ts >= sys.fault_t0 - 1e-10) {
                        double tau = sys.fault_XoR / (2.0*M_PI*60.0);
                        if (tau < 0.001) tau = 0.005;
                        double dcy = exp(-(ts - sys.fault_t0) / tau);
                        va -= Vdc_a * dcy; vb -= Vdc_b * dcy; vc -= Vdc_c * dcy;
                    }
                    if (sys.fault_clear_t > 0 && ts >= sys.fault_clear_t - 1e-10) {
                        double Vm = sqrt(var*var + vai*vai);
                        double he = exp(-(ts - sys.fault_clear_t) / 0.3);
                        va += 0.08*Vm*he*cos(3.0*wr*ts) + 0.04*Vm*he*cos(5.0*wr*ts);
                        vb += 0.08*Vm*he*cos(3.0*(wr*ts-2.0943951024)) + 0.04*Vm*he*cos(5.0*(wr*ts-2.0943951024));
                        vc += 0.08*Vm*he*cos(3.0*(wr*ts+2.0943951024)) + 0.04*Vm*he*cos(5.0*(wr*ts+2.0943951024));
                    }
                    fprintf(ofile, "%.6f,%.6f,%.6f,%.6f\n", ts, va, vb, vc);
                }
                Va_pr = Var; Va_pi = Vai; Vb_pr = Vbr; Vb_pi = Vbi; Vc_pr = Vcr; Vc_pi = Vci;
                Vr_prev = Vr_n; Vi_prev = Vi_n; t_prev = t;
            }
        }

        step++;
        while (t_next <= t + 1e-10) t_next += t_step;
        if (step % 200 == 0) log_info("t=%.3f/%.1f steps=%d", t, t_end, step);
    }

    log_info("Done: t=%.4f steps=%d", t, step);

    /* post-simulation summary */
    printf("\n--- SIMULATION SUMMARY ---\n");
    printf("Duration: %.3fs | Steps: %d\n", t, step);

    printf("\nBus voltage extremes:\n");
    for (int i = 0; i < sys.nbus; i++) {
        if (bstat[i].samples == 0) continue;
        printf("  Bus %2d (%s): Vmin=%.4f Vmax=%.4f Vavg=%.4f\n",
               sys.bus[i].id, bus_type_name(sys.bus[i].type),
               bstat[i].v_min, bstat[i].v_max,
               bstat[i].v_sum / bstat[i].samples);
    }

    printf("\nGenerator extremes:\n");
    for (int m = 0; m < sys.nmachines; m++) {
        int bid = sys.bus[sys.gen[sys.machine[m].gen_idx].bus].id;
        printf("  Gen@Bus %d: d_min=%.2f° d_max=%.2f°  w_min=%.4f w_max=%.4f  Pe: %.2f..%.2f\n",
               bid,
               gstat[m].d_min * 180.0/M_PI, gstat[m].d_max * 180.0/M_PI,
               gstat[m].w_min, gstat[m].w_max,
               gstat[m].pe_min, gstat[m].pe_max);
    }
    printf("\n");

    if (dfile) fclose(dfile);
    if (vfile) fclose(vfile);
    if (ofile) fclose(ofile);

    /* plot generation */
    if (do_plot) {
        char buf[2048]; FILE *gp;

        snprintf(buf, sizeof(buf), "%s/delta.gp", output_dir);
        gp = fopen(buf, "w");
        if (gp) {
            fprintf(gp, "set terminal pngcairo size 1200,800\n"
                "set datafile separator ','\n"
                "set output '%s/delta.png'\n"
                "set xlabel 'Time (s)'\nset ylabel 'Rotor Angle (rad) / Power (pu)'\n"
                "set title 'Generator Dynamics'\n"
                "plot '%s/delta.csv' using 1:2 with lines title 'd_g1'",
                output_dir, output_dir);
            for (int m = 1; m < sys.nmachines; m++)
                fprintf(gp, ", '' using 1:%d with lines title 'd_g%d'",
                        3*m+2, sys.bus[sys.gen[sys.machine[m].gen_idx].bus].id);
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
                fprintf(gp, ", '' using 1:%d with lines title 'Bus %d'",
                        2*i+2, sys.bus[i].id);
            fprintf(gp, "\n"); fclose(gp);
        }

        /* oscillogram plot */
        if (osc_idx >= 0) {
            snprintf(buf, sizeof(buf), "%s/osc.gp", output_dir);
            gp = fopen(buf, "w");
            if (gp) {
                fprintf(gp, "set terminal pngcairo size 1400,1000\n"
                    "set datafile separator ','\n"
                    "set output '%s/osc.png'\n"
                    "set xlabel 'Time (s)'\nset ylabel 'Instantaneous Voltage (pu)'\n"
                    "set title '3-Phase Oscillogram — Bus %d'\n"
                    "set multiplot layout 3,1\n"
                    "plot '%s/osc_bus%d.csv' using 1:2 with lines title 'Va'\n"
                    "plot '%s/osc_bus%d.csv' using 1:3 with lines title 'Vb'\n"
                    "plot '%s/osc_bus%d.csv' using 1:4 with lines title 'Vc'\n"
                    "unset multiplot\n",
                    output_dir, osc_bus, output_dir, osc_bus,
                    output_dir, osc_bus, output_dir, osc_bus);
                fclose(gp);
            }
        }

        if (osc_idx >= 0)
            snprintf(buf, sizeof(buf), "gnuplot %s/delta.gp %s/voltage.gp %s/osc.gp 2>/dev/null",
                     output_dir, output_dir, output_dir);
        else
            snprintf(buf, sizeof(buf), "gnuplot %s/delta.gp %s/voltage.gp 2>/dev/null",
                     output_dir, output_dir);
        system(buf);
    }

    free(bstat); free(gstat);
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
