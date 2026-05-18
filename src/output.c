#include "types.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

const char *bus_type_name(BusType t)
{
    switch (t) {
    case BUS_PQ:       return "PQ";
    case BUS_PV:       return "PV";
    case BUS_SLACK:    return "SLACK";
    case BUS_ISOLATED: return "ISOL";
    default:           return "?";
    }
}

void print_network_info(const System *sys)
{
    printf("\n========== NETWORK TOPOLOGY ==========\n");
    printf("Base MVA: %.1f\n", sys->base_mva);
    printf("Buses: %d | Branches: %d | Generators: %d | Loads: %d | Machines: %d\n\n",
           sys->nbus, sys->nbranch, sys->ngen, sys->nload, sys->nmachines);

    /* --- Buses --- */
    printf("--- Buses ---\n");
    printf("%4s %-12s %-6s %8s %8s %8s %8s %8s  %s\n",
           "ID", "Name", "Type", "kV", "V(pu)", "Ang°", "Pd", "Qd", "Gen");
    for (int i = 0; i < sys->nbus; i++) {
        const Bus *b = &sys->bus[i];
        char gen_info[32] = "";
        for (int g = 0; g < sys->ngen; g++)
            if (sys->gen[g].bus == i)
                snprintf(gen_info + strlen(gen_info),
                         sizeof(gen_info) - strlen(gen_info),
                         "%sG%d(%.0fMW)", gen_info[0] ? "," : "",
                         sys->gen[g].id, sys->gen[g].pg * sys->base_mva);
        printf("%4d %-12s %-6s %8.1f %8.3f %8.2f %8.1f %8.1f  %s\n",
               b->id, b->name, bus_type_name(b->type),
               b->base_kv, b->vm, b->va * 180.0 / M_PI,
               b->pd * sys->base_mva, b->qd * sys->base_mva, gen_info);
    }

    /* --- Branches --- */
    printf("\n--- Branches ---\n");
    printf("%4s %4s %4s %10s %10s %10s %8s %s\n",
           "From", "To", "Ckt", "R(pu)", "X(pu)", "B(pu)", "Tap", "Status");
    for (int i = 0; i < sys->nbranch; i++) {
        const Branch *br = &sys->branch[i];
        printf("%4d %4d %4s %10.5f %10.5f %10.5f %8.3f %s\n",
               sys->bus[br->from].id, sys->bus[br->to].id, br->ckt,
               br->r, br->x, br->b, br->tap, br->status ? "ON" : "OFF");
    }

    /* --- Generators --- */
    if (sys->ngen > 0) {
        printf("\n--- Generators ---\n");
        printf("%4s %4s %8s %8s %8s %8s\n",
               "ID", "Bus", "Pg(MW)", "Qg(MVar)", "Mbase", "Vsched");
        for (int i = 0; i < sys->ngen; i++) {
            const Gen *g = &sys->gen[i];
            printf("%4d %4d %8.1f %8.1f %8.1f %8.3f\n",
                   g->id, sys->bus[g->bus].id,
                   g->pg * sys->base_mva, g->qg * sys->base_mva,
                   g->mbase, g->vsched);
        }
    }

    /* --- Machines --- */
    if (sys->nmachines > 0) {
        printf("\n--- Dynamic Machines ---\n");
        printf("%4s %4s %-8s %8s %8s %8s\n",
               "Idx", "Bus", "Model", "H", "D", "X'd");
        for (int m = 0; m < sys->nmachines; m++) {
            const Machine *mc = &sys->machine[m];
            const Gen *g = &sys->gen[mc->gen_idx];
            printf("%4d %4d %-8s %8.3f %8.1f %8.3f\n",
                   m, sys->bus[g->bus].id,
                   "GENCLS", mc->h, mc->d, mc->xdp);
        }
    }

    /* --- Loads --- */
    if (sys->nload > 0) {
        printf("\n--- Loads ---\n");
        printf("%4s %4s %8s %8s\n", "ID", "Bus", "P(MW)", "Q(MVar)");
        for (int i = 0; i < sys->nload; i++) {
            const Load *ld = &sys->load[i];
            printf("%4d %4d %8.1f %8.1f\n",
                   ld->id, sys->bus[ld->bus].id,
                   ld->p * sys->base_mva, ld->q * sys->base_mva);
        }
    }
    printf("\n");
}

void print_network_dot(const System *sys, const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) { log_error("cannot open %s", path); return; }

    fprintf(fp, "digraph PowerNetwork {\n");
    fprintf(fp, "  rankdir=LR;\n");
    fprintf(fp, "  node [shape=ellipse, style=filled, fontname=\"Helvetica\"];\n");
    fprintf(fp, "  edge [fontname=\"Helvetica\", fontsize=9];\n\n");

    /* bus nodes — color by type */
    for (int i = 0; i < sys->nbus; i++) {
        const Bus *b = &sys->bus[i];
        const char *color = "lightgrey";
        if (b->type == BUS_SLACK) color = "tomato";
        else if (b->type == BUS_PV) color = "lightblue";
        else if (b->type == BUS_PQ) color = "lightyellow";

        fprintf(fp, "  bus%d [label=\"%d\\n%s\\n%.1f kV\", fillcolor=%s];\n",
                b->id, b->id, b->name, b->base_kv, color);
    }

    /* generator attachments */
    for (int i = 0; i < sys->ngen; i++) {
        const Gen *g = &sys->gen[i];
        int bid = sys->bus[g->bus].id;
        fprintf(fp, "  gen%d [shape=box, label=\"G%d\\n%.0f MW\", fillcolor=lightgreen];\n",
                g->id, g->id, g->pg * sys->base_mva);
        fprintf(fp, "  gen%d -> bus%d [style=dashed, arrowhead=none];\n", g->id, bid);
    }

    /* branches */
    fprintf(fp, "\n");
    for (int i = 0; i < sys->nbranch; i++) {
        const Branch *br = &sys->branch[i];
        if (!br->status) continue;
        int f = sys->bus[br->from].id, t = sys->bus[br->to].id;
        fprintf(fp, "  bus%d -> bus%d [label=\"%.4f+j%.4f\"%s];\n",
                f, t, br->r, br->x,
                (fabs(br->tap - 1.0) > 1e-6) ? ", style=bold, color=blue" : "");
    }

    fprintf(fp, "}\n");
    fclose(fp);
    log_info("DOT topology written to %s", path);
}
