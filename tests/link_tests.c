#include "link.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); link_stop(); return 1; } } while (0)
int main(void) {
    WCHAR path[MAX_PATH], command[MAX_PATH + 40], *slash;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION child = {0};
    uint8_t key[32];
    link_message message;
    ULONGLONG deadline;
    DWORD code;
    int got = 0, connected = 0;
    memset(key, 0x42, sizeof(key));
    link_init(NULL);
    CHECK(link_start(1, "127.0.0.1", "49184", key) == 0);
    /* The host thread must bind before the separate test client starts. */
    Sleep(150);
    CHECK(GetModuleFileNameW(NULL, path, MAX_PATH) > 0);
    slash = wcsrchr(path, L'\\'); CHECK(slash);
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), L"peer_tests.exe");
    swprintf_s(command, MAX_PATH + 40, L"\"%ls\" --worker-client", path);
    startup.cb = sizeof(startup);
    CHECK(CreateProcessW(path, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child));
    deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        if (link_state() == 2) connected = 1;
        if (link_receive(&message)) {
            CHECK(message.room == 1 && message.size == 11 && !memcmp(message.text, "worker ping", 11));
            CHECK(link_send(1, (const uint8_t *)"worker pong", 11) == 0);
            got = 1;
        }
        if (WaitForSingleObject(child.hProcess, 1) == WAIT_OBJECT_0) break;
    }
    CHECK(GetExitCodeProcess(child.hProcess, &code) && code == 0);
    CHECK(got && connected);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    link_stop();
    deadline = GetTickCount64() + 1000;
    while (link_busy() && GetTickCount64() < deadline) Sleep(1);
    CHECK(!link_busy());
    /* Cancellation must also work while the host has no incoming connection. */
    CHECK(link_start(1, "127.0.0.1", "49184", key) == 0);
    Sleep(50);
    link_stop();
    deadline = GetTickCount64() + 1000;
    while (link_busy() && GetTickCount64() < deadline) Sleep(1);
    CHECK(!link_busy() && link_state() == 0);
    puts("Windows worker exchange, restart and cancellation passed.");
    return 0;
}
