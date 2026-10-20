#include "friends.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    WCHAR directory[MAX_PATH], path[MAX_PATH]; friend_book book = {0}, restored = {0}, sentinel;
    HANDLE file; BYTE bytes[16384]; DWORD size, written;
    CHECK(GetTempPathW(MAX_PATH, directory));
    CHECK(GetTempFileNameW(directory, L"mfr", 0, path));
    CHECK(DeleteFileW(path));
    CHECK(!friend_book_load(path, &restored) && restored.version == 1 && !restored.count);
    book.version = 1; book.count = 1; book.items[0].offerer = 1;
    wcscpy_s(book.items[0].name, 64, L"Ami é"); memset(book.items[0].secret, 0x5a, 32);
    CHECK(!friend_book_save(path, &book)); CHECK(!friend_book_load(path, &restored));
    CHECK(!memcmp(&book, &restored, sizeof(book)));
    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    CHECK(ReadFile(file, bytes, sizeof(bytes), &size, NULL) && size > 32);
    /* No literal friendship secret in the protected file. */
    { DWORD i; for (i = 0; i+32 <= size; ++i) CHECK(memcmp(bytes+i, book.items[0].secret, 32)); }
    bytes[size-1] ^= 1; SetFilePointer(file, 0, NULL, FILE_BEGIN);
    CHECK(WriteFile(file, bytes, size, &written, NULL) && written == size); CloseHandle(file);
    sentinel = restored; CHECK(friend_book_load(path, &restored) == -1);
    CHECK(!memcmp(&sentinel, &restored, sizeof(restored)));
    CHECK(DeleteFileW(path));
    book.count = 17; CHECK(friend_book_save(path, &book) == -1);
    puts("Protected contacts, restart roundtrip, corruption rejection passed."); return 0;
}
