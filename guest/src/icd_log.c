#include "icd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cb_log_level_t cb_log_level_from_env(void) {
    const char *e = getenv("CHEESEBRIDGE_LOG");
    if (!e || !*e) return CB_LOG_WARN;
    if (!strcmp(e, "trace")) return CB_LOG_TRACE;
    if (!strcmp(e, "debug")) return CB_LOG_DEBUG;
    if (!strcmp(e, "info"))  return CB_LOG_INFO;
    if (!strcmp(e, "warn"))  return CB_LOG_WARN;
    if (!strcmp(e, "error")) return CB_LOG_ERROR;
    return CB_LOG_WARN;
}

static const char *cb_log_label(cb_log_level_t lvl) {
    switch (lvl) {
        case CB_LOG_ERROR: return "ERROR";
        case CB_LOG_WARN:  return "WARN ";
        case CB_LOG_INFO:  return "INFO ";
        case CB_LOG_DEBUG: return "DEBUG";
        case CB_LOG_TRACE: return "TRACE";
    }
    return "?    ";
}

void cb_log(cb_log_level_t lvl, const char *fmt, ...) {
    static cb_log_level_t cached = (cb_log_level_t)-1;
    if (cached == (cb_log_level_t)-1) cached = cb_log_level_from_env();
    if (lvl > cached) return;

    fprintf(stderr, "[cheesebridge %s] ", cb_log_label(lvl));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
