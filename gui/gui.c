#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transient.h"

#define PLOT_BUF 65536
#define MAX_SIGNALS 256
#define MAX_COLORS  24
#define MAX_EVENTS  64
#define PLOT_FILE "/tmp/tss_plot.dat"
#define OV_SIZE 16384

typedef enum {
    SIG_GEN_DELTA, SIG_GEN_PE, SIG_GEN_OMEGA, SIG_GEN_VT,
    SIG_BUS_VM, SIG_BUS_VA,
    SIG_OSC_VA, SIG_OSC_VB, SIG_OSC_VC,
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
    int       fault_phase;
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

    FILE    *plot_fp;
    int      plot_n_records;
    int      plot_file_nsig;

    /* overview (fixed size, always in memory) */
    double *ov_t;
    double *ov_data;
    int     ov_len;
    int     ov_nsig;

    /* full-res cache for current zoomed view */
    double *fr_t;
    double *fr_data;
    int     fr_len;
    double  fr_t0, fr_t1;
    int     fr_nsig;
    int     fr_valid;

    /* loader thread */
    int      plot_width;
    GThread *loader_thread;
    int      loader_cancel;
    GMutex   loader_lock;

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

    /* context menu for events */
    GtkWidget *event_menu;
    /* context menus for network tabs */
    GtkWidget *bus_menu, *branch_menu, *gen_menu, *load_menu;

    char raw_path[512];
    char dyr_path[512];
    char ini_path[512];

    int  scope_bus;
    GtkWidget *spin_scope;
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
static void  sim_add_event_dialog(App *app);
static void  sim_delete_selected(App *app);
static void  sim_save_events(App *app);
static void  sim_save_events_as(App *app);
static void  save_raw(App *app);
static void  save_dyr(App *app);
static void  add_bus_dialog(App *app);
static void  add_branch_dialog(App *app);
static void  add_gen_dialog(App *app);
static void  add_load_dialog(App *app);
static void  delete_selected_bus(App *app);
static void  delete_selected_branch(App *app);
static void  delete_selected_gen(App *app);
static void  delete_selected_load(App *app);
static void  on_bus_menu_save_raw(GtkMenuItem *, App *);
static void  on_bus_menu_save_dyr(GtkMenuItem *, App *);
static void  on_branch_menu_save_raw(GtkMenuItem *, App *);
static void  on_branch_menu_save_dyr(GtkMenuItem *, App *);
static void  on_gen_menu_save_raw(GtkMenuItem *, App *);
static void  on_gen_menu_save_dyr(GtkMenuItem *, App *);
static void  on_load_menu_save_raw(GtkMenuItem *, App *);
static void  on_load_menu_save_dyr(GtkMenuItem *, App *);
static void  view_zoom_in(App *app);
static void  view_zoom_out(App *app);
static void  view_fit(App *app);
static void  view_select_all(App *app);
static void  view_select_none(App *app);
static void  on_btn_run(GtkWidget *btn, App *app);
static gpointer sim_thread_fn(gpointer data);

static void loader_cancel(App *app);
static void loader_start(App *app);
static void plot_build_overview(App *app);
static gboolean redraw_idle(gpointer data);
static gboolean on_tree_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);

/* network menu callbacks */
typedef struct { const char *label; int is_int; double def; double val; } FieldDef;
typedef void (*AddCallback)(App *, FieldDef *);
static void on_bus_menu_add(GtkMenuItem *item, App *app);
static void on_bus_menu_del(GtkMenuItem *item, App *app);
static void on_branch_menu_add(GtkMenuItem *item, App *app);
static void on_branch_menu_del(GtkMenuItem *item, App *app);
static void on_gen_menu_add(GtkMenuItem *item, App *app);
static void on_gen_menu_del(GtkMenuItem *item, App *app);
static void on_load_menu_add(GtkMenuItem *item, App *app);
static void on_load_menu_del(GtkMenuItem *item, App *app);
static void add_bus_dialog(App *app);
static void add_branch_dialog(App *app);
static void add_gen_dialog(App *app);
static void add_load_dialog(App *app);
static void delete_selected_bus(App *app);
static void delete_selected_branch(App *app);
static void delete_selected_gen(App *app);
static void delete_selected_load(App *app);

static const char *btype(BusType t) {
    switch(t) { case BUS_SLACK: return "SLACK"; case BUS_PV: return "PV"; case BUS_PQ: return "PQ"; default: return "ISO"; }
}
static const char *fault_name(int type, int phase) {
    if (type == FAULT_CLEAR) return "clear";
    if (type == FAULT) return "ABC";
    static const char *map[] = { [FAULT_PHASE_AG]="AG", [FAULT_PHASE_BG]="BG", [FAULT_PHASE_CG]="CG",
                                 [FAULT_PHASE_AB]="AB", [FAULT_PHASE_BC]="BC", [FAULT_PHASE_CA]="CA" };
    if (type == FAULT_SLG && phase >= FAULT_PHASE_AG && phase <= FAULT_PHASE_CG) return map[phase];
    if (type == FAULT_LL  && phase >= FAULT_PHASE_AB && phase <= FAULT_PHASE_CA) return map[phase];
    if (type == FAULT_DLG && phase >= FAULT_PHASE_AB && phase <= FAULT_PHASE_CA) {
        static char buf[8];
        snprintf(buf, sizeof(buf), "%sG", map[phase]);
        return buf;
    }
    return "ABC";
}
static const char *fmt_dbl(double v) {
    static char bufs[4][32];
    static int idx = 0;
    idx = (idx + 1) & 3;
    snprintf(bufs[idx], sizeof(bufs[0]), "%.3e", v);
    return bufs[idx];
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
                      const double *bus_vm, const double *bus_va,
                      double osc_va, double osc_vb, double osc_vc) {
    if (!app->plot_fp) return;
    fwrite(&t, sizeof(double), 1, app->plot_fp);
    for (int s = 0; s < app->n_signals; s++) {
        Signal *sig = &app->signals[s];
        double v = 0;
        switch (sig->type) {
        case SIG_GEN_DELTA: v = gen_delta[sig->idx]; break;
        case SIG_GEN_PE:    v = gen_pe[sig->idx]; break;
        case SIG_GEN_OMEGA: v = gen_omega[sig->idx]; break;
        case SIG_GEN_VT:    v = gen_vt[sig->idx]; break;
        case SIG_BUS_VM:    v = bus_vm[sig->idx]; break;
        case SIG_BUS_VA:    v = bus_va[sig->idx]; break;
        case SIG_OSC_VA:    v = osc_va; break;
        case SIG_OSC_VB:    v = osc_vb; break;
        case SIG_OSC_VC:    v = osc_vc; break;
        }
        fwrite(&v, sizeof(double), 1, app->plot_fp);
    }
    app->plot_n_records++;
}

static void plot_clear(App *app) {
    if (app->plot_fp) { fclose(app->plot_fp); app->plot_fp = NULL; }
    app->plot_fp = fopen(PLOT_FILE, "wb");
    app->plot_n_records = 0;
    app->plot_file_nsig = app->n_signals;
    app->ov_len = 0;
    app->fr_len = 0;
    app->fr_valid = 0;
    loader_cancel(app);
}

static void plot_file_close(App *app) {
    if (app->plot_fp) { fclose(app->plot_fp); app->plot_fp = NULL; }
    loader_cancel(app);
    remove(PLOT_FILE);
    free(app->ov_t); free(app->ov_data);
    free(app->fr_t); free(app->fr_data);
    app->ov_t = NULL; app->ov_data = NULL;
    app->fr_t = NULL; app->fr_data = NULL;
}

static void plot_build_overview(App *app) {
    FILE *fp = fopen(PLOT_FILE, "rb");
    if (!fp) return;
    int nsig = app->plot_file_nsig;
    int total = app->plot_n_records;
    int rec_size = 1 + nsig;

    int n_buckets = OV_SIZE / 2;
    if (n_buckets < 1) n_buckets = 1;

    double *buf = malloc(rec_size * sizeof(double));
    int bucket_size = total / n_buckets;
    if (bucket_size < 1) bucket_size = 1;

    int out = 0;
    for (int b = 0; b < n_buckets && out < OV_SIZE - 2; b++) {
        int start = b * bucket_size;
        int end = (b + 1) * bucket_size;
        if (b == n_buckets - 1) end = total;
        if (start >= total) break;

        fseek(fp, (long)(start * rec_size) * sizeof(double), SEEK_SET);
        fread(buf, sizeof(double), rec_size, fp);
        double t_first = buf[0];
        double *minv = malloc((size_t)nsig * sizeof(double));
        double *maxv = malloc((size_t)nsig * sizeof(double));
        for (int s = 0; s < nsig; s++) {
            minv[s] = buf[1 + s];
            maxv[s] = buf[1 + s];
        }

        int n_in_bucket = end - start;
        int ss = n_in_bucket / 10;
        if (ss < 1) ss = 1;
        for (int i = start + ss; i < end; i += ss) {
            fseek(fp, (long)(i * rec_size) * sizeof(double), SEEK_SET);
            fread(buf, sizeof(double), rec_size, fp);
            for (int s = 0; s < nsig; s++) {
                double v = buf[1 + s];
                if (v < minv[s]) minv[s] = v;
                if (v > maxv[s]) maxv[s] = v;
            }
        }

        app->ov_t[out] = t_first;
        for (int s = 0; s < nsig; s++)
            app->ov_data[out * nsig + s] = (minv[s] + maxv[s]) * 0.5;
        out++;

        free(minv);
        free(maxv);
    }

    app->ov_len = out;
    app->ov_nsig = nsig;
    free(buf);
    fclose(fp);
}

static void loader_cancel(App *app) {
    GThread *old = NULL;
    g_mutex_lock(&app->loader_lock);
    app->loader_cancel = 1;
    old = app->loader_thread;
    app->loader_thread = NULL;
    g_mutex_unlock(&app->loader_lock);
    if (old) g_thread_join(old);
}

static gpointer loader_thread_fn(gpointer data) {
    App *app = (App *)data;
    double t0 = app->view_t0, t1 = app->view_t1;
    int nsig = app->plot_file_nsig;
    int rec_size = 1 + nsig;
    int max_pts = app->plot_width * 2;
    if (max_pts < 100) max_pts = 100;

    FILE *fp = fopen(PLOT_FILE, "rb");
    if (!fp) return NULL;

    g_mutex_lock(&app->loader_lock);
    int cancelled = app->loader_cancel;
    g_mutex_unlock(&app->loader_lock);
    if (cancelled) { fclose(fp); return NULL; }

    int lo = 0, hi = app->plot_n_records - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        double t;
        fseek(fp, (long)(mid * rec_size) * sizeof(double), SEEK_SET);
        fread(&t, sizeof(double), 1, fp);
        if (t < t0) lo = mid + 1; else hi = mid;
    }
    int start_rec = lo;

    /* Binary search for end record (last with t <= t1, inclusive) */
    lo = start_rec; hi = app->plot_n_records - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        double t;
        fseek(fp, (long)(mid * rec_size) * sizeof(double), SEEK_SET);
        fread(&t, sizeof(double), 1, fp);
        if (t > t1) hi = mid - 1; else lo = mid;
    }
    int end_rec = lo;
    int total_visible = end_rec - start_rec + 1;

    int stride = total_visible / max_pts;
    if (stride < 1) stride = 1;
    /* Ensure last stride-sampled point is within 'stride' of end_rec */
    int n_before = (total_visible - 1) / stride;
    if (n_before + 1 > max_pts) {
        stride = (total_visible - 1) / (max_pts - 1) + 1;
        n_before = (total_visible - 1) / stride;
    }
    int n_out = n_before + 1;  /* stride-sampled + end_rec */
    if (n_out > max_pts) n_out = max_pts;

    double *buf = malloc(rec_size * sizeof(double));
    double *ft = malloc(n_out * sizeof(double));
    double *fd = malloc(n_out * nsig * sizeof(double));
    int idx = 0;
    for (int i = start_rec; i < end_rec && idx < n_before; i += stride) {
        g_mutex_lock(&app->loader_lock);
        int cancel = app->loader_cancel;
        g_mutex_unlock(&app->loader_lock);
        if (cancel) { free(ft); free(fd); free(buf); fclose(fp); return NULL; }

        fseek(fp, (long)(i * rec_size) * sizeof(double), SEEK_SET);
        fread(buf, sizeof(double), rec_size, fp);
        ft[idx] = buf[0];
        for (int s = 0; s < nsig; s++)
            fd[idx * nsig + s] = buf[1 + s];
        idx++;
    }
    /* always include the last record in the range (end_rec) */
    if (idx < n_out) {
        fseek(fp, (long)(end_rec * rec_size) * sizeof(double), SEEK_SET);
        fread(buf, sizeof(double), rec_size, fp);
        ft[idx] = buf[0];
        for (int s = 0; s < nsig; s++)
            fd[idx * nsig + s] = buf[1 + s];
        idx++;
    }
    free(buf);
    fclose(fp);

    g_mutex_lock(&app->loader_lock);
    if (!app->loader_cancel) {
        free(app->fr_t); free(app->fr_data);
        app->fr_t = ft;
        app->fr_data = fd;
        app->fr_len = idx;
        app->fr_t0 = t0;
        app->fr_t1 = t1;
        app->fr_nsig = nsig;
        app->fr_valid = 1;
        g_idle_add(redraw_idle, app->draw_plot);
    } else {
        free(ft); free(fd);
    }
    app->loader_cancel = 0;
    g_mutex_unlock(&app->loader_lock);

    return NULL;
}

static void loader_start(App *app) {
    loader_cancel(app);
    g_mutex_lock(&app->loader_lock);
    app->fr_valid = 0;
    app->loader_cancel = 0;
    app->loader_thread = g_thread_new("loader", loader_thread_fn, app);
    g_mutex_unlock(&app->loader_lock);
}

static gboolean redraw_idle(gpointer data) {
    if (data && GTK_IS_WIDGET(data))
        gtk_widget_queue_draw(GTK_WIDGET(data));
    return FALSE;
}

/* ── unified plot drawing (uses overview + full-res cache) ── */
static gboolean on_draw_plot(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    App *app = (App *)user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    int w = alloc.width, h = alloc.height;
    app->plot_width = w;
    int pad = 55;

    cairo_set_source_rgb(cr, 0.12, 0.12, 0.16);
    cairo_paint(cr);

    int nvis = 0;
    for (int s = 0; s < app->n_signals; s++)
        if (app->signals[s].visible) nvis++;

    if (app->plot_n_records < 2 || nvis == 0 || !app->ov_data) {
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, w/2 - 60, h/2);
        cairo_show_text(cr, app->plot_n_records < 2 ? "No simulation data" : "No signals selected");
        return FALSE;
    }

    double t0 = app->view_t0, t1 = app->view_t1;
    if (t1 - t0 < 1e-10) { t0 = 0; t1 = 1; }

    /* Decide which data source to use: full-res cache or overview */
    int use_fr = 0;
    g_mutex_lock(&app->loader_lock);
    if (app->fr_valid && app->fr_t && app->fr_data) {
        if (app->fr_t0 <= t0 + 1e-10 && app->fr_t1 >= t1 - 1e-10)
            use_fr = 1;
    }
    g_mutex_unlock(&app->loader_lock);

    int nsig = use_fr ? app->fr_nsig : app->ov_nsig;
    int n_pts = use_fr ? app->fr_len : app->ov_len;
    double *src_t = use_fr ? app->fr_t : app->ov_t;
    double *src_data = use_fr ? app->fr_data : app->ov_data;

    /* Build visible signal map */
    int *vis_sig_map = malloc(nvis * sizeof(int));
    int vi = 0;
    for (int s = 0; s < app->n_signals; s++)
        if (app->signals[s].visible) vis_sig_map[vi++] = s;

    /* Find y range from visible data */
    double y_min = 1e30, y_max = -1e30;
    if (n_pts == 0) {
        /* Overview not ready yet */
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, w/2 - 40, h/2);
        cairo_show_text(cr, "Loading overview...");
        cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, pad, pad, w - 2*pad, h - 2*pad);
        cairo_stroke(cr);
        free(vis_sig_map);
        return FALSE;
    }
    for (int i = 0; i < n_pts; i++) {
        double t = src_t[i];
        if (t < t0 - 1e-10 || t > t1 + 1e-10) continue;
        for (int vi2 = 0; vi2 < nvis; vi2++) {
            double v = src_data[i * nsig + vis_sig_map[vi2]];
            if (v < y_min) y_min = v;
            if (v > y_max) y_max = v;
        }
    }
    if (y_max - y_min < 1e-10) { y_min -= 1; y_max += 1; }
    double ym = (y_max - y_min) * 0.05; y_min -= ym; y_max += ym;

    double px(double t) { return pad + (t - t0) / (t1 - t0) * (w - 2*pad); }
    double py(double v) { return h - pad - (v - y_min) / (y_max - y_min) * (h - 2*pad); }

    /* Grid */
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

    /* Axis labels */
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    char buf[32];
    for (int i = 0; i <= 5; i++) {
        double v = y_min + (y_max - y_min) * i / 5;
        snprintf(buf, sizeof(buf), "%.3f", v);
        if (fabs(v) < 1e-4 || fabs(v) > 1e4) {
            snprintf(buf, sizeof(buf), "%.3e", v);
        }
        cairo_move_to(cr, 2, py(v) + 4);
        cairo_show_text(cr, buf);
    }
    for (int i = 0; i <= 5; i++) {
        double t = t0 + (t1 - t0) * i / 5;
        const char *tf = fmt_dbl(t);
        snprintf(buf, sizeof(buf), "%s", tf);
        cairo_move_to(cr, px(t) - 12, h - 5);
        cairo_show_text(cr, buf);
    }
    cairo_move_to(cr, 2, h - 1);
    cairo_show_text(cr, "Time (s)");

    /* Draw signals */
    int ci = 0;
    int plot_pixels = w - 2 * pad;
    int stride = 1;
    if (n_pts > plot_pixels * 2)
        stride = n_pts / (plot_pixels * 2);

    for (int vi2 = 0; vi2 < nvis; vi2++) {
        const double *c = sig_colors[ci % MAX_COLORS];
        cairo_set_source_rgb(cr, c[0], c[1], c[2]);
        cairo_set_line_width(cr, 1.5);
        int start = -1;
        for (int i = 0; i < n_pts; i += stride) {
            double t = src_t[i];
            if (t < t0 - 1e-10 || t > t1 + 1e-10) { start = -1; continue; }
            double v = src_data[i * nsig + vis_sig_map[vi2]];
            if (start < 0) { cairo_move_to(cr, px(t), py(v)); start = i; }
            else cairo_line_to(cr, px(t), py(v));
        }
        cairo_stroke(cr);
        ci++;
    }

    /* Legend */
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

    /* Drag selection */
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

    free(vis_sig_map);
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
            snprintf(b, sizeof(b), "View: %s – %s s", fmt_dbl(app->view_t0), fmt_dbl(app->view_t1));
            gtk_label_set_text(GTK_LABEL(app->lbl_view_range), b);
            loader_cancel(app);
            loader_start(app);
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
    snprintf(b, sizeof(b), "View: %s – %s s", fmt_dbl(app->view_t0), fmt_dbl(app->view_t1));
    gtk_label_set_text(GTK_LABEL(app->lbl_view_range), b);
    loader_cancel(app);
    gtk_widget_queue_draw(widget);
    loader_start(app);
    return FALSE;
}

static void view_zoom_in(App *app) {
    double mid = (app->view_t0 + app->view_t1) / 2;
    double span = (app->view_t1 - app->view_t0) * 0.35;
    app->view_t0 = mid - span;
    app->view_t1 = mid + span;
    if (app->view_t0 < 0) app->view_t0 = 0;
    char b[64];
    snprintf(b, sizeof(b), "View: %s – %s s", fmt_dbl(app->view_t0), fmt_dbl(app->view_t1));
    gtk_label_set_text(GTK_LABEL(app->lbl_view_range), b);
    loader_cancel(app);
    gtk_widget_queue_draw(app->draw_plot);
    loader_start(app);
}

static void view_zoom_out(App *app) {
    double mid = (app->view_t0 + app->view_t1) / 2;
    double span = (app->view_t1 - app->view_t0) * 0.75;
    app->view_t0 = mid - span;
    app->view_t1 = mid + span;
    if (app->view_t0 < 0) app->view_t0 = 0;
    char b[64];
    snprintf(b, sizeof(b), "View: %s – %s s", fmt_dbl(app->view_t0), fmt_dbl(app->view_t1));
    gtk_label_set_text(GTK_LABEL(app->lbl_view_range), b);
    loader_cancel(app);
    gtk_widget_queue_draw(app->draw_plot);
    loader_start(app);
}

static void view_fit(App *app) {
    app->view_t0 = 0;
    app->view_t1 = app->sim_t_end > 0 ? app->sim_t_end : app->t_end;
    char b[64];
    snprintf(b, sizeof(b), "View: %s – %s s", fmt_dbl(app->view_t0), fmt_dbl(app->view_t1));
    gtk_label_set_text(GTK_LABEL(app->lbl_view_range), b);
    loader_cancel(app);
    gtk_widget_queue_draw(app->draw_plot);
    loader_start(app);
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
static void on_save_results(GtkMenuItem *item, App *app) {
    App *a = app;
    if (!a->sys_loaded || a->plot_n_records == 0) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(a->window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "Run a simulation first!");
        gtk_dialog_run(GTK_DIALOG(dlg)); gtk_widget_destroy(dlg);
        return;
    }

    GtkWidget *dlg = gtk_dialog_new_with_buttons("Export Results",
        GTK_WINDOW(a->window), GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Export", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(area), 10);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);

    GtkWidget *chk_csv = gtk_check_button_new_with_label("Export CSV (generator/bus data)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_csv), TRUE);
    gtk_grid_attach(GTK_GRID(grid), chk_csv, 0, 0, 3, 1);

    GtkWidget *chk_comtrade = gtk_check_button_new_with_label("Export COMTRADE");
    gtk_grid_attach(GTK_GRID(grid), chk_comtrade, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Bus:"), 1, 1, 1, 1);
    GtkWidget *spin_bus = gtk_spin_button_new_with_range(1, 9999, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_bus), 1);
    gtk_grid_attach(GTK_GRID(grid), spin_bus, 2, 1, 1, 1);

    GtkWidget *btn_dir = gtk_file_chooser_button_new("Output Directory", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(btn_dir), g_get_home_dir());
    gtk_grid_attach(GTK_GRID(grid), btn_dir, 0, 2, 3, 1);

    gtk_box_pack_start(GTK_BOX(area), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *outdir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(btn_dir));
        int do_csv = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_csv));
        int do_ct = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_comtrade));
        int ct_bus = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin_bus));

        FILE *fp = fopen(PLOT_FILE, "rb");
        if (!fp) { g_free(outdir); gtk_widget_destroy(dlg); return; }
        int nsig = a->plot_file_nsig;
        int nrec = a->plot_n_records;

        FILE *df = NULL, *vf = NULL;
        ComtradeWriter *ctw = NULL;
        int ct_idx = -1;
        char path[1024];

        /* COMTRADE setup */
        if (do_ct) {
            for (int i = 0; i < a->sys.nbus; i++)
                if (a->sys.bus[i].id == ct_bus) { ct_idx = i; break; }
            if (ct_idx >= 0) {
                double v_factor = comtrade_v_factor(a->sys.bus[ct_idx].base_kv);
                double i_factor = comtrade_i_factor(a->sys.base_mva, a->sys.bus[ct_idx].base_kv);
                double v_prim = a->sys.bus[ct_idx].base_kv * 1000.0;
                double i_prim_a = a->sys.base_mva * 1000.0 / (sqrt(3.0) * a->sys.bus[ct_idx].base_kv);
                ctw = comtrade_create(a->sys.bus[ct_idx].name, "TSS");
                char id[32];
                snprintf(id, sizeof(id), "V%da", ct_bus); comtrade_add_analog(ctw, id, "A", a->sys.bus[ct_idx].name, "kV", v_factor, 0, -2.5, 2.5, v_prim, 100.0, 0);
                snprintf(id, sizeof(id), "V%db", ct_bus); comtrade_add_analog(ctw, id, "B", a->sys.bus[ct_idx].name, "kV", v_factor, 0, -2.5, 2.5, v_prim, 100.0, 0);
                snprintf(id, sizeof(id), "V%dc", ct_bus); comtrade_add_analog(ctw, id, "C", a->sys.bus[ct_idx].name, "kV", v_factor, 0, -2.5, 2.5, v_prim, 100.0, 0);
                snprintf(id, sizeof(id), "I%da", ct_bus); comtrade_add_analog(ctw, id, "A", a->sys.bus[ct_idx].name, "kA", i_factor, 0, -10, 10, i_prim_a, 5.0, 0);
                snprintf(id, sizeof(id), "I%db", ct_bus); comtrade_add_analog(ctw, id, "B", a->sys.bus[ct_idx].name, "kA", i_factor, 0, -10, 10, i_prim_a, 5.0, 0);
                snprintf(id, sizeof(id), "I%dc", ct_bus); comtrade_add_analog(ctw, id, "C", a->sys.bus[ct_idx].name, "kA", i_factor, 0, -10, 10, i_prim_a, 5.0, 0);
                snprintf(path, sizeof(path), "%s/comtrade_bus%d.dat", outdir, ct_bus);
                comtrade_open_dat(ctw, path);
                double dt = a->t_step;
                ctw->sample_rate = (int)(1.0 / dt + 0.5);
                double trig_t = a->sys.fault_t0 > 0 ? a->sys.fault_t0 : 0;
                int tsec = (int)trig_t;
                int tusec = (int)((trig_t - tsec) * 1e6);
                int thh = tsec / 3600, tmm = (tsec / 60) % 60, tsss = tsec % 60;
                comtrade_set_time(ctw, 2026,1,1,0,0,0,0, 2026,1,1,thh,tmm,tsss,tusec);
            }
        }

        /* CSV setup */
        if (do_csv) {
            snprintf(path, sizeof(path), "%s/delta.csv", outdir); df = fopen(path, "w");
            snprintf(path, sizeof(path), "%s/voltage.csv", outdir); vf = fopen(path, "w");
            if (df) {
                fprintf(df, "time");
                for (int m = 0; m < a->sys.nmachines; m++) {
                    int bid = a->sys.bus[a->sys.gen[a->sys.machine[m].gen_idx].bus].id;
                    fprintf(df, ",d_g%d,Pe_g%d,Vt_g%d", bid, bid, bid);
                }
                fprintf(df, "\n");
            }
            if (vf) {
                fprintf(vf, "time");
                for (int i = 0; i < a->sys.nbus; i++)
                    fprintf(vf, ",V_%d,ang_%d", a->sys.bus[i].id, a->sys.bus[i].id);
                fprintf(vf, "\n");
            }
        }

        /* Read plot file and write */
        double *buf = malloc((size_t)(1 + nsig) * sizeof(double));
        int ct_sig_vm = -1, ct_sig_va = -1;
        for (int s = 0; s < a->n_signals; s++) {
            Signal *sig = &a->signals[s];
            if (sig->type == SIG_BUS_VM && sig->idx == ct_idx && ct_idx >= 0) ct_sig_vm = s;
            if (sig->type == SIG_BUS_VA && sig->idx == ct_idx && ct_idx >= 0) ct_sig_va = s;
        }

        /* Build Ybus for current computation */
        int have_ybus = 0;
        if (do_ct && ct_idx >= 0) {
            Arena ybus_arena = arena_new(1 << 20);
            if (ybus_build(&a->sys, &ybus_arena) == 0) {
                have_ybus = 1;
            }
            /* Find signal indices for all bus Vm/Va */
            int *vm_sig = malloc((size_t)a->sys.nbus * sizeof(int));
            int *va_sig = malloc((size_t)a->sys.nbus * sizeof(int));
            for (int i = 0; i < a->sys.nbus; i++) { vm_sig[i] = -1; va_sig[i] = -1; }
            for (int s = 0; s < a->n_signals; s++) {
                Signal *sig = &a->signals[s];
                if (sig->type == SIG_BUS_VM) vm_sig[sig->idx] = s;
                if (sig->type == SIG_BUS_VA) va_sig[sig->idx] = s;
            }

            for (int r = 0; r < nrec; r++) {
                if (fread(buf, sizeof(double), (size_t)(1 + nsig), fp) != (size_t)(1 + nsig)) break;
                double t = buf[0];

                if (df) {
                    fprintf(df, "%.6f", t);
                    for (int s = 0; s < a->n_signals; s++) {
                        Signal *sig = &a->signals[s];
                        if (sig->type == SIG_GEN_DELTA || sig->type == SIG_GEN_PE || sig->type == SIG_GEN_VT)
                            fprintf(df, ",%.12g", buf[1 + s]);
                    }
                    fprintf(df, "\n");
                }
                if (vf) {
                    fprintf(vf, "%.6f", t);
                    for (int s = 0; s < a->n_signals; s++) {
                        Signal *sig = &a->signals[s];
                        if (sig->type == SIG_BUS_VM || sig->type == SIG_BUS_VA)
                            fprintf(vf, ",%.12g", buf[1 + s]);
                    }
                    fprintf(vf, "\n");
                }
                if (ctw && ct_idx >= 0 && ct_sig_vm >= 0 && ct_sig_va >= 0) {
                    /* Reconstruct Vr,Vi for all buses from Vm/angle */
                    double *Vr_all = malloc((size_t)a->sys.nbus * sizeof(double));
                    double *Vi_all = malloc((size_t)a->sys.nbus * sizeof(double));
                    for (int i = 0; i < a->sys.nbus; i++) {
                        if (vm_sig[i] >= 0 && va_sig[i] >= 0) {
                            double Vm = buf[1+vm_sig[i]];
                            double ang = buf[1+va_sig[i]] * M_PI / 180.0;
                            Vr_all[i] = Vm * cos(ang);
                            Vi_all[i] = Vm * sin(ang);
                        } else { Vr_all[i] = 0; Vi_all[i] = 0; }
                    }
                    /* Compute injected current at ct_idx: I = Ybus * V */
                    double Ir = 0, Ii = 0;
                    for (int k = a->sys.colptr[ct_idx]; k < a->sys.colptr[ct_idx+1]; k++) {
                        int j = a->sys.rowidx[k];
                        double Gij = a->sys.yval[2*k], Bij = a->sys.yval[2*k+1];
                        Ir += Gij*Vr_all[j] - Bij*Vi_all[j];
                        Ii += Gij*Vi_all[j] + Bij*Vr_all[j];
                    }
                    free(Vr_all); free(Vi_all);
                    /* Determine fault type from events for sequence reconstruction */
                    int seq_type = 0; // 0=3ph, 1=SLG, 2=LL, 3=DLG
                    int seq_phase = FAULT_PHASE_ABC;
                    for (int ei = 0; ei < a->n_events; ei++) {
                        if (a->events[ei].type == FAULT_SLG) { seq_type = 1; seq_phase = a->events[ei].fault_phase; }
                        else if (a->events[ei].type == FAULT_LL) { seq_type = 2; seq_phase = a->events[ei].fault_phase; }
                        else if (a->events[ei].type == FAULT_DLG) { seq_type = 3; seq_phase = a->events[ei].fault_phase; }
                        else if (a->events[ei].type == FAULT) seq_type = 0;
                    }
                    /* Reconstruct phase voltages from V1 using sequence components */
                    double a2r=-0.5, a2i=-0.86602540378, ar=-0.5, ai=0.86602540378;
                    double Vm_ct = buf[1+ct_sig_vm];
                    double ang_ct = buf[1+ct_sig_va] * M_PI / 180.0;
                    double V1r = Vm_ct * cos(ang_ct), V1i = Vm_ct * sin(ang_ct);
                    double V2r=0, V2i=0, V0r=0, V0i=0;
                    if (seq_type == 2) {
                        /* LL: V2 = V1 (BC fault boundary at fault bus) */
                        V2r = V1r; V2i = V1i;
                    } else if (seq_type == 1) {
                        /* SLG: V2 = -V1/2, V0 = -V1/2 (AG fault with Z1=Z2=Z0) */
                        V2r = -0.5*V1r; V2i = -0.5*V1i;
                        V0r = V2r; V0i = V2i;
                    } else if (seq_type == 3) {
                        /* DLG: V2 = V1, V0 = V1 (BCG fault boundary) */
                        V2r = V1r; V2i = V1i;
                        V0r = V1r; V0i = V1i;
                    }
                    /* Va = V1 + V2 + V0 */
                    double Var = V1r+V2r+V0r, Vai = V1i+V2i+V0i;
                    /* Vb = a²*V1 + a*V2 + V0 */
                    double Vbr = a2r*V1r-a2i*V1i + ar*V2r-ai*V2i + V0r;
                    double Vbi = a2r*V1i+a2i*V1r + ar*V2i+ai*V2r + V0i;
                    /* Vc = a*V1 + a²*V2 + V0 */
                    double Vcr = ar*V1r-ai*V1i + a2r*V2r-a2i*V2i + V0r;
                    double Vci = ar*V1i+ai*V1r + a2r*V2i+a2i*V2r + V0i;
                    /* Rotate for phase variant (default is BC for LL/DLG, AG for SLG) */
                    double tar=Var, tai=Vai, tbr=Vbr, tbi=Vbi, tcr=Vcr, tci=Vci;
                    if (seq_type == 2 || seq_type == 3) {
                        /* Default BC; rotate based on phase (matches rotate_3ph) */
                        switch (seq_phase) {
                        case FAULT_PHASE_AB:
                            Var = tbr; Vai = tbi; Vbr = tcr; Vbi = tci; Vcr = tar; Vci = tai; break;
                        case FAULT_PHASE_CA:
                            Var = tcr; Vai = tci; Vbr = tar; Vbi = tai; Vcr = tbr; Vci = tbi; break;
                        default: break; /* BC = default, no change */
                        }
                    } else if (seq_type == 1) {
                        /* Default AG; rotate based on phase (matches rotate_3ph) */
                        switch (seq_phase) {
                        case FAULT_PHASE_BG:
                            Var = tcr; Vai = tci; Vbr = tar; Vbi = tai; Vcr = tbr; Vci = tbi; break;
                        case FAULT_PHASE_CG:
                            Var = tbr; Vai = tbi; Vbr = tcr; Vbi = tci; Vcr = tar; Vci = tai; break;
                        default: break; /* AG = default, no change */
                        }
                    }
                    /* Convert phasor to time-domain */
                    double wr = 2.0 * M_PI * 60.0;
                    double va = Var*cos(wr*t) - Vai*sin(wr*t);
                    double vb = Vbr*cos(wr*t) - Vbi*sin(wr*t);
                    double vc = Vcr*cos(wr*t) - Vci*sin(wr*t);
                    double ia = Ir*cos(wr*t) - Ii*sin(wr*t);
                    double ib = Ir*cos(wr*t - 2.0943951024) - Ii*sin(wr*t - 2.0943951024);
                    double ic = Ir*cos(wr*t + 2.0943951024) - Ii*sin(wr*t + 2.0943951024);
                    double av[6] = {va, vb, vc, ia, ib, ic};
                    comtrade_write_ascii(ctw, t, av, NULL);
                }
            }
            free(vm_sig); free(va_sig);
            arena_free(&ybus_arena);
        } else {
            for (int r = 0; r < nrec; r++) {
                if (fread(buf, sizeof(double), (size_t)(1 + nsig), fp) != (size_t)(1 + nsig)) break;
                double t = buf[0];
                if (df) {
                    fprintf(df, "%.6f", t);
                    for (int s = 0; s < a->n_signals; s++) {
                        Signal *sig = &a->signals[s];
                        if (sig->type == SIG_GEN_DELTA || sig->type == SIG_GEN_PE || sig->type == SIG_GEN_VT)
                            fprintf(df, ",%.12g", buf[1 + s]);
                    }
                    fprintf(df, "\n");
                }
                if (vf) {
                    fprintf(vf, "%.6f", t);
                    for (int s = 0; s < a->n_signals; s++) {
                        Signal *sig = &a->signals[s];
                        if (sig->type == SIG_BUS_VM || sig->type == SIG_BUS_VA)
                            fprintf(vf, ",%.12g", buf[1 + s]);
                    }
                    fprintf(vf, "\n");
                }
            }
        }

        rewind(fp);
        for (int r = 0; r < nrec; r++) {
            if (fread(buf, sizeof(double), (size_t)(1 + nsig), fp) != (size_t)(1 + nsig)) break;
            double t = buf[0];

            if (df) {
                fprintf(df, "%.6f", t);
                for (int s = 0; s < a->n_signals; s++) {
                    Signal *sig = &a->signals[s];
                    if (sig->type == SIG_GEN_DELTA || sig->type == SIG_GEN_PE || sig->type == SIG_GEN_VT)
                        fprintf(df, ",%.12g", buf[1 + s]);
                }
                fprintf(df, "\n");
            }
            if (vf) {
                fprintf(vf, "%.6f", t);
                for (int s = 0; s < a->n_signals; s++) {
                    Signal *sig = &a->signals[s];
                    if (sig->type == SIG_BUS_VM || sig->type == SIG_BUS_VA)
                        fprintf(vf, ",%.12g", buf[1 + s]);
                }
                fprintf(vf, "\n");
            }
            if (ctw && ct_idx >= 0 && ct_sig_vm >= 0 && ct_sig_va >= 0) {
                double Vm = buf[1+ct_sig_vm];
                double ang = buf[1+ct_sig_va] * M_PI / 180.0;
                double Vr = Vm * cos(ang), Vi = Vm * sin(ang);
                double wr = 2.0 * M_PI * 60.0;
                double va = Vr*cos(wr*t) - Vi*sin(wr*t);
                double vb = Vr*cos(wr*t - 2.0943951024) - Vi*sin(wr*t - 2.0943951024);
                double vc = Vr*cos(wr*t + 2.0943951024) - Vi*sin(wr*t + 2.0943951024);
                double av[6] = {va, vb, vc, 0, 0, 0};
                comtrade_write_ascii(ctw, t, av, NULL);
            }
        }

        free(buf); fclose(fp);
        if (df) fclose(df);
        if (vf) fclose(vf);
        if (ctw) {
            snprintf(path, sizeof(path), "%s/comtrade_bus%d.cfg", outdir, ct_bus);
            comtrade_write_cfg(ctw, path);
            comtrade_close(ctw);
        }
        g_free(outdir);
    }
    gtk_widget_destroy(dlg);
}
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
    GtkWidget *mi_save = gtk_menu_item_new_with_label("Export...");
    g_signal_connect(mi_run, "activate", G_CALLBACK(on_run_sim), app);
    g_signal_connect(mi_stop, "activate", G_CALLBACK(on_stop_sim), app);
    g_signal_connect(mi_save, "activate", G_CALLBACK(on_save_results), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(sim_menu), mi_run);
    gtk_menu_shell_append(GTK_MENU_SHELL(sim_menu), mi_stop);
    gtk_menu_shell_append(GTK_MENU_SHELL(sim_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(sim_menu), mi_save);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(sim_item), sim_menu);

    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), sim_item);
    return menubar;
}

/* ── file dialogs ── */
static void file_load_raw(App *app) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Load PSS/E RAW File",
        GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        strncpy(app->raw_path, fn, sizeof(app->raw_path) - 1);
        char *bn = strrchr(fn, '/'); bn = bn ? bn + 1 : fn;
        char lb[512]; snprintf(lb, sizeof(lb), "RAW: %s", bn);
        gtk_label_set_text(GTK_LABEL(app->lbl_raw), lb);
        if (raw_parse(fn, &app->sys, &app->arena) == 0) {
            app->sys.fault_bus = -1;
            populate_network(app);
            build_signal_tree(app);
        } else {
            GtkWidget *err = gtk_message_dialog_new(GTK_WINDOW(app->window),
                GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to parse RAW file");
            gtk_dialog_run(GTK_DIALOG(err)); gtk_widget_destroy(err);
        }
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

static void file_load_dyr(App *app) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Load PSS/E DYR File",
        GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        strncpy(app->dyr_path, fn, sizeof(app->dyr_path) - 1);
        char *bn = strrchr(fn, '/'); bn = bn ? bn + 1 : fn;
        char lb[512]; snprintf(lb, sizeof(lb), "DYR: %s", bn);
        gtk_label_set_text(GTK_LABEL(app->lbl_dyr), lb);
        if (dyr_parse(fn, &app->sys, &app->arena) == 0) {
            app->sys_loaded = 1;
            populate_network(app);
            build_signal_tree(app);
        } else {
            GtkWidget *err = gtk_message_dialog_new(GTK_WINDOW(app->window),
                GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to parse DYR file");
            gtk_dialog_run(GTK_DIALOG(err)); gtk_widget_destroy(err);
        }
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

static void file_load_events(App *app) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Load Event INI File",
        GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        strncpy(app->ini_path, fn, sizeof(app->ini_path) - 1);
        char *bn = strrchr(fn, '/'); bn = bn ? bn + 1 : fn;
        char lb[512]; snprintf(lb, sizeof(lb), "INI: %s", bn);
        gtk_label_set_text(GTK_LABEL(app->lbl_ini), lb);
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
static void bus_cell_edited(GtkCellRendererText *cell, gchar *path_string,
                            gchar *new_text, gpointer user_data) {
    App *app = (App *)user_data;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "col_idx"));
    if (!path_string || path_string[0] == '\0') return;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    if (!path) return;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->bus_store), &iter, path)) {
        gtk_tree_path_free(path); return;
    }
    int row = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    if (row < 0 || row >= app->sys.nbus) return;
    Bus *b = &app->sys.bus[row];

    switch (col) {
    case 1: strncpy(b->name, new_text, sizeof(b->name) - 1); break;
    case 3: b->base_kv = atof(new_text); break;
    case 4: b->vm = atof(new_text); break;
    case 5: b->pd = atof(new_text) / app->sys.base_mva; break;
    case 6: b->qd = atof(new_text) / app->sys.base_mva; break;
    case 2:
        if (strcmp(new_text, "SLACK") == 0) b->type = BUS_SLACK;
        else if (strcmp(new_text, "PV") == 0) b->type = BUS_PV;
        else b->type = BUS_PQ;
        break;
    }
    populate_network(app);
}

static void branch_cell_edited(GtkCellRendererText *cell, gchar *path_string,
                               gchar *new_text, gpointer user_data) {
    App *app = (App *)user_data;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "col_idx"));
    if (!path_string || path_string[0] == '\0') return;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    if (!path) return;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->branch_store), &iter, path)) {
        gtk_tree_path_free(path); return;
    }
    int row = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    if (row < 0 || row >= app->sys.nbranch) return;
    Branch *br = &app->sys.branch[row];

    switch (col) {
    case 2: br->r = atof(new_text); break;
    case 3: br->x = atof(new_text); break;
    case 4: br->b = atof(new_text); break;
    case 5: br->tap = atof(new_text); break;
    case 6: br->status = (strcmp(new_text, "ON") == 0) ? 1 : 0; break;
    }
    populate_network(app);
}

static void gen_cell_edited(GtkCellRendererText *cell, gchar *path_string,
                            gchar *new_text, gpointer user_data) {
    App *app = (App *)user_data;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "col_idx"));
    if (!path_string || path_string[0] == '\0') return;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    if (!path) return;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->gen_store), &iter, path)) {
        gtk_tree_path_free(path); return;
    }
    int row = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    if (row < 0 || row >= app->sys.ngen) return;
    Gen *g = &app->sys.gen[row];

    switch (col) {
    case 1: g->pg = atof(new_text) / app->sys.base_mva; break;
    case 2: g->qg = atof(new_text) / app->sys.base_mva; break;
    case 3: g->vsched = atof(new_text); break;
    case 4: g->mbase = atof(new_text); break;
    }
    populate_network(app);
}

static void load_cell_edited(GtkCellRendererText *cell, gchar *path_string,
                             gchar *new_text, gpointer user_data) {
    App *app = (App *)user_data;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "col_idx"));
    if (!path_string || path_string[0] == '\0') return;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    if (!path) return;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->load_store), &iter, path)) {
        gtk_tree_path_free(path); return;
    }
    int row = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    if (row < 0 || row >= app->sys.nload) return;
    Load *l = &app->sys.load[row];

    switch (col) {
    case 1: l->p = atof(new_text) / app->sys.base_mva; break;
    case 2: l->q = atof(new_text) / app->sys.base_mva; break;
    case 3: l->status = (strcmp(new_text, "ON") == 0) ? 1 : 0; break;
    }
    populate_network(app);
}

static void event_cell_edited(GtkCellRendererText *cell, gchar *path_string,
                              gchar *new_text, gpointer user_data) {
    App *app = (App *)user_data;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "col_idx"));
    if (!path_string || path_string[0] == '\0') return;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    if (!path) return;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->event_store), &iter, path)) {
        gtk_tree_path_free(path); return;
    }
    int row = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    if (row < 0 || row >= app->n_events) return;
    GUIEvent *e = &app->events[row];

    switch (col) {
    case 1: e->time = atof(new_text); break;
    case 2: {
        static const struct { const char *name; EventType type; int phase; } fmap[] = {
            {"ABC", FAULT, FAULT_PHASE_ABC},
            {"AG", FAULT_SLG, FAULT_PHASE_AG},
            {"BG", FAULT_SLG, FAULT_PHASE_BG},
            {"CG", FAULT_SLG, FAULT_PHASE_CG},
            {"AB", FAULT_LL, FAULT_PHASE_AB},
            {"BC", FAULT_LL, FAULT_PHASE_BC},
            {"CA", FAULT_LL, FAULT_PHASE_CA},
            {"ABG", FAULT_DLG, FAULT_PHASE_AB},
            {"BCG", FAULT_DLG, FAULT_PHASE_BC},
            {"CAG", FAULT_DLG, FAULT_PHASE_CA},
        };
        int found = 0;
        for (size_t i = 0; i < sizeof(fmap)/sizeof(fmap[0]); i++) {
            if (!strcmp(new_text, fmap[i].name)) {
                e->type = fmap[i].type; e->fault_phase = fmap[i].phase; found = 1; break;
            }
        }
        if (!found && !strcmp(new_text, "clear")) { e->type = FAULT_CLEAR; e->fault_phase = FAULT_PHASE_ABC; }
        break;
    }
    case 3: e->bus = atoi(new_text); break;
    case 4: e->fault_r = atof(new_text); break;
    case 5: e->fault_x = atof(new_text); break;
    }
    populate_events(app);
}

static void build_network_panel(App *app, GtkWidget **box) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *nb = gtk_notebook_new();

    /* --- Buses --- */
    app->bus_store = gtk_list_store_new(7, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    app->bus_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->bus_store));
    const char *bus_cols[] = {"ID", "Name", "Type", "kV", "Vm(pu)", "Pd(MW)", "Qd(MVar)"};
    for (int i = 0; i < 7; i++) {
        GtkCellRenderer *r;
        if (i == 2) {
            r = gtk_cell_renderer_combo_new();
            GtkListStore *ls = gtk_list_store_new(1, G_TYPE_STRING);
            GtkTreeIter it;
            gtk_list_store_append(ls, &it); gtk_list_store_set(ls, &it, 0, "PQ", -1);
            gtk_list_store_append(ls, &it); gtk_list_store_set(ls, &it, 0, "PV", -1);
            gtk_list_store_append(ls, &it); gtk_list_store_set(ls, &it, 0, "SLACK", -1);
            g_object_set(r, "model", ls, "text-column", 0, "has-entry", FALSE, "editable", TRUE, NULL);
            g_object_unref(ls);
        } else {
            r = gtk_cell_renderer_text_new();
            g_object_set(r, "editable", (i != 0), NULL);
        }
        g_object_set_data(G_OBJECT(r), "col_idx", GINT_TO_POINTER(i));
        g_signal_connect(r, "edited", G_CALLBACK(bus_cell_edited), app);
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->bus_tree), i, bus_cols[i], r, "text", i, NULL);
    }
    /* right-click menu */
    app->bus_menu = gtk_menu_new();
    GtkWidget *b_add = gtk_menu_item_new_with_label("Add Bus...");
    GtkWidget *b_del = gtk_menu_item_new_with_label("Delete Selected");
    GtkWidget *b_sep = gtk_separator_menu_item_new();
    GtkWidget *b_sraw = gtk_menu_item_new_with_label("Save RAW");
    GtkWidget *b_sdyr = gtk_menu_item_new_with_label("Save DYR");
    g_signal_connect(b_add, "activate", G_CALLBACK(on_bus_menu_add), app);
    g_signal_connect(b_del, "activate", G_CALLBACK(on_bus_menu_del), app);
    g_signal_connect(b_sraw, "activate", G_CALLBACK(on_bus_menu_save_raw), app);
    g_signal_connect(b_sdyr, "activate", G_CALLBACK(on_bus_menu_save_dyr), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->bus_menu), b_add);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->bus_menu), b_del);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->bus_menu), b_sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->bus_menu), b_sraw);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->bus_menu), b_sdyr);
    gtk_widget_show_all(app->bus_menu);
    GtkWidget *bus_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(bus_sw), app->bus_tree);
    g_object_set_data(G_OBJECT(app->bus_tree), "context-menu", app->bus_menu);
    g_signal_connect(app->bus_tree, "button-press-event", G_CALLBACK(on_tree_button_press), NULL);
    GtkWidget *bus_vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(bus_vb), bus_sw, TRUE, TRUE, 0);
    GtkWidget *bus_hint = gtk_label_new("Right-click for options. Double-click cells to edit.");
    gtk_label_set_xalign(GTK_LABEL(bus_hint), 0);
    gtk_widget_set_state_flags(bus_hint, GTK_STATE_FLAG_INSENSITIVE, FALSE);
    gtk_box_pack_start(GTK_BOX(bus_vb), bus_hint, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), bus_vb, gtk_label_new("Buses"));

    /* --- Branches --- */
    app->branch_store = gtk_list_store_new(7, G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    app->branch_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->branch_store));
    const char *br_cols[] = {"From", "To", "R", "X", "B", "Tap", "Status"};
    for (int i = 0; i < 7; i++) {
        GtkCellRenderer *r;
        if (i == 6) {
            r = gtk_cell_renderer_combo_new();
            GtkListStore *ls = gtk_list_store_new(1, G_TYPE_STRING);
            GtkTreeIter it;
            gtk_list_store_append(ls, &it); gtk_list_store_set(ls, &it, 0, "ON", -1);
            gtk_list_store_append(ls, &it); gtk_list_store_set(ls, &it, 0, "OFF", -1);
            g_object_set(r, "model", ls, "text-column", 0, "has-entry", FALSE, "editable", TRUE, NULL);
            g_object_unref(ls);
        } else {
            r = gtk_cell_renderer_text_new();
            g_object_set(r, "editable", (i >= 2), NULL);
        }
        g_object_set_data(G_OBJECT(r), "col_idx", GINT_TO_POINTER(i));
        g_signal_connect(r, "edited", G_CALLBACK(branch_cell_edited), app);
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->branch_tree), i, br_cols[i], r, "text", i, NULL);
    }
    app->branch_menu = gtk_menu_new();
    GtkWidget *br_add = gtk_menu_item_new_with_label("Add Branch...");
    GtkWidget *br_del = gtk_menu_item_new_with_label("Delete Selected");
    GtkWidget *br_sep = gtk_separator_menu_item_new();
    GtkWidget *br_sraw = gtk_menu_item_new_with_label("Save RAW");
    GtkWidget *br_sdyr = gtk_menu_item_new_with_label("Save DYR");
    g_signal_connect(br_add, "activate", G_CALLBACK(on_branch_menu_add), app);
    g_signal_connect(br_del, "activate", G_CALLBACK(on_branch_menu_del), app);
    g_signal_connect(br_sraw, "activate", G_CALLBACK(on_branch_menu_save_raw), app);
    g_signal_connect(br_sdyr, "activate", G_CALLBACK(on_branch_menu_save_dyr), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->branch_menu), br_add);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->branch_menu), br_del);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->branch_menu), br_sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->branch_menu), br_sraw);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->branch_menu), br_sdyr);
    gtk_widget_show_all(app->branch_menu);
    GtkWidget *br_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(br_sw), app->branch_tree);
    g_object_set_data(G_OBJECT(app->branch_tree), "context-menu", app->branch_menu);
    g_signal_connect(app->branch_tree, "button-press-event", G_CALLBACK(on_tree_button_press), NULL);
    GtkWidget *br_vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(br_vb), br_sw, TRUE, TRUE, 0);
    GtkWidget *br_hint = gtk_label_new("Right-click for options. Double-click cells to edit.");
    gtk_label_set_xalign(GTK_LABEL(br_hint), 0);
    gtk_widget_set_state_flags(br_hint, GTK_STATE_FLAG_INSENSITIVE, FALSE);
    gtk_box_pack_start(GTK_BOX(br_vb), br_hint, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), br_vb, gtk_label_new("Branches"));

    /* --- Generators --- */
    app->gen_store = gtk_list_store_new(5, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    app->gen_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->gen_store));
    const char *gen_cols[] = {"Bus", "Pg(MW)", "Qg(MVar)", "Vsched", "Mbase"};
    for (int i = 0; i < 5; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        g_object_set(r, "editable", (i != 0), NULL);
        g_object_set_data(G_OBJECT(r), "col_idx", GINT_TO_POINTER(i));
        g_signal_connect(r, "edited", G_CALLBACK(gen_cell_edited), app);
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->gen_tree), i, gen_cols[i], r, "text", i, NULL);
    }
    app->gen_menu = gtk_menu_new();
    GtkWidget *g_add = gtk_menu_item_new_with_label("Add Generator...");
    GtkWidget *g_del = gtk_menu_item_new_with_label("Delete Selected");
    GtkWidget *g_sep = gtk_separator_menu_item_new();
    GtkWidget *g_sraw = gtk_menu_item_new_with_label("Save RAW");
    GtkWidget *g_sdyr = gtk_menu_item_new_with_label("Save DYR");
    g_signal_connect(g_add, "activate", G_CALLBACK(on_gen_menu_add), app);
    g_signal_connect(g_del, "activate", G_CALLBACK(on_gen_menu_del), app);
    g_signal_connect(g_sraw, "activate", G_CALLBACK(on_gen_menu_save_raw), app);
    g_signal_connect(g_sdyr, "activate", G_CALLBACK(on_gen_menu_save_dyr), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->gen_menu), g_add);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->gen_menu), g_del);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->gen_menu), g_sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->gen_menu), g_sraw);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->gen_menu), g_sdyr);
    gtk_widget_show_all(app->gen_menu);
    GtkWidget *gen_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(gen_sw), app->gen_tree);
    g_object_set_data(G_OBJECT(app->gen_tree), "context-menu", app->gen_menu);
    g_signal_connect(app->gen_tree, "button-press-event", G_CALLBACK(on_tree_button_press), NULL);
    GtkWidget *gen_vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(gen_vb), gen_sw, TRUE, TRUE, 0);
    GtkWidget *gen_hint = gtk_label_new("Right-click for options. Double-click cells to edit.");
    gtk_label_set_xalign(GTK_LABEL(gen_hint), 0);
    gtk_widget_set_state_flags(gen_hint, GTK_STATE_FLAG_INSENSITIVE, FALSE);
    gtk_box_pack_start(GTK_BOX(gen_vb), gen_hint, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), gen_vb, gtk_label_new("Generators"));

    /* --- Loads --- */
    app->load_store = gtk_list_store_new(4, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    app->load_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->load_store));
    const char *ld_cols[] = {"Bus", "P(MW)", "Q(MVar)", "Status"};
    for (int i = 0; i < 4; i++) {
        GtkCellRenderer *r;
        if (i == 3) {
            r = gtk_cell_renderer_combo_new();
            GtkListStore *ls = gtk_list_store_new(1, G_TYPE_STRING);
            GtkTreeIter it;
            gtk_list_store_append(ls, &it); gtk_list_store_set(ls, &it, 0, "ON", -1);
            gtk_list_store_append(ls, &it); gtk_list_store_set(ls, &it, 0, "OFF", -1);
            g_object_set(r, "model", ls, "text-column", 0, "has-entry", FALSE, "editable", TRUE, NULL);
            g_object_unref(ls);
        } else {
            r = gtk_cell_renderer_text_new();
            g_object_set(r, "editable", (i != 0), NULL);
        }
        g_object_set_data(G_OBJECT(r), "col_idx", GINT_TO_POINTER(i));
        g_signal_connect(r, "edited", G_CALLBACK(load_cell_edited), app);
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->load_tree), i, ld_cols[i], r, "text", i, NULL);
    }
    app->load_menu = gtk_menu_new();
    GtkWidget *l_add = gtk_menu_item_new_with_label("Add Load...");
    GtkWidget *l_del = gtk_menu_item_new_with_label("Delete Selected");
    GtkWidget *l_sep = gtk_separator_menu_item_new();
    GtkWidget *l_sraw = gtk_menu_item_new_with_label("Save RAW");
    GtkWidget *l_sdyr = gtk_menu_item_new_with_label("Save DYR");
    g_signal_connect(l_add, "activate", G_CALLBACK(on_load_menu_add), app);
    g_signal_connect(l_del, "activate", G_CALLBACK(on_load_menu_del), app);
    g_signal_connect(l_sraw, "activate", G_CALLBACK(on_load_menu_save_raw), app);
    g_signal_connect(l_sdyr, "activate", G_CALLBACK(on_load_menu_save_dyr), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->load_menu), l_add);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->load_menu), l_del);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->load_menu), l_sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->load_menu), l_sraw);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->load_menu), l_sdyr);
    gtk_widget_show_all(app->load_menu);
    GtkWidget *ld_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(ld_sw), app->load_tree);
    g_object_set_data(G_OBJECT(app->load_tree), "context-menu", app->load_menu);
    g_signal_connect(app->load_tree, "button-press-event", G_CALLBACK(on_tree_button_press), NULL);
    GtkWidget *ld_vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(ld_vb), ld_sw, TRUE, TRUE, 0);
    GtkWidget *ld_hint = gtk_label_new("Right-click for options. Double-click cells to edit.");
    gtk_label_set_xalign(GTK_LABEL(ld_hint), 0);
    gtk_widget_set_state_flags(ld_hint, GTK_STATE_FLAG_INSENSITIVE, FALSE);
    gtk_box_pack_start(GTK_BOX(ld_vb), ld_hint, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), ld_vb, gtk_label_new("Loads"));

    gtk_box_pack_start(GTK_BOX(vbox), nb, TRUE, TRUE, 0);
    *box = vbox;
}

static void populate_network(App *app) {
    gtk_list_store_clear(app->bus_store);
    gtk_list_store_clear(app->branch_store);
    gtk_list_store_clear(app->gen_store);
    gtk_list_store_clear(app->load_store);
    System *s = &app->sys;

    for (int i = 0; i < s->nbus; i++) {
        GtkTreeIter it;
        gtk_list_store_append(app->bus_store, &it);
        gtk_list_store_set(app->bus_store, &it, 0, s->bus[i].id, 1, s->bus[i].name,
            2, btype(s->bus[i].type), 3, fmt_dbl(s->bus[i].base_kv),
            4, fmt_dbl(s->bus[i].vm),
            5, fmt_dbl(s->bus[i].pd * s->base_mva),
            6, fmt_dbl(s->bus[i].qd * s->base_mva), -1);
    }
    for (int i = 0; i < s->nbranch; i++) {
        GtkTreeIter it;
        gtk_list_store_append(app->branch_store, &it);
        double tap = s->branch[i].tap > 0 ? s->branch[i].tap : 1.0;
        gtk_list_store_set(app->branch_store, &it, 0, s->bus[s->branch[i].from].id,
            1, s->bus[s->branch[i].to].id,
            2, fmt_dbl(s->branch[i].r), 3, fmt_dbl(s->branch[i].x),
            4, fmt_dbl(s->branch[i].b), 5, fmt_dbl(tap),
            6, s->branch[i].status ? "ON" : "OFF", -1);
    }
    for (int i = 0; i < s->ngen; i++) {
        GtkTreeIter it;
        gtk_list_store_append(app->gen_store, &it);
        gtk_list_store_set(app->gen_store, &it, 0, s->bus[s->gen[i].bus].id,
            1, fmt_dbl(s->gen[i].pg * s->base_mva),
            2, fmt_dbl(s->gen[i].qg * s->base_mva),
            3, fmt_dbl(s->gen[i].vsched),
            4, fmt_dbl(s->gen[i].mbase), -1);
    }
    for (int i = 0; i < s->nload; i++) {
        GtkTreeIter it;
        gtk_list_store_append(app->load_store, &it);
        gtk_list_store_set(app->load_store, &it, 0, s->bus[s->load[i].bus].id,
            1, fmt_dbl(s->load[i].p * s->base_mva),
            2, fmt_dbl(s->load[i].q * s->base_mva),
            3, s->load[i].status ? "ON" : "OFF", -1);
    }
}

/* ── signal tree with parent-child sync ── */
enum { SIG_COL_CHECK = 0, SIG_COL_LABEL, SIG_COL_IDX, SIG_N_COLS };

static void sig_set_children(GtkTreeStore *store, GtkTreeIter *parent, gboolean state, App *app) {
    int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), parent);
    for (int i = 0; i < n; i++) {
        GtkTreeIter child;
        if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store), &child, parent, i)) {
            int idx;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &child, SIG_COL_IDX, &idx, -1);
            gtk_tree_store_set(store, &child, SIG_COL_CHECK, state, -1);
            if (idx >= 0 && idx < app->n_signals)
                app->signals[idx].visible = state;
            sig_set_children(store, &child, state, app);
        }
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
        gtk_tree_store_set(store, parent, SIG_COL_CHECK, (checked == total), -1);
    }
}

static void sig_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer user_data) {
    App *app = (App *)user_data;
    if (!path_str || path_str[0] == '\0') return;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    if (!path || gtk_tree_path_get_depth(path) == 0) {
        if (path) gtk_tree_path_free(path);
        return;
    }
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
    if (idx >= 0 && idx < app->n_signals)
        app->signals[idx].visible = active;

    sig_set_children(app->sig_store, &iter, active, app);

    GtkTreePath *pp = gtk_tree_path_copy(path);
    if (gtk_tree_path_up(pp) && gtk_tree_path_get_depth(pp) > 0) {
        GtkTreeIter pit;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->sig_store), &pit, pp))
            sig_update_parent(app->sig_store, &pit, app);
    }
    gtk_tree_path_free(pp);
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
            sig->type = subs[j].type; sig->idx = m; sig->id = bid; sig->visible = 0;
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
            sig->type = subs[j].type; sig->idx = i; sig->id = bid; sig->visible = 0;
            GtkTreeIter child;
            gtk_tree_store_append(app->sig_store, &child, &parent);
            gtk_tree_store_set(app->sig_store, &child, SIG_COL_CHECK, FALSE,
                SIG_COL_LABEL, sig->label, SIG_COL_IDX, app->n_signals, -1);
            app->n_signals++;
        }
    }

    if (app->scope_bus > 0 && app->n_signals + 3 <= MAX_SIGNALS) {
        int bid = app->scope_bus;
        char pl[64];
        snprintf(pl, sizeof(pl), "Oscillo @ Bus %d", bid);
        GtkTreeIter parent;
        gtk_tree_store_append(app->sig_store, &parent, NULL);
        gtk_tree_store_set(app->sig_store, &parent, SIG_COL_CHECK, FALSE, SIG_COL_LABEL, pl, SIG_COL_IDX, -1, -1);
        struct { SignalType type; const char *fmt; } oscs[] = {
            { SIG_OSC_VA, "B%d Va (pu)" },
            { SIG_OSC_VB, "B%d Vb (pu)" },
            { SIG_OSC_VC, "B%d Vc (pu)" },
        };
        for (int j = 0; j < 3 && app->n_signals < MAX_SIGNALS; j++) {
            Signal *sig = &app->signals[app->n_signals];
            snprintf(sig->label, sizeof(sig->label), oscs[j].fmt, bid);
            sig->type = oscs[j].type; sig->idx = 0; sig->id = bid; sig->visible = 0;
            GtkTreeIter child;
            gtk_tree_store_append(app->sig_store, &child, &parent);
            gtk_tree_store_set(app->sig_store, &child, SIG_COL_CHECK, FALSE,
                SIG_COL_LABEL, sig->label, SIG_COL_IDX, app->n_signals, -1);
            app->n_signals++;
        }
    }
}

/* ── event panel with right-click context menu ── */
static void on_event_menu_add(GtkMenuItem *item, App *app) { sim_add_event_dialog(app); }
static void on_event_menu_del(GtkMenuItem *item, App *app) { sim_delete_selected(app); }
static void on_event_menu_save(GtkMenuItem *item, App *app) { sim_save_events(app); }
static void on_event_menu_save_as(GtkMenuItem *item, App *app) { sim_save_events_as(app); }

static gboolean on_tree_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    if (event->button == 3) {
        GtkWidget *menu = g_object_get_data(G_OBJECT(widget), "context-menu");
        if (menu) gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
        return TRUE;
    }
    return FALSE;
}

static void build_event_panel(App *app, GtkWidget **box) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

    app->event_store = gtk_list_store_new(6, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    app->event_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->event_store));
    const char *ev_cols[] = {"#", "Time(s)", "Type", "Bus", "R", "X"};
    for (int i = 0; i < 6; i++) {
        GtkCellRenderer *r;
        if (i == 2) {
            r = gtk_cell_renderer_combo_new();
            GtkListStore *ls = gtk_list_store_new(1, G_TYPE_STRING);
            GtkTreeIter it;
            const char *opts[] = {"ABC","AG","BG","CG","AB","BC","CA","ABG","BCG","CAG","clear"};
            for (int j = 0; j < 11; j++) {
                gtk_list_store_append(ls, &it); gtk_list_store_set(ls, &it, 0, opts[j], -1);
            }
            g_object_set(r, "model", ls, "text-column", 0, "has-entry", FALSE, "editable", TRUE, NULL);
            g_object_unref(ls);
        } else {
            r = gtk_cell_renderer_text_new();
            g_object_set(r, "editable", (i != 0), NULL);
        }
        g_object_set_data(G_OBJECT(r), "col_idx", GINT_TO_POINTER(i));
        g_signal_connect(r, "edited", G_CALLBACK(event_cell_edited), app);
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(app->event_tree), i, ev_cols[i], r, "text", i, NULL);
    }
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(sw, -1, 150);
    gtk_container_add(GTK_CONTAINER(sw), app->event_tree);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    /* context menu */
    app->event_menu = gtk_menu_new();
    GtkWidget *mi_add = gtk_menu_item_new_with_label("Add Fault...");
    GtkWidget *mi_del = gtk_menu_item_new_with_label("Delete Selected");
    GtkWidget *mi_sep = gtk_separator_menu_item_new();
    GtkWidget *mi_sv = gtk_menu_item_new_with_label("Save");
    GtkWidget *mi_svas = gtk_menu_item_new_with_label("Save As...");
    g_signal_connect(mi_add, "activate", G_CALLBACK(on_event_menu_add), app);
    g_signal_connect(mi_del, "activate", G_CALLBACK(on_event_menu_del), app);
    g_signal_connect(mi_sv, "activate", G_CALLBACK(on_event_menu_save), app);
    g_signal_connect(mi_svas, "activate", G_CALLBACK(on_event_menu_save_as), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->event_menu), mi_add);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->event_menu), mi_del);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->event_menu), mi_sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->event_menu), mi_sv);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->event_menu), mi_svas);
    gtk_widget_show_all(app->event_menu);

    g_object_set_data(G_OBJECT(app->event_tree), "context-menu", app->event_menu);
    g_signal_connect(app->event_tree, "button-press-event", G_CALLBACK(on_tree_button_press), NULL);

    GtkWidget *hint = gtk_label_new("Right-click for options. Double-click cells to edit.");
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
        gtk_list_store_set(app->event_store, &it, 0, i,
            1, fmt_dbl(app->events[i].time),
            2, fault_name(app->events[i].type, app->events[i].fault_phase),
            3, app->events[i].bus,
            4, fmt_dbl(app->events[i].fault_r),
            5, fmt_dbl(app->events[i].fault_x), -1);
    }
}

static void sim_add_event_dialog(App *app) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Add Fault",
        GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Add", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(area), 10);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Fault type:"), 0, 0, 1, 1);
    GtkWidget *combo = gtk_combo_box_text_new();
    const char *fault_types[] = {"ABC","AG","BG","CG","AB","BC","CA","ABG","BCG","CAG","clear"};
    for (int i = 0; i < 11; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), fault_types[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_grid_attach(GTK_GRID(grid), combo, 1, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Time (s):"), 0, 1, 1, 1);
    GtkWidget *spin_t = gtk_spin_button_new_with_range(0.0, 10000.0, 0.01);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_t), 1.0);
    gtk_grid_attach(GTK_GRID(grid), spin_t, 1, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Bus:"), 0, 2, 1, 1);
    GtkWidget *spin_b = gtk_spin_button_new_with_range(1, 9999, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_b), 1);
    gtk_grid_attach(GTK_GRID(grid), spin_b, 1, 2, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("R (pu):"), 0, 3, 1, 1);
    GtkWidget *spin_r = gtk_spin_button_new_with_range(0.0, 100.0, 0.0001);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_r), 0.001);
    gtk_grid_attach(GTK_GRID(grid), spin_r, 1, 3, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("X (pu):"), 0, 4, 1, 1);
    GtkWidget *spin_x = gtk_spin_button_new_with_range(0.0, 100.0, 0.0001);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_x), 0.001);
    gtk_grid_attach(GTK_GRID(grid), spin_x, 1, 4, 1, 1);

    gtk_box_pack_start(GTK_BOX(area), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        if (app->n_events >= MAX_EVENTS) {
            GtkWidget *err = gtk_message_dialog_new(GTK_WINDOW(app->window),
                GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Max events reached");
            gtk_dialog_run(GTK_DIALOG(err)); gtk_widget_destroy(err);
        } else {
            static const struct { EventType type; int phase; } fm[] = {
                {FAULT, FAULT_PHASE_ABC},
                {FAULT_SLG, FAULT_PHASE_AG},
                {FAULT_SLG, FAULT_PHASE_BG},
                {FAULT_SLG, FAULT_PHASE_CG},
                {FAULT_LL, FAULT_PHASE_AB},
                {FAULT_LL, FAULT_PHASE_BC},
                {FAULT_LL, FAULT_PHASE_CA},
                {FAULT_DLG, FAULT_PHASE_AB},
                {FAULT_DLG, FAULT_PHASE_BC},
                {FAULT_DLG, FAULT_PHASE_CA},
            };
            int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
            GUIEvent *e = &app->events[app->n_events];
            if (idx == 10) { e->type = FAULT_CLEAR; e->fault_phase = FAULT_PHASE_ABC; }
            else if (idx >= 0 && idx < 10) { e->type = fm[idx].type; e->fault_phase = fm[idx].phase; }
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

static void sim_delete_selected(App *app) {
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
    } else if (app->n_events > 0) {
        app->n_events--;
    }
    populate_events(app);
}

static void sim_save_events_as(App *app) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save Events INI",
        GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    if (app->ini_path[0]) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), app->ini_path);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        FILE *f = fopen(fn, "w");
        if (f) {
            for (int i = 0; i < app->n_events; i++) {
                fprintf(f, "[event]\ntime=%.2f\ntype=%s\nbus=%d\nr=%.4f\nx=%.4f\n",
                    app->events[i].time, fault_name(app->events[i].type, app->events[i].fault_phase),
                    app->events[i].bus, app->events[i].fault_r, app->events[i].fault_x);
                fprintf(f, "\n");
            }
            fclose(f);
        }
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

static void sim_save_events(App *app) {
    if (!app->ini_path[0]) { sim_save_events_as(app); return; }
    FILE *f = fopen(app->ini_path, "w");
    if (f) {
        for (int i = 0; i < app->n_events; i++) {
            fprintf(f, "[event]\ntime=%.2f\ntype=%s\nbus=%d\nr=%.4f\nx=%.4f\n",
                app->events[i].time, fault_name(app->events[i].type, app->events[i].fault_phase),
                app->events[i].bus, app->events[i].fault_r, app->events[i].fault_x);
            fprintf(f, "\n");
        }
        fclose(f);
    }
}

static void save_raw(App *app) {
    if (!app->raw_path[0]) {
        GtkWidget *dlg = gtk_file_chooser_dialog_new("Save RAW",
            GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
        if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
            char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
            raw_write(fn, &app->sys);
            strncpy(app->raw_path, fn, sizeof(app->raw_path) - 1);
            g_free(fn);
        }
        gtk_widget_destroy(dlg);
    } else {
        raw_write(app->raw_path, &app->sys);
    }
}

static void save_dyr(App *app) {
    if (!app->dyr_path[0]) {
        GtkWidget *dlg = gtk_file_chooser_dialog_new("Save DYR",
            GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
        if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
            char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
            dyr_write(fn, &app->sys);
            strncpy(app->dyr_path, fn, sizeof(app->dyr_path) - 1);
            g_free(fn);
        }
        gtk_widget_destroy(dlg);
    } else {
        dyr_write(app->dyr_path, &app->sys);
    }
}

/* ── network element management ── */
static void on_bus_menu_add(GtkMenuItem *item, App *app) { add_bus_dialog(app); }
static void on_bus_menu_del(GtkMenuItem *item, App *app) { delete_selected_bus(app); }
static void on_bus_menu_save_raw(GtkMenuItem *item, App *app) { save_raw(app); }
static void on_bus_menu_save_dyr(GtkMenuItem *item, App *app) { save_dyr(app); }
static void on_branch_menu_add(GtkMenuItem *item, App *app) { add_branch_dialog(app); }
static void on_branch_menu_del(GtkMenuItem *item, App *app) { delete_selected_branch(app); }
static void on_branch_menu_save_raw(GtkMenuItem *item, App *app) { save_raw(app); }
static void on_branch_menu_save_dyr(GtkMenuItem *item, App *app) { save_dyr(app); }
static void on_gen_menu_add(GtkMenuItem *item, App *app) { add_gen_dialog(app); }
static void on_gen_menu_del(GtkMenuItem *item, App *app) { delete_selected_gen(app); }
static void on_gen_menu_save_raw(GtkMenuItem *item, App *app) { save_raw(app); }
static void on_gen_menu_save_dyr(GtkMenuItem *item, App *app) { save_dyr(app); }
static void on_load_menu_add(GtkMenuItem *item, App *app) { add_load_dialog(app); }
static void on_load_menu_del(GtkMenuItem *item, App *app) { delete_selected_load(app); }
static void on_load_menu_save_raw(GtkMenuItem *item, App *app) { save_raw(app); }
static void on_load_menu_save_dyr(GtkMenuItem *item, App *app) { save_dyr(app); }

static void delete_selected_from_tree(GtkWidget *tree, void *arr, int *n, size_t elem_size) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    if (gtk_tree_selection_count_selected_rows(sel) > 0) {
        GtkTreeModel *model;
        GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
        if (rows) {
            int idx = gtk_tree_path_get_indices(rows->data)[0];
            g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
            if (idx >= 0 && idx < *n) {
                char *base = (char *)arr;
                size_t sz = elem_size;
                memmove(base + idx * sz, base + (idx + 1) * sz, (*n - idx - 1) * sz);
                (*n)--;
            }
        }
    } else if (*n > 0) {
        (*n)--;
    }
    /* sync stretchy buffer header */
    if (arr) {
        size_t *h = ((size_t *)arr) - 2;
        h[0] = (size_t)*n;
    }
}
static void delete_selected_bus(App *app) {
    delete_selected_from_tree(app->bus_tree, app->sys.bus, &app->sys.nbus, sizeof(Bus));
    populate_network(app);
}
static void delete_selected_branch(App *app) {
    delete_selected_from_tree(app->branch_tree, app->sys.branch, &app->sys.nbranch, sizeof(Branch));
    populate_network(app);
}
static void delete_selected_gen(App *app) {
    delete_selected_from_tree(app->gen_tree, app->sys.gen, &app->sys.ngen, sizeof(Gen));
    populate_network(app);
}
static void delete_selected_load(App *app) {
    delete_selected_from_tree(app->load_tree, app->sys.load, &app->sys.nload, sizeof(Load));
    populate_network(app);
}

static void add_item_from_dialog(App *app, const char *title, FieldDef *fields, int nf, AddCallback cb) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title, GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL, "_Add", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    GtkWidget **entries = malloc(nf * sizeof(GtkWidget *));
    for (int i = 0; i < nf; i++) {
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new(fields[i].label), 0, i, 1, 1);
        entries[i] = gtk_entry_new();
        char defs[32];
        snprintf(defs, sizeof(defs), "%g", fields[i].def);
        gtk_entry_set_text(GTK_ENTRY(entries[i]), defs);
        gtk_grid_attach(GTK_GRID(grid), entries[i], 1, i, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), grid, TRUE, TRUE, 0);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        for (int i = 0; i < nf; i++) {
            const char *text = gtk_entry_get_text(GTK_ENTRY(entries[i]));
            fields[i].val = fields[i].is_int ? atoi(text) : atof(text);
        }
        cb(app, fields);
    }
    gtk_widget_destroy(dlg);
    free(entries);
}

static void add_bus_do(App *app, FieldDef *f) {
    int n = app->sys.nbus;
    arrpush(app->sys.bus, (Bus){0});
    Bus *b = &app->sys.bus[n];
    b->id = (int)f[0].val;
    b->base_kv = f[1].val;
    b->type = BUS_PQ;
    b->vm = f[2].val;
    b->va = f[3].val;
    b->pd = f[4].val / app->sys.base_mva;
    b->qd = f[5].val / app->sys.base_mva;
    snprintf(b->name, sizeof(b->name), "B%d", b->id);
    app->sys.nbus = n + 1;
    populate_network(app);
}
static void add_bus_dialog(App *app) {
    FieldDef f[] = {
        {"Bus ID", 1, 999},
        {"Base kV", 0, 100.0},
        {"Vm (pu)", 0, 1.0},
        {"Va (deg)", 0, 0.0},
        {"Pd (MW)", 0, 0.0},
        {"Qd (MVar)", 0, 0.0},
    };
    add_item_from_dialog(app, "Add Bus", f, 6, add_bus_do);
}

static void add_branch_do(App *app, FieldDef *f) {
    int n = app->sys.nbranch;
    arrpush(app->sys.branch, (Branch){0});
    Branch *b = &app->sys.branch[n];
    int from_id = (int)f[0].val, to_id = (int)f[1].val;
    b->from = 0; b->to = 0;
    for (int i = 0; i < app->sys.nbus; i++) {
        if (app->sys.bus[i].id == from_id) b->from = i;
        if (app->sys.bus[i].id == to_id) b->to = i;
    }
    b->r = f[2].val; b->x = f[3].val; b->b = f[4].val;
    b->tap = f[5].val;
    b->status = (int)f[6].val;
    app->sys.nbranch = n + 1;
    populate_network(app);
}
static void add_branch_dialog(App *app) {
    FieldDef f[] = {
        {"From Bus ID", 1, 1},
        {"To Bus ID", 1, 2},
        {"R (pu)", 0, 0.01},
        {"X (pu)", 0, 0.1},
        {"B (pu)", 0, 0.0},
        {"Tap", 0, 1.0},
        {"Status (1=ON)", 1, 1},
    };
    add_item_from_dialog(app, "Add Branch", f, 7, add_branch_do);
}

static void add_gen_do(App *app, FieldDef *f) {
    int n = app->sys.ngen;
    arrpush(app->sys.gen, (Gen){0});
    Gen *g = &app->sys.gen[n];
    int bid = (int)f[0].val;
    g->bus = 0;
    for (int i = 0; i < app->sys.nbus; i++)
        if (app->sys.bus[i].id == bid) { g->bus = i; break; }
    g->pg = f[1].val / app->sys.base_mva;
    g->qg = f[2].val / app->sys.base_mva;
    g->vsched = f[3].val;
    g->mbase = f[4].val;
    app->sys.ngen = n + 1;
    populate_network(app);
}
static void add_gen_dialog(App *app) {
    FieldDef f[] = {
        {"Bus ID", 1, 1},
        {"Pg (MW)", 0, 0.0},
        {"Qg (MVar)", 0, 0.0},
        {"Vsched (pu)", 0, 1.0},
        {"Mbase (MVA)", 0, 100.0},
    };
    add_item_from_dialog(app, "Add Generator", f, 5, add_gen_do);
}

static void add_load_do(App *app, FieldDef *f) {
    int n = app->sys.nload;
    arrpush(app->sys.load, (Load){0});
    Load *l = &app->sys.load[n];
    int bid = (int)f[0].val;
    l->bus = 0;
    for (int i = 0; i < app->sys.nbus; i++)
        if (app->sys.bus[i].id == bid) { l->bus = i; break; }
    l->p = f[1].val / app->sys.base_mva;
    l->q = f[2].val / app->sys.base_mva;
    l->status = (int)f[3].val;
    app->sys.nload = n + 1;
    populate_network(app);
}
static void add_load_dialog(App *app) {
    FieldDef f[] = {
        {"Bus ID", 1, 1},
        {"P (MW)", 0, 0.0},
        {"Q (MVar)", 0, 0.0},
        {"Status (1=ON)", 1, 1},
    };
    add_item_from_dialog(app, "Add Load", f, 4, add_load_do);
}

/* ── simulation panel ── */
static gboolean spin_output_sci(GtkSpinButton *btn, gpointer data) {
    (void)data;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3e", gtk_spin_button_get_value(btn));
    gtk_entry_set_text(GTK_ENTRY(btn), buf);
    return TRUE;
}

static void on_btn_run(GtkWidget *btn, App *app) { sim_run(app); }
static void on_btn_stop(GtkWidget *btn, App *app) { sim_stop(app); }
static void on_btn_zoom_in(GtkWidget *btn, App *app) { view_zoom_in(app); }
static void on_btn_zoom_out(GtkWidget *btn, App *app) { view_zoom_out(app); }
static void on_btn_fit(GtkWidget *btn, App *app) { view_fit(app); }
static void on_btn_sel_all(GtkWidget *btn, App *app) { view_select_all(app); }
static void on_btn_sel_none(GtkWidget *btn, App *app) { view_select_none(app); }

static void build_sim_panel(App *app, GtkWidget **box) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

    /* compact file info */
    GtkWidget *file_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    app->lbl_raw = gtk_label_new("");
    app->lbl_dyr = gtk_label_new("");
    app->lbl_ini = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(file_hbox), app->lbl_raw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(file_hbox), app->lbl_dyr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(file_hbox), app->lbl_ini, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), file_hbox, FALSE, FALSE, 2);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    app->btn_run = gtk_button_new_with_label("Run");
    app->btn_stop = gtk_button_new_with_label("Stop");
    g_signal_connect(app->btn_run, "clicked", G_CALLBACK(on_btn_run), app);
    g_signal_connect(app->btn_stop, "clicked", G_CALLBACK(on_btn_stop), app);
    gtk_box_pack_start(GTK_BOX(hbox), app->btn_run, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), app->btn_stop, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("t-end:"), FALSE, FALSE, 5);
    app->spin_tend = gtk_spin_button_new_with_range(0.001, 1e9, 0.001);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_tend), 5.0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(app->spin_tend), FALSE);
    g_signal_connect(app->spin_tend, "output", G_CALLBACK(spin_output_sci), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), app->spin_tend, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("dt:"), FALSE, FALSE, 5);
    app->spin_tstep = gtk_spin_button_new_with_range(1e-6, 1e6, 1e-6);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(app->spin_tstep), 6);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_tstep), 0.01);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(app->spin_tstep), FALSE);
    g_signal_connect(app->spin_tstep, "output", G_CALLBACK(spin_output_sci), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), app->spin_tstep, FALSE, FALSE, 0);

    app->progress = gtk_progress_bar_new();
    app->lbl_progress = gtk_label_new("Ready");
    app->t_end = 5.0;
    app->view_t0 = 0;
    app->view_t1 = 5.0;
    char vb[64];
    snprintf(vb, sizeof(vb), "View: %s – %s s", fmt_dbl(0.0), fmt_dbl(5.0));
    app->lbl_view_range = gtk_label_new(vb);
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

    GtkWidget *scope_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(scope_box), gtk_label_new("Scope Bus:"), FALSE, FALSE, 0);
    app->spin_scope = gtk_spin_button_new_with_range(0, 9999, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_scope), 0);
    gtk_widget_set_size_request(app->spin_scope, 60, -1);
    gtk_widget_set_tooltip_text(app->spin_scope, "Set scope bus for sinusoidal voltage display");
    gtk_box_pack_start(GTK_BOX(scope_box), app->spin_scope, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), scope_box, FALSE, FALSE, 2);

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

static void rotate_3ph(double *va, double *vb, double *vc, int fault_phase) {
    if (fault_phase == FAULT_PHASE_ABC || fault_phase == FAULT_PHASE_BC ||
        fault_phase == FAULT_PHASE_AG) return;
    double a = *va, b = *vb, c = *vc;
    switch (fault_phase) {
    case FAULT_PHASE_AB:
    case FAULT_PHASE_CG:
        *va = b; *vb = c; *vc = a; break;
    case FAULT_PHASE_CA:
    case FAULT_PHASE_BG:
        *va = c; *vb = a; *vc = b; break;
    }
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
        app->sim_done = 1; g_mutex_unlock(&app->sim_lock);
        arena_free(&sim_arena); return NULL;
    }
    if (dyr_parse(app->dyr_path, &s, &sim_arena) != 0) {
        g_mutex_lock(&app->sim_lock);
        snprintf(app->sim_status, sizeof(app->sim_status), "DYR parse failed");
        app->sim_done = 1; g_mutex_unlock(&app->sim_lock);
        arena_free(&sim_arena); return NULL;
    }

    Event *sim_events = NULL;
    int sim_n_events = 0;
    for (int i = 0; i < app->n_events; i++) {
        Event e; memset(&e, 0, sizeof(e));
        e.type = app->events[i].type; e.time = app->events[i].time;
        e.bus = app->events[i].bus;
        e.fault_r = app->events[i].fault_r; e.fault_x = app->events[i].fault_x;
        e.fault_phase = app->events[i].fault_phase;
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
        arrfree(sim_events); arena_free(&sim_arena); return NULL;
    }
    powerflow_solve(&s, &sim_arena);
    if (machine_init(&s) != 0) {
        g_mutex_lock(&app->sim_lock);
        snprintf(app->sim_status, sizeof(app->sim_status), "Machine init failed");
        app->sim_done = 1; g_mutex_unlock(&app->sim_lock);
        arrfree(sim_events); arena_free(&sim_arena); return NULL;
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
        arrfree(sim_events); arena_free(&sim_arena); return NULL;
    }

    Integrator itg; memset(&itg, 0, sizeof(itg));
    if (integrator_init(&itg, &dae, 0.0) != 0) {
        g_mutex_lock(&app->sim_lock);
        snprintf(app->sim_status, sizeof(app->sim_status), "Integrator init failed");
        app->sim_done = 1; g_mutex_unlock(&app->sim_lock);
        arrfree(sim_events); arena_free(&sim_arena); return NULL;
    }

    double t = 0.0, t_next = t_step;
    int step = 0, ev_idx = 0;
    int seq_type = 0;
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
            if (sim_events[ev_idx].type == FAULT) seq_type = 0;
            else if (sim_events[ev_idx].type == FAULT_SLG) seq_type = 1;
            else if (sim_events[ev_idx].type == FAULT_LL) seq_type = 2;
            else if (sim_events[ev_idx].type == FAULT_DLG) seq_type = 3;
            else if (sim_events[ev_idx].type == FAULT_CLEAR) seq_type = 0;
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
                IDASetMaxOrd(itg.ida_mem, 1);
                IDAReInit(itg.ida_mem, t, itg.nvec_y, itg.nvec_ydot);
                IDASStolerances(itg.ida_mem, 1e-4, 1e-6);
            }
            free(Vtmp);
        }

        double *y = N_VGetArrayPointer(itg.nvec_y);
        int max_gen = n_gen > 0 ? n_gen : 1;
        int max_bus = n_bus > 0 ? n_bus : 1;
        double *gen_delta = malloc((size_t)max_gen * sizeof(double));
        double *gen_pe    = malloc((size_t)max_gen * sizeof(double));
        double *gen_omega = malloc((size_t)max_gen * sizeof(double));
        double *gen_vt    = malloc((size_t)max_gen * sizeof(double));
        double *bus_vm    = malloc((size_t)max_bus * sizeof(double));
        double *bus_va    = malloc((size_t)max_bus * sizeof(double));
        for (int m = 0; m < n_gen; m++) {
            int bi = s.gen[s.machine[m].gen_idx].bus;
            double d = y[2*m], om = y[2*m+1];
            double Vr = y[ndiff + 2*bi], Vi = y[ndiff + 2*bi + 1];
            double Ep = s.machine[m].Ep, xdp = s.machine[m].xdp;
            double Pe = Vr*(Ep*sin(d)-Vi)/xdp + Vi*(-(Ep*cos(d)-Vr))/xdp;
            double Vt = sqrt(Vr*Vr + Vi*Vi);
            gen_delta[m] = d; gen_pe[m] = Pe; gen_omega[m] = om; gen_vt[m] = Vt;
        }
        for (int i = 0; i < n_bus; i++) {
            double Vr = y[ndiff + 2*i], Vi = y[ndiff + 2*i + 1];
            bus_vm[i] = sqrt(Vr*Vr + Vi*Vi);
            bus_va[i] = atan2(Vi, Vr) * 180.0 / M_PI;
        }
        double osc_va = 0, osc_vb = 0, osc_vc = 0;
        if (app->scope_bus > 0) {
            int oidx = -1;
            for (int i = 0; i < n_bus; i++) if (s.bus[i].id == app->scope_bus) { oidx = i; break; }
            if (oidx >= 0) {
                double wr = 2.0 * M_PI * 60.0;
                double Vr = y[ndiff + 2*oidx], Vi = y[ndiff + 2*oidx + 1];
                double a2r=-0.5, a2i=-0.86602540378, ar=-0.5, ai=0.86602540378;
                double V1r=Vr, V1i=Vi, V2r=0, V2i=0, V0r=0, V0i=0;
                if (seq_type == 2) { V2r=V1r; V2i=V1i; }
                else if (seq_type == 1) { V2r=-0.5*V1r; V2i=-0.5*V1i; V0r=V2r; V0i=V2i; }
                else if (seq_type == 3) { V2r=V1r; V2i=V1i; V0r=V1r; V0i=V1i; }
                double Var=V1r+V2r+V0r, Vai=V1i+V2i+V0i;
                double Vbr=a2r*V1r-a2i*V1i + ar*V2r-ai*V2i + V0r;
                double Vbi=a2r*V1i+a2i*V1r + ar*V2i+ai*V2r + V0i;
                double Vcr=ar*V1r-ai*V1i + a2r*V2r-a2i*V2i + V0r;
                double Vci=ar*V1i+ai*V1r + a2r*V2i+a2i*V2r + V0i;
                double tar=Var, tai=Vai, tbr=Vbr, tbi=Vbi, tcr=Vcr, tci=Vci;
                if (seq_type == 2 || seq_type == 3) {
                    switch (s.fault_phase) {
                    case FAULT_PHASE_AB: Var=tbr; Vai=tbi; Vbr=tcr; Vbi=tci; Vcr=tar; Vci=tai; break;
                    case FAULT_PHASE_CA: Var=tcr; Vai=tci; Vbr=tar; Vbi=tai; Vcr=tbr; Vci=tbi; break;
                    default: break;
                    }
                } else if (seq_type == 1) {
                    switch (s.fault_phase) {
                    case FAULT_PHASE_BG: Var=tcr; Vai=tci; Vbr=tar; Vbi=tai; Vcr=tbr; Vci=tbi; break;
                    case FAULT_PHASE_CG: Var=tbr; Vai=tbi; Vbr=tcr; Vbi=tci; Vcr=tar; Vci=tai; break;
                    default: break;
                    }
                }
                osc_va = Var*cos(wr*t) - Vai*sin(wr*t);
                osc_vb = Vbr*cos(wr*t) - Vbi*sin(wr*t);
                osc_vc = Vcr*cos(wr*t) - Vci*sin(wr*t);
            }
        }
        plot_push(app, t, gen_delta, gen_pe, gen_omega, gen_vt, bus_vm, bus_va, osc_va, osc_vb, osc_vc);
        free(gen_delta); free(gen_pe); free(gen_omega); free(gen_vt);
        free(bus_vm); free(bus_va);
        if (step % 1000 == 0 && app->plot_fp) fflush(app->plot_fp);

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

    if (app->plot_fp) fflush(app->plot_fp);

    /* Build overview and start initial full-res load */
    plot_build_overview(app);
    loader_start(app);

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
    if (app->sys.nbus == 0) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app->window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "No network data to simulate!");
        gtk_dialog_run(GTK_DIALOG(dlg)); gtk_widget_destroy(dlg);
        return;
    }

    /* Save current system to temp files (captures GUI edits) */
    char tmp_raw[512] = "", tmp_dyr[512] = "";
    snprintf(tmp_raw, sizeof(tmp_raw), "/tmp/tss_sim_%d.raw", getpid());
    snprintf(tmp_dyr, sizeof(tmp_dyr), "/tmp/tss_sim_%d.dyr", getpid());
    raw_write(tmp_raw, &app->sys);
    dyr_write(tmp_dyr, &app->sys);
    strncpy(app->raw_path, tmp_raw, sizeof(app->raw_path) - 1);
    strncpy(app->dyr_path, tmp_dyr, sizeof(app->dyr_path) - 1);

    app->t_end = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->spin_tend));
    app->t_step = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->spin_tstep));

    int new_scope = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->spin_scope));
    if (new_scope != app->scope_bus) {
        app->scope_bus = new_scope;
        build_signal_tree(app);
    }

    app->sim_running = 1;
    app->sim_done = 0;
    app->sim_progress = 0;
    app->view_t0 = 0;
    app->view_t1 = app->t_end;
    snprintf(app->sim_status, sizeof(app->sim_status), "Starting...");
    gtk_widget_set_sensitive(app->btn_run, FALSE);

    char b[64];
    snprintf(b, sizeof(b), "View: %s – %s s", fmt_dbl(0.0), fmt_dbl(app->t_end));
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
    app->sys.base_mva = 100.0;
    app->sys.fault_bus = -1;
    g_mutex_init(&app->sim_lock);
    g_mutex_init(&app->loader_lock);
    app->ov_t = malloc(OV_SIZE * 2 * sizeof(double));
    app->ov_data = malloc(OV_SIZE * 2 * MAX_SIGNALS * sizeof(double));
    app->fr_t = NULL;
    app->fr_data = NULL;
    for (int s = 0; s < MAX_SIGNALS; s++)
        app->signals[s].data = calloc(PLOT_BUF, sizeof(double));
}

static void app_cleanup(App *app) {
    app->sim_running = 0;
    if (app->sim_thread) { g_thread_join(app->sim_thread); app->sim_thread = NULL; }
    plot_file_close(app);
    free(app->ov_t); free(app->ov_data);
    free(app->fr_t); free(app->fr_data);
    for (int s = 0; s < MAX_SIGNALS; s++) free(app->signals[s].data);
    free(app->sys.machine_states);
    free(app->sys.colptr); free(app->sys.rowidx); free(app->sys.yval);
    arrfree(app->sys.machine); arrfree(app->sys.bus);
    arrfree(app->sys.branch); arrfree(app->sys.gen); arrfree(app->sys.load);
    arena_free(&app->arena);
    g_mutex_clear(&app->sim_lock);
    g_mutex_clear(&app->loader_lock);
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

    GtkWidget *right_vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);

    GtkWidget *sim_box;
    build_sim_panel(app, &sim_box);
    GtkWidget *sim_frame = gtk_frame_new("Simulation");
    gtk_container_add(GTK_CONTAINER(sim_frame), sim_box);
    gtk_paned_pack1(GTK_PANED(right_vpaned), sim_frame, TRUE, FALSE);

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
