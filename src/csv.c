#include "types.h"
#include <stdlib.h>
#include <string.h>

CSVWriter *csv_open(const char *path, int ncols, const char **headers)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return NULL;

    CSVWriter *w = malloc(sizeof(CSVWriter));
    if (!w) { fclose(fp); return NULL; }
    w->fp = fp;
    w->ncols = ncols;
    w->headers = NULL;

    if (headers) {
        for (int i = 0; i < ncols; i++) {
            if (i > 0) fputc(',', fp);
            fputs(headers[i], fp);
        }
        fputc('\n', fp);
    }
    return w;
}

void csv_row(CSVWriter *w, double t, const double *data)
{
    fprintf(w->fp, "%.6f", t);
    for (int i = 0; i < w->ncols; i++)
        fprintf(w->fp, ",%.12g", data[i]);
    fputc('\n', w->fp);
}

void csv_close(CSVWriter *w)
{
    if (!w) return;
    if (w->fp) fclose(w->fp);
    free(w);
}
