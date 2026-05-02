#include "demo_net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef TCP_NODELAY
#include <netinet/tcp.h>
#endif

typedef struct cb_demo_spec {
    int is_unix;
    char host[256];
    char port[32];
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} cb_demo_spec_t;

static int cb_demo_parse_spec(const char *spec, cb_demo_spec_t *out) {
    memset(out, 0, sizeof *out);
    if (!spec || !*spec) spec = CB_DEMO_DEFAULT_ENDPOINT;

    if (strncmp(spec, "unix:", 5) == 0) {
        out->is_unix = 1;
        snprintf(out->path, sizeof out->path, "%s", spec + 5);
        return out->path[0] ? 0 : -1;
    }

    const char *tcp = spec;
    if (strncmp(tcp, "tcp:", 4) == 0) tcp += 4;

    const char *colon = strrchr(tcp, ':');
    if (!colon || colon == tcp || colon[1] == '\0') return -1;

    size_t host_len = (size_t)(colon - tcp);
    if (host_len >= sizeof out->host) host_len = sizeof out->host - 1;
    memcpy(out->host, tcp, host_len);
    out->host[host_len] = '\0';
    snprintf(out->port, sizeof out->port, "%s", colon + 1);
    return 0;
}

static void cb_demo_set_common_socket_options(int fd) {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef TCP_NODELAY
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#endif
}

int cb_demo_connect(const char *spec) {
    cb_demo_spec_t parsed;
    if (cb_demo_parse_spec(spec, &parsed) != 0) {
        fprintf(stderr, "bad endpoint: %s\n", spec ? spec : "(null)");
        return -1;
    }

    if (parsed.is_unix) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", parsed.path);

        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
            perror("connect");
            close(fd);
            return -1;
        }
        return fd;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(parsed.host, parsed.port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s): %s\n",
                parsed.host, parsed.port, gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        cb_demo_set_common_socket_options(fd);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    if (fd < 0) perror("connect");
    return fd;
}

int cb_demo_listen(const char *spec) {
    cb_demo_spec_t parsed;
    if (cb_demo_parse_spec(spec, &parsed) != 0) {
        fprintf(stderr, "bad listen endpoint: %s\n", spec ? spec : "(null)");
        return -1;
    }

    if (parsed.is_unix) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", parsed.path);
        unlink(parsed.path);

        if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
            perror("bind");
            close(fd);
            return -1;
        }
        if (listen(fd, 16) != 0) {
            perror("listen");
            close(fd);
            return -1;
        }
        return fd;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(parsed.host, parsed.port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s): %s\n",
                parsed.host, parsed.port, gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        cb_demo_set_common_socket_options(fd);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            listen(fd, 16) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    if (fd < 0) perror("bind/listen");
    return fd;
}

const char *cb_demo_opcode_name(uint16_t opcode) {
    switch (opcode) {
        case CB_OP_HELLO: return "HELLO";
        case CB_OP_HELLO_REPLY: return "HELLO_REPLY";
        case CB_OP_BYE: return "BYE";
        case CB_OP_FAIL_REPLY: return "FAIL_REPLY";
        case CB_OP_GENERIC_REPLY: return "GENERIC_REPLY";
        case CB_OP_CAPABILITY_QUERY: return "CAPABILITY_QUERY";
        case CB_OP_CAPABILITY_REPLY: return "CAPABILITY_REPLY";
        case CB_OP_CREATE_INSTANCE: return "CREATE_INSTANCE";
        case CB_OP_ENUMERATE_PHYSICAL_DEVICES: return "ENUMERATE_PHYSICAL_DEVICES";
        case CB_OP_CREATE_DEVICE: return "CREATE_DEVICE";
        case CB_OP_CREATE_BUFFER: return "CREATE_BUFFER";
        case CB_OP_QUEUE_SUBMIT: return "QUEUE_SUBMIT";
        case CB_OP_QUEUE_PRESENT: return "PRESENT";
        default: return "UNKNOWN";
    }
}

void cb_demo_ignore_sigpipe(void) {
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
}

int cb_demo_write_u64_reply(int fd, uint16_t opcode, uint32_t seq, uint64_t id) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 8);
    cb_w_u64(&w, id);
    if (w.overflow) {
        cb_writer_dispose(&w);
        return -1;
    }
    int rc = cb_write_frame(fd, opcode, CB_FLAG_REPLY, seq,
                            w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return rc;
}

int cb_demo_write_ok_reply(int fd, uint32_t seq) {
    return cb_write_frame(fd, CB_OP_GENERIC_REPLY, CB_FLAG_REPLY, seq, NULL, 0);
}

int cb_demo_write_fail_reply(int fd, uint32_t seq, int32_t result) {
    return cb_write_frame(fd, CB_OP_FAIL_REPLY, CB_FLAG_REPLY, seq,
                          &result, sizeof result);
}
