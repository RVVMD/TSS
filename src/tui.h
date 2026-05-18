#ifndef TUI_H
#define TUI_H
#include "types.h"

/* launch the full TUI interface (primary mode) */
int tui_main(void);

/* embed a system struct + file paths for a project */
typedef struct {
    System    sys;
    Arena     arena;
    char      raw_path[256];
    char      dyr_path[256];
    char      ini_path[256];
    char      proj_name[64];
    Event    *events;
    int       n_events;
    int       loaded;
} TUIProject;

#endif
