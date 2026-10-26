#include "local_test.h"
#include "link.h"
#include <stdio.h>
#include <string.h>
static struct {
    HANDLE input,output,child;
    int responder,sent,received;
    size_t used;
    char incoming[MURMUR_ICE_TOKEN];
    ULONGLONG deadline;
} pair;
static int record_mode;
void local_test_record_mode(int enabled) { record_mode=!!enabled; }
static int spawn_pair_client(HWND owner,HANDLE input,HANDLE output,HANDLE error) {
    WCHAR executable[MAX_PATH],command[MAX_PATH+64]; STARTUPINFOEXW si={0}; PROCESS_INFORMATION pi={0};
    SIZE_T bytes=0; HANDLE handles[3]={input,output,error}; int result=-1;
    if (!GetModuleFileNameW(NULL,executable,MAX_PATH)) return -1;
    swprintf_s(command,MAX_PATH+64,record_mode?L"\"%ls\" --test-peer-record":L"\"%ls\" --test-peer",executable);
    InitializeProcThreadAttributeList(NULL,1,0,&bytes);
    si.lpAttributeList=(LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(),0,bytes);
    if (!si.lpAttributeList) return -1;
    if (InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&bytes)) {
        if (UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),NULL,NULL)) {
            si.StartupInfo.cb=sizeof(si); si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput=input; si.StartupInfo.hStdOutput=output; si.StartupInfo.hStdError=error;
            if (CreateProcessW(executable,command,NULL,NULL,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,NULL,NULL,&si.StartupInfo,&pi)) {
                pair.child=pi.hProcess; CloseHandle(pi.hThread); result=0;
            }
        }
        DeleteProcThreadAttributeList(si.lpAttributeList);
    }
    HeapFree(GetProcessHeap(),0,si.lpAttributeList); (void)owner; return result;
}
int local_test_launch(HWND owner) {
    WCHAR executable[MAX_PATH],command[MAX_PATH+64]; STARTUPINFOW si={0}; PROCESS_INFORMATION pi={0};
    if (!GetModuleFileNameW(NULL,executable,MAX_PATH)) return -1;
    swprintf_s(command,MAX_PATH+64,L"\"%ls\" --test-pair",executable); si.cb=sizeof(si);
    if (!CreateProcessW(executable,command,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi)) return -1;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    (void)owner;
    return 0;
}
int local_test_start(HWND owner,int responder) {
    murmur_ice_config settings={0}; HANDLE child_input=NULL,child_output=NULL,error=NULL;
    SECURITY_ATTRIBUTES access={sizeof(access),NULL,TRUE}; int result=-1;
    memset(&pair,0,sizeof(pair)); pair.responder=!!responder; pair.deadline=GetTickCount64()+20000;
    if (link_set_test_audio(1)) return -1;
    if (responder) {
        pair.input=GetStdHandle(STD_INPUT_HANDLE); pair.output=GetStdHandle(STD_OUTPUT_HANDLE);
        if (GetFileType(pair.input)!=FILE_TYPE_PIPE || GetFileType(pair.output)!=FILE_TYPE_PIPE) goto done;
    } else {
        if (!CreatePipe(&child_input,&pair.output,&access,16384) || !CreatePipe(&pair.input,&child_output,&access,16384) ||
            !SetHandleInformation(pair.input,HANDLE_FLAG_INHERIT,0) || !SetHandleInformation(pair.output,HANDLE_FLAG_INHERIT,0)) goto done;
        error=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&access,OPEN_EXISTING,0,NULL);
        if (error==INVALID_HANDLE_VALUE) { error=NULL; goto done; }
        if (spawn_pair_client(owner,child_input,child_output,error)) goto done;
        strcpy_s(settings.bind_address,sizeof(settings.bind_address),"127.0.0.1");
        if (link_ice_start(1,&settings,NULL)) goto done;
    }
    result=0;
done:
    if (child_input) CloseHandle(child_input); if (child_output) CloseHandle(child_output); if (error) CloseHandle(error);
    if (result) local_test_close(); return result;
}
void local_test_tick(void) {
    DWORD available,read,written; char token[MURMUR_ICE_TOKEN]={0};
    if (!pair.input || !pair.output) return;
    if (GetTickCount64()>pair.deadline || link_state()==3) { local_test_close(); return; }
    if (!pair.received) {
        if (!PeekNamedPipe(pair.input,NULL,0,NULL,&available,NULL)) { local_test_close(); return; }
        if (available) {
            size_t capacity=sizeof(pair.incoming)-1-pair.used;
            if (!capacity || available>capacity || !ReadFile(pair.input,pair.incoming+pair.used,available,&read,NULL)) { local_test_close(); return; }
            pair.used+=read; pair.incoming[pair.used]=0;
            if (memchr(pair.incoming,'\n',pair.used)) {
                murmur_ice_config settings={0}; char *end=strchr(pair.incoming,'\n'); *end=0;
                strcpy_s(settings.bind_address,sizeof(settings.bind_address),"127.0.0.1");
                if (pair.responder ? link_ice_start(0,&settings,pair.incoming) : link_ice_answer(pair.incoming)) { local_test_close(); return; }
                pair.received=1; SecureZeroMemory(pair.incoming,sizeof(pair.incoming));
            }
        }
    }
    if (!pair.sent && (!pair.responder || pair.received) && link_ice_token(token,sizeof(token))==1) {
        size_t size=strlen(token); token[size++]='\n';
        if (!WriteFile(pair.output,token,(DWORD)size,&written,NULL) || written!=size) { SecureZeroMemory(token,sizeof(token)); local_test_close(); return; }
        pair.sent=1;
    }
    SecureZeroMemory(token,sizeof(token));
    if (pair.sent && pair.received && link_state()==2) local_test_close();
}
void local_test_close(void) {
    if (pair.input && pair.input!=INVALID_HANDLE_VALUE) CloseHandle(pair.input);
    if (pair.output && pair.output!=INVALID_HANDLE_VALUE) CloseHandle(pair.output);
    if (pair.child) CloseHandle(pair.child);
    SecureZeroMemory(&pair,sizeof(pair));
}
