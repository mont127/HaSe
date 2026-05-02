#include "demo_net.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CB_DEMO_ERROR_FEATURE_NOT_PRESENT   (-8)
#define CB_DEMO_ERROR_INCOMPATIBLE_DRIVER   (-9)

typedef struct cb_demo_host_state {
    uint64_t next_instance;
    uint64_t next_device;
    uint64_t next_buffer;
    uint64_t next_fence;
} cb_demo_host_state_t;

static int reply_hello(int fd, uint32_t seq) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 64);
    cb_w_u32(&w, CB_PROTO_VERSION);
    cb_w_blob(&w, "CheeseBridgeFakeHost", strlen("CheeseBridgeFakeHost"));
    if (w.overflow) {
        cb_writer_dispose(&w);
        return -1;
    }
    int rc = cb_write_frame(fd, CB_OP_HELLO_REPLY, CB_FLAG_REPLY, seq,
                            w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return rc;
}

static int reply_capabilities(int fd, uint32_t seq) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 128);
    cb_w_blob(&w, "Vulkan", strlen("Vulkan"));
    cb_w_blob(&w, "fake-moltenvk-backend-placeholder",
              strlen("fake-moltenvk-backend-placeholder"));
    cb_w_u32(&w, 4096);
    cb_w_u32(&w, 1024);
    cb_w_u8(&w, 0);
    if (w.overflow) {
        cb_writer_dispose(&w);
        return -1;
    }
    int rc = cb_write_frame(fd, CB_OP_CAPABILITY_REPLY, CB_FLAG_REPLY, seq,
                            w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return rc;
}

static int reply_physical_devices(int fd, uint32_t seq) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 128);
    cb_w_u32(&w, 1);
    cb_w_u64(&w, 1);
    cb_w_blob(&w, "Apple GPU via CheeseBridge",
              strlen("Apple GPU via CheeseBridge"));
    cb_w_u32(&w, 1);
    if (w.overflow) {
        cb_writer_dispose(&w);
        return -1;
    }
    int rc = cb_write_frame(fd, CB_OP_GENERIC_REPLY, CB_FLAG_REPLY, seq,
                            w.buf, (uint32_t)w.pos);
    cb_writer_dispose(&w);
    return rc;
}

static bool handle_frame(int fd,
                         cb_demo_host_state_t *state,
                         const cb_frame_header_t *h,
                         const void *body) {
    cb_reader_t r;
    cb_reader_init(&r, body, h->length);

    printf("[host] <- %s seq=%u len=%u\n",
           cb_demo_opcode_name(h->opcode), h->sequence, h->length);

    switch (h->opcode) {
        case CB_OP_HELLO: {
            uint32_t version = cb_r_u32(&r);
            uint32_t guest_len = 0;
            const char *guest = (const char *)cb_r_blob(&r, &guest_len);
            printf("[host]    guest=%.*s protocol=%u\n",
                   (int)guest_len, guest ? guest : "", version);
            if (version != CB_PROTO_VERSION) {
                cb_demo_write_fail_reply(fd, h->sequence,
                                         CB_DEMO_ERROR_INCOMPATIBLE_DRIVER);
                return true;
            }
            reply_hello(fd, h->sequence);
            return true;
        }
        case CB_OP_CAPABILITY_QUERY:
            printf("[host]    backend=fake-moltenvk-backend-placeholder\n");
            reply_capabilities(fd, h->sequence);
            return true;
        case CB_OP_CREATE_INSTANCE: {
            uint32_t api_version = cb_r_u32(&r);
            uint32_t app_len = 0;
            const char *app = (const char *)cb_r_blob(&r, &app_len);
            uint64_t id = state->next_instance++;
            printf("[host]    app=%.*s api=0x%08x -> instance=%" PRIu64 "\n",
                   (int)app_len, app ? app : "", api_version, id);
            cb_demo_write_u64_reply(fd, CB_OP_GENERIC_REPLY, h->sequence, id);
            return true;
        }
        case CB_OP_ENUMERATE_PHYSICAL_DEVICES: {
            uint64_t instance_id = cb_r_u64(&r);
            printf("[host]    instance=%" PRIu64
                   " -> Apple GPU via CheeseBridge\n", instance_id);
            reply_physical_devices(fd, h->sequence);
            return true;
        }
        case CB_OP_CREATE_DEVICE: {
            uint64_t physical_device_id = cb_r_u64(&r);
            uint32_t queue_count = cb_r_u32(&r);
            printf("[host]    physical_device=%" PRIu64 " queues=", physical_device_id);
            for (uint32_t i = 0; i < queue_count; ++i) {
                uint32_t queue_len = 0;
                const char *queue = (const char *)cb_r_blob(&r, &queue_len);
                printf("%s%.*s", i ? "," : "", (int)queue_len, queue ? queue : "");
            }
            uint64_t id = state->next_device++;
            printf(" -> device=%" PRIu64 "\n", id);
            cb_demo_write_u64_reply(fd, CB_OP_GENERIC_REPLY, h->sequence, id);
            return true;
        }
        case CB_OP_CREATE_BUFFER: {
            uint64_t device_id = cb_r_u64(&r);
            uint64_t size = cb_r_u64(&r);
            uint32_t usage_len = 0;
            const char *usage = (const char *)cb_r_blob(&r, &usage_len);
            uint64_t id = state->next_buffer++;
            printf("[host]    device=%" PRIu64 " size=%" PRIu64
                   " usage=%.*s -> buffer=%" PRIu64 "\n",
                   device_id, size, (int)usage_len, usage ? usage : "", id);
            cb_demo_write_u64_reply(fd, CB_OP_GENERIC_REPLY, h->sequence, id);
            return true;
        }
        case CB_OP_QUEUE_SUBMIT: {
            uint64_t device_id = cb_r_u64(&r);
            uint32_t queue_len = 0;
            const char *queue = (const char *)cb_r_blob(&r, &queue_len);
            uint64_t command_buffer_id = cb_r_u64(&r);
            uint64_t fence_id = state->next_fence++;
            printf("[host]    device=%" PRIu64 " queue=%.*s command_buffer=%" PRIu64
                   " -> fence=%" PRIu64 "\n",
                   device_id, (int)queue_len, queue ? queue : "",
                   command_buffer_id, fence_id);
            cb_demo_write_u64_reply(fd, CB_OP_GENERIC_REPLY, h->sequence, fence_id);
            return true;
        }
        case CB_OP_QUEUE_PRESENT: {
            uint64_t swapchain_id = cb_r_u64(&r);
            uint64_t image_id = cb_r_u64(&r);
            printf("[host]    swapchain=%" PRIu64 " image=%" PRIu64 "\n",
                   swapchain_id, image_id);
            cb_demo_write_ok_reply(fd, h->sequence);
            return true;
        }
        case CB_OP_BYE:
            printf("[host]    guest closed session\n");
            return false;
        default:
            printf("[host]    unsupported opcode 0x%04x\n", h->opcode);
            cb_demo_write_fail_reply(fd, h->sequence,
                                     CB_DEMO_ERROR_FEATURE_NOT_PRESENT);
            return true;
    }
}

static void serve_client(int fd) {
    cb_demo_host_state_t state = {
        .next_instance = 1,
        .next_device = 1,
        .next_buffer = 1,
        .next_fence = 1,
    };

    for (;;) {
        cb_frame_header_t h;
        void *body = NULL;
        int rc = cb_read_frame(fd, &h, &body);
        if (rc != 0) {
            if (rc == -1) printf("[host] client disconnected\n");
            else printf("[host] read_frame failed: %d\n", rc);
            free(body);
            break;
        }

        bool keep_going = handle_frame(fd, &state, &h, body);
        free(body);
        fflush(stdout);
        if (!keep_going) break;
    }
}

int main(int argc, char **argv) {
    cb_demo_ignore_sigpipe();

    const char *endpoint = argc > 1 ? argv[1] : getenv("CHEESEBRIDGE_LISTEN");
    if (!endpoint || !*endpoint) endpoint = CB_DEMO_DEFAULT_ENDPOINT;

    int listen_fd = cb_demo_listen(endpoint);
    if (listen_fd < 0) return 1;

    printf("[host] CheeseBridge fake host listening on %s\n", endpoint);
    printf("[host] run guest with: CHEESEBRIDGE_HOST=%s cheesebridge_guest_demo\n",
           endpoint);
    fflush(stdout);

    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        printf("[host] client connected\n");
        serve_client(client_fd);
        close(client_fd);
    }

    close(listen_fd);
    return 0;
}
