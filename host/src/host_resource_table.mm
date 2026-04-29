#include "host.h"

#include <stdlib.h>
#include <string.h>

/*
 * Resource table is a plain open-addressed hash map keyed by uint64.
 * Single global table, guarded by a single rwlock-style mutex (not actual
 * rwlock — calls are short and contention is moderate).
 *
 * The map grows by power-of-two doubling. We never shrink.
 */

typedef struct slot {
    cb_remote_id_t id;     /* 0 = empty, ~0 = tombstone */
    host_obj_t     obj;
} slot_t;

static slot_t          *g_tab;
static size_t           g_cap;
static size_t           g_load;
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;

#define EMPTY  0ull
#define TOMB   (~0ull)

static size_t hash64(cb_remote_id_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (size_t)x;
}

static void rehash(size_t newcap) {
    slot_t *old = g_tab;
    size_t  oc  = g_cap;
    g_tab  = (slot_t *)calloc(newcap, sizeof *g_tab);
    g_cap  = newcap;
    g_load = 0;
    if (!old) return;
    for (size_t i = 0; i < oc; ++i) {
        if (old[i].id == EMPTY || old[i].id == TOMB) continue;
        size_t h = hash64(old[i].id) & (g_cap - 1);
        while (g_tab[h].id != EMPTY) h = (h + 1) & (g_cap - 1);
        g_tab[h] = old[i];
        ++g_load;
    }
    free(old);
}

void host_table_init(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_tab) rehash(1024);
    pthread_mutex_unlock(&g_lock);
}

void host_table_put(cb_remote_id_t id, host_obj_kind_t k, void *ptr,
                    cb_remote_id_t parent_id) {
    if (id == EMPTY || id == TOMB) {
        HE("host_table_put: invalid id %llu", (unsigned long long)id);
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_tab) rehash(1024);
    if ((g_load + 1) * 2 >= g_cap) rehash(g_cap * 2);
    size_t h = hash64(id) & (g_cap - 1);
    while (g_tab[h].id != EMPTY && g_tab[h].id != TOMB && g_tab[h].id != id)
        h = (h + 1) & (g_cap - 1);
    if (g_tab[h].id == EMPTY || g_tab[h].id == TOMB) ++g_load;
    g_tab[h].id  = id;
    g_tab[h].obj.kind      = k;
    g_tab[h].obj.ptr       = ptr;
    g_tab[h].obj.parent_id = parent_id;
    pthread_mutex_unlock(&g_lock);
}

void *host_table_get(cb_remote_id_t id, host_obj_kind_t k) {
    if (id == EMPTY) return NULL;
    pthread_mutex_lock(&g_lock);
    void *out = NULL;
    if (g_tab) {
        size_t h = hash64(id) & (g_cap - 1);
        while (g_tab[h].id != EMPTY) {
            if (g_tab[h].id == id) {
                if (k == 0 || g_tab[h].obj.kind == k) out = g_tab[h].obj.ptr;
                break;
            }
            h = (h + 1) & (g_cap - 1);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return out;
}

void host_table_drop(cb_remote_id_t id) {
    if (id == EMPTY) return;
    pthread_mutex_lock(&g_lock);
    if (g_tab) {
        size_t h = hash64(id) & (g_cap - 1);
        while (g_tab[h].id != EMPTY) {
            if (g_tab[h].id == id) {
                g_tab[h].id  = TOMB;
                g_tab[h].obj = (host_obj_t){0};
                break;
            }
            h = (h + 1) & (g_cap - 1);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* Convenience finders --------------------------------------------------- */

host_instance_rec_t *host_instance_for(cb_remote_id_t id) {
    return (host_instance_rec_t *)host_table_get(id, HK_HOST_INSTANCE);
}

host_device_rec_t *host_device_for(cb_remote_id_t id) {
    return (host_device_rec_t *)host_table_get(id, HK_HOST_DEVICE);
}
