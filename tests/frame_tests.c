#include "murmur/frame.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    uint8_t buffer[MURMUR_HEADER_SIZE + MURMUR_TEXT_LIMIT];
    uint8_t payload[MURMUR_TEXT_LIMIT];
    size_t written, used, i;
    murmur_frame frame;
    memset(payload, 'a', sizeof(payload));
    CHECK(murmur_frame_encode(MURMUR_TEXT, payload, sizeof(payload), buffer,
                              sizeof(buffer), &written) == MURMUR_OK);
    for (i = 0; i < written; ++i) {
        CHECK(murmur_frame_decode(buffer, i, &frame, &used) == MURMUR_INCOMPLETE);
        CHECK(used == 0 && frame.payload == NULL);
    }
    CHECK(murmur_frame_decode(buffer, written, &frame, &used) == MURMUR_OK);
    CHECK(used == written && frame.payload_size == sizeof(payload));
    CHECK(memcmp(frame.payload, payload, sizeof(payload)) == 0);
    CHECK(murmur_frame_encode(MURMUR_TEXT, payload, sizeof(payload) + 1,
                              buffer, sizeof(buffer), &used) == MURMUR_INVALID);
    CHECK(murmur_frame_encode(MURMUR_CONTROL, payload, MURMUR_CONTROL_LIMIT + 1,
                              buffer, sizeof(buffer), &used) == MURMUR_INVALID);
    CHECK(murmur_frame_encode(MURMUR_TEXT, payload, 1, buffer,
                              MURMUR_HEADER_SIZE, &used) == MURMUR_NO_SPACE);
    CHECK(murmur_frame_encode(MURMUR_TEXT, payload, 0, buffer,
                              sizeof(buffer), &used) == MURMUR_INVALID);
    buffer[8] = buffer[9] = buffer[10] = buffer[11] = 255;
    CHECK(murmur_frame_decode(buffer, MURMUR_HEADER_SIZE, &frame, &used) == MURMUR_INVALID);
    CHECK(used == 0 && frame.payload == NULL);
    for (i = 0; i < 8; ++i) {
        CHECK(murmur_frame_encode(MURMUR_TEXT, payload, 1, buffer,
                                  sizeof(buffer), &written) == MURMUR_OK);
        buffer[i] ^= 128;
        CHECK(murmur_frame_decode(buffer, written, &frame, &used) == MURMUR_INVALID);
    }
    CHECK(murmur_frame_decode(NULL, 0, &frame, &used) == MURMUR_INCOMPLETE);
    CHECK(murmur_frame_decode(NULL, 1, &frame, &used) == MURMUR_INVALID);
    CHECK(murmur_frame_decode(buffer, written, NULL, &used) == MURMUR_INVALID);
    /* Two concatenated frames: consume only one, then advance the view. */
    CHECK(murmur_frame_encode(MURMUR_TEXT, payload, 1, buffer, sizeof(buffer), &written) == MURMUR_OK);
    CHECK(murmur_frame_encode(MURMUR_CONTROL, payload, 1, buffer + written,
                              sizeof(buffer) - written, &used) == MURMUR_OK);
    CHECK(murmur_frame_decode(buffer, written + used, &frame, &i) == MURMUR_OK);
    CHECK(i == written && frame.kind == MURMUR_TEXT);
    CHECK(murmur_frame_decode(buffer + i, used, &frame, &written) == MURMUR_OK);
    CHECK(frame.kind == MURMUR_CONTROL && written == used);
    puts("Frame boundary tests passed.");
    return 0;
}
