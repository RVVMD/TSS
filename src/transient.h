#ifndef TRANSIENT_H
#define TRANSIENT_H

#include "types.h"

/* --- parser --- */
int raw_parse(const char *filename, System *sys, Arena *a);
int dyr_parse(const char *filename, System *sys, Arena *a);
int events_parse(const char *filename, Event **evlist, Arena *a);

/* --- network --- */
int ybus_build(System *sys, Arena *a);
int powerflow_solve(System *sys, Arena *a);

/* --- machines --- */
int machine_init(System *sys);

/* --- DAE --- */
int  dae_init(DAE *dae, System *sys, Arena *a);
void dae_free(DAE *dae);
int  dae_residual(double t, N_Vector yy, N_Vector yp,
                   N_Vector rr, void *user_data);

/* --- integrator --- */
int  integrator_init(Integrator *itg, DAE *dae, double t0);
int  integrator_step(Integrator *itg, double tout, double *tret);
void integrator_free(Integrator *itg);

/* --- events --- */
int events_apply(System *sys, Event *ev, double t);
int events_post_state(const System *sys, const double *delta, double *V_out);

/* --- output --- */
CSVWriter *csv_open(const char *path, int ncols, const char **headers);
void       csv_row(CSVWriter *w, double t, const double *data);
void       csv_close(CSVWriter *w);

/* --- plot --- */
int plot_csv(const char *csv_path, const char *png_path,
             const char *xlabel, const char *ylabel, const char *title,
             int nseries, const char **labels);

#endif /* TRANSIENT_H */
