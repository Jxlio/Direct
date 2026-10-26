#include "identity_store.h"
#include "datadir.h"
#include <shlobj.h>
#include <wincrypt.h>
#include <wchar.h>
#include <string.h>

static struct {
    int ready;
    uint8_t secret[MURMUR_ID_SECRET], pub[MURMUR_ID_PUBLIC], enc[MURMUR_ID_PUBLIC];
    char fingerprint[MURMUR_ID_FINGERPRINT];
} identity;

static int load(const WCHAR *path, uint8_t secret[MURMUR_ID_SECRET], int *missing) {
    BYTE encrypted[1024];
    DATA_BLOB input, output = {0};
    HANDLE file;
    DWORD size, read;
    int result = -1;
    *missing = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) { *missing = GetLastError() == ERROR_FILE_NOT_FOUND; return -1; }
    size = GetFileSize(file, NULL);
    if (!size || size > sizeof(encrypted) || !ReadFile(file, encrypted, size, &read, NULL) || read != size) goto done;
    input.pbData = encrypted; input.cbData = size;
    if (!CryptUnprotectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) goto done;
    if (output.cbData != MURMUR_ID_SECRET) goto done;
    memcpy(secret, output.pbData, MURMUR_ID_SECRET); result = 0;
done:
    CloseHandle(file); SecureZeroMemory(encrypted, sizeof(encrypted));
    if (output.pbData) { SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); }
    return result;
}
static int save(const WCHAR *path, const uint8_t secret[MURMUR_ID_SECRET]) {
    WCHAR temporary[MAX_PATH];
    DATA_BLOB input, output = {0};
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD written;
    int result = -1;
    if (wcslen(path) + 5 >= MAX_PATH) return -1;
    wcscpy_s(temporary, MAX_PATH, path); wcscat_s(temporary, MAX_PATH, L".tmp");
    input.pbData = (BYTE *)secret; input.cbData = MURMUR_ID_SECRET;
    if (!CryptProtectData(&input, L"Murmur identity v1", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) return -1;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    if (!WriteFile(file, output.pbData, output.cbData, &written, NULL) || written != output.cbData || !FlushFileBuffers(file)) goto done;
    CloseHandle(file); file = INVALID_HANDLE_VALUE;
    if (MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) result = 0;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (result) DeleteFileW(temporary);
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData);
    return result;
}
int identity_store_open(const WCHAR *path) {
    int missing;
    if (!path || identity.ready) return -1;
    if (load(path, identity.secret, &missing)) {
        /* Only a missing file may be replaced. A damaged one is left for the user. */
        if (!missing || murmur_identity_generate(identity.secret, identity.pub) || save(path, identity.secret)) goto fail;
    } else if (murmur_identity_public(identity.secret, identity.pub)) goto fail;
    if (murmur_identity_fingerprint(identity.pub, identity.fingerprint) || murmur_identity_enc_public(identity.secret, identity.enc)) goto fail;
    identity.ready = 1;
    return 0;
fail:
    SecureZeroMemory(&identity, sizeof(identity));
    return -1;
}
int identity_store_open_default(void) {
    WCHAR path[MAX_PATH];
    if (FAILED(murmur_local_appdata(path)) || wcslen(path) + 24 >= MAX_PATH) return -1;
    wcscat_s(path, MAX_PATH, L"\\Murmur");
    if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return -1;
    wcscat_s(path, MAX_PATH, L"\\identity.dat");
    return identity_store_open(path);
}
const uint8_t *identity_store_public(void) { return identity.ready ? identity.pub : NULL; }
const char *identity_store_fingerprint(void) { return identity.ready ? identity.fingerprint : NULL; }
int identity_store_sign(const uint8_t *message, size_t size, uint8_t signature[MURMUR_ID_SIGNATURE]) {
    return identity.ready ? murmur_identity_sign(identity.secret, message, size, signature) : -1;
}
const uint8_t *identity_store_enc_public(void) { return identity.ready ? identity.enc : NULL; }
int identity_store_ecdh(const uint8_t peer[MURMUR_ID_PUBLIC], uint8_t shared[32]) {
    return identity.ready ? murmur_identity_ecdh(identity.secret, peer, shared) : -1;
}
void identity_store_close(void) { SecureZeroMemory(&identity, sizeof(identity)); }
