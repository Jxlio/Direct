#ifndef MURMUR_FRAME_H
#define MURMUR_FRAME_H
#include <stddef.h>
#include <stdint.h>

/* Framing for an already authenticated, decrypted, reliable byte stream.
 * This module supplies neither encryption nor peer authentication.
 * Returned payload views borrow the input buffer; retain it until consumed. */
#define MURMUR_HEADER_SIZE 12u
#define MURMUR_TEXT_LIMIT 16384u
#define MURMUR_CONTROL_LIMIT 4096u
#define MURMUR_AUDIO_LIMIT 404u
typedef enum { MURMUR_TEXT = 1, MURMUR_CONTROL = 2, MURMUR_AUDIO = 3 } murmur_kind;
typedef enum {
    MURMUR_OK = 0, MURMUR_INCOMPLETE, MURMUR_INVALID, MURMUR_NO_SPACE
} murmur_result;
typedef struct {
    murmur_kind kind;
    const uint8_t *payload;
    size_t payload_size;
} murmur_frame;

/* On failure, consumed/written is zero and no frame is exposed.
 * NULL input is allowed only for an empty buffer. Output pointers required.
 * Encoder source and destination must not overlap. */
murmur_result murmur_frame_decode(const uint8_t *input, size_t size,
                                  murmur_frame *frame, size_t *consumed);
murmur_result murmur_frame_encode(murmur_kind kind, const uint8_t *payload,
                                  size_t payload_size, uint8_t *output,
                                  size_t capacity, size_t *written);
#endif
