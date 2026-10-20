#include "murmur/frame.h"
#include <string.h>

static size_t limit_for(murmur_kind kind) {
    switch (kind) {
        case MURMUR_TEXT: return MURMUR_TEXT_LIMIT;
        case MURMUR_CONTROL: return MURMUR_CONTROL_LIMIT;
        case MURMUR_AUDIO: return MURMUR_AUDIO_LIMIT;
        default: return 0;
    }
}

murmur_result murmur_frame_decode(const uint8_t *input, size_t size,
                                  murmur_frame *frame, size_t *consumed) {
    uint32_t length;
    size_t limit;
    murmur_kind kind;
    if (consumed) *consumed = 0;
    if (frame) memset(frame, 0, sizeof(*frame));
    if (!frame || !consumed || (!input && size)) return MURMUR_INVALID;
    if (size < MURMUR_HEADER_SIZE) return MURMUR_INCOMPLETE;
    if (memcmp(input, "MRMR", 4) != 0 || input[4] != 1 ||
        input[6] != 0 || input[7] != 0) return MURMUR_INVALID;
    kind = (murmur_kind)input[5];
    limit = limit_for(kind);
    length = ((uint32_t)input[8] << 24) | ((uint32_t)input[9] << 16) |
             ((uint32_t)input[10] << 8) | (uint32_t)input[11];
    /* Reject untrusted lengths before buffering or allocating payload bytes. */
    if (!limit || length == 0 || length > limit) return MURMUR_INVALID;
    if (size - MURMUR_HEADER_SIZE < length) return MURMUR_INCOMPLETE;
    frame->kind = kind;
    frame->payload = input + MURMUR_HEADER_SIZE;
    frame->payload_size = length;
    *consumed = MURMUR_HEADER_SIZE + length;
    return MURMUR_OK;
}

murmur_result murmur_frame_encode(murmur_kind kind, const uint8_t *payload,
                                  size_t payload_size, uint8_t *output,
                                  size_t capacity, size_t *written) {
    size_t limit = limit_for(kind);
    uint32_t length;
    if (written) *written = 0;
    if (!written || !output || !payload || !limit ||
        !payload_size || payload_size > limit) return MURMUR_INVALID;
    if (capacity < MURMUR_HEADER_SIZE ||
        capacity - MURMUR_HEADER_SIZE < payload_size) return MURMUR_NO_SPACE;
    length = (uint32_t)payload_size;
    memcpy(output, "MRMR", 4);
    output[4] = 1;
    output[5] = (uint8_t)kind;
    output[6] = output[7] = 0;
    output[8] = (uint8_t)(length >> 24);
    output[9] = (uint8_t)(length >> 16);
    output[10] = (uint8_t)(length >> 8);
    output[11] = (uint8_t)length;
    memcpy(output + MURMUR_HEADER_SIZE, payload, payload_size);
    *written = MURMUR_HEADER_SIZE + payload_size;
    return MURMUR_OK;
}
