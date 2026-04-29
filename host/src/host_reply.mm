#include "host.h"

#include <stdlib.h>
#include <string.h>

static void send_frame(host_conn_t *c, uint16_t op, uint16_t flags,
                       uint32_t seq, const void *body, uint32_t len) {
    pthread_mutex_lock(&c->write_lock);
    cb_write_frame(c->fd, op, flags, seq, body, len);
    pthread_mutex_unlock(&c->write_lock);
}

void host_reply_ok(host_conn_t *c, uint32_t seq) {
    send_frame(c, CB_OP_GENERIC_REPLY, CB_FLAG_REPLY, seq, NULL, 0);
}

void host_reply_id(host_conn_t *c, uint32_t seq, cb_remote_id_t id) {
    uint8_t buf[8]; memcpy(buf, &id, sizeof id);
    send_frame(c, CB_OP_GENERIC_REPLY, CB_FLAG_REPLY, seq, buf, sizeof buf);
}

void host_reply_bytes(host_conn_t *c, uint32_t seq,
                      const void *bytes, uint32_t n) {
    send_frame(c, CB_OP_GENERIC_REPLY, CB_FLAG_REPLY, seq, bytes, n);
}

void host_reply_writer(host_conn_t *c, uint32_t seq, cb_writer_t *w) {
    send_frame(c, CB_OP_GENERIC_REPLY, CB_FLAG_REPLY, seq,
               w->buf, (uint32_t)w->pos);
}

void host_reply_fail(host_conn_t *c, uint32_t seq, VkResult vr) {
    int32_t code = (int32_t)vr;
    send_frame(c, CB_OP_FAIL_REPLY, CB_FLAG_REPLY, seq, &code, sizeof code);
}
