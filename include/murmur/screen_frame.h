#ifndef DIRECT_SCREEN_FRAME_H
#define DIRECT_SCREEN_FRAME_H
#include <stdint.h>
#include <stddef.h>
#define DIRECT_SCREEN_CHUNK 984u
#define DIRECT_SCREEN_MAX 64944u
#define DIRECT_SCREEN_PARTS 66u
typedef struct {
    uint32_t id, total;
    unsigned count, received;
    uint64_t started;
    uint8_t seen[DIRECT_SCREEN_PARTS], bytes[DIRECT_SCREEN_MAX];
} direct_screen_frame;
/* Strictly bounded latest-frame assembly. 1 complete, 2 stop, 0 partial/drop.
 * Input must already be authenticated. No allocation. */
int direct_screen_feed(direct_screen_frame *frame, const uint8_t *packet, size_t size, uint64_t now);
size_t direct_screen_packet(uint8_t packet[1000], uint32_t id, const uint8_t *jpeg, size_t total, unsigned part);
#endif
