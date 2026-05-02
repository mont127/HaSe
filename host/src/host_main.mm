/*
 * CheeseBridge host entry point.
 *
 * Listens on a TCP port for guest ICD connections, spawns one pthread per
 * connection that runs host_conn_dispatch_loop, and keeps a Cocoa main loop
 * alive so window/swapchain ops dispatched onto the main queue actually run.
 *
 * Usage:
 *   cheesebridge_host [port]
 *   CHEESEBRIDGE_LISTEN=tcp:0.0.0.0:43210 cheesebridge_host
 *
 * Phase 3 scope: bring the dispatcher online and accept guest frames so
 * Phase 4 can swap the stub MoltenVK responses for real Metal work.
 */

#import "host.h"

#import <Cocoa/Cocoa.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void *conn_thread(void *arg) {
    host_conn_t *c = (host_conn_t *)arg;
    host_conn_dispatch_loop(c);
    close(c->fd);
    pthread_mutex_destroy(&c->write_lock);
    free(c);
    return NULL;
}

static int parse_listen_endpoint(const char *spec,
                                 char *out_host, size_t host_cap,
                                 uint16_t *out_port) {
    /* Accepts "tcp:HOST:PORT", "HOST:PORT", or just "PORT". */
    const char *s = spec;
    if (!strncmp(s, "tcp:", 4)) s += 4;
    const char *colon = strrchr(s, ':');
    if (!colon) {
        long p = strtol(s, NULL, 10);
        if (p <= 0 || p > 65535) return -1;
        snprintf(out_host, host_cap, "0.0.0.0");
        *out_port = (uint16_t)p;
        return 0;
    }
    size_t hl = (size_t)(colon - s);
    if (hl == 0 || hl >= host_cap) return -1;
    memcpy(out_host, s, hl);
    out_host[hl] = '\0';
    long p = strtol(colon + 1, NULL, 10);
    if (p <= 0 || p > 65535) return -1;
    *out_port = (uint16_t)p;
    return 0;
}

static int open_listener(const char *host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (!host || !*host || !strcmp(host, "0.0.0.0")) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid listen host: %s\n", host);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void *accept_thread(void *arg) {
    int listen_fd = (int)(intptr_t)arg;
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peerlen = sizeof peer;
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &peerlen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        host_conn_t *c = (host_conn_t *)calloc(1, sizeof *c);
        c->fd = fd;
        pthread_mutex_init(&c->write_lock, NULL);
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        host_log(HL_INFO, "guest connected from %s:%u (fd=%d)", ip,
                 (unsigned)ntohs(peer.sin_port), fd);
        pthread_create(&c->thread, NULL, conn_thread, c);
        pthread_detach(c->thread);
    }
    return NULL;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        signal(SIGPIPE, SIG_IGN);

        const char *spec = getenv("CHEESEBRIDGE_LISTEN");
        char host[64] = "0.0.0.0";
        uint16_t port = 43210;
        if (spec && *spec) {
            if (parse_listen_endpoint(spec, host, sizeof host, &port) != 0) {
                fprintf(stderr, "invalid CHEESEBRIDGE_LISTEN: %s\n", spec);
                return 2;
            }
        } else if (argc >= 2) {
            if (parse_listen_endpoint(argv[1], host, sizeof host, &port) != 0) {
                fprintf(stderr, "invalid endpoint: %s\n", argv[1]);
                return 2;
            }
        }

        if (host_load_moltenvk() != VK_SUCCESS) {
            fprintf(stderr, "failed to load MoltenVK (libMoltenVK.dylib must be on DYLD path)\n");
            return 1;
        }
        host_table_init();

        int listen_fd = open_listener(host, port);
        if (listen_fd < 0) return 1;
        host_log(HL_INFO, "listening on tcp:%s:%u", host, (unsigned)port);

        pthread_t at;
        pthread_create(&at, NULL, accept_thread, (void *)(intptr_t)listen_fd);
        pthread_detach(at);

        /* Cocoa main loop so dispatch_async(main) used by window/present
         * handlers actually fires. We do not require an .app bundle - the
         * accessory activation policy is enough to pump NSRunLoop. */
        NSApplication *app = [NSApplication sharedApplication];
        /* Regular policy gets us a normal app window in the dock. Accessory
         * mode hides the dock entry but on some macOS versions also keeps
         * windows behind other apps until the user clicks them. */
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
