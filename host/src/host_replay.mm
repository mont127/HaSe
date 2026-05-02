/*
 * CheeseBridge host: command-stream replay.
 *
 * Phase 3 stub. The guest serializes a vkCmd* sequence into an opaque byte
 * blob; this function will decode it and re-issue the calls against the host
 * VkCommandBuffer via MoltenVK. For now we accept the bytes and return OK
 * so the surrounding plumbing (record/end/submit) exercises end-to-end.
 *
 * Phase 4 replaces this with the real decoder.
 */

#include "host.h"

VkResult host_replay_command_stream(host_device_rec_t *dev,
                                    VkCommandBuffer cb,
                                    const void *bytes, uint32_t len) {
    (void)dev; (void)cb; (void)bytes; (void)len;
    return VK_SUCCESS;
}
