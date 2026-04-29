#include "host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static host_log_level_t hl_from_env(void) {
    const char *e = getenv("CHEESEBRIDGE_LOG");
    if (!e || !*e) return HL_INFO;
    if (!strcmp(e, "trace")) return HL_TRACE;
    if (!strcmp(e, "debug")) return HL_DEBUG;
    if (!strcmp(e, "info"))  return HL_INFO;
    if (!strcmp(e, "warn"))  return HL_WARN;
    if (!strcmp(e, "error")) return HL_ERROR;
    return HL_INFO;
}

static const char *hl_label(host_log_level_t l) {
    switch (l) {
        case HL_ERROR: return "ERROR";
        case HL_WARN:  return "WARN ";
        case HL_INFO:  return "INFO ";
        case HL_DEBUG: return "DEBUG";
        case HL_TRACE: return "TRACE";
    }
    return "?    ";
}

void host_log(host_log_level_t lvl, const char *fmt, ...) {
    static host_log_level_t cached = (host_log_level_t)-1;
    if (cached == (host_log_level_t)-1) cached = hl_from_env();
    if (lvl > cached) return;
    fprintf(stderr, "[cheesebridge-host %s] ", hl_label(lvl));
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
