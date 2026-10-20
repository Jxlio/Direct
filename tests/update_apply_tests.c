#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "updates.h"
#include "update_fixture.h"
int link_voice_requested(void) { return 0; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static int write_bytes(const WCHAR *path,const void *data,DWORD size) {
    HANDLE f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL); DWORD written; int r;
    if (f==INVALID_HANDLE_VALUE) return -1; r=WriteFile(f,data,size,&written,NULL)&&written==size; CloseHandle(f); return r?0:-1;
}
static int equal_files(const WCHAR *a,const WCHAR *b) {
    HANDLE fa=CreateFileW(a,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL), fb=CreateFileW(b,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    unsigned char aa[4096],bb[4096]; DWORD na,nb; int result=0;
    if (fa==INVALID_HANDLE_VALUE || fb==INVALID_HANDLE_VALUE) goto done;
    for (;;) {
        if (!ReadFile(fa,aa,sizeof(aa),&na,NULL) || !ReadFile(fb,bb,sizeof(bb),&nb,NULL) || na!=nb || memcmp(aa,bb,na)) goto done;
        if (!na) { result=1; break; }
    }
done:
    if (fa!=INVALID_HANDLE_VALUE) CloseHandle(fa); if (fb!=INVALID_HANDLE_VALUE) CloseHandle(fb); return result;
}
static int scenario(int altered) {
    WCHAR temp[MAX_PATH],root[MAX_PATH],target[MAX_PATH],file[MAX_PATH],self[MAX_PATH],command[MAX_PATH*3];
    STARTUPINFOW si={0}; PROCESS_INFORMATION holder={0},helper={0}; HANDLE f; DWORD read;
    uint8_t data[sizeof(fixture_payload)]; LARGE_INTEGER length;
    CHECK(GetTempPathW(MAX_PATH,temp) && GetTempFileNameW(temp,L"dup",0,root) && DeleteFileW(root) && CreateDirectoryW(root,NULL));
    CHECK(GetModuleFileNameW(NULL,self,MAX_PATH)); swprintf_s(target,MAX_PATH,L"%ls\\Direct.exe",root); CHECK(CopyFileW(self,target,TRUE));
    swprintf_s(file,MAX_PATH,L"%ls\\update.manifest",root); CHECK(!write_bytes(file,fixture_manifest,sizeof(fixture_manifest)));
    swprintf_s(file,MAX_PATH,L"%ls\\update.sig",root); CHECK(!write_bytes(file,fixture_signature,sizeof(fixture_signature)));
    memcpy(data,fixture_payload,sizeof(data)); if (altered) data[0]^=1;
    swprintf_s(file,MAX_PATH,L"%ls\\update.exe",root); CHECK(!write_bytes(file,data,sizeof(data)));
    swprintf_s(command,MAX_PATH*3,L"\"%ls\" --hold",target); si.cb=sizeof(si);
    CHECK(CreateProcessW(target,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&holder));
    swprintf_s(command,MAX_PATH*3,L"\"%ls\" --direct-apply-update %lu \"%ls\" \"%ls\"",self,holder.dwProcessId,target,root);
    CHECK(CreateProcessW(self,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&helper));
    CHECK(WaitForSingleObject(helper.hProcess,10000)==WAIT_OBJECT_0 && WaitForSingleObject(holder.hProcess,2000)==WAIT_OBJECT_0);
    CloseHandle(helper.hThread); CloseHandle(helper.hProcess); CloseHandle(holder.hThread); CloseHandle(holder.hProcess);
    f=CreateFileW(target,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL); CHECK(f!=INVALID_HANDLE_VALUE && GetFileSizeEx(f,&length));
    if (!altered) { CHECK(length.QuadPart==sizeof(data) && ReadFile(f,data,sizeof(data),&read,NULL) && read==sizeof(data) && !memcmp(data,fixture_payload,sizeof(data))); }
    else CHECK(length.QuadPart>sizeof(data));
    CloseHandle(f);
    if (altered) CHECK(equal_files(target,self));
    else { swprintf_s(file,MAX_PATH,L"%ls.update-backup",target); CHECK(equal_files(file,self)); }
    CHECK(DeleteFileW(target));
    { const WCHAR *names[]={L"update.exe",L"update.manifest",L"update.sig",L"Direct.exe.update-backup",L"Direct.exe.update-new"}; unsigned i;
      for (i=0;i<sizeof(names)/sizeof(names[0]);++i) { swprintf_s(file,MAX_PATH,L"%ls\\%ls",root,names[i]); DeleteFileW(file); } }
    CHECK(RemoveDirectoryW(root)); return 0;
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--hold")) { Sleep(500); return 0; }
    if (updates_apply_command()) return 0;
    CHECK(!scenario(0)); CHECK(!scenario(1)); puts("Apply after process exit and refusal of altered payload passed."); return 0;
}
