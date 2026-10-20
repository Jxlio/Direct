#include "friends.h"
#include <wincrypt.h>
#include <wchar.h>
#include <string.h>
static int valid(const friend_book *book) {
    DWORD i, j;
    if (book->version != 1 || book->count > 16) return 0;
    for (i = 0; i < book->count; ++i) {
        if (book->items[i].offerer > 1 || !book->items[i].name[0] ||
            !wmemchr(book->items[i].name, 0, 64)) return 0;
        for (j = 0; j < i; ++j)
            if (!memcmp(book->items[i].secret, book->items[j].secret, 32)) return 0;
    }
    return 1;
}
int friend_book_load(const WCHAR *path, friend_book *book) {
    BYTE encrypted[16384]; DATA_BLOB input, output = {0};
    HANDLE file; DWORD size, read; int result = -1;
    if (!path || !book) return -1;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) return -1;
        SecureZeroMemory(book, sizeof(*book)); book->version = 1; return 0;
    }
    size = GetFileSize(file, NULL);
    if (!size || size > sizeof(encrypted) || !ReadFile(file, encrypted, size, &read, NULL) || read != size) goto done;
    input.pbData = encrypted; input.cbData = size;
    if (!CryptUnprotectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) goto done;
    if (output.cbData != sizeof(*book) || !valid((const friend_book *)output.pbData)) goto done;
    memcpy(book, output.pbData, sizeof(*book)); result = 0;
done:
    CloseHandle(file); SecureZeroMemory(encrypted, sizeof(encrypted));
    if (output.pbData) { SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); }
    return result;
}
int friend_book_save(const WCHAR *path, const friend_book *book) {
    WCHAR temporary[MAX_PATH]; DATA_BLOB input, output = {0};
    HANDLE file = INVALID_HANDLE_VALUE; DWORD written; int result = -1;
    if (!path || !book || !valid(book) || wcslen(path)+5 >= MAX_PATH) return -1;
    wcscpy_s(temporary, MAX_PATH, path); wcscat_s(temporary, MAX_PATH, L".tmp");
    input.pbData = (BYTE *)book; input.cbData = sizeof(*book);
    if (!CryptProtectData(&input, L"Murmur friends v1", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) return -1;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    if (!WriteFile(file, output.pbData, output.cbData, &written, NULL) || written != output.cbData || !FlushFileBuffers(file)) goto done;
    CloseHandle(file); file = INVALID_HANDLE_VALUE;
    if (MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) result = 0;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (result) DeleteFileW(temporary);
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); return result;
}
