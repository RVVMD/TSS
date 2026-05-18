#include "types.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static void log_msg(const char *level, const char *fmt, va_list ap)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(stderr, "[%02d:%02d:%02d] %-5s ",
            t->tm_hour, t->tm_min, t->tm_sec, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    log_msg("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    log_msg("WARN", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    log_msg("ERROR", fmt, ap);
    va_end(ap);
}
