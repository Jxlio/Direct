#include "local_test.h"
#include "link.h"
#include "media.h"
#include <stdio.h>
#include <string.h>
static uint8_t png[60000];
static size_t png_size;
int main(int argc,char **argv) {
    int responder=argc==2 && !strcmp(argv[1],"--test-peer"),sent=0,okay=0;
    ULONGLONG deadline=GetTickCount64()+15000;
    HANDLE file=CreateFileW(L"media-large.png",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    DWORD read=0;
    if(file==INVALID_HANDLE_VALUE) return 1;
    png_size=GetFileSize(file,NULL);
    if(png_size<8192 || png_size>sizeof(png) || !ReadFile(file,png,(DWORD)png_size,&read,NULL) || read!=png_size) { CloseHandle(file); return 1; }
    CloseHandle(file);
    link_init(NULL);
    if(local_test_start(NULL,responder)) return 1;
    while(GetTickCount64()<deadline) {
        link_message incoming={0};
        local_test_tick();
        if(link_state()==3) break;
        if(!responder && link_state()==2 && !sent) {
            if(media_queue_bytes(png,png_size,NULL,NULL)!=1) break;
            sent=1;
        }
        if(sent && !responder && media_pump()<0) break;
        while(link_receive(&incoming)) {
            if(responder && incoming.size && incoming.text[0]==0) {
                uint32_t id; unsigned kind; const uint8_t *bytes; size_t size;
                if(media_receive(&incoming,&id,&kind)==1 && kind==2 &&
                   media_get(id,&bytes,&size,NULL) && size==png_size && !memcmp(bytes,png,size)) {
                    if(!link_send(0,(const uint8_t *)"image received",14)) { okay=1; deadline=GetTickCount64()+150; }
                }
            } else if(!responder && incoming.size==14 && !memcmp(incoming.text,"image received",14)) {
                okay=1; deadline=GetTickCount64()+150;
            }
        }
        Sleep(5);
    }
    link_stop(); local_test_close(); media_close();
    if(!responder) puts(okay?"Image crossed the encrypted local connection.":"Image transfer failed.");
    return okay?0:1;
}
