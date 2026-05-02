#include "demo_net.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct cb_demo_client {
    int fd;
    uint32_t next_seq;
} cb_demo_client_t;

static int read_reply(cb_demo_client_t *client,
                      uint32_t expected_seq,
                      cb_frame_header_t *out_header,
                      void **out_body) {
    int rc = cb_read_frame(client->fd, out_header, out_body);
    if (rc != 0) {
        fprintf(stderr, "[guest] read reply failed: %d\n", rc);
        return -1;
    }
    if (!(out_header->flags & CB_FLAG_REPLY) ||
        out_header->sequence != expected_seq) {
        fprintf(stderr,
                "[guest] bad reply: opcode=%s seq=%u flags=0x%04x expected_seq=%u\n",
                cb_demo_opcode_name(out_header->opcode),
                out_header->sequence,
                out_header->flags,
                expected_seq);
        free(*out_body);
        *out_body = NULL;
        return -1;
    }
    if (out_header->opcode == CB_OP_FAIL_REPLY) {
        cb_reader_t r;
        cb_reader_init(&r, *out_body, out_header->length);
        int32_t result = cb_r_i32(&r);
        fprintf(stderr, "[guest] host returned failure result=%d\n", result);
        free(*out_body);
        *out_body = NULL;
        return -1;
    }
    return 0;
}

static int rpc(cb_demo_client_t *client,
               uint16_t opcode,
               const void *payload,
               uint32_t payload_len,
               cb_frame_header_t *out_header,
               void **out_body) {
    uint32_t seq = client->next_seq++;
    printf("[guest] -> %s seq=%u len=%u\n",
           cb_demo_opcode_name(opcode), seq, payload_len);
    if (cb_write_frame(client->fd, opcode, 0, seq, payload, payload_len) != 0) {
        perror("write_frame");
        return -1;
    }
    if (read_reply(client, seq, out_header, out_body) != 0) return -1;
    printf("[guest] <- %s seq=%u len=%u\n",
           cb_demo_opcode_name(out_header->opcode),
           out_header->sequence,
           out_header->length);
    return 0;
}

static int rpc_writer(cb_demo_client_t *client,
                      uint16_t opcode,
                      cb_writer_t *writer,
                      cb_frame_header_t *out_header,
                      void **out_body) {
    if (writer->overflow) {
        fprintf(stderr, "[guest] payload build overflow for %s\n",
                cb_demo_opcode_name(opcode));
        return -1;
    }
    return rpc(client, opcode, writer->buf, (uint32_t)writer->pos,
               out_header, out_body);
}

static int rpc_expect_id(cb_demo_client_t *client,
                         uint16_t opcode,
                         cb_writer_t *writer,
                         uint64_t *out_id) {
    cb_frame_header_t h;
    void *body = NULL;
    if (rpc_writer(client, opcode, writer, &h, &body) != 0) return -1;
    if (h.opcode != CB_OP_GENERIC_REPLY || h.length < sizeof(uint64_t)) {
        fprintf(stderr, "[guest] expected id reply for %s\n",
                cb_demo_opcode_name(opcode));
        free(body);
        return -1;
    }

    cb_reader_t r;
    cb_reader_init(&r, body, h.length);
    *out_id = cb_r_u64(&r);
    free(body);
    return 0;
}

static int do_handshake(cb_demo_client_t *client) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 64);
    cb_w_u32(&w, CB_PROTO_VERSION);
    cb_w_blob(&w, "CheeseBridgeGuestDemo", strlen("CheeseBridgeGuestDemo"));

    cb_frame_header_t h;
    void *body = NULL;
    int rc = rpc_writer(client, CB_OP_HELLO, &w, &h, &body);
    cb_writer_dispose(&w);
    if (rc != 0) return -1;

    cb_reader_t r;
    cb_reader_init(&r, body, h.length);
    uint32_t version = cb_r_u32(&r);
    uint32_t host_len = 0;
    const char *host = (const char *)cb_r_blob(&r, &host_len);
    printf("[guest]    host=%.*s protocol=%u\n",
           (int)host_len, host ? host : "", version);
    free(body);

    if (h.opcode != CB_OP_HELLO_REPLY || version != CB_PROTO_VERSION) {
        fprintf(stderr, "[guest] incompatible host protocol\n");
        return -1;
    }
    return 0;
}

static int do_capability_query(cb_demo_client_t *client) {
    cb_frame_header_t h;
    void *body = NULL;
    if (rpc(client, CB_OP_CAPABILITY_QUERY, NULL, 0, &h, &body) != 0) return -1;
    if (h.opcode != CB_OP_CAPABILITY_REPLY) {
        fprintf(stderr, "[guest] expected capability reply\n");
        free(body);
        return -1;
    }

    cb_reader_t r;
    cb_reader_init(&r, body, h.length);
    uint32_t api_len = 0;
    const char *api = (const char *)cb_r_blob(&r, &api_len);
    uint32_t backend_len = 0;
    const char *backend = (const char *)cb_r_blob(&r, &backend_len);
    uint32_t max_buffers = cb_r_u32(&r);
    uint32_t max_images = cb_r_u32(&r);
    uint8_t supports_present = cb_r_u8(&r);

    printf("[guest]    api=%.*s backend=%.*s max_buffers=%u max_images=%u present=%s\n",
           (int)api_len, api ? api : "",
           (int)backend_len, backend ? backend : "",
           max_buffers, max_images, supports_present ? "yes" : "no");
    free(body);
    return r.overflow ? -1 : 0;
}

static int create_instance(cb_demo_client_t *client, uint64_t *out_instance) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 64);
    cb_w_u32(&w, CB_DEMO_API_VERSION_1_2);
    cb_w_blob(&w, "cheesebridge-phase1-demo",
              strlen("cheesebridge-phase1-demo"));
    int rc = rpc_expect_id(client, CB_OP_CREATE_INSTANCE, &w, out_instance);
    cb_writer_dispose(&w);
    if (rc == 0) printf("[guest]    instance_id=%" PRIu64 "\n", *out_instance);
    return rc;
}

static int enumerate_devices(cb_demo_client_t *client,
                             uint64_t instance_id,
                             uint64_t *out_physical_device) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 16);
    cb_w_u64(&w, instance_id);

    cb_frame_header_t h;
    void *body = NULL;
    int rc = rpc_writer(client, CB_OP_ENUMERATE_PHYSICAL_DEVICES, &w, &h, &body);
    cb_writer_dispose(&w);
    if (rc != 0) return -1;

    cb_reader_t r;
    cb_reader_init(&r, body, h.length);
    uint32_t count = cb_r_u32(&r);
    if (count == 0) {
        fprintf(stderr, "[guest] no physical devices returned\n");
        free(body);
        return -1;
    }

    *out_physical_device = cb_r_u64(&r);
    uint32_t name_len = 0;
    const char *name = (const char *)cb_r_blob(&r, &name_len);
    uint32_t type = cb_r_u32(&r);
    printf("[guest]    physical_device_id=%" PRIu64 " name=%.*s type=%u\n",
           *out_physical_device, (int)name_len, name ? name : "", type);
    free(body);
    return r.overflow ? -1 : 0;
}

static int create_device(cb_demo_client_t *client,
                         uint64_t physical_device_id,
                         uint64_t *out_device) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 64);
    cb_w_u64(&w, physical_device_id);
    cb_w_u32(&w, 2);
    cb_w_blob(&w, "graphics", strlen("graphics"));
    cb_w_blob(&w, "present", strlen("present"));
    int rc = rpc_expect_id(client, CB_OP_CREATE_DEVICE, &w, out_device);
    cb_writer_dispose(&w);
    if (rc == 0) printf("[guest]    device_id=%" PRIu64 "\n", *out_device);
    return rc;
}

static int create_buffer(cb_demo_client_t *client,
                         uint64_t device_id,
                         uint64_t *out_buffer) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 64);
    cb_w_u64(&w, device_id);
    cb_w_u64(&w, 1048576);
    cb_w_blob(&w, "vertex_buffer", strlen("vertex_buffer"));
    int rc = rpc_expect_id(client, CB_OP_CREATE_BUFFER, &w, out_buffer);
    cb_writer_dispose(&w);
    if (rc == 0) printf("[guest]    buffer_id=%" PRIu64 "\n", *out_buffer);
    return rc;
}

static int queue_submit(cb_demo_client_t *client,
                        uint64_t device_id,
                        uint64_t *out_fence) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 64);
    cb_w_u64(&w, device_id);
    cb_w_blob(&w, "graphics", strlen("graphics"));
    cb_w_u64(&w, 1);
    int rc = rpc_expect_id(client, CB_OP_QUEUE_SUBMIT, &w, out_fence);
    cb_writer_dispose(&w);
    if (rc == 0) printf("[guest]    fence_id=%" PRIu64 "\n", *out_fence);
    return rc;
}

static int present(cb_demo_client_t *client) {
    cb_writer_t w;
    cb_writer_init_heap(&w, 32);
    cb_w_u64(&w, 1);
    cb_w_u64(&w, 1);

    cb_frame_header_t h;
    void *body = NULL;
    int rc = rpc_writer(client, CB_OP_QUEUE_PRESENT, &w, &h, &body);
    cb_writer_dispose(&w);
    free(body);
    if (rc == 0) printf("[guest]    present_reply=OK\n");
    return rc;
}

int main(int argc, char **argv) {
    cb_demo_ignore_sigpipe();

    const char *endpoint = argc > 1 ? argv[1] : getenv("CHEESEBRIDGE_HOST");
    if (!endpoint || !*endpoint) endpoint = CB_DEMO_DEFAULT_ENDPOINT;

    int fd = cb_demo_connect(endpoint);
    if (fd < 0) return 1;

    cb_demo_client_t client = {
        .fd = fd,
        .next_seq = 1,
    };

    printf("[guest] connected to %s\n", endpoint);

    uint64_t instance_id = 0;
    uint64_t physical_device_id = 0;
    uint64_t device_id = 0;
    uint64_t buffer_id = 0;
    uint64_t fence_id = 0;

    int rc = 0;
    rc |= do_handshake(&client);
    rc |= do_capability_query(&client);
    rc |= create_instance(&client, &instance_id);
    rc |= enumerate_devices(&client, instance_id, &physical_device_id);
    rc |= create_device(&client, physical_device_id, &device_id);
    rc |= create_buffer(&client, device_id, &buffer_id);
    rc |= queue_submit(&client, device_id, &fence_id);
    rc |= present(&client);

    (void)buffer_id;
    (void)fence_id;

    cb_write_frame(fd, CB_OP_BYE, CB_FLAG_ASYNC, 0, NULL, 0);
    close(fd);

    if (rc != 0) {
        fprintf(stderr, "[guest] demo failed\n");
        return 1;
    }

    printf("[guest] demo completed successfully\n");
    return 0;
}
