#include "screen.h"
#include <stdio.h>
int main(void) {
    int result=screen_codec_test();
    if(!result) puts("Native JPEG encode/decode and oversized image rejection passed; no screen captured.");
    return result;
}
