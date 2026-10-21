#include "signaling.h"
#include <winhttp.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#define SIGNAL_QUEUE 16
static struct {
    HWND owner; CRITICAL_SECTION lock; LONG reconnect;
    HANDLE thread,stop,wake;
    direct_service profile;
    char subscription[1100]; int dirty,state,blocked;
    signal_frame incoming[SIGNAL_QUEUE],outgoing[SIGNAL_QUEUE];
    unsigned in_head,in_count,out_head,out_count;
} hub;
static int profile_path(WCHAR path[MAX_PATH]) {
    if(FAILED(SHGetFolderPathW(NULL,CSIDL_LOCAL_APPDATA,NULL,SHGFP_TYPE_CURRENT,path)) || wcslen(path)>MAX_PATH-32) return -1;
    wcscat_s(path,MAX_PATH,L"\\Murmur"); CreateDirectoryW(path,NULL);
    wcscat_s(path,MAX_PATH,L"\\service.dat"); return 0;
}
int signal_profile_load(direct_service *profile) {
    WCHAR path[MAX_PATH]; BYTE bytes[4096]; HANDLE file; DWORD read,size; DATA_BLOB input,output={0}; int result=-1;
    memset(profile,0,sizeof(*profile)); if(profile_path(path)) return -1;
    file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE) return GetLastError()==ERROR_FILE_NOT_FOUND?0:-1;
    size=GetFileSize(file,NULL);
    if(size && size<=sizeof(bytes) && ReadFile(file,bytes,size,&read,NULL) && read==size) {
        input.pbData=bytes; input.cbData=size;
        if(CryptUnprotectData(&input,NULL,NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&output) && output.cbData==sizeof(*profile) && direct_service_valid((const direct_service *)output.pbData)) { memcpy(profile,output.pbData,sizeof(*profile)); result=1; }
    }
    CloseHandle(file); SecureZeroMemory(bytes,sizeof(bytes));
    if(output.pbData) { SecureZeroMemory(output.pbData,output.cbData); LocalFree(output.pbData); } return result;
}
int signal_profile_save(const direct_service *profile) {
    WCHAR path[MAX_PATH],temporary[MAX_PATH]; DATA_BLOB input,output={0}; HANDLE file; DWORD written; int result=-1;
    if(!direct_service_valid(profile) || profile_path(path)) return -1;
    swprintf_s(temporary,MAX_PATH,L"%ls.tmp",path);
    input.pbData=(BYTE *)profile; input.cbData=sizeof(*profile);
    if(!CryptProtectData(&input,L"Direct service v1",NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&output)) return -1;
    file=CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file!=INVALID_HANDLE_VALUE) {
        if(WriteFile(file,output.pbData,output.cbData,&written,NULL) && written==output.cbData && FlushFileBuffers(file)) result=0;
        CloseHandle(file);
        if(!result && !MoveFileExW(temporary,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) result=-1;
    }
    if(result) DeleteFileW(temporary);
    SecureZeroMemory(output.pbData,output.cbData); LocalFree(output.pbData); return result;
}
static void status(int state) {
    EnterCriticalSection(&hub.lock); hub.state=state; LeaveCriticalSection(&hub.lock);
    PostMessageW(hub.owner,WM_DIRECT_SIGNAL,0,0);
}
static DWORD WINAPI receive_thread(void *arg) {
    HINTERNET socket=(HINTERNET)arg; signal_frame frame; DWORD received,error; size_t used=0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE kind;
    while(WaitForSingleObject(hub.stop,0)!=WAIT_OBJECT_0) {
        error=WinHttpWebSocketReceive(socket,frame.bytes+used,(DWORD)(sizeof(frame.bytes)-used),&received,&kind);
        if(error) break;
        if(kind==WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT code=0; DWORD used_reason=0; BYTE reason[128];
            if(!WinHttpWebSocketQueryCloseStatus(socket,&code,reason,sizeof(reason),&used_reason) && code==1008) {
                EnterCriticalSection(&hub.lock); hub.blocked=1; LeaveCriticalSection(&hub.lock);
            }
            break;
        }
        if(kind!=WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE && kind!=WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) break;
        used+=received;
        if(used>sizeof(frame.bytes) || (used==sizeof(frame.bytes) && kind==WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)) break;
        if(kind==WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            if(used<33) break;
            frame.size=used;
            EnterCriticalSection(&hub.lock);
            if(hub.in_count==SIGNAL_QUEUE) { LeaveCriticalSection(&hub.lock); break; }
            hub.incoming[(hub.in_head+hub.in_count)%SIGNAL_QUEUE]=frame; ++hub.in_count;
            LeaveCriticalSection(&hub.lock); PostMessageW(hub.owner,WM_DIRECT_SIGNAL,0,0); used=0;
        }
    }
    SecureZeroMemory(&frame,sizeof(frame)); SetEvent(hub.wake); return 0;
}
static HINTERNET connect_socket(HINTERNET session) {
    WCHAR host[256],authorization[100]; HINTERNET connection,request,socket=NULL;
    DWORD code=0,length=sizeof(code),redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,hub.profile.origin+8,-1,host,256);
    connection=WinHttpConnect(session,host,INTERNET_DEFAULT_HTTPS_PORT,0); if(!connection) return NULL;
    request=WinHttpOpenRequest(connection,L"GET",L"/v1/connect",NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    if(request) {
        swprintf_s(authorization,100,L"Authorization: Bearer %hs",hub.profile.access);
        if(WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect)) &&
           WinHttpSetOption(request,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,NULL,0) &&
           WinHttpSendRequest(request,authorization,(DWORD)-1L,WINHTTP_NO_REQUEST_DATA,0,0,0) && WinHttpReceiveResponse(request,NULL) &&
           WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,NULL,&code,&length,NULL) && code==101)
            socket=WinHttpWebSocketCompleteUpgrade(request,0);
        SecureZeroMemory(authorization,sizeof(authorization)); WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connection); return socket;
}
static DWORD WINAPI worker(void *unused) {
    HINTERNET session; unsigned retry=1; HANDLE events[2]={hub.stop,hub.wake}; (void)unused;
    session=WinHttpOpen(L"Direct/0.7",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session) { status(3); return 0; }
    WinHttpSetTimeouts(session,5000,5000,5000,5000);
    while(WaitForSingleObject(hub.stop,0)!=WAIT_OBJECT_0) {
        HINTERNET socket; HANDLE receiver; status(1); socket=connect_socket(session);
        if(!socket) { status(3); if(WaitForSingleObject(hub.stop,retry*1000)==WAIT_OBJECT_0) break; if(retry<30) retry*=2; continue; }
        receiver=CreateThread(NULL,0,receive_thread,socket,0,NULL);
        if(!receiver) { WinHttpCloseHandle(socket); break; }
        EnterCriticalSection(&hub.lock); hub.dirty=1; LeaveCriticalSection(&hub.lock);
        retry=1; status(2);
        while(WaitForSingleObject(hub.stop,0)!=WAIT_OBJECT_0 && WaitForSingleObject(receiver,0)==WAIT_TIMEOUT && !InterlockedCompareExchange(&hub.reconnect,0,0)) {
            char subscription[1100]; signal_frame frame={0}; int dirty,have; DWORD error=0;
            EnterCriticalSection(&hub.lock);
            dirty=hub.dirty; hub.dirty=0; strcpy_s(subscription,sizeof(subscription),hub.subscription);
            have=hub.out_count!=0;
            if(have) { frame=hub.outgoing[hub.out_head]; SecureZeroMemory(&hub.outgoing[hub.out_head],sizeof(frame)); hub.out_head=(hub.out_head+1)%SIGNAL_QUEUE; --hub.out_count; }
            LeaveCriticalSection(&hub.lock);
            if(dirty) error=WinHttpWebSocketSend(socket,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,subscription,(DWORD)strlen(subscription));
            if(!error && have) error=WinHttpWebSocketSend(socket,WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,frame.bytes,(DWORD)frame.size);
            SecureZeroMemory(&frame,sizeof(frame)); if(error) break;
            if(!have) WaitForMultipleObjects(2,events,FALSE,10000);
        }
        WinHttpWebSocketClose(socket,WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,NULL,0);
        WinHttpCloseHandle(socket); WaitForSingleObject(receiver,INFINITE); CloseHandle(receiver);
        InterlockedExchange(&hub.reconnect,0);
        if(hub.blocked) break;
        status(3);
        if(WaitForSingleObject(hub.stop,1000)==WAIT_OBJECT_0) break;
    }
    WinHttpCloseHandle(session); status(hub.blocked?4:0); return 0;
}
void signal_init(HWND owner) {
    hub.owner=owner; InitializeCriticalSection(&hub.lock);
    hub.stop=CreateEventW(NULL,TRUE,FALSE,NULL); hub.wake=CreateEventW(NULL,FALSE,FALSE,NULL);
}
void signal_subscribe(const friend_book *book) {
    char text[1100]="JOIN|"; size_t used=5; DWORD i; static const char hex[]="0123456789abcdef";
    for(i=0;i<book->count && i<16;++i) {
        uint8_t route[32]; unsigned j; if(direct_friend_route(book->items[i].secret,route)) continue;
        for(j=0;j<32;++j) { text[used++]=hex[route[j]>>4]; text[used++]=hex[route[j]&15]; }
        text[used++]=book->items[i].offerer?'0':'1'; text[used++]='|';
    }
    text[used]=0; EnterCriticalSection(&hub.lock); strcpy_s(hub.subscription,sizeof(hub.subscription),text); hub.dirty=1; LeaveCriticalSection(&hub.lock); SetEvent(hub.wake);
}
int signal_start(const direct_service *profile,const friend_book *book) {
    if(!direct_service_valid(profile) || !book->count || !hub.stop || !hub.wake) return -1;
    if(hub.thread && WaitForSingleObject(hub.thread,0)==WAIT_TIMEOUT) { signal_subscribe(book); return 0; }
    if(hub.thread) CloseHandle(hub.thread);
    hub.profile=*profile; hub.blocked=0; ResetEvent(hub.stop); signal_subscribe(book);
    hub.thread=CreateThread(NULL,0,worker,NULL,0,NULL); return hub.thread?0:-1;
}
int signal_send(const uint8_t route[32],const uint8_t *packet,size_t size) {
    signal_frame *frame;
    if(size>6064 || size<66) return -1;
    EnterCriticalSection(&hub.lock);
    if(hub.out_count==SIGNAL_QUEUE || hub.state!=2) { LeaveCriticalSection(&hub.lock); return -1; }
    frame=&hub.outgoing[(hub.out_head+hub.out_count)%SIGNAL_QUEUE]; memcpy(frame->bytes,route,32); frame->bytes[32]=1;
    memcpy(frame->bytes+33,packet,size); frame->size=size+33; ++hub.out_count;
    LeaveCriticalSection(&hub.lock); SetEvent(hub.wake); return 0;
}
int signal_receive(signal_frame *frame) {
    EnterCriticalSection(&hub.lock);
    if(!hub.in_count) { LeaveCriticalSection(&hub.lock); return 0; }
    *frame=hub.incoming[hub.in_head]; SecureZeroMemory(&hub.incoming[hub.in_head],sizeof(*frame)); hub.in_head=(hub.in_head+1)%SIGNAL_QUEUE; --hub.in_count;
    LeaveCriticalSection(&hub.lock); return 1;
}
int signal_state(void) { int value; EnterCriticalSection(&hub.lock); value=hub.state; LeaveCriticalSection(&hub.lock); return value; }
void signal_stop(void) { SetEvent(hub.stop); SetEvent(hub.wake); }

#ifdef MURMUR_INTERNET_TEST
void signal_test_reconnect(void) { InterlockedExchange(&hub.reconnect,1); SetEvent(hub.wake); }
#endif
