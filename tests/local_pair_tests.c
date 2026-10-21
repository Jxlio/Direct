#include "local_test.h"
#include "link.h"
#include <stdio.h>
#include <string.h>
static int send_text(const char *text) { return link_send(0,(const uint8_t *)text,strlen(text)); }
int main(int argc,char **argv) {
    int responder=argc==2 && !strcmp(argv[1],"--test-peer"),stage=0,okay=0;
    ULONGLONG deadline=GetTickCount64()+12000,finish=0; link_message message;
    link_init(NULL); if(local_test_start(NULL,responder)) return 1;
    while(GetTickCount64()<deadline) {
        local_test_tick(); if(link_state()==3) break;
        if(link_state()==2 && stage==0) {
            if(!responder) { if(link_voice_toggle()) break; stage=1; }
            else if(link_voice_remote()&1) {
                /* An incoming request must never opt us in or open a device. */
                if(link_voice_requested() || link_voice_state()!=0 || link_voice_decoded_frames()) break;
                if(link_voice_toggle()) break; stage=1;
            }
        }
        if(responder && stage==1 && link_voice_decoded_frames()>=3) { link_voice_mute_toggle(); stage=2; }
        if(!responder && stage==1 && link_voice_remote()==3 && link_voice_decoded_frames()>=3) {
            if(send_text("mute seen")) break; stage=2;
        }
        while(link_receive(&message)) {
            if(responder && stage==2 && message.size==9 && !memcmp(message.text,"mute seen",9)) {
                if(link_voice_toggle()) goto done; stage=3;
            } else if(responder && stage==3 && message.size==10 && !memcmp(message.text,"call ended",10)) {
                if(link_voice_requested() || send_text("done")) goto done;
                stage=4; finish=GetTickCount64()+300;
            } else if(!responder && stage==3 && message.size==4 && !memcmp(message.text,"done",4)) {
                okay=1; finish=GetTickCount64()+150; stage=4;
            }
        }
        if(!responder && stage==2 && !link_voice_remote() && !link_voice_requested() && !link_voice_state()) {
            if(send_text("call ended")) break; stage=3;
        }
        if(stage==4 && GetTickCount64()>=finish) { okay=1; break; }
        Sleep(5);
    }
done:
    link_stop(); local_test_close(); deadline=GetTickCount64()+2000;
    while(link_busy() && GetTickCount64()<deadline) Sleep(2);
    if(link_busy()) okay=0;
    if(!responder) puts(okay?"Local encrypted messages, incoming call opt-in, simulated audio, remote mute and mutual hangup passed.":"Local call transitions failed.");
    return okay?0:1;
}
