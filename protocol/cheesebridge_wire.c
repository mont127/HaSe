#include "cheesebridge_wire.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* ----- Socket I/O ---------------------------------------------------------- */

int cb_read_full(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    size_t left = n;
    while (left) {
        ssize_t r = recv(fd, p, left, 0);
        if (r > 0) { p += r; left -= (size_t)r; continue; }
        if (r == 0) return -1;                   /* clean EOF */
        if (errno == EINTR) continue;
        return -2;
    }
    return 0;
}

int cb_write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = n;
    while (left) {
        ssize_t w = send(fd, p, left,
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
#else
                         0
#endif
                         );
        if (w >= 0) { p += w; left -= (size_t)w; continue; }
        if (errno == EINTR) continue;
        return -2;
    }
    return 0;
}

int cb_read_frame(int fd, cb_frame_header_t *out_header, void **out_payload) {
    uint8_t hdr[CB_HEADER_SIZE];
    int rc = cb_read_full(fd, hdr, sizeof hdr);
    if (rc != 0) return rc;

    cb_reader_t r;
    cb_reader_init(&r, hdr, sizeof hdr);
    cb_frame_header_t h;
    h.magic    = cb_r_u32(&r);
    h.length   = cb_r_u32(&r);
    h.opcode   = cb_r_u16(&r);
    h.flags    = cb_r_u16(&r);
    h.sequence = cb_r_u32(&r);

    if (h.magic != CB_PROTO_MAGIC)         return -2;
    if (h.length > CB_MAX_FRAME_BYTES)     return -2;

    void *payload = NULL;
    if (h.length) {
        payload = malloc(h.length);
        if (!payload) return -2;
        rc = cb_read_full(fd, payload, h.length);
        if (rc != 0) { free(payload); return rc; }
    }

    *out_header  = h;
    *out_payload = payload;
    return 0;
}

int cb_write_frame(int fd, uint16_t opcode, uint16_t flags, uint32_t sequence,
                   const void *payload, uint32_t len) {
    uint8_t hdr[CB_HEADER_SIZE];
    cb_writer_t w;
    cb_writer_init_fixed(&w, hdr, sizeof hdr);
    cb_w_u32(&w, CB_PROTO_MAGIC);
    cb_w_u32(&w, len);
    cb_w_u16(&w, opcode);
    cb_w_u16(&w, flags);
    cb_w_u32(&w, sequence);

    if (len == 0) return cb_write_full(fd, hdr, sizeof hdr);

    struct iovec iov[2] = {
        { .iov_base = hdr,                .iov_len = sizeof hdr },
        { .iov_base = (void *)payload,    .iov_len = len        },
    };
    size_t total = sizeof hdr + len;
    size_t sent  = 0;
    int    iidx  = 0;

    while (sent < total) {
        ssize_t n = writev(fd, &iov[iidx], 2 - iidx);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -2;
        }
        sent += (size_t)n;
        /* advance iov windows */
        size_t rem = (size_t)n;
        while (rem && iidx < 2) {
            if (rem >= iov[iidx].iov_len) {
                rem -= iov[iidx].iov_len;
                iidx++;
            } else {
                iov[iidx].iov_base = (uint8_t *)iov[iidx].iov_base + rem;
                iov[iidx].iov_len -= rem;
                rem = 0;
            }
        }
    }
    return 0;
}

/* ----- Writer -------------------------------------------------------------- */

void cb_writer_init_heap(cb_writer_t *w, size_t initial_cap) {
    if (initial_cap < 64) initial_cap = 64;
    w->buf = (uint8_t *)malloc(initial_cap);
    w->cap = w->buf ? initial_cap : 0;
    w->pos = 0;
    w->owns = true;
    w->overflow = (w->buf == NULL);
}

void cb_writer_init_fixed(cb_writer_t *w, void *buf, size_t cap) {
    w->buf = (uint8_t *)buf;
    w->cap = cap;
    w->pos = 0;
    w->owns = false;
    w->overflow = false;
}

void cb_writer_dispose(cb_writer_t *w) {
    if (w->owns && w->buf) free(w->buf);
    w->buf = NULL;
    w->cap = w->pos = 0;
}

bool cb_writer_reserve(cb_writer_t *w, size_t additional) {
    if (w->overflow) return false;
    size_t need = w->pos + additional;
    if (need <= w->cap) return true;
    if (!w->owns) { w->overflow = true; return false; }
    size_t cap = w->cap ? w->cap : 64;
    while (cap < need) cap *= 2;
    uint8_t *nb = (uint8_t *)realloc(w->buf, cap);
    if (!nb) { w->overflow = true; return false; }
    w->buf = nb;
    w->cap = cap;
    return true;
}

#define CB_WRITE_INT(w, v, type)                      \
    do {                                              \
        if (!cb_writer_reserve((w), sizeof(type))) return; \
        type _v = (type)(v);                          \
        memcpy((w)->buf + (w)->pos, &_v, sizeof _v);  \
        (w)->pos += sizeof _v;                        \
    } while (0)

void cb_w_u8 (cb_writer_t *w, uint8_t  v) { CB_WRITE_INT(w, v, uint8_t);  }
void cb_w_u16(cb_writer_t *w, uint16_t v) { CB_WRITE_INT(w, v, uint16_t); }
void cb_w_u32(cb_writer_t *w, uint32_t v) { CB_WRITE_INT(w, v, uint32_t); }
void cb_w_u64(cb_writer_t *w, uint64_t v) { CB_WRITE_INT(w, v, uint64_t); }
void cb_w_i32(cb_writer_t *w, int32_t  v) { CB_WRITE_INT(w, v, int32_t);  }
void cb_w_f32(cb_writer_t *w, float    v) { CB_WRITE_INT(w, v, float);    }

void cb_w_bytes(cb_writer_t *w, const void *src, size_t n) {
    if (!cb_writer_reserve(w, n)) return;
    if (n) memcpy(w->buf + w->pos, src, n);
    w->pos += n;
}

void cb_w_blob(cb_writer_t *w, const void *src, size_t n) {
    cb_w_u32(w, (uint32_t)n);
    cb_w_bytes(w, src, n);
}

void cb_w_opt_blob(cb_writer_t *w, const void *src, size_t n) {
    if (!src) { cb_w_u32(w, 0xFFFFFFFFu); return; }
    cb_w_blob(w, src, n);
}

/* ----- Reader -------------------------------------------------------------- */

void cb_reader_init(cb_reader_t *r, const void *buf, size_t cap) {
    r->buf = (const uint8_t *)buf;
    r->cap = cap;
    r->pos = 0;
    r->overflow = false;
}

bool cb_reader_eof(const cb_reader_t *r) { return r->pos >= r->cap; }

#define CB_READ_INT(r, type)                                  \
    ({                                                        \
        type _v = 0;                                          \
        if ((r)->pos + sizeof(type) > (r)->cap) {             \
            (r)->overflow = true;                             \
        } else {                                              \
            memcpy(&_v, (r)->buf + (r)->pos, sizeof _v);      \
            (r)->pos += sizeof _v;                            \
        }                                                     \
        _v;                                                   \
    })

uint8_t  cb_r_u8 (cb_reader_t *r) { return CB_READ_INT(r, uint8_t);  }
uint16_t cb_r_u16(cb_reader_t *r) { return CB_READ_INT(r, uint16_t); }
uint32_t cb_r_u32(cb_reader_t *r) { return CB_READ_INT(r, uint32_t); }
uint64_t cb_r_u64(cb_reader_t *r) { return CB_READ_INT(r, uint64_t); }
int32_t  cb_r_i32(cb_reader_t *r) { return CB_READ_INT(r, int32_t);  }
float    cb_r_f32(cb_reader_t *r) { return CB_READ_INT(r, float);    }

const void *cb_r_bytes(cb_reader_t *r, size_t n) {
    if (r->pos + n > r->cap) { r->overflow = true; return NULL; }
    const void *p = r->buf + r->pos;
    r->pos += n;
    return p;
}

const void *cb_r_blob(cb_reader_t *r, uint32_t *out_len) {
    uint32_t n = cb_r_u32(r);
    if (out_len) *out_len = n;
    if (n == 0) return r->buf + r->pos;
    return cb_r_bytes(r, n);
}

const void *cb_r_opt_blob(cb_reader_t *r, uint32_t *out_len) {
    uint32_t n = cb_r_u32(r);
    if (n == 0xFFFFFFFFu) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (out_len) *out_len = n;
    if (n == 0) return r->buf + r->pos;
    return cb_r_bytes(r, n);
}
