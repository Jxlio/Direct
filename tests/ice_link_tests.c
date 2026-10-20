#include "link.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); link_stop(); return 1; } } while (0)
int main(void) {
    murmur_ice_config config = {0};
    char offer[MURMUR_ICE_TOKEN], answer[MURMUR_ICE_TOKEN] = {0};
    WCHAR path[MAX_PATH], command[MAX_PATH + 40], *slash;
    STARTUPINFOW startup = {0}; PROCESS_INFORMATION child = {0};
    SECURITY_ATTRIBUTES access = {sizeof(access), NULL, TRUE};
    HANDLE input_read, input_write, output_read, output_write;
    DWORD available, count, exit_code; size_t used = 0;
    ULONGLONG deadline;
    link_message message;
    int got = 0, exported = 0;
    link_init(NULL); strcpy(config.bind_address, "127.0.0.1");
    CHECK(!link_ice_start(1, &config, NULL));
    deadline = GetTickCount64() + 6000;
    do { exported = link_ice_token(offer, sizeof(offer)); if (exported) break; Sleep(2); } while (GetTickCount64() < deadline);
    CHECK(exported == 1);
    CHECK(CreatePipe(&input_read, &input_write, &access, MURMUR_ICE_TOKEN * 2));
    CHECK(CreatePipe(&output_read, &output_write, &access, MURMUR_ICE_TOKEN * 2));
    CHECK(SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0));
    CHECK(SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0));
    CHECK(GetModuleFileNameW(NULL, path, MAX_PATH)); slash = wcsrchr(path, L'\\'); CHECK(slash);
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), L"ice_tests.exe");
    swprintf_s(command, MAX_PATH + 40, L"\"%ls\" --worker-client", path);
    startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input_read; startup.hStdOutput = output_write; startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    CHECK(CreateProcessW(path, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child));
    CloseHandle(input_read); CloseHandle(output_write);
    CHECK(WriteFile(input_write, offer, (DWORD)strlen(offer), &count, NULL));
    CHECK(WriteFile(input_write, "\n", 1, &count, NULL)); CloseHandle(input_write);
    SecureZeroMemory(offer, sizeof(offer));
    deadline = GetTickCount64() + 8000;
    while (GetTickCount64() < deadline) {
        CHECK(PeekNamedPipe(output_read, NULL, 0, NULL, &available, NULL));
        if (available) {
            CHECK(available < sizeof(answer) - used);
            CHECK(ReadFile(output_read, answer + used, available, &count, NULL)); used += count; answer[used] = 0;
            if (strchr(answer, '\n')) break;
        }
        Sleep(2);
    }
    CHECK(used && !link_ice_answer(answer));
    SecureZeroMemory(answer, sizeof(answer)); CloseHandle(output_read);
    deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        if (link_receive(&message)) {
            CHECK(message.room == 2 && message.size == 14 && !memcmp(message.text, "hello over ICE", 14));
            CHECK(link_ice_mode() && link_state() == 2 && link_ice_phase() == 4 && !link_ice_relayed());
            /* A remote vocal request must not open the local microphone. */
            CHECK(!link_voice_requested() && link_voice_state() == 0 && !link_voice_received_packets());
            CHECK(!link_send(2, (const uint8_t *)"ICE ack", 7)); got = 1;
        }
        if (WaitForSingleObject(child.hProcess, 2) == WAIT_OBJECT_0) break;
    }
    CHECK(got && GetExitCodeProcess(child.hProcess, &exit_code) && exit_code == 0);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    link_stop(); deadline = GetTickCount64() + 1500;
    while (link_busy() && GetTickCount64() < deadline) Sleep(2);
    CHECK(!link_busy());
    CHECK(link_ice_token(offer, sizeof(offer)) == 0);
    /* Stopping during candidate gathering must also finish without UI input. */
    CHECK(!link_ice_start(1, &config, NULL)); link_stop();
    deadline = GetTickCount64() + 1500;
    while (link_busy() && GetTickCount64() < deadline) Sleep(2);
    CHECK(!link_busy());
    puts("Windows ICE offer/answer, reliable text, consent gating and cancellation passed without public services or devices.");
    return 0;
}
