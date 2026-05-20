#ifndef COMTRADE_H
#define COMTRADE_H

#include "types.h"

typedef struct {
    int    index;
    char   id[32];
    char   phase[8];
    char   circuit[32];
    char   unit[8];
    double a;
    double b;
    double min;
    double max;
    double primary;
    double secondary;
    int    ps;
} ComtradeAnalogCh;

typedef struct {
    int    index;
    char   id[32];
    char   phase[8];
    char   circuit[32];
} ComtradeDigitalCh;

typedef struct {
    char   station[64];
    char   device[64];
    int    rev_year;

    ComtradeAnalogCh  *analogs;
    int    n_analogs;
    int    cap_analogs;

    ComtradeDigitalCh *digitals;
    int    n_digitals;
    int    cap_digitals;

    double line_freq;
    int    sample_rate;
    int    total_samples;
    int    fmt;

    int sy, sm, sd, sh, smin, ss, sus;
    int ty, tm, td, th, tmin, ts, tus;

    FILE  *fp_cfg;
    FILE  *fp_dat;
    int    sample_num;
} ComtradeWriter;

ComtradeWriter *comtrade_create(const char *station, const char *device);
void comtrade_set_time(ComtradeWriter *w,
                       int sy, int sm, int sd, int sh, int smin, int ss, int sus,
                       int ty, int tm, int td, int th, int tmin, int ts, int tus);
int  comtrade_add_analog(ComtradeWriter *w, const char *id, const char *phase,
                         const char *circuit, const char *unit,
                         double a, double b, double min, double max,
                         double primary, double secondary, int ps);
int  comtrade_add_digital(ComtradeWriter *w, const char *id, const char *phase,
                          const char *circuit);
int  comtrade_write_cfg(ComtradeWriter *w, const char *path);
int  comtrade_open_dat(ComtradeWriter *w, const char *path);
void comtrade_write_ascii(ComtradeWriter *w, double timestamp,
                          const double *analog_vals, const int *digital_vals);
void comtrade_close(ComtradeWriter *w);

double comtrade_v_factor(double base_kv);
double comtrade_i_factor(double base_mva, double base_kv);

#endif
