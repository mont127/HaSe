#include "icd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

static cb_transport_t g_xport = {
    .fd        = -1,
    .lock      = PTHREAD_MUTEX_INITIALIZER,
    .next_seq  = 1,
    .connected = false,
};

cb_transport_t *cb_transport_get(void) { return &g_xport; }

/*
 * Endpoint selection:
 *   CHEESEBRIDGE_HOST  - "host:port" for TCP, or "unix:/path" for AF_UNIX,
 *                        or "vsock:cid:port" for AF_VSOCK (where supported).
 * Defaults to TCP localhost:43210 if unset, which is the right thing when
 * the macOS host runs on the same machine and the Linux guest forwards
 * the port via the VMM.
 */
static int cb_open_socket(void) {
    const char *spec = getenv("CHEESEBRIDGE_HOST");
    if (!spec || !*spec) spec = "tcp:127.0.0.1:43210";

    if (!strncmp(spec, "unix:", 5)) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, spec + 5, sizeof sa.sun_path - 1);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
            CB_E("connect(unix %s): %s", sa.sun_path, strerror(errno));
            close(fd); return -1;
        }
        return fd;
    }

    /* tcp:host:port  -- with optional "tcp:" prefix */
    const char *h = spec;
    if (!strncmp(h, "tcp:", 4)) h += 4;
    char host[256] = {0};
    const char *colon = strrchr(h, ':');
    if (!colon || colon == h) { CB_E("bad CHEESEBRIDGE_HOST=%s", spec); return -1; }
    size_t hl = (size_t)(colon - h);
    if (hl >= sizeof host) hl = sizeof host - 1;
    memcpy(host, h, hl);
    const char *port = colon + 1;

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        CB_E("getaddrinfo(%s:%s): %s", host, port, gai_strerror(gai));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        CB_E("connect(%s:%s) failed", host, port);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

/* HELLO/HELLO_REPLY exchange: reject host on protocol mismatch. */
static VkResult cb_handshake(int fd) {
    cb_writer_t w; cb_writer_init_heap(&w, 64);
    cb_w_u32(&w, CB_PROTO_VERSION);
    const char banner[] = "CheeseBridgeGuest";
    cb_w_blob(&w, banner, sizeof banner - 1);
    if (w.overflow) { cb_writer_dispose(&w); return VK_ERROR_OUT_OF_HOST_MEMORY; }

    if (cb_write_frame(fd, CB_OP_HELLO, 0, 0, w.buf, (uint32_t)w.pos) != 0) {
        cb_writer_dispose(&w);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    cb_writer_dispose(&w);

    cb_frame_header_t h;
    void *payload = NULL;
    if (cb_read_frame(fd, &h, &payload) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (h.opcode != CB_OP_HELLO_REPLY) {
        free(payload); return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    cb_reader_t r; cb_reader_init(&r, payload, h.length);
    uint32_t host_version = cb_r_u32(&r);
    free(payload);
    if (host_version != CB_PROTO_VERSION) {
        CB_E("protocol mismatch: guest=%u host=%u", CB_PROTO_VERSION, host_version);
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    return VK_SUCCESS;
}

VkResult cb_transport_connect(void) {
    pthread_mutex_lock(&g_xport.lock);
    if (g_xport.connected) {
        pthread_mutex_unlock(&g_xport.lock);
        return VK_SUCCESS;
    }
    int fd = cb_open_socket();
    if (fd < 0) {
        pthread_mutex_unlock(&g_xport.lock);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult vr = cb_handshake(fd);
    if (vr != VK_SUCCESS) {
        close(fd);
        pthread_mutex_unlock(&g_xport.lock);
        return vr;
    }
    g_xport.fd = fd;
    g_xport.connected = true;
    pthread_mutex_unlock(&g_xport.lock);
    CB_I("connected to host");
    return VK_SUCCESS;
}

void cb_transport_disconnect(void) {
    pthread_mutex_lock(&g_xport.lock);
    if (g_xport.connected) {
        cb_write_frame(g_xport.fd, CB_OP_BYE, CB_FLAG_ASYNC, 0, NULL, 0);
        close(g_xport.fd);
        g_xport.fd = -1;
        g_xport.connected = false;
    }
    pthread_mutex_unlock(&g_xport.lock);
}

/* Decode a CB_OP_FAIL_REPLY payload into a VkResult. */
static VkResult cb_decode_fail(const void *payload, uint32_t len) {
    if (len < sizeof(int32_t)) return VK_ERROR_DEVICE_LOST;
    int32_t code;
    memcpy(&code, payload, sizeof code);
    return (VkResult)code;
}

VkResult cb_rpc_call(uint16_t opcode, const void *payload, uint32_t len,
                     uint16_t *out_opcode, void **out_reply,
                     uint32_t *out_reply_len) {
    if (!g_xport.connected) {
        VkResult vr = cb_transport_connect();
        if (vr != VK_SUCCESS) return vr;
    }

    uint32_t seq = atomic_fetch_add(&g_xport.next_seq, 1);

    pthread_mutex_lock(&g_xport.lock);
    if (cb_write_frame(g_xport.fd, opcode, 0, seq, payload, len) != 0) {
        pthread_mutex_unlock(&g_xport.lock);
        CB_E("rpc write failed (opcode=0x%04x)", opcode);
        return VK_ERROR_DEVICE_LOST;
    }
    cb_frame_header_t h;
    void *body = NULL;
    int rc = cb_read_frame(g_xport.fd, &h, &body);
    pthread_mutex_unlock(&g_xport.lock);
    if (rc != 0) {
        CB_E("rpc read failed (opcode=0x%04x)", opcode);
        return VK_ERROR_DEVICE_LOST;
    }
    if (!(h.flags & CB_FLAG_REPLY) || h.sequence != seq) {
        CB_E("rpc out-of-order reply: got seq=%u flags=0x%04x op=0x%04x, expected seq=%u",
             h.sequence, h.flags, h.opcode, seq);
        free(body);
        return VK_ERROR_DEVICE_LOST;
    }
    if (h.opcode == CB_OP_FAIL_REPLY) {
        VkResult vr = cb_decode_fail(body, h.length);
        free(body);
        return vr;
    }
    if (out_opcode)     *out_opcode     = h.opcode;
    if (out_reply)      *out_reply      = body; else free(body);
    if (out_reply_len)  *out_reply_len  = h.length;
    return VK_SUCCESS;
}

VkResult cb_rpc_call_void(uint16_t opcode, const void *payload, uint32_t len) {
    void *r = NULL; uint16_t op = 0; uint32_t rl = 0;
    VkResult vr = cb_rpc_call(opcode, payload, len, &op, &r, &rl);
    free(r);
    return vr;
}

VkResult cb_rpc_send_async(uint16_t opcode, const void *payload, uint32_t len) {
    if (!g_xport.connected) {
        VkResult vr = cb_transport_connect();
        if (vr != VK_SUCCESS) return vr;
    }
    pthread_mutex_lock(&g_xport.lock);
    int rc = cb_write_frame(g_xport.fd, opcode, CB_FLAG_ASYNC, 0, payload, len);
    pthread_mutex_unlock(&g_xport.lock);
    return rc == 0 ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}
