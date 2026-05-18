#include "types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s) - 1;
    while (e >= s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

int events_parse(const char *filename, Event **evlist, Arena *a)
{
    (void)a;
    FILE *fp = fopen(filename, "r");
    if (!fp) { log_error("cannot open %s", filename); return -1; }

    Event curev;
    memset(&curev, 0, sizeof(curev));
    int in_event = 0;
    char line[512];

    while (fgets(line, sizeof(line), fp)) {
        char *t = trim(line);
        if (t[0] == '#' || t[0] == ';' || t[0] == '\0') continue;

        if (t[0] == '[') {
            /* save previous event if complete */
            if (in_event) {
                arrpush(*evlist, curev);
                memset(&curev, 0, sizeof(curev));
            }
            in_event = 1;
            continue;
        }

        if (!in_event) continue;

        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(t);
        char *val = trim(eq + 1);

        if (!strcmp(key, "time"))     curev.time     = atof(val);
        else if (!strcmp(key, "type")) {
            if (!strcmp(val, "fault"))      curev.type = FAULT;
            else if (!strcmp(val, "slg"))    curev.type = FAULT_SLG;
            else if (!strcmp(val, "ll"))     curev.type = FAULT_LL;
            else if (!strcmp(val, "dlg"))    curev.type = FAULT_DLG;
            else if (!strcmp(val, "line-open"))  curev.type = LINE_OPEN;
            else if (!strcmp(val, "clear"))      curev.type = FAULT_CLEAR;
            else if (!strcmp(val, "end"))       curev.type = END_SIM;
        }
        else if (!strcmp(key, "bus"))       curev.bus       = atoi(val);
        else if (!strcmp(key, "from"))      curev.from      = atoi(val);
        else if (!strcmp(key, "to"))        curev.to        = atoi(val);
        else if (!strcmp(key, "r"))         curev.fault_r   = atof(val);
        else if (!strcmp(key, "x"))         curev.fault_x   = atof(val);
    }

    if (in_event) arrpush(*evlist, curev);

    fclose(fp);
    log_info("EVENTS: %d events loaded", arrlen(*evlist));
    return 0;
}
