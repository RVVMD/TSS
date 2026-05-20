#ifndef TYPES_H
#define TYPES_H

#include <stddef.h>
#include <complex.h>
#include <stdio.h>

#include <ida/ida.h>
#include <sundials/sundials_nvector.h>
#include <sundials/sundials_context.h>
#include <sundials/sundials_matrix.h>
#include <sundials/sundials_linearsolver.h>

/* --- enums --- */
typedef enum {
    BUS_PQ = 1,
    BUS_PV = 2,
    BUS_SLACK = 3,
    BUS_ISOLATED = 4
} BusType;

typedef enum {
    GENCLS = 1,
    GENROU = 2,
    GENSAL = 3
} MachineModelType;

typedef enum {
    EXDC1 = 1,
    IEEET1 = 2,
    EXST1 = 3
} ExciterType;

typedef enum {
    TGOV1 = 1,
    IEEEG1 = 2,
    HYGOV = 3
} GovernorType;

typedef enum {
    FAULT        = 1,
    LINE_OPEN    = 2,
    LINE_CLOSE   = 3,
    GEN_TRIP     = 4,
    LOAD_SHED    = 5,
    FAULT_CLEAR  = 6,
    FAULT_SLG    = 7,
    FAULT_LL     = 8,
    FAULT_DLG    = 9,
    END_SIM      = 99
} EventType;

/* fault phase codes: which phases are involved */
#define FAULT_PHASE_ABC  0
#define FAULT_PHASE_AB   1
#define FAULT_PHASE_BC   2
#define FAULT_PHASE_CA   3
#define FAULT_PHASE_AG   4
#define FAULT_PHASE_BG   5
#define FAULT_PHASE_CG   6

/* --- arena allocator --- */
typedef struct {
    char  *mem;
    size_t cap;
    size_t off;
} Arena;

Arena arena_new(size_t cap);
void *arena_alloc(Arena *a, size_t n);
void  arena_free(Arena *a);

/* --- stretchy buffer macros (header-only) --- */
#define arrlen(a)   ((a) ? ((size_t *)(a))[-2] : 0)
#define arrcap(a)   ((a) ? ((size_t *)(a))[-1] : 0)
#define arrfree(a)  do { if (a) { free((size_t *)(a) - 2); (a) = NULL; } } while (0)

void *_arrgrow(void *arr, size_t elem_sz);
#define arrpush(a, v) do { \
    if (!(a) || arrlen(a) >= arrcap(a)) \
        (a) = _arrgrow((a), sizeof(*(a))); \
    else { \
        size_t *_h = ((size_t *)(a)) - 2; \
        _h[0] += 1; \
    } \
    (a)[arrlen(a) - 1] = (v); \
} while (0)

/* arrlast returns pointer to final element */
#define arrlast(a) ((a) ? &(a)[arrlen(a) - 1] : NULL)

/* --- int→ptr hash map (header-only) --- */
typedef struct {
    int   key;
    void *val;
    int   hash;
    struct { int key; void *val; } entry;
    int   occupied;
} HMEntry;

typedef struct {
    HMEntry *buckets;
    size_t   cap;
    size_t   len;
} HashMap;

HashMap *hashmap_new(void);
void     hashmap_free(HashMap *m);
void    *hashmap_get(HashMap *m, int key);
void     hashmap_put(HashMap *m, int key, void *val);
void     hashmap_remove(HashMap *m, int key);

/* --- network --- */
typedef struct {
    int     id;
    BusType type;
    double  base_kv;
    double  vm, va;
    double  pd, qd;
    double  gs, bs;
    double  vmin, vmax;
    char    name[16];
    double  gl, bl;   /* constant-Z load eq. from initial pf V */
} Bus;

typedef struct {
    int    id;
    int    from, to;
    char   ckt[3];
    double r, x;
    double b;
    double rate_a, rate_b, rate_c;
    double tap, shift;
    int    status;
} Branch;

typedef struct {
    int    id;
    int    bus;
    double pg, qg;
    double mbase;
    double vsched;
    int    machine_idx;
} Gen;

typedef struct {
    int    id;
    int    bus;
    double p, q;
    double ip, iq;
    double yp, yq;
    int    status;
} Load;

/* --- dynamic models --- */
typedef struct {
    int    gen_idx;
    double h;
    double d;
    double xdp;
    double xq;
    double Ep;   /* internal voltage magnitude (pre-fault) */
} Machine;

/* --- events --- */
typedef struct {
    EventType type;
    double    time;
    int       bus;
    int       from, to;
    double    fault_r, fault_x;
    int       fault_phase;
} Event;

/* --- system --- */
typedef struct {
    int     nbus, nbranch, ngen, nload;
    Bus    *bus;
    Branch *branch;
    Gen    *gen;
    Load   *load;
    int    *colptr, *rowidx;
    int     nnz;
    double *yval;
    double  base_mva;
    int     nmachines;
    Machine *machine;
    double  *machine_states;
    /* fault state */
    int     fault_bus;       /* -1 = no fault */
    double  fault_Y_r, fault_Y_i;
    int     fault_type;      /* 0=balanced, 1=SLG, 2=LL, 3=DLG */
    int     fault_phase;     /* FAULT_PHASE_* */
    double  fault_Zth_r, fault_Zth_i; /* Thevenin impedance at fault bus */
    double  fault_Vth_r, fault_Vth_i; /* Thevenin voltage at fault bus */
    double  fault_t0;        /* fault inception time */
    double  fault_XoR;       /* X/R ratio at fault bus */
    double  fault_clear_t;   /* last fault clear time (for inrush) */
} System;

/* --- DAE --- */
typedef struct {
    System *sys;
    int     ndiff, nalg, neq;
    int    *jcolptr, *jrowidx;
    int     jnnz;
    double *jval;
} DAE;

/* --- integrator --- */
typedef struct {
    void    *ida_mem;
    SUNContext sunctx;
    N_Vector nvec_y, nvec_ydot;
    SUNMatrix sunmat_J;
    SUNLinearSolver sunls;
    DAE     *dae;
    int      neq;
} Integrator;

/* --- CSV writer --- */
typedef struct {
    FILE  *fp;
    int    ncols;
    char **headers;
} CSVWriter;

/* --- logging --- */
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

#endif /* TYPES_H */
