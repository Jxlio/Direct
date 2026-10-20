#include "updates.h"
#include "panel_style.h"
#include "murmur/update.h"
#include "direct_version.h"
#include "link.h"
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#include "third_party/jsmn.h"
static struct {
    HWND parent, panel, status, notes, download;
    HFONT font;
    HANDLE worker;
    CRITICAL_SECTION lock;
    int busy, available, staged, install, closing;
    direct_update update;
    WCHAR message[512], history[32768], folder[MAX_PATH];
} updater;
static direct_version current_version(void) {
    direct_version v = {DIRECT_VERSION_MAJOR, DIRECT_VERSION_MINOR, DIRECT_VERSION_PATCH}; return v;
}
static DWORD WINAPI download(void *unused);
static int embedded_text(unsigned id,WCHAR *out,int capacity) {
    HMODULE module=GetModuleHandleW(NULL); HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA);
    HGLOBAL loaded; const char *bytes; DWORD size; WCHAR *plain; int count,i,used=0;
    if (!resource || !(size=SizeofResource(module,resource)) || size>262144 || !(loaded=LoadResource(module,resource)) || !(bytes=(const char *)LockResource(loaded))) return -1;
    plain=(WCHAR *)calloc((size_t)capacity,sizeof(WCHAR)); if (!plain) return -1;
    count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,(int)size,plain,capacity-1);
    if (!count) { free(plain); return -1; }
    for (i=0;i<count;++i) {
        if (used+2>=capacity) { free(plain); return -1; }
        if (plain[i]=='\n' && (!i || plain[i-1]!='\r')) out[used++]='\r';
        out[used++]=plain[i];
    }
    out[used]=0; free(plain); return 0;
}
static int fetch(const WCHAR *host, const WCHAR *path, size_t limit, uint8_t **out, size_t *size, DWORD *status) {
    HINTERNET session = NULL, connection = NULL, request = NULL; uint8_t *data = NULL;
    DWORD read, length = sizeof(*status), policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP, disabled = WINHTTP_DISABLE_COOKIES;
    ULONGLONG started = GetTickCount64(); size_t used = 0; int result = -1;
    *out = NULL; *size = 0; *status = 0;
    session = WinHttpOpen(L"Direct/" DIRECT_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!session) goto done;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0); if (!connection) goto done;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request || !WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy)) ||
        !WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, status, &length, NULL)) goto done;
    if (*status != 200) { result = 1; goto done; }
    data = (uint8_t *)calloc(limit+1, 1); if (!data) goto done;
    for (;;) {
        DWORD capacity = (DWORD)((limit-used) > 16384 ? 16384 : limit-used);
        if (GetTickCount64()-started > 60000) goto done;
        if (!capacity) {
            uint8_t extra;
            if (!WinHttpReadData(request, &extra, 1, &read) || read) goto done;
            break;
        }
        if (!WinHttpReadData(request, data+used, capacity, &read)) goto done;
        if (!read) break; used += read;
    }
    *out = data; *size = used; data = NULL; result = 0;
done:
    free(data); if (request) WinHttpCloseHandle(request); if (connection) WinHttpCloseHandle(connection); if (session) WinHttpCloseHandle(session);
    return result;
}
static int file_write(const WCHAR *path, const uint8_t *data, size_t size) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written; int result = -1;
    if (f == INVALID_HANDLE_VALUE) return -1;
    if (WriteFile(f, data, (DWORD)size, &written, NULL) && written == size && FlushFileBuffers(f)) result = 0;
    CloseHandle(f); return result;
}
static int file_read(const WCHAR *path, size_t limit, uint8_t **data, size_t *size) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); DWORD n, read;
    *data = NULL; *size = 0; if (f == INVALID_HANDLE_VALUE) return -1;
    n = GetFileSize(f, NULL);
    if (!n || n > limit) { CloseHandle(f); return -1; }
    *data = (uint8_t *)calloc((size_t)n+1, 1);
    if (!*data || !ReadFile(f, *data, n, &read, NULL) || read != n) { free(*data); *data = NULL; CloseHandle(f); return -1; }
    CloseHandle(f); *size = n; return 0;
}
static int child_path(WCHAR out[MAX_PATH], const WCHAR *root, const WCHAR *name) {
    return swprintf_s(out, MAX_PATH, L"%ls\\%ls", root, name) < 0 ? -1 : 0;
}
static void state(const WCHAR *message) {
    EnterCriticalSection(&updater.lock); wcscpy_s(updater.message, 512, message); LeaveCriticalSection(&updater.lock);
    PostMessageW(updater.parent, WM_DIRECT_UPDATE, 0, 0);
}
static int token_equal(const char *json, const jsmntok_t *t, const char *value) {
    size_t length = strlen(value); return t->end >= t->start && (size_t)(t->end-t->start) == length && !memcmp(json+t->start, value, length);
}
static int field(const char *json, jsmntok_t *t, int count, int object, const char *key) {
    int i; for (i=object+1; i<count && t[i].start < t[object].end; ++i)
        if (t[i].parent == object && t[i].type == JSMN_STRING && token_equal(json, &t[i], key) && i+1<count) return i+1;
    return -1;
}
static int hex4(const char *p, unsigned *out) {
    int i; unsigned n = 0;
    for (i=0;i<4;++i) { unsigned c = (unsigned char)p[i], v;
        if (c>='0' && c<='9') v=c-'0'; else if (c>='a' && c<='f') v=c-'a'+10; else if (c>='A' && c<='F') v=c-'A'+10; else return -1;
        n = (n<<4)|v;
    } *out=n; return 0;
}
static int json_text(const char *json, const jsmntok_t *t, WCHAR *out, int capacity) {
    char text[32768]; size_t used = 0; int i, n;
    if (t->type != JSMN_STRING) return -1;
    for (i=t->start;i<t->end;++i) {
        unsigned c = (unsigned char)json[i];
        if (c == '\\') {
            if (++i >= t->end) return -1; c = (unsigned char)json[i];
            if (c=='n') c='\n'; else if (c=='r') c='\r'; else if (c=='t') c='\t';
            else if (c=='b' || c=='f') c=' ';
            else if (c=='u') {
                if (i+4 >= t->end || hex4(json+i+1, &c)) return -1; i+=4;
                if (c>=0xd800 && c<=0xdbff) { unsigned low;
                    if (i+6 >= t->end || json[i+1]!='\\' || json[i+2]!='u' || hex4(json+i+3,&low) || low<0xdc00 || low>0xdfff) return -1;
                    c=0x10000+((c-0xd800)<<10)+(low-0xdc00); i+=6;
                } else if (c>=0xdc00 && c<=0xdfff) return -1;
                if (used+4 >= sizeof(text) || !c) return -1;
                if (c>=0x10000) { text[used++]=(char)(0xf0|(c>>18)); text[used++]=(char)(0x80|((c>>12)&63)); text[used++]=(char)(0x80|((c>>6)&63)); c=0x80|(c&63); }
                else if (c>=0x800) { text[used++]=(char)(0xe0|(c>>12)); text[used++]=(char)(0x80|((c>>6)&63)); c=0x80|(c&63); }
                else if (c>=0x80) { text[used++]=(char)(0xc0|(c>>6)); c=0x80|(c&63); }
            } else if (c!='\\' && c!='/' && c!='"') return -1;
        }
        if (!c || used+2 >= sizeof(text)) return -1;
        if (c=='\n' && (!used || text[used-1]!='\r')) text[used++]='\r';
        text[used++]=(char)c;
    }
    text[used]=0; n=MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, capacity); return n ? 0 : -1;
}
static void history(void) {
    uint8_t *json=NULL; size_t size; DWORD status; jsmn_parser parser; jsmntok_t *tokens=NULL;
    WCHAR result[32768] = L"Historique disponible sur github.com/Jxlio/Direct/releases.\r\n\r\nVersion installée : " DIRECT_VERSION_W L"\r\n", title[128], body[12000];
    int count, i;
    (void)embedded_text(3,result,32768);
    if (fetch(L"api.github.com", L"/repos/Jxlio/Direct/releases?per_page=10", 262144, &json, &size, &status)) goto done;
    tokens=(jsmntok_t *)calloc(8192,sizeof(*tokens)); if (!tokens) goto done;
    jsmn_init(&parser); count=jsmn_parse(&parser,(const char *)json,size,tokens,8192);
    if (count<1 || tokens[0].type!=JSMN_ARRAY) goto done;
    for (i=1;i<count;++i) if (tokens[i].parent==0 && tokens[i].type==JSMN_OBJECT) {
        int a=field((const char *)json,tokens,count,i,"tag_name"), b=field((const char *)json,tokens,count,i,"body");
        if (a<0 || b<0 || json_text((const char *)json,&tokens[a],title,128) || json_text((const char *)json,&tokens[b],body,12000)) continue;
        if (wcslen(result)+wcslen(title)+wcslen(body)+16>=32768) break;
        wcscat_s(result,32768,L"\r\n────────  "); wcscat_s(result,32768,title); wcscat_s(result,32768,L"\r\n\r\n");
        wcscat_s(result,32768,body); wcscat_s(result,32768,L"\r\n");
    }
done:
    EnterCriticalSection(&updater.lock); wcscpy_s(updater.history,32768,result); LeaveCriticalSection(&updater.lock);
    free(tokens); free(json);
}
static DWORD WINAPI check(void *unused) {
    uint8_t *manifest=NULL, *sig=NULL; size_t size, signature_size; DWORD status; direct_update update; int r;
    (void)unused; state(L"Recherche d’une nouvelle version sur GitHub…");
    r=fetch(L"github.com",L"/Jxlio/Direct/releases/latest/download/Direct-win-x64.manifest",255,&manifest,&size,&status);
    if (r) { state(status==404 ? L"Aucune mise à jour publiée pour le moment." : L"Vérification indisponible. Vous pouvez réessayer plus tard."); goto done; }
    if (fetch(L"github.com",L"/Jxlio/Direct/releases/latest/download/Direct-win-x64.sig",80,&sig,&signature_size,&status) ||
        direct_update_verify(manifest,size,sig,signature_size,&update)) { state(L"Mise à jour refusée : signature absente ou invalide."); goto done; }
    EnterCriticalSection(&updater.lock); updater.update=update; updater.available=direct_version_compare(update.version,current_version())>0; LeaveCriticalSection(&updater.lock);
    if (direct_version_compare(update.version,current_version())>0) {
        WCHAR text[256]; swprintf_s(text,256,L"Version %u.%u.%u disponible. Téléchargez-la pour l’installer à la fermeture.",update.version.major,update.version.minor,update.version.patch); state(text);
    } else state(L"Direct est à jour.");
done:
    free(manifest); free(sig); history();
    EnterCriticalSection(&updater.lock); r=updater.available && !updater.closing; LeaveCriticalSection(&updater.lock);
    /* A check started during a call offers the download button instead. */
    if (r && !link_voice_requested()) return download(NULL);
    EnterCriticalSection(&updater.lock); updater.busy=0; LeaveCriticalSection(&updater.lock); PostMessageW(updater.parent,WM_DIRECT_UPDATE,0,0); return 0;
}
static DWORD WINAPI download(void *unused) {
    uint8_t *manifest=NULL,*sig=NULL,*exe=NULL; size_t size,sig_size,exe_size; DWORD status; direct_update update;
    WCHAR file[MAX_PATH], url[256]; int okay=0; (void)unused;
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    state(L"Téléchargement et vérification de la mise à jour…");
    if (!updater.folder[0] || fetch(L"github.com",L"/Jxlio/Direct/releases/latest/download/Direct-win-x64.manifest",255,&manifest,&size,&status) ||
        fetch(L"github.com",L"/Jxlio/Direct/releases/latest/download/Direct-win-x64.sig",80,&sig,&sig_size,&status) ||
        direct_update_verify(manifest,size,sig,sig_size,&update) || direct_version_compare(update.version,current_version())<=0) goto done;
    swprintf_s(url,256,L"/Jxlio/Direct/releases/download/v%u.%u.%u/Direct-win-x64.exe",update.version.major,update.version.minor,update.version.patch);
    if (fetch(L"github.com",url,DIRECT_UPDATE_LIMIT,&exe,&exe_size,&status) || direct_update_hash(&update,exe,exe_size) || exe_size<2 || exe[0]!='M' || exe[1]!='Z') goto done;
    if (child_path(file,updater.folder,L"update.exe") || file_write(file,exe,exe_size) ||
        child_path(file,updater.folder,L"update.manifest") || file_write(file,manifest,size) ||
        child_path(file,updater.folder,L"update.sig") || file_write(file,sig,sig_size)) goto done;
    okay=1;
done:
    free(manifest); free(sig); free(exe);
    EnterCriticalSection(&updater.lock); updater.staged=okay; updater.install=okay; updater.busy=0; LeaveCriticalSection(&updater.lock);
    state(okay ? L"Mise à jour vérifiée. Elle sera installée lorsque vous quitterez Direct." : L"Téléchargement ou vérification impossible. Votre version actuelle est conservée."); return 0;
}
static void start(int downloading) {
    EnterCriticalSection(&updater.lock);
    if (updater.busy || updater.staged || updater.closing) { LeaveCriticalSection(&updater.lock); return; }
    if (updater.worker) { CloseHandle(updater.worker); updater.worker=NULL; }
    updater.busy=1; updater.worker=CreateThread(NULL,0,downloading?download:check,NULL,0,NULL);
    if (!updater.worker) updater.busy=0;
    LeaveCriticalSection(&updater.lock); updates_refresh();
}
void updates_init(HWND parent) {
    WCHAR base[MAX_PATH], folder[MAX_PATH];
    InitializeCriticalSection(&updater.lock); updater.parent=parent;
    wcscpy_s(updater.message,512,L"Vérification des mises à jour…");
    if (embedded_text(3,updater.history,32768)) wcscpy_s(updater.history,32768,L"Direct " DIRECT_VERSION_W);
    if (SUCCEEDED(SHGetFolderPathW(NULL,CSIDL_LOCAL_APPDATA,NULL,SHGFP_TYPE_CURRENT,base)) && !child_path(folder,base,L"Murmur")) {
        CreateDirectoryW(folder,NULL); if (!child_path(updater.folder,folder,L"updates")) CreateDirectoryW(updater.folder,NULL);
    }
    start(0);
}
void updates_refresh(void) {
    if (!updater.panel) return;
    EnterCriticalSection(&updater.lock);
    SetWindowTextW(updater.status,updater.message); SetWindowTextW(updater.notes,updater.history);
    EnableWindow(updater.download,updater.available && !updater.busy && !updater.staged);
    EnableWindow(GetDlgItem(updater.panel,601),!updater.busy && !updater.staged);
    LeaveCriticalSection(&updater.lock);
}
static LRESULT CALLBACK panel_proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    switch (message) {
    case WM_COMMAND:
        if (HIWORD(wp)==BN_CLICKED) switch (LOWORD(wp)) {
        case 601: start(0); break;
        case 602: start(1); break;
        case 603: ShellExecuteW(window,L"open",L"https://github.com/Jxlio/Direct/releases",NULL,NULL,SW_SHOWNORMAL); break;
        case 604: {
            WCHAR *licenses=(WCHAR *)calloc(131072,sizeof(WCHAR));
            if (licenses && !embedded_text(2,licenses,131072)) SetWindowTextW(updater.notes,licenses);
            free(licenses); break;
        }
        } return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: return direct_panel_color(message,wp,lp);
    case WM_DRAWITEM: return direct_panel_button((DRAWITEMSTRUCT *)lp,updater.font);
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: updater.panel=NULL; DeleteObject(updater.font); updater.font=NULL; return 0;
    default: return DefWindowProcW(window,message,wp,lp);
    }
}
void updates_show(HWND parent) {
    WNDCLASSW wc={0}; HWND title,check_button,web;
    if (updater.panel) { SetForegroundWindow(updater.panel); return; }
    wc.lpfnWndProc=panel_proc; wc.hInstance=GetModuleHandleW(NULL); wc.lpszClassName=L"DirectUpdates";
    wc.hCursor=LoadCursorW(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)GetStockObject(WHITE_BRUSH); RegisterClassW(&wc);
    updater.panel=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"Direct — Versions et mises à jour",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,704,690,parent,NULL,wc.hInstance,NULL);
    if (!updater.panel) return;
    updater.font=CreateFontW(-15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    title=CreateWindowW(L"STATIC",L"Direct " DIRECT_VERSION_W L" · Historique des versions",WS_CHILD|WS_VISIBLE,24,24,640,26,updater.panel,NULL,NULL,NULL);
    updater.status=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,24,66,640,52,updater.panel,NULL,NULL,NULL);
    check_button=CreateWindowW(L"BUTTON",L"Vérifier maintenant",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,24,132,204,38,updater.panel,(HMENU)601,NULL,NULL);
    updater.download=CreateWindowW(L"BUTTON",L"Télécharger la mise à jour",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,248,132,240,38,updater.panel,(HMENU)602,NULL,NULL);
    web=CreateWindowW(L"BUTTON",L"GitHub Releases",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,508,132,152,38,updater.panel,(HMENU)603,NULL,NULL);
    updater.notes=CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_MULTILINE|ES_READONLY|WS_VSCROLL,24,192,636,420,updater.panel,NULL,NULL,NULL);
    CreateWindowW(L"BUTTON",L"Licences intégrées",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,24,620,204,28,updater.panel,(HMENU)604,NULL,NULL);
    SendMessageW(updater.notes,EM_SETLIMITTEXT,130000,0);
    { HWND controls[]={title,updater.status,check_button,updater.download,web,updater.notes}; unsigned i;
      for (i=0;i<sizeof(controls)/sizeof(controls[0]);++i) SendMessageW(controls[i],WM_SETFONT,(WPARAM)updater.font,TRUE); }
    updates_refresh(); ShowWindow(updater.panel,SW_SHOW);
}
int updates_translate(MSG *message) { return updater.panel && IsDialogMessageW(updater.panel,message); }
void updates_shutdown(void) {
    WCHAR executable[MAX_PATH],helper[MAX_PATH],command[MAX_PATH*3],runtime[MAX_PATH],runtime_copy[MAX_PATH],*slash; int install;
    EnterCriticalSection(&updater.lock); updater.closing=1; install=updater.install && updater.staged; LeaveCriticalSection(&updater.lock);
    if (!install) return;
    if (!GetModuleFileNameW(NULL,executable,MAX_PATH) || child_path(helper,updater.folder,L"helper.exe") || !CopyFileW(executable,helper,FALSE)) {
        MessageBoxW(NULL,L"La mise à jour n’a pas pu démarrer. La version actuelle est conservée.",L"Direct · Mise à jour",MB_OK|MB_ICONWARNING); return;
    }
    /* A portable installation may have no VC runtime installed system-wide. */
    wcscpy_s(runtime,MAX_PATH,executable); slash=wcsrchr(runtime,L'\\');
    if (!slash) return;
    *slash=0;
    if (wcscat_s(runtime,MAX_PATH,L"\\vcruntime140.dll") || child_path(runtime_copy,updater.folder,L"vcruntime140.dll")) return;
    if (GetFileAttributesW(runtime)!=INVALID_FILE_ATTRIBUTES && !CopyFileW(runtime,runtime_copy,FALSE)) {
        MessageBoxW(NULL,L"L’assistant de mise à jour n’a pas pu être préparé. La version actuelle est conservée.",L"Direct · Mise à jour",MB_OK|MB_ICONWARNING); return;
    }
    swprintf_s(command,MAX_PATH*3,L"\"%ls\" --direct-apply-update %lu \"%ls\" \"%ls\"",helper,GetCurrentProcessId(),executable,updater.folder);
    { STARTUPINFOW si={0}; PROCESS_INFORMATION pi={0}; si.cb=sizeof(si);
      if (CreateProcessW(helper,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
      else MessageBoxW(NULL,L"L’installation n’a pas pu démarrer. La version actuelle reste disponible.",L"Direct · Mise à jour",MB_OK|MB_ICONWARNING); }
}
int updates_apply_command(void) {
    int argc; WCHAR **args=CommandLineToArgvW(GetCommandLineW(),&argc), *end;
    WCHAR manifest_file[MAX_PATH],sig_file[MAX_PATH],exe_file[MAX_PATH],temporary[MAX_PATH],backup[MAX_PATH];
    uint8_t *manifest=NULL,*sig=NULL,*data=NULL; size_t size,sig_size,data_size; direct_update update; DWORD pid; HANDLE process; int okay=0;
    if (!args || argc<2 || wcscmp(args[1],L"--direct-apply-update")) { if (args) LocalFree(args); return 0; }
    if (argc!=5 || !GetFullPathNameW(args[3],MAX_PATH,temporary,NULL) || wcscmp(temporary,args[3]) || wcslen(args[3])+16>=MAX_PATH) goto done;
    pid=wcstoul(args[2],&end,10); if (!pid || *end) goto done;
    process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (process) {
        WCHAR parent_exe[MAX_PATH]; DWORD parent_size=MAX_PATH;
        if (!QueryFullProcessImageNameW(process,0,parent_exe,&parent_size) || _wcsicmp(parent_exe,args[3]) ||
            WaitForSingleObject(process,120000)!=WAIT_OBJECT_0) { CloseHandle(process); goto done; }
        CloseHandle(process);
    } else if (GetLastError()!=ERROR_INVALID_PARAMETER) goto done;
    if (child_path(manifest_file,args[4],L"update.manifest") || child_path(sig_file,args[4],L"update.sig") || child_path(exe_file,args[4],L"update.exe") ||
        file_read(manifest_file,255,&manifest,&size) || file_read(sig_file,80,&sig,&sig_size) ||
        direct_update_verify(manifest,size,sig,sig_size,&update) || direct_version_compare(update.version,current_version())<=0 ||
        file_read(exe_file,DIRECT_UPDATE_LIMIT,&data,&data_size) || direct_update_hash(&update,data,data_size)) goto done;
    swprintf_s(temporary,MAX_PATH,L"%ls.update-new",args[3]); swprintf_s(backup,MAX_PATH,L"%ls.update-backup",args[3]);
    if (file_write(temporary,data,data_size) || !CopyFileW(args[3],backup,FALSE)) goto done;
    if (!MoveFileExW(temporary,args[3],MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) goto done;
    okay=1;
done:
    free(manifest); free(sig); free(data);
#ifndef DIRECT_UPDATER_TEST
    if (!okay) MessageBoxW(NULL,L"La mise à jour n’a pas pu être installée. Votre exécutable actuel est conservé.",L"Direct · Mise à jour",MB_OK|MB_ICONWARNING);
#else
    (void)okay;
#endif
    if (args) LocalFree(args); return 1;
}
