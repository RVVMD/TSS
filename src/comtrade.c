#include "comtrade.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

ComtradeWriter *comtrade_create(const char *station, const char *device)
{
    ComtradeWriter *w = calloc(1, sizeof(ComtradeWriter));
    if (!w) return NULL;
    if (station) snprintf(w->station, sizeof(w->station), "%s", station);
    else         snprintf(w->station, sizeof(w->station), "TSS");
    if (device)  snprintf(w->device, sizeof(w->device), "%s", device);
    else         snprintf(w->device, sizeof(w->device), "TSS Simulator");
    w->rev_year = 1999;
    w->line_freq = 50.0;
    w->sample_rate = 1000;
    w->total_samples = 0;
    w->fmt = 0;
    w->sample_num = 0;

    time_t now = time(NULL);
    struct tm *g = gmtime(&now);
    w->sy = g->tm_year + 1900;
    w->sm = g->tm_mon + 1;
    w->sd = g->tm_mday;
    w->sh = g->tm_hour;
    w->smin = g->tm_min;
    w->ss = g->tm_sec;
    w->sus = 0;
    w->ty = w->sy; w->tm = w->sm; w->td = w->sd;
    w->th = w->sh; w->tmin = w->smin; w->ts = w->ss; w->tus = 0;

    return w;
}

void comtrade_set_time(ComtradeWriter *w,
                       int sy, int sm, int sd, int sh, int smin, int ss, int sus,
                       int ty, int tm, int td, int th, int tmin, int ts, int tus)
{
    w->sy = sy; w->sm = sm; w->sd = sd;
    w->sh = sh; w->smin = smin; w->ss = ss; w->sus = sus;
    w->ty = ty; w->tm = tm; w->td = td;
    w->th = th; w->tmin = tmin; w->ts = ts; w->tus = tus;
}

int comtrade_add_analog(ComtradeWriter *w, const char *id, const char *phase,
                        const char *circuit, const char *unit,
                        double a, double b, double min, double max,
                        double primary, double secondary, int ps)
{
    if (w->n_analogs >= w->cap_analogs) {
        int newcap = w->cap_analogs ? w->cap_analogs * 2 : 16;
        ComtradeAnalogCh *tmp = realloc(w->analogs, (size_t)newcap * sizeof(ComtradeAnalogCh));
        if (!tmp) return -1;
        w->analogs = tmp;
        w->cap_analogs = newcap;
    }
    ComtradeAnalogCh *ch = &w->analogs[w->n_analogs++];
    ch->index = w->n_analogs;
    snprintf(ch->id, sizeof(ch->id), "%s", id);
    snprintf(ch->phase, sizeof(ch->phase), "%s", phase);
    snprintf(ch->circuit, sizeof(ch->circuit), "%s", circuit);
    snprintf(ch->unit, sizeof(ch->unit), "%s", unit);
    ch->a = a;
    ch->b = b;
    ch->min = min;
    ch->max = max;
    ch->primary = primary;
    ch->secondary = secondary;
    ch->ps = ps;
    return 0;
}

int comtrade_add_digital(ComtradeWriter *w, const char *id, const char *phase,
                         const char *circuit)
{
    if (w->n_digitals >= w->cap_digitals) {
        int newcap = w->cap_digitals ? w->cap_digitals * 2 : 16;
        ComtradeDigitalCh *tmp = realloc(w->digitals, (size_t)newcap * sizeof(ComtradeDigitalCh));
        if (!tmp) return -1;
        w->digitals = tmp;
        w->cap_digitals = newcap;
    }
    ComtradeDigitalCh *ch = &w->digitals[w->n_digitals++];
    ch->index = w->n_digitals;
    snprintf(ch->id, sizeof(ch->id), "%s", id);
    snprintf(ch->phase, sizeof(ch->phase), "%s", phase);
    snprintf(ch->circuit, sizeof(ch->circuit), "%s", circuit);
    return 0;
}

int comtrade_write_cfg(ComtradeWriter *w, const char *path)
{
    w->fp_cfg = fopen(path, "w");
    if (!w->fp_cfg) return -1;

    int aa = w->n_analogs;
    int dd = w->n_digitals;
    int tt = aa + dd;

    fprintf(w->fp_cfg, "%s,%s,%d\n", w->station, w->device, w->rev_year);
    fprintf(w->fp_cfg, "%d,%dA,%dD\n", tt, aa, dd);

    for (int i = 0; i < w->n_analogs; i++) {
        ComtradeAnalogCh *ch = &w->analogs[i];
        fprintf(w->fp_cfg, "%d,%s,%s,%s,%s,%.12g,%.12g,0,%.12g,%.12g,%.12g,%.12g,%s\n",
                ch->index, ch->id, ch->phase, ch->circuit, ch->unit,
                ch->a, ch->b, ch->min, ch->max,
                ch->primary, ch->secondary,
                ch->ps ? "S" : "P");
    }

    for (int i = 0; i < w->n_digitals; i++) {
        ComtradeDigitalCh *ch = &w->digitals[i];
        fprintf(w->fp_cfg, "%d,%s,%s,%s,0\n",
                ch->index, ch->id, ch->phase, ch->circuit);
    }

    fprintf(w->fp_cfg, "%.0f\n", w->line_freq);
    fprintf(w->fp_cfg, "1\n");
    fprintf(w->fp_cfg, "%d,%d\n", w->sample_rate, w->total_samples);
    fprintf(w->fp_cfg, "%02d/%02d/%04d,%02d:%02d:%02d.%06d\n",
            w->sd, w->sm, w->sy, w->sh, w->smin, w->ss, w->sus);
    fprintf(w->fp_cfg, "%02d/%02d/%04d,%02d:%02d:%02d.%06d\n",
            w->td, w->tm, w->ty, w->th, w->tmin, w->ts, w->tus);
    fprintf(w->fp_cfg, "%s\n", w->fmt ? "BINARY" : "ASCII");
    if (w->rev_year >= 1999) {
        fprintf(w->fp_cfg, "1.0\n");
    }

    fclose(w->fp_cfg);
    w->fp_cfg = NULL;
    return 0;
}

int comtrade_open_dat(ComtradeWriter *w, const char *path)
{
    w->fp_dat = fopen(path, "w");
    if (!w->fp_dat) return -1;
    w->sample_num = 0;
    return 0;
}

void comtrade_write_ascii(ComtradeWriter *w, double timestamp,
                          const double *analog_vals, const int *digital_vals)
{
    if (!w->fp_dat) return;
    w->sample_num++;
    w->total_samples++;

    fprintf(w->fp_dat, "%d,%.0f", w->sample_num, timestamp * 1e6);

    for (int i = 0; i < w->n_analogs; i++) {
        double v = analog_vals[i];
        double min = w->analogs[i].min;
        double max = w->analogs[i].max;
        if (v < min) v = min;
        if (v > max) v = max;
        fprintf(w->fp_dat, ",%.12g", v);
    }

    for (int i = 0; i < w->n_digitals; i++) {
        int v = digital_vals ? digital_vals[i] : 0;
        fprintf(w->fp_dat, ",%d", v ? 1 : 0);
    }

    fprintf(w->fp_dat, "\n");
}

void comtrade_close(ComtradeWriter *w)
{
    if (w->fp_dat) { fclose(w->fp_dat); w->fp_dat = NULL; }
    free(w->analogs);
    free(w->digitals);
    free(w);
}

double comtrade_v_factor(double base_kv)
{
    return base_kv * sqrt(2.0 / 3.0);
}

double comtrade_i_factor(double base_mva, double base_kv)
{
    if (base_kv < 1e-10) return 0;
    return base_mva * sqrt(2.0) / (sqrt(3.0) * base_kv);
}
