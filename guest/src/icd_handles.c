#include "icd.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static atomic_uint_least64_t g_next_id = 1;

cb_remote_id_t cb_next_id(void) {
    return (cb_remote_id_t)atomic_fetch_add(&g_next_id, 1);
}

void *cb_alloc_dispatchable(size_t size) {
    void *p = calloc(1, size);
    if (!p) return NULL;
    /* The Vulkan loader requires the first machine word of every
     * dispatchable handle to carry a magic value so the trampoline can
     * recognise it. */
    set_loader_magic_value(p);
    return p;
}

void cb_free_dispatchable(void *p) {
    free(p);
}
