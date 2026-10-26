#include "identity_store.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    WCHAR temp[MAX_PATH], path[MAX_PATH];
    uint8_t first[MURMUR_ID_PUBLIC], signature[MURMUR_ID_SIGNATURE];
    const uint8_t message[] = "hello";
    char fingerprint[MURMUR_ID_FINGERPRINT];
    HANDLE file;
    DWORD written;
    CHECK(GetTempPathW(MAX_PATH, temp) && GetTempFileNameW(temp, L"mid", 0, path) && DeleteFileW(path));
    /* First use creates the identity; a second open reads the same one back. */
    CHECK(!identity_store_open(path) && identity_store_public());
    memcpy(first, identity_store_public(), sizeof(first)); strcpy_s(fingerprint, sizeof(fingerprint), identity_store_fingerprint());
    CHECK(!identity_store_sign(message, sizeof(message), signature) && !murmur_identity_verify(first, message, sizeof(message), signature));
    CHECK(identity_store_open(path)); /* already open */
    identity_store_close();
    CHECK(!identity_store_public() && identity_store_sign(message, sizeof(message), signature));
    CHECK(!identity_store_open(path) && !memcmp(first, identity_store_public(), sizeof(first)) && !strcmp(fingerprint, identity_store_fingerprint()));
    identity_store_close();
    /* A damaged file is refused and left untouched, never silently replaced. */
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE && WriteFile(file, "damaged", 7, &written, NULL) && written == 7);
    CloseHandle(file);
    CHECK(identity_store_open(path) && !identity_store_public());
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(file != INVALID_HANDLE_VALUE && GetFileSize(file, NULL) == 7);
    CloseHandle(file);
    CHECK(DeleteFileW(path));
    puts("Protected identity creation, reload, signing and damaged-file protection passed.");
    return 0;
}
