#ifndef MURMUR_DATADIR_H
#define MURMUR_DATADIR_H
#include <windows.h>
#include <shlobj.h>

/* Where per-user data lives: %LOCALAPPDATA% unless MURMUR_DATA_ROOT names another folder. Every
 * file the application keeps goes in "<root>\Murmur". The override lets two instances run side by
 * side with separate identities (tests, or a second profile). */
static inline HRESULT murmur_local_appdata(WCHAR path[MAX_PATH]) {
    DWORD length = GetEnvironmentVariableW(L"MURMUR_DATA_ROOT", path, MAX_PATH);
    if (length > 0 && length < MAX_PATH - 32) return S_OK;
    return SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, path);
}
#endif
