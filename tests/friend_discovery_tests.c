#include <windows.h>
#include <stdio.h>
#include <string.h>
int friends_test_endpoint(int offerer);
int main(int argc, char **argv) {
    WCHAR path[MAX_PATH], command[MAX_PATH+32]; STARTUPINFOW startup = {0}; PROCESS_INFORMATION child = {0};
    DWORD exit_code; int result;
    if (argc == 2 && !strcmp(argv[1], "--client")) return friends_test_endpoint(0) ? 1 : 0;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return 1;
    swprintf_s(command, MAX_PATH+32, L"\"%ls\" --client", path); startup.cb = sizeof(startup);
    if (!CreateProcessW(path, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child)) return 1;
    result = friends_test_endpoint(1);
    if (WaitForSingleObject(child.hProcess, 15000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(child.hProcess, &exit_code) || exit_code) result = -1;
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    if (result) { fputs("Automatic friend discovery failed.\n", stderr); return 1; }
    puts("Two processes discovered each other and exchanged authenticated ICE and text without session codes."); return 0;
}
