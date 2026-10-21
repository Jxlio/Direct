/* Explicit opt-in integration test against the configured private service.
   No saved contacts, public STUN, audio devices or public IP candidates. */
#include "link.h"
#include "friends.h"
#include "signaling.h"
#include <stdio.h>
#include <string.h>
extern int friends_test_online(int offerer);
extern void signal_test_reconnect(void);
int main(int argc,char **argv) {
    int peer=argc==2 && !strcmp(argv[1],"--peer"),sent=0,got=0,okay=0,reconnecting=0,saw_offline=0,final_sent=0,final_got=0;
    ULONGLONG deadline=GetTickCount64()+30000; PROCESS_INFORMATION child={0};
    if(!peer && (argc!=2 || strcmp(argv[1],"--run"))) { puts("Use --run explicitly to test the configured Internet service."); return 2; }
    if(!peer) {
        WCHAR executable[MAX_PATH],command[MAX_PATH+40]; STARTUPINFOW startup={sizeof(startup)};
        GetModuleFileNameW(NULL,executable,MAX_PATH); swprintf_s(command,MAX_PATH+40,L"\"%ls\" --peer",executable);
        if(!CreateProcessW(executable,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&child)) return 1;
        CloseHandle(child.hThread);
    }
    link_init(NULL); if(friends_test_online(!peer)) goto done;
    while(GetTickCount64()<deadline) {
        link_message message; friends_internet_event();
        if(link_state()==2 && !sent) { if(link_send(0,(const uint8_t *)"encrypted hello",15)) break; sent=1; }
        while(link_receive(&message)) {
            if(message.size==15 && !memcmp(message.text,"encrypted hello",15)) got=1;
            if(peer && message.size==15 && !memcmp(message.text,"after reconnect",15)) { if(!link_send(0,(const uint8_t *)"reconnect ack",13)) final_got=1; }
            if(!peer && message.size==13 && !memcmp(message.text,"reconnect ack",13)) final_got=1;
        }
        if(!peer && sent && got && !reconnecting) { signal_test_reconnect(); reconnecting=1; }
        if(reconnecting && signal_state()!=2) saw_offline=1;
        if(reconnecting && saw_offline && signal_state()==2 && !final_sent) { if(!link_send(0,(const uint8_t *)"after reconnect",15)) final_sent=1; }
        if(final_got) { okay=1; Sleep(300); break; }
        Sleep(10);
    }
done:
    signal_stop(); link_stop(); deadline=GetTickCount64()+2000;
    while(link_busy() && GetTickCount64()<deadline) Sleep(5);
    if(!peer && child.hProcess) {
        DWORD exit_code=1; WaitForSingleObject(child.hProcess,35000); GetExitCodeProcess(child.hProcess,&exit_code); CloseHandle(child.hProcess);
        if(exit_code) okay=0;
        printf("Hosted signaling, automatic encrypted session and WSS reconnection: %s (service state %d).\n",okay?"PASS":"FAIL",signal_state());
    }
    return okay?0:1;
}
