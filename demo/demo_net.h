#ifndef CHEESEBRIDGE_DEMO_NET_H
#define CHEESEBRIDGE_DEMO_NET_H

#include "cheesebridge_wire.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CB_DEMO_DEFAULT_ENDPOINT "tcp:127.0.0.1:43210"
#define CB_DEMO_API_VERSION_1_2  4202496u

int cb_demo_connect(const char *spec);
int cb_demo_listen(const char *spec);

const char *cb_demo_opcode_name(uint16_t opcode);
void cb_demo_ignore_sigpipe(void);

int cb_demo_write_u64_reply(int fd, uint16_t opcode, uint32_t seq, uint64_t id);
int cb_demo_write_ok_reply(int fd, uint32_t seq);
int cb_demo_write_fail_reply(int fd, uint32_t seq, int32_t result);

#ifdef __cplusplus
}
#endif

#endif /* CHEESEBRIDGE_DEMO_NET_H */
