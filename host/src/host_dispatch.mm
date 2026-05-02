/*
 * The big opcode switch. Decodes a request, calls into MoltenVK, and writes
 * a reply. Each handler is a leaf function; the switch keeps things uniform.
 *
 * Conventions:
 *   - The very first u64 of every device-scoped request is the device id.
 *   - Successful replies that produce a new object echo back its remote_id
 *     using host_reply_id().
 *   - Errors are reported via host_reply_fail(VkResult).
 *
 * Newly-created Vulkan handles are registered in the resource table under
 * the id the guest just allocated. The guest's id is the canonical one.
 */
#import "host.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdlib.h>
#include <string.h>

/* ---- Helpers ----------------------------------------------------------- */

/* Forward declaration: defined later in the file. Used by op_create_device
 * before its definition site. */
static host_device_rec_t *g_last_device;

#define READ_DEV(R, REC)                                              \
    cb_remote_id_t _devid = cb_r_u64(R);                              \
    host_device_rec_t *REC = host_device_for(_devid);                 \
    if (!REC) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }

/* ---- Session ----------------------------------------------------------- */

static void op_hello(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)r;
    cb_writer_t w; cb_writer_init_heap(&w, 8);
    cb_w_u32(&w, CB_PROTO_VERSION);
    /* HELLO_REPLY uses its own opcode (not GENERIC_REPLY). */
    pthread_mutex_lock(&c->write_lock);
    cb_write_frame(c->fd, CB_OP_HELLO_REPLY, CB_FLAG_REPLY, seq,
                   w.buf, (uint32_t)w.pos);
    pthread_mutex_unlock(&c->write_lock);
    cb_writer_dispose(&w);
}

/* ---- Instance ---------------------------------------------------------- */

static void op_create_instance(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    uint32_t api = cb_r_u32(r);
    uint32_t app_len = 0; const char *app = (const char *)cb_r_blob(r, &app_len);
    (void)app;

    if (host_load_moltenvk() != VK_SUCCESS) {
        host_reply_fail(c, seq, VK_ERROR_INCOMPATIBLE_DRIVER);
        return;
    }

    /* Enable the Metal surface ext on the host so we can wrap CAMetalLayer.
     * We talk to MoltenVK directly via dlopen (no Vulkan loader in the host
     * process), so we must NOT request loader-level extensions like
     * VK_KHR_portability_enumeration or set the portability bit — MoltenVK
     * rejects both with VK_ERROR_EXTENSION_NOT_PRESENT.
     *
     * We also probe what MoltenVK actually exports and only ask for what's
     * really there, so a stripped-down build of MoltenVK (no surface ext)
     * still gets us a valid instance for compute/headless work. */
    const char *kInstExtsWanted[] = {
        "VK_KHR_surface",
        "VK_EXT_metal_surface",
    };
    const char *kInstExts[sizeof kInstExtsWanted / sizeof kInstExtsWanted[0]];
    uint32_t kInstExtCount = 0;

    {
        uint32_t n = 0;
        g_vk.EnumerateInstanceExtensionProperties(NULL, &n, NULL);
        VkExtensionProperties *avail =
            (VkExtensionProperties *)calloc(n ? n : 1, sizeof *avail);
        g_vk.EnumerateInstanceExtensionProperties(NULL, &n, avail);
        for (size_t i = 0; i < sizeof kInstExtsWanted / sizeof kInstExtsWanted[0]; ++i) {
            for (uint32_t j = 0; j < n; ++j) {
                if (!strcmp(kInstExtsWanted[i], avail[j].extensionName)) {
                    kInstExts[kInstExtCount++] = kInstExtsWanted[i];
                    break;
                }
            }
        }
        free(avail);
    }

    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = app_len ? app : "CheeseBridge guest",
        .apiVersion = api ? api : VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai,
        .enabledExtensionCount = kInstExtCount,
        .ppEnabledExtensionNames = kInstExtCount ? kInstExts : NULL,
    };
    VkInstance vki;
    VkResult vr = g_vk.CreateInstance(&ci, NULL, &vki);
    if (vr != VK_SUCCESS) { host_reply_fail(c, seq, vr); return; }

    host_instance_rec_t *rec = (host_instance_rec_t *)calloc(1, sizeof *rec);
    rec->vk = vki;
    host_load_instance_funcs(vki, &rec->ifn);

    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)rec;  /* opaque non-zero id */
    host_table_put(id, HK_HOST_INSTANCE, rec, 0);
    HI("CreateInstance -> %llu (vk=%p) [%u/%zu instance exts enabled]",
       (unsigned long long)id, (void *)vki, kInstExtCount,
       sizeof kInstExtsWanted / sizeof kInstExtsWanted[0]);
    host_reply_id(c, seq, id);
}

static void op_destroy_instance(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    host_instance_rec_t *rec = host_instance_for(id);
    if (!rec) return;
    if (rec->ifn.DestroyInstance) rec->ifn.DestroyInstance(rec->vk, NULL);
    free(rec->pds);
    free(rec);
    host_table_drop(id);
}

static void op_enumerate_pds(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t iid = cb_r_u64(r);
    host_instance_rec_t *rec = host_instance_for(iid);
    if (!rec) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    uint32_t n = 0;
    rec->ifn.EnumeratePhysicalDevices(rec->vk, &n, NULL);
    if (rec->pds) free(rec->pds);
    rec->pds = (VkPhysicalDevice *)calloc(n, sizeof *rec->pds);
    rec->pd_count = n;
    rec->ifn.EnumeratePhysicalDevices(rec->vk, &n, rec->pds);

    cb_writer_t w; cb_writer_init_heap(&w, 8 + n * 8);
    cb_w_u32(&w, n);
    for (uint32_t i = 0; i < n; ++i) {
        cb_remote_id_t pid = (cb_remote_id_t)(uintptr_t)rec->pds[i];
        host_table_put(pid, HK_PHYSICAL_DEVICE, rec->pds[i], iid);
        cb_w_u64(&w, pid);
    }
    host_reply_writer(c, seq, &w);
    cb_writer_dispose(&w);
}

#define SIMPLE_PD_QUERY(name, fn, type)                                       \
static void op_pd_##name(host_conn_t *c, uint32_t seq, cb_reader_t *r) {      \
    cb_remote_id_t pid = cb_r_u64(r);                                         \
    VkPhysicalDevice pd = (VkPhysicalDevice)host_table_get(pid, HK_PHYSICAL_DEVICE); \
    if (!pd) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }       \
    /* find the instance that owns this pd to access function table */       \
    host_obj_t obj; (void)obj;                                                \
    /* the parent_id stored is the instance id */                             \
    /* We re-walk the resource table to fetch the instance rec.       */     \
    /* Simpler: use any of g_instances; here, fetch by parent_id.     */     \
    cb_remote_id_t iid = 0; {                                                 \
        /* Ad hoc: the resource_table doesn't expose parent_id, so we   */    \
        /* keep a thread-local cache. Fallback: scan via `host_instance_*` */ \
    }                                                                         \
    (void)iid;                                                                \
    type val; memset(&val, 0, sizeof val);                                    \
    /* Use any host instance funcs — function pointers are loader-bound to */\
    /* the instance, which we lost. Cache via a small registry at create  */\
    /* time below.                                                        */\
    extern host_instance_funcs_t *host_pd_funcs(VkPhysicalDevice pd);         \
    host_instance_funcs_t *ifn = host_pd_funcs(pd);                           \
    if (!ifn || !ifn->fn) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; } \
    ifn->fn(pd, &val);                                                        \
    host_reply_bytes(c, seq, &val, sizeof val);                               \
}

/* Map physical device -> owning instance funcs.
 * Implemented inline below as a tiny linear cache (capped). */
#define HOST_PD_CACHE_MAX 16
static struct { VkPhysicalDevice pd; host_instance_funcs_t *ifn; }
    g_pd_cache[HOST_PD_CACHE_MAX];
static int g_pd_cache_n = 0;
static pthread_mutex_t g_pd_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static void host_pd_cache_put(VkPhysicalDevice pd, host_instance_funcs_t *ifn) {
    pthread_mutex_lock(&g_pd_cache_lock);
    for (int i = 0; i < g_pd_cache_n; ++i)
        if (g_pd_cache[i].pd == pd) {
            g_pd_cache[i].ifn = ifn;
            pthread_mutex_unlock(&g_pd_cache_lock); return;
        }
    if (g_pd_cache_n < HOST_PD_CACHE_MAX) {
        g_pd_cache[g_pd_cache_n++] = (typeof(g_pd_cache[0])){ pd, ifn };
    }
    pthread_mutex_unlock(&g_pd_cache_lock);
}
host_instance_funcs_t *host_pd_funcs(VkPhysicalDevice pd) {
    pthread_mutex_lock(&g_pd_cache_lock);
    host_instance_funcs_t *out = NULL;
    for (int i = 0; i < g_pd_cache_n; ++i)
        if (g_pd_cache[i].pd == pd) { out = g_pd_cache[i].ifn; break; }
    pthread_mutex_unlock(&g_pd_cache_lock);
    return out;
}

/* Re-do op_enumerate_pds to also populate the pd-funcs cache. */
static void register_pd_funcs(host_instance_rec_t *rec) {
    for (uint32_t i = 0; i < rec->pd_count; ++i)
        host_pd_cache_put(rec->pds[i], &rec->ifn);
}

SIMPLE_PD_QUERY(properties, GetPhysicalDeviceProperties,  VkPhysicalDeviceProperties)
SIMPLE_PD_QUERY(features,   GetPhysicalDeviceFeatures,    VkPhysicalDeviceFeatures)
SIMPLE_PD_QUERY(memprops,   GetPhysicalDeviceMemoryProperties, VkPhysicalDeviceMemoryProperties)

static void op_pd_qfp(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t pid = cb_r_u64(r);
    VkPhysicalDevice pd = (VkPhysicalDevice)host_table_get(pid, HK_PHYSICAL_DEVICE);
    if (!pd) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    host_instance_funcs_t *ifn = host_pd_funcs(pd);
    if (!ifn) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    uint32_t n = 0;
    ifn->GetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    VkQueueFamilyProperties *p =
        (VkQueueFamilyProperties *)calloc(n, sizeof *p);
    ifn->GetPhysicalDeviceQueueFamilyProperties(pd, &n, p);

    cb_writer_t w; cb_writer_init_heap(&w, 8 + n * sizeof *p);
    cb_w_u32(&w, n);
    cb_w_bytes(&w, p, n * sizeof *p);
    host_reply_writer(c, seq, &w);
    cb_writer_dispose(&w);
    free(p);
}

static void op_pd_format_props(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t pid = cb_r_u64(r);
    VkFormat fmt = (VkFormat)cb_r_u32(r);
    VkPhysicalDevice pd = (VkPhysicalDevice)host_table_get(pid, HK_PHYSICAL_DEVICE);
    host_instance_funcs_t *ifn = host_pd_funcs(pd);
    if (!pd || !ifn) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkFormatProperties fp;
    ifn->GetPhysicalDeviceFormatProperties(pd, fmt, &fp);
    host_reply_bytes(c, seq, &fp, sizeof fp);
}

static void op_pd_image_format_props(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t pid = cb_r_u64(r);
    VkFormat fmt   = (VkFormat)cb_r_u32(r);
    VkImageType it = (VkImageType)cb_r_u32(r);
    VkImageTiling tl = (VkImageTiling)cb_r_u32(r);
    VkImageUsageFlags us = (VkImageUsageFlags)cb_r_u32(r);
    VkImageCreateFlags fl = (VkImageCreateFlags)cb_r_u32(r);
    VkPhysicalDevice pd = (VkPhysicalDevice)host_table_get(pid, HK_PHYSICAL_DEVICE);
    host_instance_funcs_t *ifn = host_pd_funcs(pd);
    if (!pd || !ifn) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkImageFormatProperties out;
    VkResult vr = ifn->GetPhysicalDeviceImageFormatProperties(pd, fmt, it, tl, us, fl, &out);
    if (vr != VK_SUCCESS) { host_reply_fail(c, seq, vr); return; }
    host_reply_bytes(c, seq, &out, sizeof out);
}

/* ---- Device ------------------------------------------------------------ */

static void op_create_device(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t pid = cb_r_u64(r);
    VkPhysicalDevice pd = (VkPhysicalDevice)host_table_get(pid, HK_PHYSICAL_DEVICE);
    host_instance_funcs_t *ifn = host_pd_funcs(pd);
    if (!pd || !ifn) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }

    uint32_t qci_count = cb_r_u32(r);
    VkDeviceQueueCreateInfo *qcis =
        (VkDeviceQueueCreateInfo *)calloc(qci_count, sizeof *qcis);
    float **prios = (float **)calloc(qci_count, sizeof *prios);
    for (uint32_t i = 0; i < qci_count; ++i) {
        qcis[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[i].queueFamilyIndex = cb_r_u32(r);
        qcis[i].queueCount       = cb_r_u32(r);
        qcis[i].flags            = cb_r_u32(r);
        prios[i] = (float *)calloc(qcis[i].queueCount, sizeof(float));
        for (uint32_t j = 0; j < qcis[i].queueCount; ++j)
            prios[i][j] = cb_r_f32(r);
        qcis[i].pQueuePriorities = prios[i];
    }
    uint32_t ext_count = cb_r_u32(r);
    char **exts = ext_count ? (char **)calloc(ext_count, sizeof *exts) : NULL;
    for (uint32_t i = 0; i < ext_count; ++i) {
        uint32_t l = 0; const char *p = (const char *)cb_r_blob(r, &l);
        exts[i] = (char *)malloc(l + 1);
        memcpy(exts[i], p, l); exts[i][l] = 0;
    }
    /* The guest may have asked for VK_KHR_swapchain etc. Always make sure
     * VK_KHR_swapchain is present, since most clients want WSI. */
    bool has_swapchain = false;
    for (uint32_t i = 0; i < ext_count; ++i)
        if (!strcmp(exts[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { has_swapchain = true; break; }
    if (!has_swapchain) {
        char **ne = (char **)realloc(exts, (ext_count + 1) * sizeof *exts);
        if (ne) { exts = ne; exts[ext_count++] = strdup(VK_KHR_SWAPCHAIN_EXTENSION_NAME); }
    }

    uint32_t feat_len = 0;
    const VkPhysicalDeviceFeatures *feat =
        (const VkPhysicalDeviceFeatures *)cb_r_opt_blob(r, &feat_len);

    VkDeviceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = qci_count,
        .pQueueCreateInfos       = qcis,
        .enabledExtensionCount   = ext_count,
        .ppEnabledExtensionNames = (const char *const *)exts,
        .pEnabledFeatures        = (feat_len ? feat : NULL),
    };
    VkDevice vkd;
    VkResult vr = ifn->CreateDevice(pd, &ci, NULL, &vkd);
    /* Cleanup */
    for (uint32_t i = 0; i < qci_count; ++i) free(prios[i]);
    free(prios); free(qcis);
    if (vr != VK_SUCCESS) {
        for (uint32_t i = 0; i < ext_count; ++i) free(exts[i]);
        free(exts);
        host_reply_fail(c, seq, vr); return;
    }

    host_device_rec_t *drec = (host_device_rec_t *)calloc(1, sizeof *drec);
    drec->vk = vkd;
    drec->pd = pd;
    /* find owning instance from any cache lookup */
    drec->inst = NULL;  /* not strictly needed */
    /* Use the instance recorded in the function table cache. */
    /* host_load_device_funcs needs an instance to obtain GetDeviceProcAddr. */
    /* We can fetch it from the instance funcs table that owns this PD.     */
    /* Walk g_pd_cache to get the matching ifn, then find the instance      */
    /* via reverse lookup; for simplicity we re-resolve through any         */
    /* registered host_instance_rec_t.                                      */
    {
        host_instance_funcs_t *pdifn = host_pd_funcs(pd);
        if (pdifn) {
            /* The funcs pointer lives inside a host_instance_rec_t. */
            host_instance_rec_t *outer =
                (host_instance_rec_t *)((uint8_t *)pdifn - offsetof(host_instance_rec_t, ifn));
            drec->inst = outer;
            host_load_device_funcs(outer->vk, vkd, &drec->fn);
        }
    }

    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)drec;
    host_table_put(id, HK_HOST_DEVICE, drec, pid);
    g_last_device = drec;
    for (uint32_t i = 0; i < ext_count; ++i) free(exts[i]);
    free(exts);
    host_reply_id(c, seq, id);
}

static void op_destroy_device(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    host_device_rec_t *d = host_device_for(id);
    if (!d) return;
    if (d->fn.DestroyDevice) d->fn.DestroyDevice(d->vk, NULL);
    host_table_drop(id);
    free(d);
}

static void op_get_device_queue(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    uint32_t fam = cb_r_u32(r), idx = cb_r_u32(r);
    VkQueue q;
    d->fn.GetDeviceQueue(d->vk, fam, idx, &q);
    cb_remote_id_t qid = (cb_remote_id_t)(uintptr_t)q;
    host_table_put(qid, HK_QUEUE, q, _devid);
    host_reply_id(c, seq, qid);
}

static void op_device_wait_idle(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkResult vr = d->fn.DeviceWaitIdle(d->vk);
    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}

static void op_queue_wait_idle(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t qid = cb_r_u64(r);
    VkQueue q = (VkQueue)host_table_get(qid, HK_QUEUE);
    if (!q) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    /* Find owning device via... easiest, scan through known. We can't easily
     * walk back the parent. Use vkQueueWaitIdle from any device that owns it.
     * Since QueueWaitIdle resolves through the dispatch table on the queue
     * handle itself in the loader, the function pointer is identical across
     * all devices loaded by the same instance — call any. */
    extern host_device_rec_t *host_any_device_rec(void);
    host_device_rec_t *any = host_any_device_rec();
    if (!any || !any->fn.QueueWaitIdle) { host_reply_ok(c, seq); return; }
    VkResult vr = any->fn.QueueWaitIdle(q);
    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}

/* Tracks the most-recently-created device so QueueWaitIdle has a function
 * pointer to use. (Storage forward-declared above.) */
host_device_rec_t *host_any_device_rec(void) { return g_last_device; }

/* ---- Memory / buffers / images ----------------------------------------- */

static void op_alloc_memory(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkDeviceSize size = cb_r_u64(r);
    uint32_t     ti   = cb_r_u32(r);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = size,
        .memoryTypeIndex = ti,
    };
    VkDeviceMemory m;
    VkResult vr = d->fn.AllocateMemory(d->vk, &ai, NULL, &m);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)m;
    host_table_put(id, HK_DEVICE_MEMORY, (void *)(uintptr_t)m, _devid);
    host_reply_id(c, seq, id);
}

static void op_free_memory(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkDeviceMemory m = (VkDeviceMemory)(uintptr_t)host_table_get(id, HK_DEVICE_MEMORY);
    if (!m) return;
    /* find a device that can free this — use last device */
    host_device_rec_t *d = g_last_device;
    if (d && d->fn.FreeMemory) d->fn.FreeMemory(d->vk, m, NULL);
    host_table_drop(id);
}

static void op_write_memory(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    VkDeviceSize off = cb_r_u64(r);
    uint32_t blob_len = 0;
    const void *src = cb_r_blob(r, &blob_len);
    VkDeviceMemory m = (VkDeviceMemory)(uintptr_t)host_table_get(id, HK_DEVICE_MEMORY);
    host_device_rec_t *d = g_last_device;
    if (!m || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    void *p = NULL;
    VkResult vr = d->fn.MapMemory(d->vk, m, off, blob_len, 0, &p);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    memcpy(p, src, blob_len);
    VkMappedMemoryRange mr = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = m, .offset = off, .size = blob_len,
    };
    d->fn.FlushMappedMemoryRanges(d->vk, 1, &mr);
    d->fn.UnmapMemory(d->vk, m);
    host_reply_ok(c, seq);
}

static void op_read_memory(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    VkDeviceSize off = cb_r_u64(r);
    VkDeviceSize sz  = cb_r_u64(r);
    VkDeviceMemory m = (VkDeviceMemory)(uintptr_t)host_table_get(id, HK_DEVICE_MEMORY);
    host_device_rec_t *d = g_last_device;
    if (!m || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    void *p = NULL;
    VkResult vr = d->fn.MapMemory(d->vk, m, off, sz, 0, &p);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    VkMappedMemoryRange mr = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = m, .offset = off, .size = sz,
    };
    d->fn.InvalidateMappedMemoryRanges(d->vk, 1, &mr);
    host_reply_bytes(c, seq, p, (uint32_t)sz);
    d->fn.UnmapMemory(d->vk, m);
}

static void op_create_buffer(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size  = cb_r_u64(r);
    ci.usage = cb_r_u32(r);
    ci.flags = cb_r_u32(r);
    ci.sharingMode = (VkSharingMode)cb_r_u32(r);
    ci.queueFamilyIndexCount = cb_r_u32(r);
    uint32_t *qfis = ci.queueFamilyIndexCount
        ? (uint32_t *)calloc(ci.queueFamilyIndexCount, sizeof(uint32_t)) : NULL;
    for (uint32_t i = 0; i < ci.queueFamilyIndexCount; ++i) qfis[i] = cb_r_u32(r);
    ci.pQueueFamilyIndices = qfis;
    VkBuffer b;
    VkResult vr = d->fn.CreateBuffer(d->vk, &ci, NULL, &b);
    free(qfis);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)b;
    host_table_put(id, HK_BUFFER, (void *)(uintptr_t)b, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_buffer(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkBuffer b = (VkBuffer)(uintptr_t)host_table_get(id, HK_BUFFER);
    if (!b) return;
    host_device_rec_t *d = g_last_device;
    if (d) d->fn.DestroyBuffer(d->vk, b, NULL);
    host_table_drop(id);
}

static void op_get_buffer_mem_reqs(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    VkBuffer b = (VkBuffer)(uintptr_t)host_table_get(id, HK_BUFFER);
    host_device_rec_t *d = g_last_device;
    if (!b || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkMemoryRequirements mr;
    d->fn.GetBufferMemoryRequirements(d->vk, b, &mr);
    host_reply_bytes(c, seq, &mr, sizeof mr);
}

static void op_bind_buffer_memory(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t bid = cb_r_u64(r);
    cb_remote_id_t mid = cb_r_u64(r);
    VkDeviceSize off = cb_r_u64(r);
    VkBuffer b = (VkBuffer)(uintptr_t)host_table_get(bid, HK_BUFFER);
    VkDeviceMemory m = (VkDeviceMemory)(uintptr_t)host_table_get(mid, HK_DEVICE_MEMORY);
    host_device_rec_t *d = g_last_device;
    if (!b || !m || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkResult vr = d->fn.BindBufferMemory(d->vk, b, m, off);
    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}

/* Image (basic create / destroy / mem reqs / bind) */
static void op_create_image(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.flags     = cb_r_u32(r);
    ci.imageType = (VkImageType)cb_r_u32(r);
    ci.format    = (VkFormat)cb_r_u32(r);
    ci.extent.width  = cb_r_u32(r);
    ci.extent.height = cb_r_u32(r);
    ci.extent.depth  = cb_r_u32(r);
    ci.mipLevels   = cb_r_u32(r);
    ci.arrayLayers = cb_r_u32(r);
    ci.samples     = (VkSampleCountFlagBits)cb_r_u32(r);
    ci.tiling      = (VkImageTiling)cb_r_u32(r);
    ci.usage       = cb_r_u32(r);
    ci.sharingMode = (VkSharingMode)cb_r_u32(r);
    ci.queueFamilyIndexCount = cb_r_u32(r);
    uint32_t *qfis = ci.queueFamilyIndexCount
        ? (uint32_t *)calloc(ci.queueFamilyIndexCount, sizeof(uint32_t)) : NULL;
    for (uint32_t i = 0; i < ci.queueFamilyIndexCount; ++i) qfis[i] = cb_r_u32(r);
    ci.pQueueFamilyIndices = qfis;
    ci.initialLayout = (VkImageLayout)cb_r_u32(r);
    VkImage im;
    VkResult vr = d->fn.CreateImage(d->vk, &ci, NULL, &im);
    free(qfis);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)im;
    host_table_put(id, HK_IMAGE, (void *)(uintptr_t)im, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_image(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkImage im = (VkImage)(uintptr_t)host_table_get(id, HK_IMAGE);
    if (!im) return;
    host_device_rec_t *d = g_last_device;
    if (d) d->fn.DestroyImage(d->vk, im, NULL);
    host_table_drop(id);
}

static void op_get_image_mem_reqs(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    VkImage im = (VkImage)(uintptr_t)host_table_get(id, HK_IMAGE);
    host_device_rec_t *d = g_last_device;
    if (!im || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkMemoryRequirements mr;
    d->fn.GetImageMemoryRequirements(d->vk, im, &mr);
    host_reply_bytes(c, seq, &mr, sizeof mr);
}

static void op_bind_image_memory(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t iid = cb_r_u64(r);
    cb_remote_id_t mid = cb_r_u64(r);
    VkDeviceSize off = cb_r_u64(r);
    VkImage im = (VkImage)(uintptr_t)host_table_get(iid, HK_IMAGE);
    VkDeviceMemory m = (VkDeviceMemory)(uintptr_t)host_table_get(mid, HK_DEVICE_MEMORY);
    host_device_rec_t *d = g_last_device;
    if (!im || !m || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkResult vr = d->fn.BindImageMemory(d->vk, im, m, off);
    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}

/* ---- Shader module ----------------------------------------------------- */

static void op_create_shader_module(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    uint32_t code_len = 0;
    const void *code = cb_r_blob(r, &code_len);
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code_len,
        .pCode = (const uint32_t *)code,
    };
    VkShaderModule m;
    VkResult vr = d->fn.CreateShaderModule(d->vk, &ci, NULL, &m);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)m;
    host_table_put(id, HK_SHADER_MODULE, (void *)(uintptr_t)m, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_shader_module(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkShaderModule m = (VkShaderModule)(uintptr_t)host_table_get(id, HK_SHADER_MODULE);
    host_device_rec_t *d = g_last_device;
    if (m && d) d->fn.DestroyShaderModule(d->vk, m, NULL);
    host_table_drop(id);
}

/* ---- Command pool / buffer / submit ------------------------------------ */

static void op_create_command_pool(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkCommandPoolCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = cb_r_u32(r),
        .queueFamilyIndex = cb_r_u32(r),
    };
    VkCommandPool p;
    VkResult vr = d->fn.CreateCommandPool(d->vk, &ci, NULL, &p);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)p;
    host_table_put(id, HK_COMMAND_POOL, (void *)(uintptr_t)p, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_command_pool(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkCommandPool p = (VkCommandPool)(uintptr_t)host_table_get(id, HK_COMMAND_POOL);
    host_device_rec_t *d = g_last_device;
    if (p && d) d->fn.DestroyCommandPool(d->vk, p, NULL);
    host_table_drop(id);
}

static void op_alloc_command_buffers(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    cb_remote_id_t pid = cb_r_u64(r);
    VkCommandPool pool = (VkCommandPool)(uintptr_t)host_table_get(pid, HK_COMMAND_POOL);
    if (!pool) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    uint32_t lvl = cb_r_u32(r);
    uint32_t cnt = cb_r_u32(r);
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = (VkCommandBufferLevel)lvl,
        .commandBufferCount = cnt,
    };
    VkCommandBuffer *bufs = (VkCommandBuffer *)calloc(cnt, sizeof *bufs);
    VkResult vr = d->fn.AllocateCommandBuffers(d->vk, &ai, bufs);
    if (vr) { free(bufs); host_reply_fail(c, seq, vr); return; }
    cb_writer_t w; cb_writer_init_heap(&w, cnt * 8);
    for (uint32_t i = 0; i < cnt; ++i) {
        cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)bufs[i];
        host_table_put(id, HK_COMMAND_BUFFER, (void *)bufs[i], _devid);
        cb_w_u64(&w, id);
    }
    free(bufs);
    host_reply_writer(c, seq, &w);
    cb_writer_dispose(&w);
}

static void op_begin_command_buffer(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    uint32_t flags = cb_r_u32(r);
    VkCommandBuffer b = (VkCommandBuffer)host_table_get(id, HK_COMMAND_BUFFER);
    host_device_rec_t *d = g_last_device;
    if (!b || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = flags,
    };
    VkResult vr = d->fn.BeginCommandBuffer(b, &bi);
    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}

static void op_end_command_buffer(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    VkCommandBuffer b = (VkCommandBuffer)host_table_get(id, HK_COMMAND_BUFFER);
    host_device_rec_t *d = g_last_device;
    if (!b || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkResult vr = d->fn.EndCommandBuffer(b);
    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}

static void op_record_command_stream(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    uint32_t blen = 0;
    const void *bytes = cb_r_blob(r, &blen);
    VkCommandBuffer b = (VkCommandBuffer)host_table_get(id, HK_COMMAND_BUFFER);
    host_device_rec_t *d = g_last_device;
    if (!b || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkResult vr = host_replay_command_stream(d, b, bytes, blen);
    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}

static void op_queue_submit(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t qid = cb_r_u64(r);
    cb_remote_id_t fid = cb_r_u64(r);
    uint32_t cnt = cb_r_u32(r);
    VkQueue q = (VkQueue)host_table_get(qid, HK_QUEUE);
    VkFence f = fid ? (VkFence)(uintptr_t)host_table_get(fid, HK_FENCE) : VK_NULL_HANDLE;
    if (!q) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }

    VkSubmitInfo *sis = (VkSubmitInfo *)calloc(cnt, sizeof *sis);
    /* dynamic side arrays */
    VkSemaphore         **wsem  = (VkSemaphore **)calloc(cnt, sizeof *wsem);
    VkPipelineStageFlags**wstg  = (VkPipelineStageFlags **)calloc(cnt, sizeof *wstg);
    VkCommandBuffer     **bufs  = (VkCommandBuffer **)calloc(cnt, sizeof *bufs);
    VkSemaphore         **ssem  = (VkSemaphore **)calloc(cnt, sizeof *ssem);

    for (uint32_t i = 0; i < cnt; ++i) {
        sis[i].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        sis[i].waitSemaphoreCount = cb_r_u32(r);
        wsem[i] = (VkSemaphore *)calloc(sis[i].waitSemaphoreCount, sizeof(VkSemaphore));
        wstg[i] = (VkPipelineStageFlags *)calloc(sis[i].waitSemaphoreCount, sizeof(VkPipelineStageFlags));
        for (uint32_t j = 0; j < sis[i].waitSemaphoreCount; ++j) {
            cb_remote_id_t sid = cb_r_u64(r);
            wsem[i][j] = (VkSemaphore)(uintptr_t)host_table_get(sid, HK_SEMAPHORE);
            wstg[i][j] = cb_r_u32(r);
        }
        sis[i].pWaitSemaphores = wsem[i];
        sis[i].pWaitDstStageMask = wstg[i];

        sis[i].commandBufferCount = cb_r_u32(r);
        bufs[i] = (VkCommandBuffer *)calloc(sis[i].commandBufferCount, sizeof(VkCommandBuffer));
        for (uint32_t j = 0; j < sis[i].commandBufferCount; ++j) {
            cb_remote_id_t bid = cb_r_u64(r);
            bufs[i][j] = (VkCommandBuffer)host_table_get(bid, HK_COMMAND_BUFFER);
        }
        sis[i].pCommandBuffers = bufs[i];

        sis[i].signalSemaphoreCount = cb_r_u32(r);
        ssem[i] = (VkSemaphore *)calloc(sis[i].signalSemaphoreCount, sizeof(VkSemaphore));
        for (uint32_t j = 0; j < sis[i].signalSemaphoreCount; ++j) {
            cb_remote_id_t sid = cb_r_u64(r);
            ssem[i][j] = (VkSemaphore)(uintptr_t)host_table_get(sid, HK_SEMAPHORE);
        }
        sis[i].pSignalSemaphores = ssem[i];
    }

    host_device_rec_t *d = g_last_device;
    VkResult vr = d ? d->fn.QueueSubmit(q, cnt, sis, f) : VK_ERROR_DEVICE_LOST;

    for (uint32_t i = 0; i < cnt; ++i) {
        free(wsem[i]); free(wstg[i]); free(bufs[i]); free(ssem[i]);
    }
    free(wsem); free(wstg); free(bufs); free(ssem); free(sis);

    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}

/* ---- Sync -------------------------------------------------------------- */

static void op_create_fence(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkFenceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                             .flags = cb_r_u32(r) };
    VkFence h; VkResult vr = d->fn.CreateFence(d->vk, &ci, NULL, &h);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)h;
    host_table_put(id, HK_FENCE, (void *)(uintptr_t)h, _devid);
    host_reply_id(c, seq, id);
}
static void op_destroy_fence(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkFence h = (VkFence)(uintptr_t)host_table_get(id, HK_FENCE);
    host_device_rec_t *d = g_last_device;
    if (h && d) d->fn.DestroyFence(d->vk, h, NULL);
    host_table_drop(id);
}
static void op_wait_for_fences(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    uint32_t n = cb_r_u32(r);
    VkFence *arr = (VkFence *)calloc(n, sizeof *arr);
    for (uint32_t i = 0; i < n; ++i) {
        cb_remote_id_t fid = cb_r_u64(r);
        arr[i] = (VkFence)(uintptr_t)host_table_get(fid, HK_FENCE);
    }
    VkBool32 wait_all = cb_r_u32(r);
    uint64_t timeout  = cb_r_u64(r);
    host_device_rec_t *d = g_last_device;
    VkResult vr = d ? d->fn.WaitForFences(d->vk, n, arr, wait_all, timeout)
                    : VK_ERROR_DEVICE_LOST;
    free(arr);
    if (vr == VK_SUCCESS || vr == VK_TIMEOUT) host_reply_ok(c, seq);
    else host_reply_fail(c, seq, vr);
}
static void op_reset_fences(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    uint32_t n = cb_r_u32(r);
    VkFence *arr = (VkFence *)calloc(n, sizeof *arr);
    for (uint32_t i = 0; i < n; ++i) {
        cb_remote_id_t fid = cb_r_u64(r);
        arr[i] = (VkFence)(uintptr_t)host_table_get(fid, HK_FENCE);
    }
    host_device_rec_t *d = g_last_device;
    VkResult vr = d ? d->fn.ResetFences(d->vk, n, arr) : VK_ERROR_DEVICE_LOST;
    free(arr);
    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}
static void op_create_semaphore(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkSemaphoreCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                 .flags = cb_r_u32(r) };
    VkSemaphore h; VkResult vr = d->fn.CreateSemaphore(d->vk, &ci, NULL, &h);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)h;
    host_table_put(id, HK_SEMAPHORE, (void *)(uintptr_t)h, _devid);
    host_reply_id(c, seq, id);
}
static void op_destroy_semaphore(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkSemaphore h = (VkSemaphore)(uintptr_t)host_table_get(id, HK_SEMAPHORE);
    host_device_rec_t *d = g_last_device;
    if (h && d) d->fn.DestroySemaphore(d->vk, h, NULL);
    host_table_drop(id);
}

/* ---- Surface / swapchain ---------------------------------------------- */

static void op_create_surface(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    /*
     * Payload: u64 instance_id, u32 width, u32 height.
     * Allocate a CheeseBridge window (Cocoa NSWindow + CAMetalLayer),
     * wrap its layer with vkCreateMetalSurfaceEXT, register the surface.
     */
    cb_remote_id_t iid = cb_r_u64(r);
    uint32_t w = cb_r_u32(r), h = cb_r_u32(r);
    host_instance_rec_t *inst = host_instance_for(iid);
    if (!inst || !inst->ifn.CreateMetalSurfaceEXT) {
        host_reply_fail(c, seq, VK_ERROR_EXTENSION_NOT_PRESENT); return;
    }
    NSWindow *nw = host_create_window(w ? w : 1280, h ? h : 720);
    CAMetalLayer *layer = host_window_layer(nw);

    VkMetalSurfaceCreateInfoEXT mi = {
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pLayer = layer,
    };
    VkSurfaceKHR surf;
    VkResult vr = inst->ifn.CreateMetalSurfaceEXT(inst->vk, &mi, NULL, &surf);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)surf;
    host_table_put(id, HK_SURFACE, (void *)(uintptr_t)surf, iid);
    host_reply_id(c, seq, id);
}

/* ---- Image view / sampler --------------------------------------------- */

static void op_create_image_view(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    cb_remote_id_t img_id = cb_r_u64(r);
    VkImage img = (VkImage)(uintptr_t)host_table_get(img_id, HK_IMAGE);
    if (!img) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkImageViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ci.image    = img;
    ci.flags    = cb_r_u32(r);
    ci.viewType = (VkImageViewType)cb_r_u32(r);
    ci.format   = (VkFormat)cb_r_u32(r);
    ci.components.r = (VkComponentSwizzle)cb_r_u32(r);
    ci.components.g = (VkComponentSwizzle)cb_r_u32(r);
    ci.components.b = (VkComponentSwizzle)cb_r_u32(r);
    ci.components.a = (VkComponentSwizzle)cb_r_u32(r);
    ci.subresourceRange.aspectMask     = cb_r_u32(r);
    ci.subresourceRange.baseMipLevel   = cb_r_u32(r);
    ci.subresourceRange.levelCount     = cb_r_u32(r);
    ci.subresourceRange.baseArrayLayer = cb_r_u32(r);
    ci.subresourceRange.layerCount     = cb_r_u32(r);
    VkImageView iv;
    VkResult vr = d->fn.CreateImageView(d->vk, &ci, NULL, &iv);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)iv;
    host_table_put(id, HK_IMAGE_VIEW, (void *)(uintptr_t)iv, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_image_view(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkImageView iv = (VkImageView)(uintptr_t)host_table_get(id, HK_IMAGE_VIEW);
    host_device_rec_t *d = g_last_device;
    if (iv && d) d->fn.DestroyImageView(d->vk, iv, NULL);
    host_table_drop(id);
}

static void op_create_sampler(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.flags        = cb_r_u32(r);
    ci.magFilter    = (VkFilter)cb_r_u32(r);
    ci.minFilter    = (VkFilter)cb_r_u32(r);
    ci.mipmapMode   = (VkSamplerMipmapMode)cb_r_u32(r);
    ci.addressModeU = (VkSamplerAddressMode)cb_r_u32(r);
    ci.addressModeV = (VkSamplerAddressMode)cb_r_u32(r);
    ci.addressModeW = (VkSamplerAddressMode)cb_r_u32(r);
    ci.mipLodBias   = cb_r_f32(r);
    ci.anisotropyEnable = cb_r_u32(r);
    ci.maxAnisotropy    = cb_r_f32(r);
    ci.compareEnable    = cb_r_u32(r);
    ci.compareOp        = (VkCompareOp)cb_r_u32(r);
    ci.minLod           = cb_r_f32(r);
    ci.maxLod           = cb_r_f32(r);
    ci.borderColor      = (VkBorderColor)cb_r_u32(r);
    ci.unnormalizedCoordinates = cb_r_u32(r);
    VkSampler s;
    VkResult vr = d->fn.CreateSampler(d->vk, &ci, NULL, &s);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)s;
    host_table_put(id, HK_SAMPLER, (void *)(uintptr_t)s, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_sampler(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkSampler s = (VkSampler)(uintptr_t)host_table_get(id, HK_SAMPLER);
    host_device_rec_t *d = g_last_device;
    if (s && d) d->fn.DestroySampler(d->vk, s, NULL);
    host_table_drop(id);
}

/* ---- Pipeline cache --------------------------------------------------- */

static void op_create_pipeline_cache(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    uint32_t flags = cb_r_u32(r);
    uint32_t blen = 0;
    const void *blob = cb_r_blob(r, &blen);
    VkPipelineCacheCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags = flags,
        .initialDataSize = blen,
        .pInitialData = blob,
    };
    VkPipelineCache pc;
    VkResult vr = d->fn.CreatePipelineCache(d->vk, &ci, NULL, &pc);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)pc;
    host_table_put(id, HK_PIPELINE_CACHE, (void *)(uintptr_t)pc, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_pipeline_cache(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkPipelineCache pc = (VkPipelineCache)(uintptr_t)host_table_get(id, HK_PIPELINE_CACHE);
    host_device_rec_t *d = g_last_device;
    if (pc && d) d->fn.DestroyPipelineCache(d->vk, pc, NULL);
    host_table_drop(id);
}

/* ---- Pipeline layout -------------------------------------------------- */

static void op_create_pipeline_layout(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    uint32_t set_count = cb_r_u32(r);
    VkDescriptorSetLayout *dsls = set_count
        ? (VkDescriptorSetLayout *)calloc(set_count, sizeof *dsls) : NULL;
    for (uint32_t i = 0; i < set_count; ++i) {
        cb_remote_id_t lid = cb_r_u64(r);
        dsls[i] = (VkDescriptorSetLayout)(uintptr_t)host_table_get(lid, HK_DESC_SET_LAYOUT);
    }
    uint32_t pcr_count = cb_r_u32(r);
    VkPushConstantRange *pcr = pcr_count
        ? (VkPushConstantRange *)calloc(pcr_count, sizeof *pcr) : NULL;
    for (uint32_t i = 0; i < pcr_count; ++i) {
        pcr[i].stageFlags = cb_r_u32(r);
        pcr[i].offset     = cb_r_u32(r);
        pcr[i].size       = cb_r_u32(r);
    }
    VkPipelineLayoutCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = set_count,
        .pSetLayouts    = dsls,
        .pushConstantRangeCount = pcr_count,
        .pPushConstantRanges    = pcr,
    };
    VkPipelineLayout pl;
    VkResult vr = d->fn.CreatePipelineLayout(d->vk, &ci, NULL, &pl);
    free(dsls); free(pcr);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)pl;
    host_table_put(id, HK_PIPELINE_LAYOUT, (void *)(uintptr_t)pl, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_pipeline_layout(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkPipelineLayout pl = (VkPipelineLayout)(uintptr_t)host_table_get(id, HK_PIPELINE_LAYOUT);
    host_device_rec_t *d = g_last_device;
    if (pl && d) d->fn.DestroyPipelineLayout(d->vk, pl, NULL);
    host_table_drop(id);
}

/* ---- Render pass / framebuffer ---------------------------------------- */

static void op_create_render_pass(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.flags = cb_r_u32(r);
    ci.attachmentCount = cb_r_u32(r);
    VkAttachmentDescription *atts = ci.attachmentCount
        ? (VkAttachmentDescription *)calloc(ci.attachmentCount, sizeof *atts) : NULL;
    for (uint32_t i = 0; i < ci.attachmentCount; ++i) {
        atts[i].flags          = cb_r_u32(r);
        atts[i].format         = (VkFormat)cb_r_u32(r);
        atts[i].samples        = (VkSampleCountFlagBits)cb_r_u32(r);
        atts[i].loadOp         = (VkAttachmentLoadOp)cb_r_u32(r);
        atts[i].storeOp        = (VkAttachmentStoreOp)cb_r_u32(r);
        atts[i].stencilLoadOp  = (VkAttachmentLoadOp)cb_r_u32(r);
        atts[i].stencilStoreOp = (VkAttachmentStoreOp)cb_r_u32(r);
        atts[i].initialLayout  = (VkImageLayout)cb_r_u32(r);
        atts[i].finalLayout    = (VkImageLayout)cb_r_u32(r);
    }
    ci.pAttachments = atts;

    ci.subpassCount = cb_r_u32(r);
    VkSubpassDescription *subs = ci.subpassCount
        ? (VkSubpassDescription *)calloc(ci.subpassCount, sizeof *subs) : NULL;
    /* per-subpass attachment arrays — track for free() after CreateRenderPass */
    VkAttachmentReference **inp_refs = (VkAttachmentReference **)calloc(ci.subpassCount, sizeof *inp_refs);
    VkAttachmentReference **col_refs = (VkAttachmentReference **)calloc(ci.subpassCount, sizeof *col_refs);
    VkAttachmentReference **res_refs = (VkAttachmentReference **)calloc(ci.subpassCount, sizeof *res_refs);
    VkAttachmentReference **dep_refs = (VkAttachmentReference **)calloc(ci.subpassCount, sizeof *dep_refs);
    uint32_t **pres_arr = (uint32_t **)calloc(ci.subpassCount, sizeof *pres_arr);

    for (uint32_t i = 0; i < ci.subpassCount; ++i) {
        subs[i].flags             = cb_r_u32(r);
        subs[i].pipelineBindPoint = (VkPipelineBindPoint)cb_r_u32(r);
        subs[i].inputAttachmentCount = cb_r_u32(r);
        if (subs[i].inputAttachmentCount) {
            inp_refs[i] = (VkAttachmentReference *)calloc(subs[i].inputAttachmentCount, sizeof(VkAttachmentReference));
            for (uint32_t j = 0; j < subs[i].inputAttachmentCount; ++j) {
                inp_refs[i][j].attachment = cb_r_u32(r);
                inp_refs[i][j].layout     = (VkImageLayout)cb_r_u32(r);
            }
            subs[i].pInputAttachments = inp_refs[i];
        }
        subs[i].colorAttachmentCount = cb_r_u32(r);
        if (subs[i].colorAttachmentCount) {
            col_refs[i] = (VkAttachmentReference *)calloc(subs[i].colorAttachmentCount, sizeof(VkAttachmentReference));
            res_refs[i] = (VkAttachmentReference *)calloc(subs[i].colorAttachmentCount, sizeof(VkAttachmentReference));
            bool any_resolve = false;
            for (uint32_t j = 0; j < subs[i].colorAttachmentCount; ++j) {
                col_refs[i][j].attachment = cb_r_u32(r);
                col_refs[i][j].layout     = (VkImageLayout)cb_r_u32(r);
                res_refs[i][j].attachment = cb_r_u32(r);
                res_refs[i][j].layout     = (VkImageLayout)cb_r_u32(r);
                if (res_refs[i][j].attachment != VK_ATTACHMENT_UNUSED) any_resolve = true;
            }
            subs[i].pColorAttachments   = col_refs[i];
            subs[i].pResolveAttachments = any_resolve ? res_refs[i] : NULL;
        }
        /* depth/stencil: always sent on the wire, but use NULL when UNUSED */
        uint32_t dsa = cb_r_u32(r);
        uint32_t dsl = cb_r_u32(r);
        if (dsa != VK_ATTACHMENT_UNUSED) {
            dep_refs[i] = (VkAttachmentReference *)calloc(1, sizeof(VkAttachmentReference));
            dep_refs[i]->attachment = dsa;
            dep_refs[i]->layout     = (VkImageLayout)dsl;
            subs[i].pDepthStencilAttachment = dep_refs[i];
        }
        subs[i].preserveAttachmentCount = cb_r_u32(r);
        if (subs[i].preserveAttachmentCount) {
            pres_arr[i] = (uint32_t *)calloc(subs[i].preserveAttachmentCount, sizeof(uint32_t));
            for (uint32_t j = 0; j < subs[i].preserveAttachmentCount; ++j)
                pres_arr[i][j] = cb_r_u32(r);
            subs[i].pPreserveAttachments = pres_arr[i];
        }
    }
    ci.pSubpasses = subs;

    ci.dependencyCount = cb_r_u32(r);
    VkSubpassDependency *deps = ci.dependencyCount
        ? (VkSubpassDependency *)calloc(ci.dependencyCount, sizeof *deps) : NULL;
    for (uint32_t i = 0; i < ci.dependencyCount; ++i) {
        deps[i].srcSubpass      = cb_r_u32(r);
        deps[i].dstSubpass      = cb_r_u32(r);
        deps[i].srcStageMask    = cb_r_u32(r);
        deps[i].dstStageMask    = cb_r_u32(r);
        deps[i].srcAccessMask   = cb_r_u32(r);
        deps[i].dstAccessMask   = cb_r_u32(r);
        deps[i].dependencyFlags = cb_r_u32(r);
    }
    ci.pDependencies = deps;

    VkRenderPass rp;
    VkResult vr = d->fn.CreateRenderPass(d->vk, &ci, NULL, &rp);

    for (uint32_t i = 0; i < ci.subpassCount; ++i) {
        free(inp_refs[i]); free(col_refs[i]);
        free(res_refs[i]); free(dep_refs[i]); free(pres_arr[i]);
    }
    free(inp_refs); free(col_refs); free(res_refs); free(dep_refs); free(pres_arr);
    free(atts); free(subs); free(deps);

    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)rp;
    host_table_put(id, HK_RENDER_PASS, (void *)(uintptr_t)rp, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_render_pass(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkRenderPass rp = (VkRenderPass)(uintptr_t)host_table_get(id, HK_RENDER_PASS);
    host_device_rec_t *d = g_last_device;
    if (rp && d) d->fn.DestroyRenderPass(d->vk, rp, NULL);
    host_table_drop(id);
}

static void op_create_framebuffer(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    cb_remote_id_t rp_id = cb_r_u64(r);
    VkRenderPass rp = (VkRenderPass)(uintptr_t)host_table_get(rp_id, HK_RENDER_PASS);
    VkFramebufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    ci.renderPass = rp;
    ci.flags = cb_r_u32(r);
    ci.attachmentCount = cb_r_u32(r);
    VkImageView *views = ci.attachmentCount
        ? (VkImageView *)calloc(ci.attachmentCount, sizeof *views) : NULL;
    for (uint32_t i = 0; i < ci.attachmentCount; ++i) {
        cb_remote_id_t ivid = cb_r_u64(r);
        views[i] = (VkImageView)(uintptr_t)host_table_get(ivid, HK_IMAGE_VIEW);
    }
    ci.pAttachments = views;
    ci.width  = cb_r_u32(r);
    ci.height = cb_r_u32(r);
    ci.layers = cb_r_u32(r);
    VkFramebuffer fb;
    VkResult vr = d->fn.CreateFramebuffer(d->vk, &ci, NULL, &fb);
    free(views);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)fb;
    host_table_put(id, HK_FRAMEBUFFER, (void *)(uintptr_t)fb, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_framebuffer(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkFramebuffer fb = (VkFramebuffer)(uintptr_t)host_table_get(id, HK_FRAMEBUFFER);
    host_device_rec_t *d = g_last_device;
    if (fb && d) d->fn.DestroyFramebuffer(d->vk, fb, NULL);
    host_table_drop(id);
}

/* ---- Descriptor set layout / pool / sets ----------------------------- */

static void op_create_dsl(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkDescriptorSetLayoutCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    };
    ci.flags        = cb_r_u32(r);
    ci.bindingCount = cb_r_u32(r);
    VkDescriptorSetLayoutBinding *bnd = ci.bindingCount
        ? (VkDescriptorSetLayoutBinding *)calloc(ci.bindingCount, sizeof *bnd) : NULL;
    /* immutable-sampler arrays per binding */
    VkSampler **immss = (VkSampler **)calloc(ci.bindingCount, sizeof *immss);
    for (uint32_t i = 0; i < ci.bindingCount; ++i) {
        bnd[i].binding         = cb_r_u32(r);
        bnd[i].descriptorType  = (VkDescriptorType)cb_r_u32(r);
        bnd[i].descriptorCount = cb_r_u32(r);
        bnd[i].stageFlags      = cb_r_u32(r);
        uint32_t imm = cb_r_u32(r);
        if (imm) {
            immss[i] = (VkSampler *)calloc(imm, sizeof(VkSampler));
            for (uint32_t j = 0; j < imm; ++j) {
                cb_remote_id_t sid = cb_r_u64(r);
                immss[i][j] = (VkSampler)(uintptr_t)host_table_get(sid, HK_SAMPLER);
            }
            bnd[i].pImmutableSamplers = immss[i];
        }
    }
    ci.pBindings = bnd;

    VkDescriptorSetLayout dsl;
    VkResult vr = d->fn.CreateDescriptorSetLayout(d->vk, &ci, NULL, &dsl);
    for (uint32_t i = 0; i < ci.bindingCount; ++i) free(immss[i]);
    free(immss); free(bnd);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)dsl;
    host_table_put(id, HK_DESC_SET_LAYOUT, (void *)(uintptr_t)dsl, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_dsl(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkDescriptorSetLayout dsl =
        (VkDescriptorSetLayout)(uintptr_t)host_table_get(id, HK_DESC_SET_LAYOUT);
    host_device_rec_t *d = g_last_device;
    if (dsl && d) d->fn.DestroyDescriptorSetLayout(d->vk, dsl, NULL);
    host_table_drop(id);
}

static void op_create_desc_pool(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkDescriptorPoolCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    };
    ci.flags         = cb_r_u32(r);
    ci.maxSets       = cb_r_u32(r);
    ci.poolSizeCount = cb_r_u32(r);
    /* MoltenVK requires FREE_DESCRIPTOR_SET_BIT when guests use FreeDescriptorSets. */
    ci.flags |= VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    VkDescriptorPoolSize *ps = ci.poolSizeCount
        ? (VkDescriptorPoolSize *)calloc(ci.poolSizeCount, sizeof *ps) : NULL;
    for (uint32_t i = 0; i < ci.poolSizeCount; ++i) {
        ps[i].type            = (VkDescriptorType)cb_r_u32(r);
        ps[i].descriptorCount = cb_r_u32(r);
    }
    ci.pPoolSizes = ps;
    VkDescriptorPool pool;
    VkResult vr = d->fn.CreateDescriptorPool(d->vk, &ci, NULL, &pool);
    free(ps);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)pool;
    host_table_put(id, HK_DESC_POOL, (void *)(uintptr_t)pool, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_desc_pool(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkDescriptorPool p = (VkDescriptorPool)(uintptr_t)host_table_get(id, HK_DESC_POOL);
    host_device_rec_t *d = g_last_device;
    if (p && d) d->fn.DestroyDescriptorPool(d->vk, p, NULL);
    host_table_drop(id);
}

static void op_alloc_desc_sets(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    cb_remote_id_t pool_id = cb_r_u64(r);
    VkDescriptorPool pool = (VkDescriptorPool)(uintptr_t)host_table_get(pool_id, HK_DESC_POOL);
    if (!pool) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    uint32_t cnt = cb_r_u32(r);
    VkDescriptorSetLayout *layouts = cnt
        ? (VkDescriptorSetLayout *)calloc(cnt, sizeof *layouts) : NULL;
    for (uint32_t i = 0; i < cnt; ++i) {
        cb_remote_id_t lid = cb_r_u64(r);
        layouts[i] = (VkDescriptorSetLayout)(uintptr_t)host_table_get(lid, HK_DESC_SET_LAYOUT);
    }
    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = pool,
        .descriptorSetCount = cnt,
        .pSetLayouts        = layouts,
    };
    VkDescriptorSet *sets = (VkDescriptorSet *)calloc(cnt, sizeof *sets);
    VkResult vr = d->fn.AllocateDescriptorSets(d->vk, &ai, sets);
    free(layouts);
    if (vr) { free(sets); host_reply_fail(c, seq, vr); return; }
    cb_writer_t w; cb_writer_init_heap(&w, cnt * 8);
    for (uint32_t i = 0; i < cnt; ++i) {
        cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)sets[i];
        host_table_put(id, HK_DESC_SET, (void *)(uintptr_t)sets[i], pool_id);
        cb_w_u64(&w, id);
    }
    free(sets);
    host_reply_writer(c, seq, &w);
    cb_writer_dispose(&w);
}

static void op_free_desc_sets(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    cb_remote_id_t pool_id = cb_r_u64(r);
    VkDescriptorPool pool = (VkDescriptorPool)(uintptr_t)host_table_get(pool_id, HK_DESC_POOL);
    uint32_t cnt = cb_r_u32(r);
    VkDescriptorSet *sets = cnt
        ? (VkDescriptorSet *)calloc(cnt, sizeof *sets) : NULL;
    cb_remote_id_t *ids = cnt
        ? (cb_remote_id_t *)calloc(cnt, sizeof *ids) : NULL;
    for (uint32_t i = 0; i < cnt; ++i) {
        ids[i] = cb_r_u64(r);
        sets[i] = (VkDescriptorSet)(uintptr_t)host_table_get(ids[i], HK_DESC_SET);
    }
    VkResult vr = pool ? d->fn.FreeDescriptorSets(d->vk, pool, cnt, sets) : VK_SUCCESS;
    for (uint32_t i = 0; i < cnt; ++i) host_table_drop(ids[i]);
    free(sets); free(ids);
    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}

static void op_update_desc_sets(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    READ_DEV(r, d);
    uint32_t writeCount = cb_r_u32(r);
    VkWriteDescriptorSet *writes = writeCount
        ? (VkWriteDescriptorSet *)calloc(writeCount, sizeof *writes) : NULL;
    /* per-write side arrays */
    VkDescriptorImageInfo  **imgs = (VkDescriptorImageInfo  **)calloc(writeCount, sizeof *imgs);
    VkDescriptorBufferInfo **bufs = (VkDescriptorBufferInfo **)calloc(writeCount, sizeof *bufs);
    VkBufferView           **bvs  = (VkBufferView           **)calloc(writeCount, sizeof *bvs);

    for (uint32_t i = 0; i < writeCount; ++i) {
        cb_remote_id_t dst_id = cb_r_u64(r);
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = (VkDescriptorSet)(uintptr_t)host_table_get(dst_id, HK_DESC_SET);
        writes[i].dstBinding      = cb_r_u32(r);
        writes[i].dstArrayElement = cb_r_u32(r);
        writes[i].descriptorCount = cb_r_u32(r);
        writes[i].descriptorType  = (VkDescriptorType)cb_r_u32(r);
        uint32_t n = writes[i].descriptorCount;
        switch (writes[i].descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
            imgs[i] = (VkDescriptorImageInfo *)calloc(n, sizeof(VkDescriptorImageInfo));
            for (uint32_t j = 0; j < n; ++j) {
                cb_remote_id_t sid = cb_r_u64(r);
                cb_remote_id_t vid = cb_r_u64(r);
                imgs[i][j].sampler     = sid ? (VkSampler)(uintptr_t)host_table_get(sid, HK_SAMPLER) : VK_NULL_HANDLE;
                imgs[i][j].imageView   = vid ? (VkImageView)(uintptr_t)host_table_get(vid, HK_IMAGE_VIEW) : VK_NULL_HANDLE;
                imgs[i][j].imageLayout = (VkImageLayout)cb_r_u32(r);
            }
            writes[i].pImageInfo = imgs[i];
        } break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
            bvs[i] = (VkBufferView *)calloc(n, sizeof(VkBufferView));
            for (uint32_t j = 0; j < n; ++j) {
                cb_remote_id_t bvid = cb_r_u64(r);
                bvs[i][j] = bvid ? (VkBufferView)(uintptr_t)host_table_get(bvid, HK_BUFFER_VIEW) : VK_NULL_HANDLE;
            }
            writes[i].pTexelBufferView = bvs[i];
        } break;
        default: {
            bufs[i] = (VkDescriptorBufferInfo *)calloc(n, sizeof(VkDescriptorBufferInfo));
            for (uint32_t j = 0; j < n; ++j) {
                cb_remote_id_t bid = cb_r_u64(r);
                bufs[i][j].buffer = bid ? (VkBuffer)(uintptr_t)host_table_get(bid, HK_BUFFER) : VK_NULL_HANDLE;
                bufs[i][j].offset = cb_r_u64(r);
                bufs[i][j].range  = cb_r_u64(r);
            }
            writes[i].pBufferInfo = bufs[i];
        } break;
        }
    }
    uint32_t copyCount = cb_r_u32(r);
    VkCopyDescriptorSet *copies = copyCount
        ? (VkCopyDescriptorSet *)calloc(copyCount, sizeof *copies) : NULL;
    for (uint32_t i = 0; i < copyCount; ++i) {
        cb_remote_id_t src_id = cb_r_u64(r);
        copies[i].sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
        copies[i].srcSet         = (VkDescriptorSet)(uintptr_t)host_table_get(src_id, HK_DESC_SET);
        copies[i].srcBinding     = cb_r_u32(r);
        copies[i].srcArrayElement = cb_r_u32(r);
        cb_remote_id_t dst_id = cb_r_u64(r);
        copies[i].dstSet         = (VkDescriptorSet)(uintptr_t)host_table_get(dst_id, HK_DESC_SET);
        copies[i].dstBinding     = cb_r_u32(r);
        copies[i].dstArrayElement = cb_r_u32(r);
        copies[i].descriptorCount = cb_r_u32(r);
    }
    d->fn.UpdateDescriptorSets(d->vk, writeCount, writes, copyCount, copies);
    for (uint32_t i = 0; i < writeCount; ++i) {
        free(imgs[i]); free(bufs[i]); free(bvs[i]);
    }
    free(imgs); free(bufs); free(bvs); free(writes); free(copies);
}

/* ---- Graphics / compute pipelines ------------------------------------- */

/* Helper: parse one shader stage off the wire. The caller owns the
 * returned `name` string (must free()) and the optional spec data buffer
 * (also free()). Output entries are owned by the caller in the same way. */
typedef struct cb_stage_scratch {
    char *name;
    VkSpecializationInfo  spec;
    VkSpecializationMapEntry *spec_entries;
    void *spec_data;
} cb_stage_scratch_t;

static void cb_parse_shader_stage(cb_reader_t *r,
                                  VkPipelineShaderStageCreateInfo *out,
                                  cb_stage_scratch_t *scratch) {
    memset(out, 0, sizeof *out);
    memset(scratch, 0, sizeof *scratch);
    out->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    out->flags = cb_r_u32(r);
    out->stage = (VkShaderStageFlagBits)cb_r_u32(r);
    cb_remote_id_t mid = cb_r_u64(r);
    out->module = mid ? (VkShaderModule)(uintptr_t)host_table_get(mid, HK_SHADER_MODULE) : VK_NULL_HANDLE;
    uint32_t nlen = 0;
    const char *nptr = (const char *)cb_r_blob(r, &nlen);
    /* the wire blob is not null-terminated — copy into a fresh C string */
    scratch->name = (char *)malloc(nlen + 1);
    if (nlen) memcpy(scratch->name, nptr, nlen);
    scratch->name[nlen] = 0;
    out->pName = scratch->name;
    /* specialization */
    uint32_t map_count = cb_r_u32(r);
    if (map_count) {
        scratch->spec_entries = (VkSpecializationMapEntry *)calloc(map_count, sizeof(VkSpecializationMapEntry));
        for (uint32_t i = 0; i < map_count; ++i) {
            scratch->spec_entries[i].constantID = cb_r_u32(r);
            scratch->spec_entries[i].offset     = cb_r_u32(r);
            scratch->spec_entries[i].size       = cb_r_u32(r);
        }
    }
    uint32_t dlen = 0;
    const void *dptr = cb_r_blob(r, &dlen);
    if (dlen) {
        scratch->spec_data = malloc(dlen);
        memcpy(scratch->spec_data, dptr, dlen);
    }
    if (map_count || dlen) {
        scratch->spec.mapEntryCount = map_count;
        scratch->spec.pMapEntries   = scratch->spec_entries;
        scratch->spec.dataSize      = dlen;
        scratch->spec.pData         = scratch->spec_data;
        out->pSpecializationInfo    = &scratch->spec;
    }
}

static void cb_free_stage_scratch(cb_stage_scratch_t *s) {
    free(s->name); free(s->spec_entries); free(s->spec_data);
}

static void op_create_graphics_pipelines(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    cb_remote_id_t pc_id = cb_r_u64(r);
    VkPipelineCache pc = pc_id ? (VkPipelineCache)(uintptr_t)host_table_get(pc_id, HK_PIPELINE_CACHE) : VK_NULL_HANDLE;
    uint32_t count = cb_r_u32(r);

    VkGraphicsPipelineCreateInfo *cis = (VkGraphicsPipelineCreateInfo *)calloc(count, sizeof *cis);

    /* per-pipeline allocations to free after CreateGraphicsPipelines */
    VkPipelineShaderStageCreateInfo **stage_infos = (VkPipelineShaderStageCreateInfo **)calloc(count, sizeof *stage_infos);
    cb_stage_scratch_t              **stage_scr   = (cb_stage_scratch_t **)calloc(count, sizeof *stage_scr);
    uint32_t                         *stage_count = (uint32_t *)calloc(count, sizeof *stage_count);

    VkPipelineVertexInputStateCreateInfo  *vis      = (VkPipelineVertexInputStateCreateInfo *)calloc(count, sizeof *vis);
    VkVertexInputBindingDescription      **vibs     = (VkVertexInputBindingDescription **)calloc(count, sizeof *vibs);
    VkVertexInputAttributeDescription    **viattrs  = (VkVertexInputAttributeDescription **)calloc(count, sizeof *viattrs);
    VkPipelineInputAssemblyStateCreateInfo *ias     = (VkPipelineInputAssemblyStateCreateInfo *)calloc(count, sizeof *ias);
    VkPipelineTessellationStateCreateInfo  *tss     = (VkPipelineTessellationStateCreateInfo *)calloc(count, sizeof *tss);
    VkPipelineViewportStateCreateInfo      *vps     = (VkPipelineViewportStateCreateInfo *)calloc(count, sizeof *vps);
    VkPipelineRasterizationStateCreateInfo *rss     = (VkPipelineRasterizationStateCreateInfo *)calloc(count, sizeof *rss);
    VkPipelineMultisampleStateCreateInfo   *mss     = (VkPipelineMultisampleStateCreateInfo *)calloc(count, sizeof *mss);
    VkPipelineDepthStencilStateCreateInfo  *dsstate = (VkPipelineDepthStencilStateCreateInfo *)calloc(count, sizeof *dsstate);
    VkPipelineColorBlendStateCreateInfo    *cbs     = (VkPipelineColorBlendStateCreateInfo *)calloc(count, sizeof *cbs);
    VkPipelineColorBlendAttachmentState   **cbatts  = (VkPipelineColorBlendAttachmentState **)calloc(count, sizeof *cbatts);
    VkPipelineDynamicStateCreateInfo       *dys     = (VkPipelineDynamicStateCreateInfo *)calloc(count, sizeof *dys);
    VkDynamicState                        **dy_arr  = (VkDynamicState **)calloc(count, sizeof *dy_arr);

    for (uint32_t pi = 0; pi < count; ++pi) {
        VkGraphicsPipelineCreateInfo *p = &cis[pi];
        p->sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        p->flags = cb_r_u32(r);
        p->stageCount = cb_r_u32(r);
        stage_count[pi] = p->stageCount;
        if (p->stageCount) {
            stage_infos[pi] = (VkPipelineShaderStageCreateInfo *)calloc(p->stageCount, sizeof(VkPipelineShaderStageCreateInfo));
            stage_scr[pi]   = (cb_stage_scratch_t *)calloc(p->stageCount, sizeof(cb_stage_scratch_t));
            for (uint32_t i = 0; i < p->stageCount; ++i)
                cb_parse_shader_stage(r, &stage_infos[pi][i], &stage_scr[pi][i]);
            p->pStages = stage_infos[pi];
        }
        /* vertex input */
        vis[pi].sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vis[pi].vertexBindingDescriptionCount = cb_r_u32(r);
        if (vis[pi].vertexBindingDescriptionCount) {
            vibs[pi] = (VkVertexInputBindingDescription *)calloc(vis[pi].vertexBindingDescriptionCount, sizeof(VkVertexInputBindingDescription));
            for (uint32_t i = 0; i < vis[pi].vertexBindingDescriptionCount; ++i) {
                vibs[pi][i].binding   = cb_r_u32(r);
                vibs[pi][i].stride    = cb_r_u32(r);
                vibs[pi][i].inputRate = (VkVertexInputRate)cb_r_u32(r);
            }
            vis[pi].pVertexBindingDescriptions = vibs[pi];
        }
        vis[pi].vertexAttributeDescriptionCount = cb_r_u32(r);
        if (vis[pi].vertexAttributeDescriptionCount) {
            viattrs[pi] = (VkVertexInputAttributeDescription *)calloc(vis[pi].vertexAttributeDescriptionCount, sizeof(VkVertexInputAttributeDescription));
            for (uint32_t i = 0; i < vis[pi].vertexAttributeDescriptionCount; ++i) {
                viattrs[pi][i].location = cb_r_u32(r);
                viattrs[pi][i].binding  = cb_r_u32(r);
                viattrs[pi][i].format   = (VkFormat)cb_r_u32(r);
                viattrs[pi][i].offset   = cb_r_u32(r);
            }
            vis[pi].pVertexAttributeDescriptions = viattrs[pi];
        }
        p->pVertexInputState = &vis[pi];
        /* input assembly */
        ias[pi].sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ias[pi].topology = (VkPrimitiveTopology)cb_r_u32(r);
        ias[pi].primitiveRestartEnable = cb_r_u32(r);
        p->pInputAssemblyState = &ias[pi];
        /* tessellation */
        tss[pi].sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
        tss[pi].patchControlPoints = cb_r_u32(r);
        if (tss[pi].patchControlPoints) p->pTessellationState = &tss[pi];
        /* viewport */
        vps[pi].sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps[pi].viewportCount = cb_r_u32(r);
        vps[pi].scissorCount  = cb_r_u32(r);
        p->pViewportState = &vps[pi];
        /* raster */
        rss[pi].sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rss[pi].depthClampEnable        = cb_r_u32(r);
        rss[pi].rasterizerDiscardEnable = cb_r_u32(r);
        rss[pi].polygonMode             = (VkPolygonMode)cb_r_u32(r);
        rss[pi].cullMode                = cb_r_u32(r);
        rss[pi].frontFace               = (VkFrontFace)cb_r_u32(r);
        rss[pi].depthBiasEnable         = cb_r_u32(r);
        rss[pi].depthBiasConstantFactor = cb_r_f32(r);
        rss[pi].depthBiasClamp          = cb_r_f32(r);
        rss[pi].depthBiasSlopeFactor    = cb_r_f32(r);
        rss[pi].lineWidth               = cb_r_f32(r);
        p->pRasterizationState = &rss[pi];
        /* multisample */
        mss[pi].sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        mss[pi].rasterizationSamples  = (VkSampleCountFlagBits)cb_r_u32(r);
        mss[pi].sampleShadingEnable   = cb_r_u32(r);
        mss[pi].minSampleShading      = cb_r_f32(r);
        mss[pi].alphaToCoverageEnable = cb_r_u32(r);
        mss[pi].alphaToOneEnable      = cb_r_u32(r);
        p->pMultisampleState = &mss[pi];
        /* depth stencil */
        dsstate[pi].sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        dsstate[pi].depthTestEnable       = cb_r_u32(r);
        dsstate[pi].depthWriteEnable      = cb_r_u32(r);
        dsstate[pi].depthCompareOp        = (VkCompareOp)cb_r_u32(r);
        dsstate[pi].depthBoundsTestEnable = cb_r_u32(r);
        dsstate[pi].stencilTestEnable     = cb_r_u32(r);
        p->pDepthStencilState = &dsstate[pi];
        /* color blend */
        cbs[pi].sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs[pi].attachmentCount = cb_r_u32(r);
        if (cbs[pi].attachmentCount) {
            cbatts[pi] = (VkPipelineColorBlendAttachmentState *)calloc(cbs[pi].attachmentCount, sizeof(VkPipelineColorBlendAttachmentState));
            for (uint32_t i = 0; i < cbs[pi].attachmentCount; ++i) {
                cbatts[pi][i].blendEnable         = cb_r_u32(r);
                cbatts[pi][i].srcColorBlendFactor = (VkBlendFactor)cb_r_u32(r);
                cbatts[pi][i].dstColorBlendFactor = (VkBlendFactor)cb_r_u32(r);
                cbatts[pi][i].colorBlendOp        = (VkBlendOp)cb_r_u32(r);
                cbatts[pi][i].srcAlphaBlendFactor = (VkBlendFactor)cb_r_u32(r);
                cbatts[pi][i].dstAlphaBlendFactor = (VkBlendFactor)cb_r_u32(r);
                cbatts[pi][i].alphaBlendOp        = (VkBlendOp)cb_r_u32(r);
                cbatts[pi][i].colorWriteMask      = cb_r_u32(r);
            }
            cbs[pi].pAttachments = cbatts[pi];
        }
        p->pColorBlendState = &cbs[pi];
        /* dynamic state */
        dys[pi].sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dys[pi].dynamicStateCount = cb_r_u32(r);
        if (dys[pi].dynamicStateCount) {
            dy_arr[pi] = (VkDynamicState *)calloc(dys[pi].dynamicStateCount, sizeof(VkDynamicState));
            for (uint32_t i = 0; i < dys[pi].dynamicStateCount; ++i)
                dy_arr[pi][i] = (VkDynamicState)cb_r_u32(r);
            dys[pi].pDynamicStates = dy_arr[pi];
            p->pDynamicState = &dys[pi];
        }
        /* layout / render pass / subpass */
        cb_remote_id_t pl_id = cb_r_u64(r);
        cb_remote_id_t rp_id = cb_r_u64(r);
        p->layout     = pl_id ? (VkPipelineLayout)(uintptr_t)host_table_get(pl_id, HK_PIPELINE_LAYOUT) : VK_NULL_HANDLE;
        p->renderPass = rp_id ? (VkRenderPass)(uintptr_t)host_table_get(rp_id, HK_RENDER_PASS) : VK_NULL_HANDLE;
        p->subpass    = cb_r_u32(r);
    }

    VkPipeline *pipes = (VkPipeline *)calloc(count, sizeof *pipes);
    VkResult vr = d->fn.CreateGraphicsPipelines(d->vk, pc, count, cis, NULL, pipes);

    /* free per-pipeline scratch */
    for (uint32_t pi = 0; pi < count; ++pi) {
        if (stage_scr[pi]) for (uint32_t i = 0; i < stage_count[pi]; ++i)
            cb_free_stage_scratch(&stage_scr[pi][i]);
        free(stage_infos[pi]); free(stage_scr[pi]);
        free(vibs[pi]); free(viattrs[pi]); free(cbatts[pi]); free(dy_arr[pi]);
    }
    free(stage_infos); free(stage_scr); free(stage_count);
    free(vis); free(vibs); free(viattrs); free(ias); free(tss); free(vps);
    free(rss); free(mss); free(dsstate); free(cbs); free(cbatts);
    free(dys); free(dy_arr); free(cis);

    if (vr) { free(pipes); host_reply_fail(c, seq, vr); return; }
    cb_writer_t w; cb_writer_init_heap(&w, count * 8);
    for (uint32_t i = 0; i < count; ++i) {
        cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)pipes[i];
        host_table_put(id, HK_PIPELINE, (void *)(uintptr_t)pipes[i], _devid);
        cb_w_u64(&w, id);
    }
    free(pipes);
    host_reply_writer(c, seq, &w);
    cb_writer_dispose(&w);
}

static void op_create_compute_pipelines(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    cb_remote_id_t pc_id = cb_r_u64(r);
    VkPipelineCache pc = pc_id ? (VkPipelineCache)(uintptr_t)host_table_get(pc_id, HK_PIPELINE_CACHE) : VK_NULL_HANDLE;
    uint32_t count = cb_r_u32(r);
    VkComputePipelineCreateInfo *cis = (VkComputePipelineCreateInfo *)calloc(count, sizeof *cis);
    cb_stage_scratch_t *stage_scr = (cb_stage_scratch_t *)calloc(count, sizeof *stage_scr);
    for (uint32_t i = 0; i < count; ++i) {
        cis[i].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cis[i].flags = cb_r_u32(r);
        cb_parse_shader_stage(r, &cis[i].stage, &stage_scr[i]);
        cb_remote_id_t pl_id = cb_r_u64(r);
        cis[i].layout = pl_id ? (VkPipelineLayout)(uintptr_t)host_table_get(pl_id, HK_PIPELINE_LAYOUT) : VK_NULL_HANDLE;
    }
    VkPipeline *pipes = (VkPipeline *)calloc(count, sizeof *pipes);
    VkResult vr = d->fn.CreateComputePipelines(d->vk, pc, count, cis, NULL, pipes);
    for (uint32_t i = 0; i < count; ++i) cb_free_stage_scratch(&stage_scr[i]);
    free(stage_scr); free(cis);
    if (vr) { free(pipes); host_reply_fail(c, seq, vr); return; }
    cb_writer_t w; cb_writer_init_heap(&w, count * 8);
    for (uint32_t i = 0; i < count; ++i) {
        cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)pipes[i];
        host_table_put(id, HK_PIPELINE, (void *)(uintptr_t)pipes[i], _devid);
        cb_w_u64(&w, id);
    }
    free(pipes);
    host_reply_writer(c, seq, &w);
    cb_writer_dispose(&w);
}

static void op_destroy_pipeline(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkPipeline p = (VkPipeline)(uintptr_t)host_table_get(id, HK_PIPELINE);
    host_device_rec_t *d = g_last_device;
    if (p && d) d->fn.DestroyPipeline(d->vk, p, NULL);
    host_table_drop(id);
}

/* ---- Sync extras: get_fence_status / events --------------------------- */

static void op_get_fence_status(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    VkFence f = (VkFence)(uintptr_t)host_table_get(id, HK_FENCE);
    host_device_rec_t *d = g_last_device;
    if (!f || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkResult vr = d->fn.GetFenceStatus(d->vk, f);
    if (vr == VK_SUCCESS) host_reply_ok(c, seq);
    else host_reply_fail(c, seq, vr);
}

static void op_create_event(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    VkEventCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
                             .flags = cb_r_u32(r) };
    VkEvent e;
    VkResult vr = d->fn.CreateEvent(d->vk, &ci, NULL, &e);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)e;
    host_table_put(id, HK_EVENT, (void *)(uintptr_t)e, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_event(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkEvent e = (VkEvent)(uintptr_t)host_table_get(id, HK_EVENT);
    host_device_rec_t *d = g_last_device;
    if (e && d) d->fn.DestroyEvent(d->vk, e, NULL);
    host_table_drop(id);
}

/* ---- Command pool/buffer extras --------------------------------------- */

static void op_reset_command_pool(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    uint32_t flags = cb_r_u32(r);
    VkCommandPool p = (VkCommandPool)(uintptr_t)host_table_get(id, HK_COMMAND_POOL);
    host_device_rec_t *d = g_last_device;
    if (!p || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkResult vr = d->fn.ResetCommandPool(d->vk, p, flags);
    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}

static void op_reset_command_buffer(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    uint32_t flags = cb_r_u32(r);
    VkCommandBuffer b = (VkCommandBuffer)host_table_get(id, HK_COMMAND_BUFFER);
    host_device_rec_t *d = g_last_device;
    if (!b || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkResult vr = d->fn.ResetCommandBuffer(b, flags);
    if (vr) host_reply_fail(c, seq, vr); else host_reply_ok(c, seq);
}

static void op_free_command_buffers(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t pool_id = cb_r_u64(r);
    uint32_t cnt = cb_r_u32(r);
    VkCommandPool pool = (VkCommandPool)(uintptr_t)host_table_get(pool_id, HK_COMMAND_POOL);
    VkCommandBuffer *bufs = cnt ? (VkCommandBuffer *)calloc(cnt, sizeof *bufs) : NULL;
    cb_remote_id_t  *ids  = cnt ? (cb_remote_id_t *)calloc(cnt, sizeof *ids) : NULL;
    for (uint32_t i = 0; i < cnt; ++i) {
        ids[i] = cb_r_u64(r);
        bufs[i] = (VkCommandBuffer)host_table_get(ids[i], HK_COMMAND_BUFFER);
    }
    host_device_rec_t *d = g_last_device;
    if (pool && d) d->fn.FreeCommandBuffers(d->vk, pool, cnt, bufs);
    for (uint32_t i = 0; i < cnt; ++i) host_table_drop(ids[i]);
    free(bufs); free(ids);
}

/* ---- Surface queries / swapchain / present ---------------------------- */

static void op_get_surface_support(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t pid = cb_r_u64(r);
    uint32_t qf = cb_r_u32(r);
    cb_remote_id_t sid = cb_r_u64(r);
    VkPhysicalDevice pd = (VkPhysicalDevice)host_table_get(pid, HK_PHYSICAL_DEVICE);
    VkSurfaceKHR surf = (VkSurfaceKHR)(uintptr_t)host_table_get(sid, HK_SURFACE);
    host_instance_funcs_t *ifn = host_pd_funcs(pd);
    if (!pd || !surf || !ifn) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkBool32 supp = VK_FALSE;
    VkResult vr = ifn->GetPhysicalDeviceSurfaceSupportKHR(pd, qf, surf, &supp);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    uint32_t v = supp ? 1 : 0;
    host_reply_bytes(c, seq, &v, sizeof v);
}

static void op_get_surface_caps(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t pid = cb_r_u64(r);
    cb_remote_id_t sid = cb_r_u64(r);
    VkPhysicalDevice pd = (VkPhysicalDevice)host_table_get(pid, HK_PHYSICAL_DEVICE);
    VkSurfaceKHR surf = (VkSurfaceKHR)(uintptr_t)host_table_get(sid, HK_SURFACE);
    host_instance_funcs_t *ifn = host_pd_funcs(pd);
    if (!pd || !surf || !ifn) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    VkSurfaceCapabilitiesKHR caps;
    VkResult vr = ifn->GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    host_reply_bytes(c, seq, &caps, sizeof caps);
}

static void op_get_surface_formats(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t pid = cb_r_u64(r);
    cb_remote_id_t sid = cb_r_u64(r);
    VkPhysicalDevice pd = (VkPhysicalDevice)host_table_get(pid, HK_PHYSICAL_DEVICE);
    VkSurfaceKHR surf = (VkSurfaceKHR)(uintptr_t)host_table_get(sid, HK_SURFACE);
    host_instance_funcs_t *ifn = host_pd_funcs(pd);
    if (!pd || !surf || !ifn) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    uint32_t n = 0;
    ifn->GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &n, NULL);
    VkSurfaceFormatKHR *fs = (VkSurfaceFormatKHR *)calloc(n, sizeof *fs);
    ifn->GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &n, fs);
    cb_writer_t w; cb_writer_init_heap(&w, 8 + n * sizeof *fs);
    cb_w_u32(&w, n);
    cb_w_bytes(&w, fs, n * sizeof *fs);
    host_reply_writer(c, seq, &w);
    cb_writer_dispose(&w);
    free(fs);
}

static void op_get_surface_present_modes(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t pid = cb_r_u64(r);
    cb_remote_id_t sid = cb_r_u64(r);
    VkPhysicalDevice pd = (VkPhysicalDevice)host_table_get(pid, HK_PHYSICAL_DEVICE);
    VkSurfaceKHR surf = (VkSurfaceKHR)(uintptr_t)host_table_get(sid, HK_SURFACE);
    host_instance_funcs_t *ifn = host_pd_funcs(pd);
    if (!pd || !surf || !ifn) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    uint32_t n = 0;
    ifn->GetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &n, NULL);
    VkPresentModeKHR *pm = (VkPresentModeKHR *)calloc(n, sizeof *pm);
    ifn->GetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &n, pm);
    cb_writer_t w; cb_writer_init_heap(&w, 8 + n * 4);
    cb_w_u32(&w, n);
    for (uint32_t i = 0; i < n; ++i) cb_w_u32(&w, (uint32_t)pm[i]);
    host_reply_writer(c, seq, &w);
    cb_writer_dispose(&w);
    free(pm);
}

static void op_destroy_surface(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkSurfaceKHR surf = (VkSurfaceKHR)(uintptr_t)host_table_get(id, HK_SURFACE);
    if (!surf) return;
    /* find the owning instance via parent_id walk: we use the first
     * registered instance because its CreateMetalSurfaceEXT/Destroy share
     * the same loader. Linear scan via host_pd_cache->ifn. */
    pthread_mutex_lock(&g_pd_cache_lock);
    host_instance_funcs_t *ifn = (g_pd_cache_n > 0) ? g_pd_cache[0].ifn : NULL;
    pthread_mutex_unlock(&g_pd_cache_lock);
    if (ifn && ifn->DestroySurfaceKHR) {
        host_instance_rec_t *outer =
            (host_instance_rec_t *)((uint8_t *)ifn - offsetof(host_instance_rec_t, ifn));
        ifn->DestroySurfaceKHR(outer->vk, surf, NULL);
    }
    host_table_drop(id);
}

static void op_create_swapchain(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    READ_DEV(r, d);
    cb_remote_id_t sid = cb_r_u64(r);
    VkSurfaceKHR surf = (VkSurfaceKHR)(uintptr_t)host_table_get(sid, HK_SURFACE);
    if (!surf) { host_reply_fail(c, seq, VK_ERROR_SURFACE_LOST_KHR); return; }
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface          = surf;
    ci.flags            = cb_r_u32(r);
    ci.minImageCount    = cb_r_u32(r);
    ci.imageFormat      = (VkFormat)cb_r_u32(r);
    ci.imageColorSpace  = (VkColorSpaceKHR)cb_r_u32(r);
    ci.imageExtent.width  = cb_r_u32(r);
    ci.imageExtent.height = cb_r_u32(r);
    ci.imageArrayLayers = cb_r_u32(r);
    ci.imageUsage       = cb_r_u32(r);
    ci.imageSharingMode = (VkSharingMode)cb_r_u32(r);
    ci.queueFamilyIndexCount = cb_r_u32(r);
    uint32_t *qfis = ci.queueFamilyIndexCount
        ? (uint32_t *)calloc(ci.queueFamilyIndexCount, sizeof(uint32_t)) : NULL;
    for (uint32_t i = 0; i < ci.queueFamilyIndexCount; ++i) qfis[i] = cb_r_u32(r);
    ci.pQueueFamilyIndices = qfis;
    ci.preTransform   = (VkSurfaceTransformFlagBitsKHR)cb_r_u32(r);
    ci.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)cb_r_u32(r);
    ci.presentMode    = (VkPresentModeKHR)cb_r_u32(r);
    ci.clipped        = cb_r_u32(r);
    cb_remote_id_t old_id = cb_r_u64(r);
    ci.oldSwapchain = old_id ? (VkSwapchainKHR)(uintptr_t)host_table_get(old_id, HK_SWAPCHAIN) : VK_NULL_HANDLE;

    VkSwapchainKHR sc;
    VkResult vr = d->fn.CreateSwapchainKHR(d->vk, &ci, NULL, &sc);
    free(qfis);
    if (vr) { host_reply_fail(c, seq, vr); return; }
    cb_remote_id_t id = (cb_remote_id_t)(uintptr_t)sc;
    host_table_put(id, HK_SWAPCHAIN, (void *)(uintptr_t)sc, _devid);
    host_reply_id(c, seq, id);
}

static void op_destroy_swapchain(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq;
    cb_remote_id_t id = cb_r_u64(r);
    VkSwapchainKHR sc = (VkSwapchainKHR)(uintptr_t)host_table_get(id, HK_SWAPCHAIN);
    host_device_rec_t *d = g_last_device;
    if (sc && d) d->fn.DestroySwapchainKHR(d->vk, sc, NULL);
    host_table_drop(id);
}

static void op_get_swapchain_images(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    VkSwapchainKHR sc = (VkSwapchainKHR)(uintptr_t)host_table_get(id, HK_SWAPCHAIN);
    host_device_rec_t *d = g_last_device;
    if (!sc || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    uint32_t n = 0;
    d->fn.GetSwapchainImagesKHR(d->vk, sc, &n, NULL);
    VkImage *imgs = (VkImage *)calloc(n, sizeof *imgs);
    d->fn.GetSwapchainImagesKHR(d->vk, sc, &n, imgs);
    cb_writer_t w; cb_writer_init_heap(&w, 8 + n * 8);
    cb_w_u32(&w, n);
    for (uint32_t i = 0; i < n; ++i) {
        cb_remote_id_t iid = (cb_remote_id_t)(uintptr_t)imgs[i];
        host_table_put(iid, HK_IMAGE, (void *)(uintptr_t)imgs[i], 0);
        cb_w_u64(&w, iid);
    }
    free(imgs);
    host_reply_writer(c, seq, &w);
    cb_writer_dispose(&w);
}

static void op_acquire_next_image(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t id = cb_r_u64(r);
    uint64_t timeout = cb_r_u64(r);
    cb_remote_id_t sem_id = cb_r_u64(r);
    cb_remote_id_t fence_id = cb_r_u64(r);
    VkSwapchainKHR sc = (VkSwapchainKHR)(uintptr_t)host_table_get(id, HK_SWAPCHAIN);
    VkSemaphore sem = sem_id ? (VkSemaphore)(uintptr_t)host_table_get(sem_id, HK_SEMAPHORE) : VK_NULL_HANDLE;
    VkFence fnc = fence_id ? (VkFence)(uintptr_t)host_table_get(fence_id, HK_FENCE) : VK_NULL_HANDLE;
    host_device_rec_t *d = g_last_device;
    if (!sc || !d) { host_reply_fail(c, seq, VK_ERROR_DEVICE_LOST); return; }
    uint32_t idx = 0;
    VkResult vr = d->fn.AcquireNextImageKHR(d->vk, sc, timeout, sem, fnc, &idx);
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        host_reply_fail(c, seq, vr); return;
    }
    /* Reply payload: just the index. The guest treats SUBOPTIMAL as success
     * with a returned index, matching its decode logic. */
    host_reply_bytes(c, seq, &idx, sizeof idx);
}

static void op_queue_present(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    cb_remote_id_t qid = cb_r_u64(r);
    VkQueue q = (VkQueue)host_table_get(qid, HK_QUEUE);
    uint32_t ws = cb_r_u32(r);
    VkSemaphore *wsem = ws ? (VkSemaphore *)calloc(ws, sizeof *wsem) : NULL;
    for (uint32_t i = 0; i < ws; ++i) {
        cb_remote_id_t sid = cb_r_u64(r);
        wsem[i] = (VkSemaphore)(uintptr_t)host_table_get(sid, HK_SEMAPHORE);
    }
    uint32_t scnt = cb_r_u32(r);
    VkSwapchainKHR *scs = scnt ? (VkSwapchainKHR *)calloc(scnt, sizeof *scs) : NULL;
    uint32_t *idxs = scnt ? (uint32_t *)calloc(scnt, sizeof *idxs) : NULL;
    for (uint32_t i = 0; i < scnt; ++i) {
        cb_remote_id_t sid = cb_r_u64(r);
        scs[i]  = (VkSwapchainKHR)(uintptr_t)host_table_get(sid, HK_SWAPCHAIN);
        idxs[i] = cb_r_u32(r);
    }
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = ws,
        .pWaitSemaphores    = wsem,
        .swapchainCount     = scnt,
        .pSwapchains        = scs,
        .pImageIndices      = idxs,
    };
    host_device_rec_t *d = g_last_device;
    VkResult vr = (q && d) ? d->fn.QueuePresentKHR(q, &pi) : VK_ERROR_DEVICE_LOST;
    free(wsem); free(scs); free(idxs);
    if (vr && vr != VK_SUBOPTIMAL_KHR) host_reply_fail(c, seq, vr);
    else host_reply_ok(c, seq);
}

/* BYE: connection-end notification, no reply expected. */
static void op_bye(host_conn_t *c, uint32_t seq, cb_reader_t *r) {
    (void)c; (void)seq; (void)r;
    HI("guest sent BYE");
}

/* ---- Catch-all + dispatch table ---------------------------------------- */

typedef void (*op_handler_t)(host_conn_t *, uint32_t, cb_reader_t *);

static op_handler_t lookup(uint16_t opcode) {
    switch (opcode) {
    case CB_OP_HELLO:                   return op_hello;
    case CB_OP_BYE:                     return op_bye;
    case CB_OP_CREATE_INSTANCE:         return op_create_instance;
    case CB_OP_DESTROY_INSTANCE:        return op_destroy_instance;
    case CB_OP_ENUMERATE_PHYSICAL_DEVICES: return op_enumerate_pds;
    case CB_OP_GET_PD_PROPERTIES:       return op_pd_properties;
    case CB_OP_GET_PD_FEATURES:         return op_pd_features;
    case CB_OP_GET_PD_QUEUE_FAMILY_PROPS: return op_pd_qfp;
    case CB_OP_GET_PD_MEMORY_PROPS:     return op_pd_memprops;
    case CB_OP_GET_PD_FORMAT_PROPS:     return op_pd_format_props;
    case CB_OP_GET_PD_IMAGE_FORMAT_PROPS:return op_pd_image_format_props;
    case CB_OP_CREATE_DEVICE:           return op_create_device;
    case CB_OP_DESTROY_DEVICE:          return op_destroy_device;
    case CB_OP_GET_DEVICE_QUEUE:        return op_get_device_queue;
    case CB_OP_DEVICE_WAIT_IDLE:        return op_device_wait_idle;
    case CB_OP_QUEUE_WAIT_IDLE:         return op_queue_wait_idle;
    case CB_OP_ALLOCATE_MEMORY:         return op_alloc_memory;
    case CB_OP_FREE_MEMORY:             return op_free_memory;
    case CB_OP_WRITE_MEMORY:            return op_write_memory;
    case CB_OP_READ_MEMORY:             return op_read_memory;
    case CB_OP_CREATE_BUFFER:           return op_create_buffer;
    case CB_OP_DESTROY_BUFFER:          return op_destroy_buffer;
    case CB_OP_GET_BUFFER_MEM_REQS:     return op_get_buffer_mem_reqs;
    case CB_OP_BIND_BUFFER_MEMORY:      return op_bind_buffer_memory;
    case CB_OP_CREATE_IMAGE:            return op_create_image;
    case CB_OP_DESTROY_IMAGE:           return op_destroy_image;
    case CB_OP_GET_IMAGE_MEM_REQS:      return op_get_image_mem_reqs;
    case CB_OP_BIND_IMAGE_MEMORY:       return op_bind_image_memory;
    case CB_OP_CREATE_IMAGE_VIEW:       return op_create_image_view;
    case CB_OP_DESTROY_IMAGE_VIEW:      return op_destroy_image_view;
    case CB_OP_CREATE_SAMPLER:          return op_create_sampler;
    case CB_OP_DESTROY_SAMPLER:         return op_destroy_sampler;
    case CB_OP_CREATE_SHADER_MODULE:    return op_create_shader_module;
    case CB_OP_DESTROY_SHADER_MODULE:   return op_destroy_shader_module;
    case CB_OP_CREATE_PIPELINE_CACHE:   return op_create_pipeline_cache;
    case CB_OP_DESTROY_PIPELINE_CACHE:  return op_destroy_pipeline_cache;
    case CB_OP_CREATE_PIPELINE_LAYOUT:  return op_create_pipeline_layout;
    case CB_OP_DESTROY_PIPELINE_LAYOUT: return op_destroy_pipeline_layout;
    case CB_OP_CREATE_RENDER_PASS:      return op_create_render_pass;
    case CB_OP_DESTROY_RENDER_PASS:     return op_destroy_render_pass;
    case CB_OP_CREATE_FRAMEBUFFER:      return op_create_framebuffer;
    case CB_OP_DESTROY_FRAMEBUFFER:     return op_destroy_framebuffer;
    case CB_OP_CREATE_DESC_SET_LAYOUT:  return op_create_dsl;
    case CB_OP_DESTROY_DESC_SET_LAYOUT: return op_destroy_dsl;
    case CB_OP_CREATE_DESC_POOL:        return op_create_desc_pool;
    case CB_OP_DESTROY_DESC_POOL:       return op_destroy_desc_pool;
    case CB_OP_ALLOCATE_DESC_SETS:      return op_alloc_desc_sets;
    case CB_OP_FREE_DESC_SETS:          return op_free_desc_sets;
    case CB_OP_UPDATE_DESC_SETS:        return op_update_desc_sets;
    case CB_OP_CREATE_GRAPHICS_PIPELINES: return op_create_graphics_pipelines;
    case CB_OP_CREATE_COMPUTE_PIPELINES:  return op_create_compute_pipelines;
    case CB_OP_DESTROY_PIPELINE:        return op_destroy_pipeline;
    case CB_OP_CREATE_COMMAND_POOL:     return op_create_command_pool;
    case CB_OP_DESTROY_COMMAND_POOL:    return op_destroy_command_pool;
    case CB_OP_RESET_COMMAND_POOL:      return op_reset_command_pool;
    case CB_OP_ALLOCATE_COMMAND_BUFFERS:return op_alloc_command_buffers;
    case CB_OP_FREE_COMMAND_BUFFERS:    return op_free_command_buffers;
    case CB_OP_BEGIN_COMMAND_BUFFER:    return op_begin_command_buffer;
    case CB_OP_END_COMMAND_BUFFER:      return op_end_command_buffer;
    case CB_OP_RESET_COMMAND_BUFFER:    return op_reset_command_buffer;
    case CB_OP_RECORD_COMMAND_STREAM:   return op_record_command_stream;
    case CB_OP_QUEUE_SUBMIT:            return op_queue_submit;
    case CB_OP_QUEUE_PRESENT:           return op_queue_present;
    case CB_OP_CREATE_FENCE:            return op_create_fence;
    case CB_OP_DESTROY_FENCE:           return op_destroy_fence;
    case CB_OP_WAIT_FOR_FENCES:         return op_wait_for_fences;
    case CB_OP_RESET_FENCES:            return op_reset_fences;
    case CB_OP_GET_FENCE_STATUS:        return op_get_fence_status;
    case CB_OP_CREATE_SEMAPHORE:        return op_create_semaphore;
    case CB_OP_DESTROY_SEMAPHORE:       return op_destroy_semaphore;
    case CB_OP_CREATE_EVENT:            return op_create_event;
    case CB_OP_DESTROY_EVENT:           return op_destroy_event;
    case CB_OP_CREATE_SURFACE:          return op_create_surface;
    case CB_OP_DESTROY_SURFACE:         return op_destroy_surface;
    case CB_OP_GET_SURFACE_SUPPORT:     return op_get_surface_support;
    case CB_OP_GET_SURFACE_CAPABILITIES:return op_get_surface_caps;
    case CB_OP_GET_SURFACE_FORMATS:     return op_get_surface_formats;
    case CB_OP_GET_SURFACE_PRESENT_MODES: return op_get_surface_present_modes;
    case CB_OP_CREATE_SWAPCHAIN:        return op_create_swapchain;
    case CB_OP_DESTROY_SWAPCHAIN:       return op_destroy_swapchain;
    case CB_OP_GET_SWAPCHAIN_IMAGES:    return op_get_swapchain_images;
    case CB_OP_ACQUIRE_NEXT_IMAGE:      return op_acquire_next_image;
    default: return NULL;
    }
}

void host_conn_dispatch_loop(host_conn_t *c) {
    HI("dispatch loop started for fd=%d", c->fd);
    for (;;) {
        cb_frame_header_t h; void *body = NULL;
        int rc = cb_read_frame(c->fd, &h, &body);
        if (rc != 0) {
            if (rc == -1) HI("guest disconnected");
            else          HE("read_frame error %d", rc);
            break;
        }
        cb_reader_t r; cb_reader_init(&r, body, h.length);
        op_handler_t fn = lookup(h.opcode);

        /* Side-effect: track newly created device for fallback paths. */
        if (h.opcode == CB_OP_CREATE_INSTANCE) {
            /* nothing; but ensure pd cache also gets re-registered after enum */
        }

        if (!fn) {
            HW("unhandled opcode 0x%04x len=%u", h.opcode, h.length);
            if (!(h.flags & CB_FLAG_ASYNC))
                host_reply_fail(c, h.sequence, VK_ERROR_FEATURE_NOT_PRESENT);
            free(body); continue;
        }
        fn(c, h.sequence, &r);

        /* Post-processing for create_device / enumerate_pds: keep g_last_device
         * and pd-cache up to date. We sniff by inspecting table side effects. */
        if (h.opcode == CB_OP_CREATE_DEVICE) {
            /* find the most recently registered device. Walk by re-reading
             * the body: first u64 is pd_id, but we want the newly created
             * device id, which is what we sent back. We cached it locally
             * inside op_create_device — instead just stash it via g_last_device
             * directly there: */
        }
        if (h.opcode == CB_OP_ENUMERATE_PHYSICAL_DEVICES) {
            /* refresh pd cache for each pd of the instance whose id we
             * just received in r */
            cb_reader_t r2; cb_reader_init(&r2, body, h.length);
            cb_remote_id_t iid = cb_r_u64(&r2);
            host_instance_rec_t *rec = host_instance_for(iid);
            if (rec) register_pd_funcs(rec);
        }
        free(body);
    }
}

