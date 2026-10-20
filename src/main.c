#include "murmur/frame.h"
#include <stdio.h>

int main(void) {
    static const uint8_t message[] = "Bonjour depuis Murmur";
    uint8_t buffer[128];
    size_t written = 0, consumed = 0;
    murmur_frame frame;
    puts("Murmur 0.1 - demonstration locale du format de messages");
    puts("Aucune connexion reseau ni protection cryptographique active.");
    if (murmur_frame_encode(MURMUR_TEXT, message, sizeof(message) - 1,
                            buffer, sizeof(buffer), &written) != MURMUR_OK ||
        murmur_frame_decode(buffer, written, &frame, &consumed) != MURMUR_OK)
        return 1;
    printf("Message verifie (%zu octets): ", consumed);
    if (fwrite(frame.payload, 1, frame.payload_size, stdout) != frame.payload_size)
        return 1;
    putchar('\n');
    return 0;
}
