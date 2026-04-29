/*
 * CheeseBridge wire helpers: frame I/O and a tiny TLV cursor used by both
 * the guest ICD and the macOS host. Network byte order is little-endian
 * (we only target ARM64 and x86_64 hosts/guests).
 */
#ifndef CHEESEBRIDGE_WIRE_H
#define CHEESEBRIDGE_WIRE_H

#include "cheesebridge_proto.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Socket I/O ---------------------------------------------------------- */

/*
 * Read exactly `n` bytes from a blocking socket. Retries on EINTR.
 * Returns 0 on success, -1 on EOF, -2 on other error (errno set).
 */
int  cb_read_full(int fd, void *buf, size_t n);

/* Same shape as cb_read_full but writing. */
int  cb_write_full(int fd, const void *buf, size_t n);

/*
 * Read one frame: header + payload. `*out_payload` is a heap buffer the caller
 * must free(). On success returns 0 and fills `*out_header` and `*out_payload`.
 */
int  cb_read_frame(int fd, cb_frame_header_t *out_header, void **out_payload);

/*
 * Write one frame in a single writev call when possible. The header's `magic`
 * and `length` fields are filled in for you; pass payload + len + opcode +
 * flags + sequence.
 */
int  cb_write_frame(int fd, uint16_t opcode, uint16_t flags, uint32_t sequence,
                    const void *payload, uint32_t len);

/* ----- Cursor: forward-only writer ----------------------------------------- */

typedef struct cb_writer {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    bool     owns;       /* true if `buf` is heap-owned and must be free()d */
    bool     overflow;   /* sticky: any write past capacity sets this */
} cb_writer_t;

void cb_writer_init_heap(cb_writer_t *w, size_t initial_cap);
void cb_writer_init_fixed(cb_writer_t *w, void *buf, size_t cap);
void cb_writer_dispose(cb_writer_t *w);
bool cb_writer_reserve(cb_writer_t *w, size_t additional);

void cb_w_u8 (cb_writer_t *w, uint8_t  v);
void cb_w_u16(cb_writer_t *w, uint16_t v);
void cb_w_u32(cb_writer_t *w, uint32_t v);
void cb_w_u64(cb_writer_t *w, uint64_t v);
void cb_w_i32(cb_writer_t *w, int32_t  v);
void cb_w_f32(cb_writer_t *w, float    v);
void cb_w_bytes(cb_writer_t *w, const void *src, size_t n);
/* length-prefixed (u32 length, then bytes) */
void cb_w_blob(cb_writer_t *w, const void *src, size_t n);
/* nullable blob: writes u32 = 0xFFFFFFFF when src == NULL */
void cb_w_opt_blob(cb_writer_t *w, const void *src, size_t n);

/* ----- Cursor: forward-only reader ----------------------------------------- */

typedef struct cb_reader {
    const uint8_t *buf;
    size_t         cap;
    size_t         pos;
    bool           overflow;
} cb_reader_t;

void cb_reader_init(cb_reader_t *r, const void *buf, size_t cap);
bool cb_reader_eof(const cb_reader_t *r);

uint8_t  cb_r_u8 (cb_reader_t *r);
uint16_t cb_r_u16(cb_reader_t *r);
uint32_t cb_r_u32(cb_reader_t *r);
uint64_t cb_r_u64(cb_reader_t *r);
int32_t  cb_r_i32(cb_reader_t *r);
float    cb_r_f32(cb_reader_t *r);
/* Returns a pointer into the reader's buffer (no copy). */
const void *cb_r_bytes(cb_reader_t *r, size_t n);
const void *cb_r_blob (cb_reader_t *r, uint32_t *out_len);
/* Same as cb_r_blob but returns NULL when length sentinel is 0xFFFFFFFF. */
const void *cb_r_opt_blob(cb_reader_t *r, uint32_t *out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHEESEBRIDGE_WIRE_H */
