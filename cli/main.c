#include "transient.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mpi.h>
#include <nvector/nvector_serial.h>
#include <ida/ida.h>
#include <getopt.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* symmetrical-component rotation operators: a = e^(j*120°) */
#define A2R  -0.5
#define A2I  -0.8660254037844386
#define AR   -0.5
#define AI   0.8660254037844386

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
        "  --real-time       Sync simulation to wall-clock time\n"
        "\n"
        "=== Output ===\n"
        "  --output DIR      Output directory (default: ./results)\n"
        "  --osc BUS         3-phase oscillogram at 1 kHz\n"
        "  --comtrade BUS    3-phase COMTRADE (IEEE C37.111) with real-world units\n"
        "  --plot            Generate gnuplot PNGs\n"
        "  --res WxH         Plot resolution (default: 2400x1600)\n"
        "\n"
        "=== Inspection (no simulation) ===\n"
        "  --info            Print network topology report\n"
        "  --diagram         Print ASCII one-line network diagram\n"
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

static void rotate_3ph(double *va_r, double *va_i, double *vb_r, double *vb_i,
                        double *vc_r, double *vc_i, int fault_phase)
{
    (void)va_r; (void)va_i;
    if (fault_phase == FAULT_PHASE_ABC || fault_phase == FAULT_PHASE_BC ||
        fault_phase == FAULT_PHASE_AG) return;
    double tr = *va_r, ti = *va_i;
    double br = *vb_r, bi = *vb_i;
    double cr = *vc_r, ci = *vc_i;
    switch (fault_phase) {
    case FAULT_PHASE_AB:
    case FAULT_PHASE_CG:
        *va_r = br; *va_i = bi; *vb_r = cr; *vb_i = ci; *vc_r = tr; *vc_i = ti; break;
    case FAULT_PHASE_CA:
    case FAULT_PHASE_BG:
        *va_r = cr; *va_i = ci; *vb_r = tr; *vb_i = ti; *vc_r = br; *vc_i = bi; break;
    }
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
    int do_plot = 0, osc_bus = -1, do_info = 0, do_diagram = 0;
    int comtrade_bus = -1;
    int realtime = 0;
    int plot_w = 2400, plot_h = 1600;

    static struct option long_opts[] = {
        {"raw",    required_argument, 0, 'r'},
        {"dyr",    required_argument, 0, 'd'},
        {"t-end",  required_argument, 0, 'T'},
        {"t-step", required_argument, 0, 's'},
        {"events", required_argument, 0, 'e'},
        {"output", required_argument, 0, 'o'},
        {"osc",    required_argument, 0, 'O'},
        {"comtrade", required_argument, 0, 'C'},
        {"plot",   no_argument,       0, 'p'},
        {"res",    required_argument, 0, 'R'},
        {"real-time", no_argument,    0, 'Z'},
        {"info",   no_argument,       0, 'I'},
        {"diagram",no_argument,       0, 'D'},
        {"topo",   required_argument, 0, 'G'},
        {"help",   no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "r:d:T:s:e:o:O:C:R:Z:I:D:G:ph", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'r': raw_file    = optarg; break;
        case 'd': dyr_file    = optarg; break;
        case 'T': t_end       = atof(optarg); break;
        case 's': t_step      = atof(optarg); break;
        case 'e': events_file = optarg; break;
        case 'o': output_dir  = optarg; break;
        case 'O': osc_bus     = atoi(optarg); break;
        case 'C': comtrade_bus = atoi(optarg); break;
        case 'R': sscanf(optarg, "%dx%d", &plot_w, &plot_h); break;
        case 'Z': realtime    = 1; break;
        case 'I': do_info     = 1; break;
        case 'D': do_diagram  = 1; break;
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

    /* --diagram: print one-line diagram and exit */
    if (do_diagram) { print_network_diagram(&sys); MPI_Finalize(); return 0; }

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

    int ncols_d = 2 * sys.nmachines + 1; /* 2 per machine (d, w) */

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

    /* COMTRADE output */
    ComtradeWriter *ctw = NULL;
    int ct_idx = -1;
    if (comtrade_bus > 0) {
        for (int i = 0; i < sys.nbus; i++)
            if (sys.bus[i].id == comtrade_bus) { ct_idx = i; break; }
        if (ct_idx >= 0) {
            ctw = comtrade_create(sys.bus[ct_idx].name, "TSS");
            double v_factor = comtrade_v_factor(sys.bus[ct_idx].base_kv);
            double i_factor = comtrade_i_factor(sys.base_mva, sys.bus[ct_idx].base_kv);
            double v_prim = sys.bus[ct_idx].base_kv * 1000.0;
            double i_prim_a = sys.base_mva * 1000.0 / (sqrt(3.0) * sys.bus[ct_idx].base_kv);
            double min_v = -2.5, max_v = 2.5;
            double min_i = -10.0, max_i = 10.0;
            char id[32]; int bid = comtrade_bus;
            snprintf(id, sizeof(id), "V%da", bid); comtrade_add_analog(ctw, id, "A", sys.bus[ct_idx].name, "kV", v_factor, 0, min_v, max_v, v_prim, 100.0, 0);
            snprintf(id, sizeof(id), "V%db", bid); comtrade_add_analog(ctw, id, "B", sys.bus[ct_idx].name, "kV", v_factor, 0, min_v, max_v, v_prim, 100.0, 0);
            snprintf(id, sizeof(id), "V%dc", bid); comtrade_add_analog(ctw, id, "C", sys.bus[ct_idx].name, "kV", v_factor, 0, min_v, max_v, v_prim, 100.0, 0);
            snprintf(id, sizeof(id), "I%da", bid); comtrade_add_analog(ctw, id, "A", sys.bus[ct_idx].name, "kA", i_factor, 0, min_i, max_i, i_prim_a, 5.0, 0);
            snprintf(id, sizeof(id), "I%db", bid); comtrade_add_analog(ctw, id, "B", sys.bus[ct_idx].name, "kA", i_factor, 0, min_i, max_i, i_prim_a, 5.0, 0);
            snprintf(id, sizeof(id), "I%dc", bid); comtrade_add_analog(ctw, id, "C", sys.bus[ct_idx].name, "kA", i_factor, 0, min_i, max_i, i_prim_a, 5.0, 0);
            char cpath[1024], dpath[1024];
            snprintf(cpath, sizeof(cpath), "%s/comtrade_bus%d.cfg", output_dir, comtrade_bus);
            snprintf(dpath, sizeof(dpath), "%s/comtrade_bus%d.dat", output_dir, comtrade_bus);
            comtrade_open_dat(ctw, dpath);
            comtrade_set_time(ctw, 2026,1,1,0,0,0,0, 2026,1,1,0,0,0,0);
            ctw->sample_rate = 1000;
        } else log_warn("comtrade bus %d not found", comtrade_bus);
    }

    double t = 0.0, t_next = t_step, t_prev = 0.0;
    double Vr_prev = 0, Vi_prev = 0;
    double Ir_prev = 0, Ii_prev = 0;
    double Va_pr = 0, Va_pi = 0, Vb_pr = 0, Vb_pi = 0, Vc_pr = 0, Vc_pi = 0;
    double Ia_pr = 0, Ia_pi = 0, Ib_pr = 0, Ib_pi = 0, Ic_pr = 0, Ic_pi = 0;
    double Vdc_a = 0, Vdc_b = 0, Vdc_c = 0;
    double Idc_a = 0, Idc_b = 0, Idc_c = 0;
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

    if (osc_idx >= 0 || ct_idx >= 0) {
        double *y0 = N_VGetArrayPointer(itg.nvec_y);
        int idx = (osc_idx >= 0) ? osc_idx : ct_idx;
        Vr_prev = y0[ndiff + 2*idx];
        Vi_prev = y0[ndiff + 2*idx + 1];
        Va_pr = Vr_prev; Va_pi = Vi_prev;
        Vb_pr = A2R*Vr_prev - A2I*Vi_prev; Vb_pi = A2R*Vi_prev + A2I*Vr_prev;
        Vc_pr = AR*Vr_prev - AI*Vi_prev;   Vc_pi = AR*Vi_prev + AI*Vr_prev;
    }
    if (ct_idx >= 0) {
        double *y0 = N_VGetArrayPointer(itg.nvec_y);
        int bi = ct_idx;
        Ir_prev = 0; Ii_prev = 0;
        for (int k = sys.colptr[bi]; k < sys.colptr[bi+1]; k++) {
            int j = sys.rowidx[k];
            double Gij = sys.yval[2*k], Bij = sys.yval[2*k+1];
            double Vr_j = y0[ndiff + 2*j], Vi_j = y0[ndiff + 2*j+1];
            Ir_prev += Gij*Vr_j - Bij*Vi_j;
            Ii_prev += Gij*Vi_j + Bij*Vr_j;
        }
        Ia_pr = Ir_prev; Ia_pi = Ii_prev;
        Ib_pr = A2R*Ir_prev - A2I*Ii_prev; Ib_pi = A2R*Ii_prev + A2I*Ir_prev;
        Ic_pr = AR*Ir_prev - AI*Ii_prev;   Ic_pi = AR*Ii_prev + AI*Ir_prev;
    }

    log_info("Simulating: t_end=%.1f t_step=%.3f%s",
             t_end, t_step, realtime ? " [REAL-TIME]" : "");

    double t_rt = 0.0;  /* last simulated time for real-time tracking */

    while (t < t_end - 1e-10) {
        struct timespec step_start;
        if (realtime) clock_gettime(CLOCK_MONOTONIC, &step_start);

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
            if ((ofile || ctw) && (events[ev_idx].type == FAULT || events[ev_idx].type == FAULT_SLG
                || events[ev_idx].type == FAULT_LL || events[ev_idx].type == FAULT_DLG)) {
                double *ypre = N_VGetArrayPointer(itg.nvec_y);
                int vidx = osc_idx >= 0 ? osc_idx : (ct_idx >= 0 ? ct_idx : 0);
                double Vr = ypre[ndiff + 2*vidx], Vi = ypre[ndiff + 2*vidx + 1];
                double wr = 2.0 * M_PI * 60.0;
                Vdc_a = Vr * cos(wr * t) - Vi * sin(wr * t);
                Vdc_b = Vr * cos(wr * t - 2.0943951024) - Vi * sin(wr * t - 2.0943951024);
                Vdc_c = Vr * cos(wr * t + 2.0943951024) - Vi * sin(wr * t + 2.0943951024);
                if (ct_idx >= 0) {
                    int bi = ct_idx;
                    double Ir = 0, Ii = 0;
                    for (int k = sys.colptr[bi]; k < sys.colptr[bi+1]; k++) {
                        int j = sys.rowidx[k];
                        double Gij = sys.yval[2*k], Bij = sys.yval[2*k+1];
                        double Vr_j = ypre[ndiff + 2*j], Vi_j = ypre[ndiff + 2*j+1];
                        Ir += Gij*Vr_j - Bij*Vi_j;
                        Ii += Gij*Vi_j + Bij*Vr_j;
                    }
                    Idc_a = Ir * cos(wr * t) - Ii * sin(wr * t);
                    Idc_b = Ir * cos(wr * t - 2.0943951024) - Ii * sin(wr * t - 2.0943951024);
                    Idc_c = Ir * cos(wr * t + 2.0943951024) - Ii * sin(wr * t + 2.0943951024);
                }
            }
            if ((ofile || ctw) && events[ev_idx].type == FAULT_CLEAR)
                { Vdc_a = 0; Vdc_b = 0; Vdc_c = 0; Idc_a = 0; Idc_b = 0; Idc_c = 0; }
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
                    if (om < 0.1 || om > 2.0) {
                        log_warn("omega clamped from %.4f to 1.0 at gen %d", om, m);
                        om = 1.0;
                    }
                    ypd[2*m]   = ws * (om - 1.0);
                    ypd[2*m+1] = ws / TwoH * (gen->pg - Pe - mc->d*(om-1.0));
                }
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

            /* 3-phase oscillogram + COMTRADE at 1 kHz */
            if (ofile || ctw) {
                double wr = 2.0 * M_PI * 60.0;
                int vidx = osc_idx >= 0 ? osc_idx : (ct_idx >= 0 ? ct_idx : 0);
                double Vr_n = y[ndiff + 2*vidx];
                double Vi_n = y[ndiff + 2*vidx + 1];

                double Ir_n = 0, Ii_n = 0;
                if (ct_idx >= 0) {
                    int bi = ct_idx;
                    for (int k = sys.colptr[bi]; k < sys.colptr[bi+1]; k++) {
                        int j = sys.rowidx[k];
                        double Gij = sys.yval[2*k], Bij = sys.yval[2*k+1];
                        double Vr_j = y[ndiff + 2*j], Vi_j = y[ndiff + 2*j+1];
                        Ir_n += Gij*Vr_j - Bij*Vi_j;
                        Ii_n += Gij*Vi_j + Bij*Vr_j;
                    }
                }

                double a2r=-0.5, a2i=-0.86602540378, ar=-0.5, ai=0.86602540378;

                double Var = Vr_n, Vai = Vi_n;
                double Vbr = a2r*Vr_n - a2i*Vi_n, Vbi = a2r*Vi_n + a2i*Vr_n;
                double Vcr = ar*Vr_n - ai*Vi_n, Vci = ar*Vi_n + ai*Vr_n;

                double Iar = Ir_n, Iai = Ii_n;
                double Ibr = a2r*Ir_n - a2i*Ii_n, Ibi = a2r*Ii_n + a2i*Ir_n;
                double Icr = ar*Ir_n - ai*Ii_n, Ici = ar*Ii_n + ai*Ir_n;

                if (sys.fault_bus >= 0 && sys.fault_type > 0 && vidx == sys.fault_bus) {
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
                        double I_f2r = I2r, I_f2i = I2i, I_f0r = I0r, I_f0i = I0i;
                        Var=Vr_n+V2r+V0r; Vai=Vi_n+V2i+V0i;
                        Vbr=(a2r*Vr_n-a2i*Vi_n)+(ar*V2r-ai*V2i)+V0r; Vbi=(a2r*Vi_n+a2i*Vr_n)+(ar*V2i+ai*V2r)+V0i;
                        Vcr=(ar*Vr_n-ai*Vi_n)+(a2r*V2r-a2i*V2i)+V0r; Vci=(ar*Vi_n+ai*Vr_n)+(a2r*V2i+a2i*V2r)+V0i;
                        Iar=Ir_n+I_f2r+I_f0r; Iai=Ii_n+I_f2i+I_f0i;
                        Ibr=(a2r*Ir_n-a2i*Ii_n)+(ar*I_f2r-ai*I_f2i)+I_f0r; Ibi=(a2r*Ii_n+a2i*Ir_n)+(ar*I_f2i+ai*I_f2r)+I_f0i;
                        Icr=(ar*Ir_n-ai*Ii_n)+(a2r*I_f2r-a2i*I_f2i)+I_f0r; Ici=(ar*Ii_n+ai*Ir_n)+(a2r*I_f2i+a2i*I_f2r)+I_f0i;
                        rotate_3ph(&Var,&Vai,&Vbr,&Vbi,&Vcr,&Vci, sys.fault_phase);
                        rotate_3ph(&Iar,&Iai,&Ibr,&Ibi,&Icr,&Ici, sys.fault_phase);
                    }
                }

                for (double ts = t_prev; ts < t - 1e-10; ts += 0.001) {
                    double alpha = (ts - t_prev) / (t - t_prev + 1e-20);
                    double var = Va_pr*(1-alpha) + Var*alpha, vai = Va_pi*(1-alpha) + Vai*alpha;
                    double vbr = Vb_pr*(1-alpha) + Vbr*alpha, vbi = Vb_pi*(1-alpha) + Vbi*alpha;
                    double vcr = Vc_pr*(1-alpha) + Vcr*alpha, vci = Vc_pi*(1-alpha) + Vci*alpha;
                    double iar = Ia_pr*(1-alpha) + Iar*alpha, iai = Ia_pi*(1-alpha) + Iai*alpha;
                    double ibr = Ib_pr*(1-alpha) + Ibr*alpha, ibi = Ib_pi*(1-alpha) + Ibi*alpha;
                    double icr = Ic_pr*(1-alpha) + Icr*alpha, ici = Ic_pi*(1-alpha) + Ici*alpha;

                    double va = var*cos(wr*ts) - vai*sin(wr*ts);
                    double vb = vbr*cos(wr*ts) - vbi*sin(wr*ts);
                    double vc = vcr*cos(wr*ts) - vci*sin(wr*ts);
                    double ia = iar*cos(wr*ts) - iai*sin(wr*ts);
                    double ib = ibr*cos(wr*ts) - ibi*sin(wr*ts);
                    double ic = icr*cos(wr*ts) - ici*sin(wr*ts);

                    if (sys.fault_t0 > 0 && ts >= sys.fault_t0 - 1e-10) {
                        double tau = sys.fault_XoR / (2.0*M_PI*60.0);
                        if (tau < 0.001) tau = 0.005;
                        double dcy = exp(-(ts - sys.fault_t0) / tau);
                        va -= Vdc_a * dcy; vb -= Vdc_b * dcy; vc -= Vdc_c * dcy;
                        ia -= Idc_a * dcy; ib -= Idc_b * dcy; ic -= Idc_c * dcy;
                    }
                    if (ofile) fprintf(ofile, "%.6f,%.6f,%.6f,%.6f\n", ts, va, vb, vc);
                    if (ctw) {
                        double av[6] = {va, vb, vc, ia, ib, ic};
                        comtrade_write_ascii(ctw, ts, av, NULL);
                    }
                }
                Va_pr = Var; Va_pi = Vai; Vb_pr = Vbr; Vb_pi = Vbi; Vc_pr = Vcr; Vc_pi = Vci;
                Ia_pr = Iar; Ia_pi = Iai; Ib_pr = Ibr; Ib_pi = Ibi; Ic_pr = Icr; Ic_pi = Ici;
                Vr_prev = Vr_n; Vi_prev = Vi_n; Ir_prev = Ir_n; Ii_prev = Ii_n; t_prev = t;
            }
        }

        step++;
        while (t_next <= t + 1e-10) t_next += t_step;

        /* progress bar + real-time sync + live data */
        {
            double pct = (t / t_end) * 100.0;

            if (realtime) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double step_wall = (now.tv_sec - step_start.tv_sec)
                                 + (now.tv_nsec - step_start.tv_nsec) * 1e-9;
                double lag = (t - t_rt) - step_wall;
                if (lag > 0.0001) usleep((useconds_t)(lag * 1e6));
                t_rt = t;
            }

            int bar_w = 30, filled = (int)(pct * bar_w / 100.0);
            fprintf(stderr, "\r\033[K[");
            for (int b = 0; b < bar_w; b++) fputc(b < filled ? '=' : (b == filled ? '>' : ' '), stderr);
            fprintf(stderr, "] %5.1f%% t=%.2fs %s",
                    pct, t, realtime ? "[RT]" : "");

            /* live data in real-time mode */
            if (realtime && step > 0) {
                double *y = N_VGetArrayPointer(itg.nvec_y);
                fprintf(stderr, "  |");
                /* show V at first 4 buses + fault bus */
                for (int i = 0; i < sys.nbus && i < 5; i++) {
                    double Vr = y[ndiff + 2*i], Vi = y[ndiff + 2*i + 1];
                    fprintf(stderr, " V%d=%.3f", sys.bus[i].id, sqrt(Vr*Vr + Vi*Vi));
                }
                if (sys.fault_bus >= 0 && sys.fault_bus >= 5)
                    fprintf(stderr, " FB%d=%.3f", sys.bus[sys.fault_bus].id,
                            sqrt(y[ndiff+2*sys.fault_bus]*y[ndiff+2*sys.fault_bus]
                               + y[ndiff+2*sys.fault_bus+1]*y[ndiff+2*sys.fault_bus+1]));
                fprintf(stderr, "  |");
                /* show gen deltas */
                for (int m = 0; m < sys.nmachines && m < 4; m++) {
                    int bid = sys.bus[sys.gen[sys.machine[m].gen_idx].bus].id;
                    fprintf(stderr, " G%d:%.1f°", bid, y[2*m] * 180.0/M_PI);
                }
            }
            fprintf(stderr, "   ");
            fflush(stderr);
        }
    }

    fprintf(stderr, "\r[==============================] 100.0%%  Done: t=%.4f steps=%d\n\n", t, step);

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

    if (ctw) {
        double trig_t = sys.fault_t0 > 0 ? sys.fault_t0 : t * 0.5;
        int tsec = (int)trig_t;
        int tusec = (int)((trig_t - tsec) * 1e6);
        int thh = tsec / 3600, tmm = (tsec / 60) % 60, tsss = tsec % 60;
        comtrade_set_time(ctw, 2026,1,1,0,0,0,0, 2026,1,1,thh,tmm,tsss,tusec);
        char cpath[1024];
        snprintf(cpath, sizeof(cpath), "%s/comtrade_bus%d.cfg", output_dir, comtrade_bus);
        comtrade_write_cfg(ctw, cpath);
        comtrade_close(ctw);
        log_info("COMTRADE: written to %s", cpath);
    }

    /* plot generation */
    if (do_plot) {
        char buf[2048]; FILE *gp;

        snprintf(buf, sizeof(buf), "%s/delta.gp", output_dir);
        gp = fopen(buf, "w");
        if (gp) {
            fprintf(gp, "set terminal pngcairo size %d,%d\n"
                "set datafile separator ','\n"
                "set output '%s/delta.png'\n"
                "set xlabel 'Time (s)'\nset ylabel 'Rotor Angle (rad) / Power (pu)'\n"
                "set title 'Generator Dynamics'\n"
                "plot '%s/delta.csv' using 1:2 with lines title 'd_g1'",
                plot_w, plot_h, output_dir, output_dir);
            for (int m = 1; m < sys.nmachines; m++)
                fprintf(gp, ", '' using 1:%d with lines title 'd_g%d'",
                        3*m+2, sys.bus[sys.gen[sys.machine[m].gen_idx].bus].id);
            fprintf(gp, "\n"); fclose(gp);
        }

        snprintf(buf, sizeof(buf), "%s/voltage.gp", output_dir);
        gp = fopen(buf, "w");
        if (gp) {
            fprintf(gp, "set terminal pngcairo size %d,%d\n"
                "set datafile separator ','\n"
                "set output '%s/voltage.png'\n"
                "set xlabel 'Time (s)'\nset ylabel 'Voltage Magnitude (pu)'\n"
                "set title 'Bus Voltages'\n"
                "plot '%s/voltage.csv' using 1:2 with lines title 'Bus 1'",
                plot_w, plot_h, output_dir, output_dir);
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
                fprintf(gp, "set terminal pngcairo size %d,%d\n"
                    "set datafile separator ','\n"
                    "set output '%s/osc.png'\n"
                    "set xlabel 'Time (s)'\nset ylabel 'Instantaneous Voltage (pu)'\n"
                    "set title '3-Phase Oscillogram — Bus %d'\n"
                    "set multiplot layout 3,1\n"
                    "plot '%s/osc_bus%d.csv' using 1:2 with lines title 'Va'\n"
                    "plot '%s/osc_bus%d.csv' using 1:3 with lines title 'Vb'\n"
                    "plot '%s/osc_bus%d.csv' using 1:4 with lines title 'Vc'\n"
                    "unset multiplot\n",
                    plot_w, plot_h * 3 / 2, output_dir, osc_bus,
                    output_dir, osc_bus,
                    output_dir, osc_bus,
                    output_dir, osc_bus);
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
