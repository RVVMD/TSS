#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transient.h"

#define PLOT_BUF 65536
#define MAX_SIGNALS 128
#define MAX_COLORS  24
#define MAX_EVENTS  64

typedef enum {
    SIG_GEN_DELTA, SIG_GEN_PE, SIG_GEN_OMEGA, SIG_GEN_VT,
    SIG_BUS_VM, SIG_BUS_VA,
} SignalType;

typedef struct {
    char       label[64];
    SignalType type;
    int        idx;
    int        id;
    int        visible;
    double    *data;
} Signal;

typedef struct {
    EventType type;
    double    time;
    int       bus;
    double    fault_r, fault_x;
} GUIEvent;

typedef struct {
    System sys;
    Arena  arena;
    int    sys_loaded;

    GUIEvent events[MAX_EVENTS];
    int      n_events;

    double   plot_t[PLOT_BUF];
    Signal   signals[MAX_SIGNALS];
    int      n_signals;
    int      plot_len;
    int      plot_head;

    double   view_t0, view_t1;
    double   sim_t_end;

    int      mouse_down;
    double   mouse_x0;
    int      dragging;

    double t_end;
    double t_step;

    GThread *sim_thread;
    GMutex   sim_lock;
    int      sim_running;
    int      sim_done;
    double   sim_progress;
    char     sim_status[256];

    GtkWidget *window;
    GtkWidget *bus_tree, *branch_tree, *gen_tree, *load_tree;
    GtkListStore *bus_store, *branch_store, *gen_store, *load_store;
    GtkWidget *event_tree;
    GtkListStore *event_store;
    GtkWidget *draw_plot;
    GtkWidget *lbl_raw, *lbl_dyr, *lbl_ini;
    GtkWidget *btn_run, *btn_stop;
    GtkWidget *spin_tend, *spin_tstep;
    GtkWidget *progress;
    GtkWidget *lbl_progress;
    GtkWidget *lbl_status;
    GtkWidget *lbl_view_range;

    GtkTreeStore *sig_store;
    GtkWidget    *sig_tree;

    /* property editor form */
    GtkWidget *prop_notebook;
    GtkWidget *prop_box;
    GtkWidget *prop_label;
    GtkWidget **prop_entries;
    int         prop_n_entries;

    char raw_path[512];
    char dyr_path[512];
    char ini_path[512];
} App;

/* forward declarations */
static void  app_init(App *app);
static void  app_cleanup(App *app);
static void  build_ui(App *app);
static GtkWidget *build_menu(App *app);
static void  build_network_panel(App *app, GtkWidget **box);
static void  build_sim_panel(App *app, GtkWidget **box);
static void  build_event_panel(App *app, GtkWidget **box);
static void  populate_network(App *app);
static void  populate_events(App *app);
static void  build_signal_tree(App *app);
static void  file_load_raw(App *app);
static void  file_load_dyr(App *app);
static void  file_load_events(App *app);
static void  sim_run(App *app);
static void  sim_stop(App *app);
static void  sim_add_event(App *app);
static void  sim_delete_event(App *app);
static void  sim_save_events(App *app);
static void  view_zoom_in(App *app);
static void  view_zoom_out(App *app);
static void  view_fit(App *app);
static void  view_select_all(App *app);
static void  view_select_none(App *app);
static void  on_btn_run(GtkWidget *btn, App *app);
static gpointer sim_thread_fn(gpointer data);

/* ── cell editing callbacks ── */
static void bus_cell_edited(GtkCellRendererText *cell, gchar *path_string,
                            gchar *new_text, gpointer user_data) {
    App *app = (App *)user_data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    GtkTreeIter iter;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "col_idx"));
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->bus_store), &iter, path)) {
        gtk_tree_path_free(path);
        return;
    }
    int row = gtk_tree_path_get_indices(path)[0];
    if (row < 0 || row >= app->sys.nbus) { gtk_tree_path_free(path); return; }
    Bus *b = &app->sys.bus[row];

    switch (col) {
    case 1: strncpy(b->name, new_text, sizeof(b->name) - 1); break;
    case 3: b->base_kv = atof(new_text); break;
    case 4: b->vm = atof(new_text); break;
    case 5: b->pd = atof(new_text) / app->sys.base_mva; break;
    case 6: b->qd = atof(new_text) / app->sys.base_mva; break;
    case 2: /* type dropdown */
        if (strcmp(new_text, "SLACK") == 0) b->type = BUS_SLACK;
        else if (strcmp(new_text, "PV") == 0) b->type = BUS_PV;
        else b->type = BUS_PQ;
        break;
    }
    populate_network(app);
    gtk_tree_path_free(path);
}

static void branch_cell_edited(GtkCellRendererText *cell, gchar *path_string,
                               gchar *new_text, gpointer user_data) {
    App *app = (App *)user_data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "col_idx"));
    if (!path) return;
    int row = gtk_tree_path_get_indices(path)[0];
    if (row < 0 || row >= app->sys.nbranch) { gtk_tree_path_free(path); return; }
    Branch *br = &app->sys.branch[row];

    switch (col) {
    case 2: br->r = atof(new_text); break;
    case 3: br->x = atof(new_text); break;
    case 4: br->b = atof(new_text); break;
    case 5: br->tap = atof(new_text); break;
    case 6: br->status = (strcmp(new_text, "ON") == 0) ? 1 : 0; break;
    }
    populate_network(app);
    gtk_tree_path_free(path);
}

static void gen_cell_edited(GtkCellRendererText *cell, gchar *path_string,
                            gchar *new_text, gpointer user_data) {
    App *app = (App *)user_data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "col_idx"));
    if (!path) return;
    int row = gtk_tree_path_get_indices(path)[0];
    if (row < 0 || row >= app->sys.ngen) { gtk_tree_path_free(path); return; }
    Gen *g = &app->sys.gen[row];

    switch (col) {
    case 1: g->pg = atof(new_text) / app->sys.base_mva; break;
    case 2: g->qg = atof(new_text) / app->sys.base_mva; break;
    case 3: g->vsched = atof(new_text); break;
    case 4: g->mbase = atof(new_text); break;
    }
    populate_network(app);
    gtk_tree_path_free(path);
}

static void load_cell_edited(GtkCellRendererText *cell, gchar *path_string,
                             gchar *new_text, gpointer user_data) {
    App *app = (App *)user_data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "col_idx"));
    if (!path) return;
    int row = gtk_tree_path_get_indices(path)[0];
    if (row < 0 || row >= app->sys.nload) { gtk_tree_path_free(path); return; }
    Load *l = &app->sys.load[row];

    switch (col) {
    case 1: l->p = atof(new_text) / app->sys.base_mva; break;
    case 2: l->q = atof(new_text) / app->sys.base_mva; break;
    case 3: l->status = (strcmp(new_text, "ON") == 0) ? 1 : 0; break;
    }
    populate_network(app);
    gtk_tree_path_free(path);
}

static void event_cell_edited(GtkCellRendererText *cell, gchar *path_string,
                              gchar *new_text, gpointer user_data) {
    App *app = (App *)user_data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "col_idx"));
    if (!path) return;
    int row = gtk_tree_path_get_indices(path)[0];
    if (row < 0 || row >= app->n_events) { gtk_tree_path_free(path); return; }
    GUIEvent *e = &app->events[row];

    switch (col) {
    case 1: e->time = atof(new_text); break;
    case 2: /* type */
        if (strcmp(new_text, "3ph") == 0) e->type = FAULT;
        else if (strcmp(new_text, "SLG") == 0) e->type = FAULT_SLG;
        else if (strcmp(new_text, "LL") == 0) e->type = FAULT_LL;
        else if (strcmp(new_text, "DLG") == 0) e->type = FAULT_DLG;
        else if (strcmp(new_text, "clear") == 0) e->type = FAULT_CLEAR;
        break;
    case 3: e->bus = atoi(new_text); break;
    case 4: e->fault_r = atof(new_text); break;
    case 5: e->fault_x = atof(new_text); break;
    }
    populate_events(app);
    gtk_tree_path_free(path);
}

static const char *btype(BusType t) {
    switch(t) { case BUS_SLACK: return "SLACK"; case BUS_PV: return "PV"; case BUS_PQ: return "PQ"; default: return "ISO"; }
}
static const char *ftype_name(int t) {
    switch(t) { case FAULT: return "3ph"; case FAULT_SLG: return "SLG"; case FAULT_LL: return "LL"; case FAULT_DLG: return "DLG"; case FAULT_CLEAR: return "clear"; default: return "?"; }
}

static const double sig_colors[MAX_COLORS][3] = {
    {1.00, 0.40, 0.30}, {0.30, 0.80, 0.50}, {0.40, 0.60, 1.00},
    {1.00, 0.80, 0.30}, {0.80, 0.30, 1.00}, {0.30, 1.00, 0.90},
    {1.00, 0.60, 0.70}, {0.60, 1.00, 0.40}, {0.50, 0.50, 1.00},
    {1.00, 0.50, 0.50}, {0.50, 1.00, 0.50}, {0.50, 0.50, 0.90},
    {1.00, 1.00, 0.50}, {0.50, 1.00, 1.00}, {1.00, 0.50, 1.00},
    {0.70, 0.70, 0.70}, {0.90, 0.70, 0.30}, {0.30, 0.70, 0.90},
    {0.90, 0.30, 0.70}, {0.70, 0.90, 0.30}, {0.30, 0.90, 0.70},
    {0.80, 0.60, 0.40}, {0.60, 0.40, 0.80}, {0.40, 0.80, 0.60},
};

static void plot_push(App *app, double t, const double *gen_delta, const double *gen_pe,
                      const double *gen_omega, const double *gen_vt,
                      const double *bus_vm, const double *bus_va) {
    int idx = app->plot_head;
    app->plot_t[idx] = t;
    for (int s = 0; s < app->n_signals; s++) {
        Signal *sig = &app->signals[s];
        switch (sig->type) {
        case SIG_GEN_DELTA: sig->data[idx] = gen_delta[sig->idx]; break;
        case SIG_GEN_PE:    sig->data[idx] = gen_pe[sig->idx]; break;
        case SIG_GEN_OMEGA: sig->data[idx] = gen_omega[sig->idx]; break;
        case SIG_GEN_VT:    sig->data[idx] = gen_vt[sig->idx]; break;
        case SIG_BUS_VM:    sig->data[idx] = bus_vm[sig->idx]; break;
        case SIG_BUS_VA:    sig->data[idx] = bus_va[sig->idx]; break;
        }
    }
    app->plot_head = (idx + 1) % PLOT_BUF;
    if (app->plot_len < PLOT_BUF) app->plot_len++;
}

static void plot_clear(App *app) {
    app->plot_len = 0;
    app->plot_head = 0;
}

/* ── unified plot drawing ── */
static gboolean on_draw_plot(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    App *app = (App *)user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    int w = alloc.width, h = alloc.height;
    int pad = 55;

    cairo_set_source_rgb(cr, 0.12, 0.12, 0.16);
    cairo_paint(cr);

    int nvis = 0;
    for (int s = 0; s < app->n_signals; s++)
        if (app->signals[s].visible) nvis++;

    if (app->plot_len < 2 || nvis == 0) {
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, w/2 - 60, h/2);
        cairo_show_text(cr, app->plot_len < 2 ? "No simulation data" : "No signals selected");
        return FALSE;
    }

    double t0 = app->view_t0, t1 = app->view_t1;
    if (t1 - t0 < 1e-10) { t0 = 0; t1 = 1; }

    double y_min = 1e30, y_max = -1e30;
    for (int s = 0; s < app->n_signals; s++) {
        if (!app->signals[s].visible) continue;
        for (int i = 0; i < app->plot_len; i++) {
            int idx = (app->plot_head - app->plot_len + i + PLOT_BUF) % PLOT_BUF;
            double v = app->signals[s].data[idx];
            if (v < y_min) y_min = v;
            if (v > y_max) y_max = v;
        }
    }
    if (y_max - y_min < 1e-10) { y_min -= 1; y_max += 1; }
    double ym = (y_max - y_min) * 0.05; y_min -= ym; y_max += ym;

    double px(double t) { return pad + (t - t0) / (t1 - t0) * (w - 2*pad); }
    double py(double v) { return h - pad - (v - y_min) / (y_max - y_min) * (h - 2*pad); }

    cairo_set_source_rgba(cr, 0.3, 0.3, 0.35, 0.5);
    cairo_set_line_width(cr, 0.5);
    for (int i = 0; i <= 5; i++) {
        double y = py(y_min + (y_max - y_min) * i / 5);
        cairo_move_to(cr, pad, y); cairo_line_to(cr, w - pad, y);
    }
    for (int i = 0; i <= 5; i++) {
        double t = t0 + (t1 - t0) * i / 5;
        cairo_move_to(cr, px(t), pad); cairo_line_to(cr, px(t), h - pad);
    }
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    char buf[32];
    for (int i = 0; i <= 5; i++) {
        double v = y_min + (y_max - y_min) * i / 5;
        snprintf(buf, sizeof(buf), "%.3f", v);
        cairo_move_to(cr, 2, py(v) + 4);
        cairo_show_text(cr, buf);
    }
    for (int i = 0; i <= 5; i++) {
        double t = t0 + (t1 - t0) * i / 5;
        snprintf(buf, sizeof(buf), "%.2f", t);
        cairo_move_to(cr, px(t) - 12, h - 5);
        cairo_show_text(cr, buf);
    }
    cairo_move_to(cr, 2, h - 1);
    cairo_show_text(cr, "Time (s)");

    int ci = 0;
    for (int s = 0; s < app->n_signals; s++) {
        if (!app->signals[s].visible) continue;
        const double *c = sig_colors[ci % MAX_COLORS];
        cairo_set_source_rgb(cr, c[0], c[1], c[2]);
        cairo_set_line_width(cr, 1.5);
        int start = -1;
        for (int i = 0; i < app->plot_len; i++) {
            int idx = (app->plot_head - app->plot_len + i + PLOT_BUF) % PLOT_BUF;
            double t = app->plot_t[idx];
            if (t < t0 - 1e-10 || t > t1 + 1e-10) { start = -1; continue; }
            double v = app->signals[s].data[idx];
            if (start < 0) { cairo_move_to(cr, px(t), py(v)); start = i; }
            else cairo_line_to(cr, px(t), py(v));
        }
        cairo_stroke(cr);
        ci++;
    }

    int ly = pad + 5;
    cairo_set_font_size(cr, 9);
    ci = 0;
    for (int s = 0; s < app->n_signals; s++) {
        if (!app->signals[s].visible) continue;
        const double *c = sig_colors[ci % MAX_COLORS];
        cairo_set_source_rgb(cr, c[0], c[1], c[2]);
        cairo_rectangle(cr, w - pad - 140, ly, 12, 3);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
        cairo_move_to(cr, w - pad - 124, ly + 4);
        cairo_show_text(cr, app->signals[s].label);
        ly += 14;
        ci++;
    }

    if (app->dragging) {
        double x0 = app->mouse_x0;
        double x1 = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "mouse_x"));
        if (x1 > x0 + 2) {
            cairo_set_source_rgba(cr, 0.3, 0.6, 1.0, 0.15);
            cairo_rectangle(cr, x0, pad, x1 - x0, h - 2*pad);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.3, 0.6, 1.0, 0.5);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, x0, pad, x1 - x0, h - 2*pad);
            cairo_stroke(cr);
        }
    }

    cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, pad, pad, w - 2*pad, h - 2*pad);
    cairo_stroke(cr);
    return FALSE;
}

static gboolean on_plot_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    App *app = (App *)user_data;
    if (event->button == 1) {
        app->mouse_down = 1;
        app->mouse_x0 = event->x;
        app->dragging = 0;
    }
    return FALSE;
}

static gboolean on_plot_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data) {
    App *app = (App *)user_data;
    g_object_set_data(G_OBJECT(widget), "mouse_x", GINT_TO_POINTER((int)event->x));
    if (app->mouse_down && abs((int)event->x - (int)app->mouse_x0) > 3) {
        app->dragging = 1;
        gtk_widget_queue_draw(widget);
    }
    return FALSE;
}

static gboolean on_plot_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    App *app = (App *)user_data;
    if (event->button == 1 && app->dragging) {
        GtkAllocation alloc;
        gtk_widget_get_allocation(widget, &alloc);
        int pad = 55;
        double x0 = app->mouse_x0, x1 = (double)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "mouse_x"));
        if (x1 < x0) { double tmp = x0; x0 = x1; x1 = tmp; }
        if (x1 - x0 > 5) {
            double t0 = app->view_t0 + (x0 - pad) / (alloc.width - 2*pad) * (app->view_t1 - app->view_t0);
            double t1 = app->view_t0 + (x1 - pad) / (alloc.width - 2*pad) * (app->view_t1 - app->view_t0);
            if (t0 < 0) t0 = 0;
            app->view_t0 = t0;
            app->view_t1 = t1;
            char b[64];
            snprintf(b, sizeof(b), "View: %.2f – %.2f s", app->view_t0, app->view_t1);
            gtk_label_set_text(GTK_LABEL(app->lbl_view_range), b);
        }
    }
    app->mouse_down = 0;
    app->dragging = 0;
    gtk_widget_queue_draw(widget);
    return FALSE;
}

static gboolean on_plot_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data) {
    App *app = (App *)user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    int pad = 55;
    double t0 = app->view_t0, t1 = app->view_t1;
    double frac = (event->x - pad) / (alloc.width - 2*pad);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    double cursor_t = t0 + frac * (t1 - t0);
    double factor = event->direction == GDK_SCROLL_UP ? 0.8 : 1.25;
    double new_span = (t1 - t0) * factor / 2;
    if (new_span < 0.001) new_span = 0.001;
    double left_frac = (cursor_t - t0) / (t1 - t0 + 1e-10);
    app->view_t0 = cursor_t - left_frac * new_span * 2;
    app->view_t1 = cursor_t + (1 - left_frac) * new_span * 2;
    if (app->view_t0 < 0) { app->view_t1 -= app->view_t0; app->view_t0 = 0; }
    char b[64];
    snprintf(b, sizeof(b), "View: %.2f – %.2f s", app->view_t0, app->view_t1);
    gtk_label_set_text(GTK_LABEL(app->lbl_view_range), b);
    gtk_widget_queue_draw(widget);
    return FALSE;
}

static void view_zoom_in(App *app) {
    double mid = (app->view_t0 + app->view_t1) / 2;
    double span = (app->view_t1 - app->view_t0) * 0.35;
    app->view_t0 = mid - span;
    app->view_t1 = mid + span;
    if (app->view_t0 < 0) app->view_t0 = 0;
    char b[64];
    snprintf(b, sizeof(b), "View: %.2f – %.2f s", app->view_t0, app->view_t1);
    gtk_label_set_text(GTK_LABEL(app->lbl_view_range), b);
    gtk_widget_queue_draw(app->draw_plot);
}

static void view_zoom_out(App *app) {
    double mid = (app->view_t0 + app->view_t1) / 2;
    double span = (app->view_t1 - app->view_t0) * 0.75;
    app->view_t0 = mid - span;
    app->view_t1 = mid + span;
    if (app->view_t0 < 0) app->view_t0 = 0;
    char b[64];
    snprintf(b, sizeof(b), "View: %.2f – %.2f s", app->view_t0, app->view_t1);
    gtk_label_set_text(GTK_LABEL(app->lbl_view_range), b);
    gtk_widget_queue_draw(app->draw_plot);
}

static void view_fit(App *app) {
    app->view_t0 = 0;
    app->view_t1 = app->sim_t_end > 0 ? app->sim_t_end : app->t_end;
    char b[64];
    snprintf(b, sizeof(b), "View: %.2f – %.2f s", app->view_t0, app->view_t1);
    gtk_label_set_text(GTK_LABEL(app->lbl_view_range), b);
    gtk_widget_queue_draw(app->draw_plot);
}

static void view_select_all(App *app) {
    for (int s = 0; s < app->n_signals; s++)
        app->signals[s].visible = 1;
    gtk_widget_queue_draw(app->draw_plot);
}

static void view_select_none(App *app) {
    for (int s = 0; s < app->n_signals; s++)
        app->signals[s].visible = 0;
    gtk_widget_queue_draw(app->draw_plot);
}

/* ── menu ── */
static void on_load_raw(GtkMenuItem *item, App *app) { file_load_raw(app); }
static void on_load_dyr(GtkMenuItem *item, App *app) { file_load_dyr(app); }
static void on_load_events(GtkMenuItem *item, App *app) { file_load_events(app); }
static void on_run_sim(GtkMenuItem *item, App *app) { sim_run(app); }
static void on_stop_sim(GtkMenuItem *item, App *app) { sim_stop(app); }
static void on_quit(GtkMenuItem *item, App *app) { gtk_widget_destroy(app->window); }

static GtkWidget *build_menu(App *app) {
    GtkWidget *menubar = gtk_menu_bar_new();
    GtkWidget *file_menu = gtk_menu_new();
    GtkWidget *file_item = gtk_menu_item_new_with_label("File");
    GtkWidget *sim_menu = gtk_menu_new();
    GtkWidget *sim_item = gtk_menu_item_new_with_label("Simulation");

    GtkWidget *mi_raw = gtk_menu_item_new_with_label("Load RAW...");
    GtkWidget *mi_dyr = gtk_menu_item_new_with_label("Load DYR...");
    GtkWidget *mi_ini = gtk_menu_item_new_with_label("Load Events...");
    GtkWidget *sep = gtk_separator_menu_item_new();
    GtkWidget *mi_quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(mi_raw, "activate", G_CALLBACK(on_load_raw), app);
    g_signal_connect(mi_dyr, "activate", G_CALLBACK(on_load_dyr), app);
    g_signal_connect(mi_ini, "activate", G_CALLBACK(on_load_events), app);
    g_signal_connect(mi_quit, "activate", G_CALLBACK(on_quit), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), mi_raw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), mi_dyr);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), mi_ini);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), mi_quit);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);

    GtkWidget *mi_run = gtk_menu_item_new_with_label("Run");
    GtkWidget *mi_stop = gtk_menu_item_new_with_label("Stop");
    g_signal_connect(mi_run, "activate", G_CALLBACK(on_run_sim), app);
    g_signal_connect(mi_stop, "activate", G_CALLBACK(on_stop_sim), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(sim_menu), mi_run);
    gtk_menu_shell_append(GTK_MENU_SHELL(sim_menu), mi_stop);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(sim_item), sim_menu);

    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), sim_item);
    return menubar;
}

/* ── file dialogs ── */
static void file_load_raw(App *app) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Load PSS/E RAW File",
        NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        strncpy(app->raw_path, fn, sizeof(app->raw_path) - 1);
        gtk_label_set_text(GTK_LABEL(app->lbl_raw), fn);
        if (raw_parse(fn, &app->sys, &app->arena) == 0) {
            app->sys.fault_bus = -1;
            populate_network(app);
            build_signal_tree(app);
        } else {
            GtkWidget *err = gtk_message_dialog_new(NULL,
                GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to parse RAW file");
            gtk_dialog_run(GTK_DIALOG(err)); gtk_widget_destroy(err);
        }
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

static void file_load_dyr(App *app) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Load PSS/E DYR File",
        NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        strncpy(app->dyr_path, fn, sizeof(app->dyr_path) - 1);
        gtk_label_set_text(GTK_LABEL(app->lbl_dyr), fn);
        if (dyr_parse(fn, &app->sys, &app->arena) == 0) {
            app->sys_loaded = 1;
            populate_network(app);
            build_signal_tree(app);
        } else {
            GtkWidget *err = gtk_message_dialog_new(NULL,
                GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to parse DYR file");
            gtk_dialog_run(GTK_DIALOG(err)); gtk_widget_destroy(err);
        }
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

static void file_load_events(App *app) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Load Event INI File",
        NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        strncpy(app->ini_path, fn, sizeof(app->ini_path) - 1);
        gtk_label_set_text(GTK_LABEL(app->lbl_ini), fn);
        Arena tmp = arena_new(1 << 16);
        Event *tmp_ev = NULL;
        if (events_parse(fn, &tmp_ev, &tmp) == 0) {
            app->n_events = 0;
            int n = (int)arrlen(tmp_ev);
            for (int i = 0; i < n && i < MAX_EVENTS; i++) {
                app->events[i].type = tmp_ev[i].type;
                app->events[i].time = tmp_ev[i].time;
                app->events[i].bus = tmp_ev[i].bus;
                app->events[i].fault_r = tmp_ev[i].fault_r;
                app->events[i].fault_x = tmp_ev[i].fault_x;
                app->n_events++;
            }
            populate_events(app);
        }
        arena_free(&tmp);
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

/* ── network panel with inline editing ── */
static void build_network_panel(App *app, GtkWidget **box) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *nb = gtk_notebook_new();

    /* Buses */
    app->bus_store = gtk_list_store_new(7, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE);
    app->bus_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->bus_store));
    const char *bus_cols[] = {"ID", "Name", "Type", "kV", "Vm(pu)", "Pd(MW)", "Qd(MVar)"};
    for (int i = 0; i < 7; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (i == 0) { g_object_set(r, "editable", FALSE, NULL); }
        else {
            g_object_set(r, "editable", TRUE, NULL);
            g_object_set_data(G_OBJECT(r), "col_idx", GINT_TO_POINTER(i));
            g_signal_connect(r, "edited", G_CALLBACK(bus_cell_edited), app);
        }
        if (i == 2) {
            GtkCellRenderer *rc = gtk_cell_renderer_combo_new();
            GtkListStore *ls = gtk_list_store_new(1, G_TYPE_STRING);
            GtkTreeIter it;
            gtk_list_store_append(ls, &it); gtk_list_store_set(ls, &it, 0, "PQ", -1);
            gtk_list_store_append(ls, &it); gtk_list_store_set(ls, &it, 0, "PV", -1);
            gtk_list_store_append(ls, &it); gtk_list_store_set(ls, &it, 0, "SLACK", -1);
            g_object_set(rc, "model", ls, "text-column", 0, "has-entry", FALSE, "editable", TRUE, NULL);
            g_object_unref(ls);
            g_object_set_data(G_OBJECT(rc), "col_idx", GINT_TO_POINTER(i));
            g_signal_connect(rc, "edited", G_CALLBACK(bus_cell_edited), app);
            gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->bus_tree), i, bus_cols[i], rc, "text", i, NULL);
            continue;
        }
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->bus_tree), i, bus_cols[i], r, "text", i, NULL);
    }
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), app->bus_tree);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, gtk_label_new("Buses"));

    /* Branches */
    app->branch_store = gtk_list_store_new(7, G_TYPE_INT, G_TYPE_INT, G_TYPE_DOUBLE,
        G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_STRING);
    app->branch_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->branch_store));
    const char *br_cols[] = {"From", "To", "R", "X", "B", "Tap", "Status"};
    for (int i = 0; i < 7; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (i < 2) { g_object_set(r, "editable", FALSE, NULL); }
        else {
            g_object_set(r, "editable", TRUE, NULL);
            g_object_set_data(G_OBJECT(r), "col_idx", GINT_TO_POINTER(i));
            g_signal_connect(r, "edited", G_CALLBACK(branch_cell_edited), app);
        }
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->branch_tree), i, br_cols[i], r, "text", i, NULL);
    }
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), app->branch_tree);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, gtk_label_new("Branches"));

    /* Generators */
    app->gen_store = gtk_list_store_new(5, G_TYPE_INT, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE);
    app->gen_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->gen_store));
    const char *gen_cols[] = {"Bus", "Pg(MW)", "Qg(MVar)", "Vsched", "Mbase"};
    for (int i = 0; i < 5; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (i == 0) { g_object_set(r, "editable", FALSE, NULL); }
        else {
            g_object_set(r, "editable", TRUE, NULL);
            g_object_set_data(G_OBJECT(r), "col_idx", GINT_TO_POINTER(i));
            g_signal_connect(r, "edited", G_CALLBACK(gen_cell_edited), app);
        }
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->gen_tree), i, gen_cols[i], r, "text", i, NULL);
    }
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), app->gen_tree);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, gtk_label_new("Generators"));

    /* Loads */
    app->load_store = gtk_list_store_new(4, G_TYPE_INT, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_STRING);
    app->load_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->load_store));
    const char *ld_cols[] = {"Bus", "P(MW)", "Q(MVar)", "Status"};
    for (int i = 0; i < 4; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (i == 0) { g_object_set(r, "editable", FALSE, NULL); }
        else {
            g_object_set(r, "editable", TRUE, NULL);
            g_object_set_data(G_OBJECT(r), "col_idx", GINT_TO_POINTER(i));
            g_signal_connect(r, "edited", G_CALLBACK(load_cell_edited), app);
        }
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->load_tree), i, ld_cols[i], r, "text", i, NULL);
    }
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), app->load_tree);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, gtk_label_new("Loads"));

    gtk_box_pack_start(GTK_BOX(vbox), nb, TRUE, TRUE, 0);

    /* hint */
    GtkWidget *hint = gtk_label_new("Double-click a cell to edit. Changes apply immediately.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_set_state_flags(hint, GTK_STATE_FLAG_INSENSITIVE, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), hint, FALSE, FALSE, 0);

    *box = vbox;
}

static void populate_network(App *app) {
    gtk_list_store_clear(app->bus_store);
    gtk_list_store_clear(app->branch_store);
    gtk_list_store_clear(app->gen_store);
    gtk_list_store_clear(app->load_store);
    if (!app->sys_loaded) return;
    System *s = &app->sys;

    for (int i = 0; i < s->nbus; i++) {
        GtkTreeIter it;
        gtk_list_store_append(app->bus_store, &it);
        gtk_list_store_set(app->bus_store, &it, 0, s->bus[i].id, 1, s->bus[i].name,
            2, btype(s->bus[i].type), 3, s->bus[i].base_kv, 4, s->bus[i].vm,
            5, s->bus[i].pd * s->base_mva, 6, s->bus[i].qd * s->base_mva, -1);
    }
    for (int i = 0; i < s->nbranch; i++) {
        GtkTreeIter it;
        gtk_list_store_append(app->branch_store, &it);
        gtk_list_store_set(app->branch_store, &it, 0, s->bus[s->branch[i].from].id,
            1, s->bus[s->branch[i].to].id, 2, s->branch[i].r, 3, s->branch[i].x,
            4, s->branch[i].b, 5, s->branch[i].tap > 0 ? s->branch[i].tap : 1.0,
            6, s->branch[i].status ? "ON" : "OFF", -1);
    }
    for (int i = 0; i < s->ngen; i++) {
        GtkTreeIter it;
        gtk_list_store_append(app->gen_store, &it);
        gtk_list_store_set(app->gen_store, &it, 0, s->bus[s->gen[i].bus].id,
            1, s->gen[i].pg * s->base_mva, 2, s->gen[i].qg * s->base_mva,
            3, s->gen[i].vsched, 4, s->gen[i].mbase, -1);
    }
    for (int i = 0; i < s->nload; i++) {
        GtkTreeIter it;
        gtk_list_store_append(app->load_store, &it);
        gtk_list_store_set(app->load_store, &it, 0, s->bus[s->load[i].bus].id,
            1, s->load[i].p * s->base_mva, 2, s->load[i].q * s->base_mva,
            3, s->load[i].status ? "ON" : "OFF", -1);
    }
}

/* ── signal tree with parent-child sync ── */
enum { SIG_COL_CHECK = 0, SIG_COL_LABEL, SIG_COL_IDX, SIG_N_COLS };

static void sig_set_children(GtkTreeStore *store, GtkTreeIter *parent, gboolean state, App *app) {
    GtkTreeIter child;
    if (gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &child, parent)) {
        do {
            int idx;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &child, SIG_COL_IDX, &idx, -1);
            gtk_tree_store_set(store, &child, SIG_COL_CHECK, state, -1);
            if (idx >= 0 && idx < app->n_signals)
                app->signals[idx].visible = state;
            sig_set_children(store, &child, state, app);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &child));
    }
}

static void sig_update_parent(GtkTreeStore *store, GtkTreeIter *parent, App *app) {
    GtkTreeIter child;
    int total = 0, checked = 0;
    if (gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &child, parent)) {
        do {
            gboolean c;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &child, SIG_COL_CHECK, &c, -1);
            total++;
            if (c) checked++;
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &child));
    }
    if (total > 0) {
        gboolean state = (checked == total);
        gtk_tree_store_set(store, parent, SIG_COL_CHECK, state, -1);
    }
}

static void sig_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer user_data) {
    App *app = (App *)user_data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->sig_store), &iter, path)) {
        gtk_tree_path_free(path);
        return;
    }
    gboolean active;
    gtk_tree_model_get(GTK_TREE_MODEL(app->sig_store), &iter, SIG_COL_CHECK, &active, -1);
    active = !active;
    gtk_tree_store_set(app->sig_store, &iter, SIG_COL_CHECK, active, -1);

    int idx;
    gtk_tree_model_get(GTK_TREE_MODEL(app->sig_store), &iter, SIG_COL_IDX, &idx, -1);
    if (idx >= 0 && idx < app->n_signals) {
        app->signals[idx].visible = active;
    }
    sig_set_children(app->sig_store, &iter, active, app);

    GtkTreePath *parent_path = gtk_tree_path_copy(path);
    if (gtk_tree_path_up(parent_path)) {
        GtkTreeIter p_iter;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->sig_store), &p_iter, parent_path)) {
            sig_update_parent(app->sig_store, &p_iter, app);
        }
    }
    gtk_tree_path_free(parent_path);
    gtk_tree_path_free(path);
    gtk_widget_queue_draw(app->draw_plot);
}

static void build_signal_tree(App *app) {
    if (!app->sys_loaded) return;
    System *s = &app->sys;
    app->n_signals = 0;
    gtk_tree_store_clear(app->sig_store);

    for (int m = 0; m < s->nmachines; m++) {
        int bid = s->bus[s->gen[s->machine[m].gen_idx].bus].id;
        char pl[32];
        snprintf(pl, sizeof(pl), "Generator @ Bus %d", bid);
        GtkTreeIter parent;
        gtk_tree_store_append(app->sig_store, &parent, NULL);
        gtk_tree_store_set(app->sig_store, &parent, SIG_COL_CHECK, FALSE, SIG_COL_LABEL, pl, SIG_COL_IDX, -1, -1);

        struct { SignalType type; const char *fmt; } subs[] = {
            { SIG_GEN_DELTA, "G%d δ (rad)" },
            { SIG_GEN_PE,    "G%d Pe (pu)"  },
            { SIG_GEN_OMEGA, "G%d ω (pu)"   },
            { SIG_GEN_VT,    "G%d Vt (pu)"  },
        };
        for (int j = 0; j < 4 && app->n_signals < MAX_SIGNALS; j++) {
            Signal *sig = &app->signals[app->n_signals];
            snprintf(sig->label, sizeof(sig->label), subs[j].fmt, bid);
            sig->type = subs[j].type;
            sig->idx = m;
            sig->id = bid;
            sig->visible = 0;
            GtkTreeIter child;
            gtk_tree_store_append(app->sig_store, &child, &parent);
            gtk_tree_store_set(app->sig_store, &child, SIG_COL_CHECK, FALSE,
                SIG_COL_LABEL, sig->label, SIG_COL_IDX, app->n_signals, -1);
            app->n_signals++;
        }
    }

    for (int i = 0; i < s->nbus; i++) {
        int bid = s->bus[i].id;
        char pl[32];
        snprintf(pl, sizeof(pl), "Bus %d (%s)", bid, btype(s->bus[i].type));
        GtkTreeIter parent;
        gtk_tree_store_append(app->sig_store, &parent, NULL);
        gtk_tree_store_set(app->sig_store, &parent, SIG_COL_CHECK, FALSE, SIG_COL_LABEL, pl, SIG_COL_IDX, -1, -1);

        struct { SignalType type; const char *fmt; } subs[] = {
            { SIG_BUS_VM, "B%d Vm (pu)" },
            { SIG_BUS_VA, "B%d angle (deg)" },
        };
        for (int j = 0; j < 2 && app->n_signals < MAX_SIGNALS; j++) {
            Signal *sig = &app->signals[app->n_signals];
            snprintf(sig->label, sizeof(sig->label), subs[j].fmt, bid);
            sig->type = subs[j].type;
            sig->idx = i;
            sig->id = bid;
            sig->visible = 0;
            GtkTreeIter child;
            gtk_tree_store_append(app->sig_store, &child, &parent);
            gtk_tree_store_set(app->sig_store, &child, SIG_COL_CHECK, FALSE,
                SIG_COL_LABEL, sig->label, SIG_COL_IDX, app->n_signals, -1);
            app->n_signals++;
        }
    }
}

/* ── event panel ── */
static void build_event_panel(App *app, GtkWidget **box) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

    app->event_store = gtk_list_store_new(6, G_TYPE_INT, G_TYPE_DOUBLE, G_TYPE_STRING,
        G_TYPE_INT, G_TYPE_DOUBLE, G_TYPE_DOUBLE);
    app->event_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->event_store));
    const char *ev_cols[] = {"#", "Time(s)", "Type", "Bus", "R", "X"};
    for (int i = 0; i < 6; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (i == 0) { g_object_set(r, "editable", FALSE, NULL); }
        else {
            g_object_set(r, "editable", TRUE, NULL);
            g_object_set_data(G_OBJECT(r), "col_idx", GINT_TO_POINTER(i));
            g_signal_connect(r, "edited", G_CALLBACK(event_cell_edited), app);
        }
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->event_tree), i, ev_cols[i], r, "text", i, NULL);
    }
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(sw, -1, 150);
    gtk_container_add(GTK_CONTAINER(sw), app->event_tree);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *btn_add = gtk_button_new_with_label("Add Fault...");
    GtkWidget *btn_del = gtk_button_new_with_label("Delete");
    GtkWidget *btn_save = gtk_button_new_with_label("Save INI");
    g_signal_connect(btn_add, "clicked", G_CALLBACK(sim_add_event), app);
    g_signal_connect(btn_del, "clicked", G_CALLBACK(sim_delete_event), app);
    g_signal_connect(btn_save, "clicked", G_CALLBACK(sim_save_events), app);
    gtk_box_pack_start(GTK_BOX(hbox), btn_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), btn_del, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), btn_save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    GtkWidget *hint = gtk_label_new("Double-click a cell to edit. Changes apply immediately.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_set_state_flags(hint, GTK_STATE_FLAG_INSENSITIVE, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), hint, FALSE, FALSE, 0);

    *box = vbox;
}

static void populate_events(App *app) {
    if (!app->event_store) return;
    gtk_list_store_clear(app->event_store);
    for (int i = 0; i < app->n_events; i++) {
        GtkTreeIter it;
        gtk_list_store_append(app->event_store, &it);
        gtk_list_store_set(app->event_store, &it, 0, i, 1, app->events[i].time,
            2, ftype_name(app->events[i].type), 3, app->events[i].bus,
            4, app->events[i].fault_r, 5, app->events[i].fault_x, -1);
    }
}

static void sim_add_event(App *app) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Add Fault",
        NULL, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Add", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(area), 10);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Type:"), 0, 0, 1, 1);
    GtkWidget *combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "3-phase fault");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "SLG fault");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "LL fault");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "DLG fault");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Fault clear");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_grid_attach(GTK_GRID(grid), combo, 1, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Time (s):"), 0, 1, 1, 1);
    GtkWidget *spin_t = gtk_spin_button_new_with_range(0.0, 100.0, 0.01);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_t), 1.0);
    gtk_grid_attach(GTK_GRID(grid), spin_t, 1, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Bus:"), 0, 2, 1, 1);
    GtkWidget *spin_b = gtk_spin_button_new_with_range(1, 999, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_b), 1);
    gtk_grid_attach(GTK_GRID(grid), spin_b, 1, 2, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("R (pu):"), 0, 3, 1, 1);
    GtkWidget *spin_r = gtk_spin_button_new_with_range(0.0, 10.0, 0.001);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_r), 0.001);
    gtk_grid_attach(GTK_GRID(grid), spin_r, 1, 3, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("X (pu):"), 0, 4, 1, 1);
    GtkWidget *spin_x = gtk_spin_button_new_with_range(0.0, 10.0, 0.001);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_x), 0.001);
    gtk_grid_attach(GTK_GRID(grid), spin_x, 1, 4, 1, 1);

    gtk_box_pack_start(GTK_BOX(area), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        if (app->n_events >= MAX_EVENTS) {
            GtkWidget *err = gtk_message_dialog_new(NULL,
                GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Max events reached");
            gtk_dialog_run(GTK_DIALOG(err)); gtk_widget_destroy(err);
        } else {
            int type_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
            EventType types[] = { FAULT, FAULT_SLG, FAULT_LL, FAULT_DLG, FAULT_CLEAR };
            GUIEvent *e = &app->events[app->n_events];
            e->type = types[type_idx];
            e->time = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_t));
            e->bus = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_b));
            e->fault_r = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_r));
            e->fault_x = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_x));
            app->n_events++;
            populate_events(app);
        }
    }
    gtk_widget_destroy(dlg);
}

static void sim_delete_event(App *app) {
    if (app->n_events <= 0) return;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->event_tree));
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(sel, NULL, &iter)) {
        int idx;
        gtk_tree_model_get(GTK_TREE_MODEL(app->event_store), &iter, 0, &idx, -1);
        if (idx >= 0 && idx < app->n_events) {
            for (int i = idx; i < app->n_events - 1; i++)
                app->events[i] = app->events[i + 1];
            app->n_events--;
        }
    } else {
        app->n_events--;
    }
    populate_events(app);
}

static void sim_save_events(App *app) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save Events INI",
        NULL, GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        FILE *f = fopen(fn, "w");
        if (f) {
            for (int i = 0; i < app->n_events; i++)
                fprintf(f, "[event]\ntime=%.2f\ntype=%s\nbus=%d\nr=%.4f\nx=%.4f\n\n",
                    app->events[i].time, ftype_name(app->events[i].type),
                    app->events[i].bus, app->events[i].fault_r, app->events[i].fault_x);
            fclose(f);
        }
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

/* ── simulation panel ── */
static void on_btn_run(GtkWidget *btn, App *app) { sim_run(app); }
static void on_btn_stop(GtkWidget *btn, App *app) { sim_stop(app); }
static void on_btn_zoom_in(GtkWidget *btn, App *app) { view_zoom_in(app); }
static void on_btn_zoom_out(GtkWidget *btn, App *app) { view_zoom_out(app); }
static void on_btn_fit(GtkWidget *btn, App *app) { view_fit(app); }
static void on_btn_sel_all(GtkWidget *btn, App *app) { view_select_all(app); }
static void on_btn_sel_none(GtkWidget *btn, App *app) { view_select_none(app); }

static void build_sim_panel(App *app, GtkWidget **box) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("RAW:"), 0, 0, 1, 1);
    app->lbl_raw = gtk_label_new("(not loaded)");
    gtk_widget_set_halign(app->lbl_raw, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), app->lbl_raw, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("DYR:"), 0, 1, 1, 1);
    app->lbl_dyr = gtk_label_new("(not loaded)");
    gtk_widget_set_halign(app->lbl_dyr, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), app->lbl_dyr, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Events:"), 0, 2, 1, 1);
    app->lbl_ini = gtk_label_new("(not loaded)");
    gtk_widget_set_halign(app->lbl_ini, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), app->lbl_ini, 1, 2, 1, 1);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 5);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    app->btn_run = gtk_button_new_with_label("Run");
    app->btn_stop = gtk_button_new_with_label("Stop");
    g_signal_connect(app->btn_run, "clicked", G_CALLBACK(on_btn_run), app);
    g_signal_connect(app->btn_stop, "clicked", G_CALLBACK(on_btn_stop), app);
    gtk_box_pack_start(GTK_BOX(hbox), app->btn_run, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), app->btn_stop, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("t-end:"), FALSE, FALSE, 5);
    app->spin_tend = gtk_spin_button_new_with_range(0.1, 100.0, 0.1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_tend), 5.0);
    gtk_box_pack_start(GTK_BOX(hbox), app->spin_tend, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("dt:"), FALSE, FALSE, 5);
    app->spin_tstep = gtk_spin_button_new_with_range(0.001, 1.0, 0.001);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_tstep), 0.01);
    gtk_box_pack_start(GTK_BOX(hbox), app->spin_tstep, FALSE, FALSE, 0);

    app->progress = gtk_progress_bar_new();
    app->lbl_progress = gtk_label_new("Ready");
    app->lbl_view_range = gtk_label_new("View: 0.00 – 0.00 s");
    gtk_widget_set_halign(app->lbl_view_range, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(vbox), app->progress, FALSE, FALSE, 5);

    GtkWidget *zoom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *btn_zi = gtk_button_new_with_label("Zoom +");
    GtkWidget *btn_zo = gtk_button_new_with_label("Zoom -");
    GtkWidget *btn_fit = gtk_button_new_with_label("Fit");
    GtkWidget *btn_all = gtk_button_new_with_label("All signals");
    GtkWidget *btn_none = gtk_button_new_with_label("No signals");
    g_signal_connect(btn_zi, "clicked", G_CALLBACK(on_btn_zoom_in), app);
    g_signal_connect(btn_zo, "clicked", G_CALLBACK(on_btn_zoom_out), app);
    g_signal_connect(btn_fit, "clicked", G_CALLBACK(on_btn_fit), app);
    g_signal_connect(btn_all, "clicked", G_CALLBACK(on_btn_sel_all), app);
    g_signal_connect(btn_none, "clicked", G_CALLBACK(on_btn_sel_none), app);
    gtk_box_pack_start(GTK_BOX(zoom_box), btn_zi, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zoom_box), btn_zo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zoom_box), btn_fit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zoom_box), app->lbl_view_range, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(zoom_box), btn_all, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zoom_box), btn_none, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), zoom_box, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(vbox), app->lbl_progress, FALSE, FALSE, 5);

    app->draw_plot = gtk_drawing_area_new();
    gtk_widget_set_size_request(app->draw_plot, -1, 400);
    g_signal_connect(app->draw_plot, "draw", G_CALLBACK(on_draw_plot), app);
    g_signal_connect(app->draw_plot, "button-press-event", G_CALLBACK(on_plot_button_press), app);
    g_signal_connect(app->draw_plot, "motion-notify-event", G_CALLBACK(on_plot_motion), app);
    g_signal_connect(app->draw_plot, "button-release-event", G_CALLBACK(on_plot_button_release), app);
    g_signal_connect(app->draw_plot, "scroll-event", G_CALLBACK(on_plot_scroll), app);
    gtk_widget_add_events(app->draw_plot,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
    gtk_box_pack_start(GTK_BOX(vbox), app->draw_plot, TRUE, TRUE, 0);

    *box = vbox;
}

/* ── simulation thread ── */
static gboolean update_progress(gpointer data) {
    App *app = (App *)data;
    g_mutex_lock(&app->sim_lock);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), app->sim_progress);
    gtk_label_set_text(GTK_LABEL(app->lbl_progress), app->sim_status);
    g_mutex_unlock(&app->sim_lock);
    gtk_widget_queue_draw(app->draw_plot);
    if (app->sim_done) {
        gtk_widget_set_sensitive(app->btn_run, TRUE);
        return FALSE;
    }
    return TRUE;
}

static gpointer sim_thread_fn(gpointer data) {
    App *app = (App *)data;

    Arena sim_arena = arena_new(1 << 24);
    System s;
    memset(&s, 0, sizeof(s));
    s.fault_bus = -1;

    if (raw_parse(app->raw_path, &s, &sim_arena) != 0) {
        g_mutex_lock(&app->sim_lock);
        snprintf(app->sim_status, sizeof(app->sim_status), "RAW parse failed");
        app->sim_done = 1;
        g_mutex_unlock(&app->sim_lock);
        arena_free(&sim_arena);
        return NULL;
    }
    if (dyr_parse(app->dyr_path, &s, &sim_arena) != 0) {
        g_mutex_lock(&app->sim_lock);
        snprintf(app->sim_status, sizeof(app->sim_status), "DYR parse failed");
        app->sim_done = 1;
        g_mutex_unlock(&app->sim_lock);
        arena_free(&sim_arena);
        return NULL;
    }

    Event *sim_events = NULL;
    int sim_n_events = 0;
    for (int i = 0; i < app->n_events; i++) {
        Event e; memset(&e, 0, sizeof(e));
        e.type = app->events[i].type;
        e.time = app->events[i].time;
        e.bus = app->events[i].bus;
        e.fault_r = app->events[i].fault_r;
        e.fault_x = app->events[i].fault_x;
        arrpush(sim_events, e);
    }
    sim_n_events = (int)arrlen(sim_events);

    double t_end = app->t_end;
    double t_step = app->t_step;
    app->sim_t_end = t_end;

    if (ybus_build(&s, &sim_arena) != 0) {
        g_mutex_lock(&app->sim_lock);
        snprintf(app->sim_status, sizeof(app->sim_status), "Y-bus build failed");
        app->sim_done = 1; g_mutex_unlock(&app->sim_lock);
        arrfree(sim_events); arena_free(&sim_arena);
        return NULL;
    }
    powerflow_solve(&s, &sim_arena);
    if (machine_init(&s) != 0) {
        g_mutex_lock(&app->sim_lock);
        snprintf(app->sim_status, sizeof(app->sim_status), "Machine init failed");
        app->sim_done = 1; g_mutex_unlock(&app->sim_lock);
        arrfree(sim_events); arena_free(&sim_arena);
        return NULL;
    }

    for (int i = 0; i < s.nbus; i++) {
        Bus *b = &s.bus[i];
        double Vm2 = b->vm * b->vm;
        b->gl = Vm2 > 1e-10 ? b->pd / Vm2 : 0;
        b->bl = Vm2 > 1e-10 ? -b->qd / Vm2 : 0;
    }

    DAE dae; memset(&dae, 0, sizeof(dae));
    if (dae_init(&dae, &s, &sim_arena) != 0) {
        g_mutex_lock(&app->sim_lock);
        snprintf(app->sim_status, sizeof(app->sim_status), "DAE init failed");
        app->sim_done = 1; g_mutex_unlock(&app->sim_lock);
        arrfree(sim_events); arena_free(&sim_arena);
        return NULL;
    }

    Integrator itg; memset(&itg, 0, sizeof(itg));
    if (integrator_init(&itg, &dae, 0.0) != 0) {
        g_mutex_lock(&app->sim_lock);
        snprintf(app->sim_status, sizeof(app->sim_status), "Integrator init failed");
        app->sim_done = 1; g_mutex_unlock(&app->sim_lock);
        arrfree(sim_events); arena_free(&sim_arena);
        return NULL;
    }

    double t = 0.0, t_next = t_step;
    int step = 0, ev_idx = 0;
    int ndiff = dae.ndiff;
    int n_gen = s.nmachines;
    int n_bus = s.nbus;

    plot_clear(app);

    while (t < t_end - 1e-10 && app->sim_running) {
        double tout = t_next;
        if (tout > t_end) tout = t_end;
        for (int ei = ev_idx; ei < sim_n_events; ei++) {
            if (sim_events[ei].time > t + 1e-10 && sim_events[ei].time < tout + 1e-10) {
                tout = sim_events[ei].time; break;
            }
        }

        double tret;
        int ret = IDASolve(itg.ida_mem, tout, &tret, itg.nvec_y, itg.nvec_ydot, IDA_NORMAL);
        if (ret < 0) {
            g_mutex_lock(&app->sim_lock);
            snprintf(app->sim_status, sizeof(app->sim_status), "IDA failed at t=%.4f", t);
            app->sim_done = 1; g_mutex_unlock(&app->sim_lock);
            break;
        }
        t = tret;

        int events_fired = 0;
        while (ev_idx < sim_n_events && sim_events[ev_idx].time <= t + 1e-10) {
            events_apply(&s, &sim_events[ev_idx], t);
            ev_idx++; events_fired = 1;
        }
        if (events_fired) {
            ybus_build(&s, &sim_arena);
            double *ydata = N_VGetArrayPointer(itg.nvec_y);
            double *ypd = N_VGetArrayPointer(itg.nvec_ydot);
            double *Vtmp = malloc((size_t)(2 * s.nbus) * sizeof(double));
            if (events_post_state(&s, ydata, Vtmp) == 0) {
                for (int i = 0; i < s.nbus; i++) {
                    ydata[ndiff + 2*i] = Vtmp[2*i];
                    ydata[ndiff + 2*i + 1] = Vtmp[2*i + 1];
                }
                memset(ypd, 0, (size_t)dae.neq * sizeof(double));
                double ws = 2.0 * M_PI * 60.0;
                for (int m = 0; m < s.nmachines; m++) {
                    Machine *mc = &s.machine[m];
                    Gen *gen = &s.gen[mc->gen_idx];
                    int bi = gen->bus;
                    double Vr = ydata[ndiff + 2*bi], Vi = ydata[ndiff + 2*bi + 1];
                    double d = ydata[2*m], Ep = mc->Ep, xdp = mc->xdp;
                    double Pe = Vr*(Ep*sin(d)-Vi)/xdp + Vi*(-(Ep*cos(d)-Vr))/xdp;
                    double TwoH = 2.0 * mc->h;
                    double om = ydata[2*m+1];
                    if (om < 0.1 || om > 2.0) om = 1.0;
                    ypd[2*m] = ws * (om - 1.0);
                    ypd[2*m+1] = ws / TwoH * (gen->pg - Pe - mc->d*(om-1.0));
                }
                double *y_save = malloc((size_t)dae.neq * sizeof(double));
                memcpy(y_save, ydata, (size_t)dae.neq * sizeof(double));
                int icr = IDACalcIC(itg.ida_mem, IDA_Y_INIT, 0.01);
                if (icr != 0) {
                    memcpy(ydata, y_save, (size_t)dae.neq * sizeof(double));
                    memset(ypd, 0, (size_t)dae.neq * sizeof(double));
                }
                free(y_save);
                IDASetMaxOrd(itg.ida_mem, 1);
                IDAReInit(itg.ida_mem, t, itg.nvec_y, itg.nvec_ydot);
                IDASStolerances(itg.ida_mem, 1e-4, 1e-6);
            }
            free(Vtmp);
        }

        double *y = N_VGetArrayPointer(itg.nvec_y);
        double gen_delta[16], gen_pe[16], gen_omega[16], gen_vt[16];
        double bus_vm[32], bus_va[32];
        for (int m = 0; m < n_gen && m < 16; m++) {
            int bi = s.gen[s.machine[m].gen_idx].bus;
            double d = y[2*m], om = y[2*m+1];
            double Vr = y[ndiff + 2*bi], Vi = y[ndiff + 2*bi + 1];
            double Ep = s.machine[m].Ep, xdp = s.machine[m].xdp;
            double Pe = Vr*(Ep*sin(d)-Vi)/xdp + Vi*(-(Ep*cos(d)-Vr))/xdp;
            double Vt = sqrt(Vr*Vr + Vi*Vi);
            gen_delta[m] = d; gen_pe[m] = Pe; gen_omega[m] = om; gen_vt[m] = Vt;
        }
        for (int i = 0; i < n_bus && i < 32; i++) {
            double Vr = y[ndiff + 2*i], Vi = y[ndiff + 2*i + 1];
            bus_vm[i] = sqrt(Vr*Vr + Vi*Vi);
            bus_va[i] = atan2(Vi, Vr) * 180.0 / M_PI;
        }
        plot_push(app, t, gen_delta, gen_pe, gen_omega, gen_vt, bus_vm, bus_va);

        g_mutex_lock(&app->sim_lock);
        app->sim_progress = t / t_end;
        snprintf(app->sim_status, sizeof(app->sim_status), "t=%.3f / %.1f  step=%d", t, t_end, step);
        g_mutex_unlock(&app->sim_lock);

        step++;
        while (t_next <= t + 1e-10) t_next += t_step;
    }

    g_mutex_lock(&app->sim_lock);
    if (app->sim_running)
        snprintf(app->sim_status, sizeof(app->sim_status), "Done: t=%.4f steps=%d", t, step);
    else
        snprintf(app->sim_status, sizeof(app->sim_status), "Stopped at t=%.4f", t);
    app->sim_running = 0;
    app->sim_done = 1;
    g_mutex_unlock(&app->sim_lock);

    integrator_free(&itg);
    dae_free(&dae);
    free(s.machine_states);
    free(s.colptr); free(s.rowidx); free(s.yval);
    arrfree(s.machine); arrfree(s.bus);
    arrfree(s.branch); arrfree(s.gen); arrfree(s.load);
    arrfree(sim_events);
    arena_free(&sim_arena);
    return NULL;
}

static void sim_run(App *app) {
    if (app->sim_running) return;
    if (!app->sys_loaded) {
        GtkWidget *dlg = gtk_message_dialog_new(NULL,
            GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Load RAW and DYR files first!");
        gtk_dialog_run(GTK_DIALOG(dlg)); gtk_widget_destroy(dlg);
        return;
    }

    app->t_end = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->spin_tend));
    app->t_step = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->spin_tstep));

    app->sim_running = 1;
    app->sim_done = 0;
    app->sim_progress = 0;
    app->view_t0 = 0;
    app->view_t1 = app->t_end;
    snprintf(app->sim_status, sizeof(app->sim_status), "Starting...");
    gtk_widget_set_sensitive(app->btn_run, FALSE);

    char b[64];
    snprintf(b, sizeof(b), "View: 0.00 – %.2f s", app->t_end);
    gtk_label_set_text(GTK_LABEL(app->lbl_view_range), b);

    g_timeout_add(100, update_progress, app);
    app->sim_thread = g_thread_new("sim", sim_thread_fn, app);
}

static void sim_stop(App *app) {
    app->sim_running = 0;
}

/* ── app init/cleanup ── */
static void app_init(App *app) {
    memset(app, 0, sizeof(*app));
    app->arena = arena_new(1 << 24);
    memset(&app->sys, 0, sizeof(app->sys));
    app->sys.fault_bus = -1;
    g_mutex_init(&app->sim_lock);
    for (int s = 0; s < MAX_SIGNALS; s++)
        app->signals[s].data = calloc(PLOT_BUF, sizeof(double));
}

static void app_cleanup(App *app) {
    app->sim_running = 0;
    if (app->sim_thread) { g_thread_join(app->sim_thread); app->sim_thread = NULL; }
    for (int s = 0; s < MAX_SIGNALS; s++) free(app->signals[s].data);
    free(app->sys.machine_states);
    free(app->sys.colptr); free(app->sys.rowidx); free(app->sys.yval);
    arrfree(app->sys.machine); arrfree(app->sys.bus);
    arrfree(app->sys.branch); arrfree(app->sys.gen); arrfree(app->sys.load);
    arena_free(&app->arena);
    g_mutex_clear(&app->sim_lock);
}

/* ── build UI ── */
static void build_ui(App *app) {
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "TSS — Transient Stability Simulator");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1400, 900);
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *menubar = build_menu(app);
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    /* left: network on top, events on bottom */
    GtkWidget *left_vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    GtkWidget *net_box;
    build_network_panel(app, &net_box);
    GtkWidget *net_frame = gtk_frame_new("Network");
    gtk_container_add(GTK_CONTAINER(net_frame), net_box);
    gtk_paned_pack1(GTK_PANED(left_vpaned), net_frame, TRUE, FALSE);

    GtkWidget *ev_box;
    build_event_panel(app, &ev_box);
    GtkWidget *ev_frame = gtk_frame_new("Events");
    gtk_container_add(GTK_CONTAINER(ev_frame), ev_box);
    gtk_paned_pack2(GTK_PANED(left_vpaned), ev_frame, FALSE, FALSE);

    gtk_paned_pack1(GTK_PANED(hpaned), left_vpaned, TRUE, FALSE);

    /* right: sim+plot on top, signals on bottom */
    GtkWidget *right_vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);

    GtkWidget *sim_box;
    build_sim_panel(app, &sim_box);
    GtkWidget *sim_frame = gtk_frame_new("Simulation");
    gtk_container_add(GTK_CONTAINER(sim_frame), sim_box);
    gtk_paned_pack1(GTK_PANED(right_vpaned), sim_frame, TRUE, FALSE);

    /* signal tree */
    GtkWidget *sig_frame = gtk_frame_new("Signals");
    GtkWidget *sig_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    app->sig_store = gtk_tree_store_new(SIG_N_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_INT);
    app->sig_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->sig_store));
    GtkCellRenderer *r_check = gtk_cell_renderer_toggle_new();
    g_signal_connect(r_check, "toggled", G_CALLBACK(sig_toggled), app);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->sig_tree), 0, "", r_check, "active", SIG_COL_CHECK, NULL);
    GtkCellRenderer *r_text = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->sig_tree), 1, "Signal", r_text, "text", SIG_COL_LABEL, NULL);
    GtkWidget *sig_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(sig_sw, -1, 200);
    gtk_container_add(GTK_CONTAINER(sig_sw), app->sig_tree);
    gtk_box_pack_start(GTK_BOX(sig_vbox), sig_sw, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(sig_frame), sig_vbox);
    gtk_paned_pack2(GTK_PANED(right_vpaned), sig_frame, FALSE, FALSE);

    gtk_paned_pack2(GTK_PANED(hpaned), right_vpaned, TRUE, FALSE);

    gtk_box_pack_start(GTK_BOX(vbox), hpaned, TRUE, TRUE, 0);

    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(status_box), 3);
    app->lbl_status = gtk_label_new("Ready");
    gtk_box_pack_start(GTK_BOX(status_box), app->lbl_status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status_box, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(app->window), vbox);
    gtk_widget_show_all(app->window);
}

int main(int argc, char **argv) {
    MPI_Init(NULL, NULL);
    gtk_init(&argc, &argv);
    App app;
    app_init(&app);
    build_ui(&app);
    gtk_main();
    app_cleanup(&app);
    MPI_Finalize();
    return 0;
}
