#include "link.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); link_stop(); return 1; } } while (0)
int main(void) {
    WCHAR path[MAX_PATH], command[MAX_PATH + 48], *slash;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION child = {0};
    uint8_t key[32];
    link_message message;
    ULONGLONG deadline;
    DWORD code;
    int request = 0, muted = 0, left = 0, done = 0;
    unsigned level_self = 99, level_peer = 99;
    memset(key, 0x42, sizeof(key));
    link_init(NULL);
    link_voice_levels(&level_self, &level_peer);
    CHECK(level_self == 0 && level_peer == 0);                 /* nobody speaks before a call */
    CHECK(!link_start(1, "127.0.0.1", "49186", key));
    Sleep(150);
    CHECK(GetModuleFileNameW(NULL, path, MAX_PATH));
    slash = wcsrchr(path, L'\\'); CHECK(slash);
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), L"peer_tests.exe");
    swprintf_s(command, MAX_PATH + 48, L"\"%ls\" --voice-worker-client", path);
    startup.cb = sizeof(startup);
    CHECK(CreateProcessW(path, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child));
    deadline = GetTickCount64() + 12000;
    while (GetTickCount64() < deadline) {
        if (link_receive(&message)) {
            if (message.size == 13 && !memcmp(message.text, "voice request", 13)) {
                CHECK(!link_voice_requested() && link_voice_state() == 0 && !link_voice_received_packets());
                CHECK(!link_voice_toggle()); request = 1;
            }
            if (message.size == 10 && !memcmp(message.text, "voice left", 10)) done = 1;
        }
        if (request && !muted && link_voice_state() == 3 && link_voice_decoded_frames()) {
            link_voice_mute_toggle(); muted = 1;
        }
        if (muted && !left && link_voice_state() == 4) { CHECK(!link_voice_toggle()); left = 1; }
        link_voice_levels(&level_self, &level_peer);
        CHECK(level_self <= 100 && level_peer <= 100);
        if (done && WaitForSingleObject(child.hProcess, 1) == WAIT_OBJECT_0) break;
        Sleep(1);
    }
    CHECK(request && muted && left && done);
    CHECK(GetExitCodeProcess(child.hProcess, &code) && code == 0);
    CHECK(!link_voice_requested());
    link_voice_levels(&level_self, &level_peer);
    CHECK(level_self == 0 && level_peer == 0);                 /* and nobody once it ended */
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    link_stop();
    deadline = GetTickCount64() + 1000;
    while (link_busy() && GetTickCount64() < deadline) Sleep(1);
    CHECK(!link_busy());
    puts("Voice opt-in, encrypted Opus in both directions, mute and leave passed; no devices opened.");
    return 0;
}
