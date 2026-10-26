#include "murmur/screen_frame.h"
#include <string.h>
#include <stdio.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    static direct_screen_frame f;
    uint8_t jpeg[DIRECT_SCREEN_MAX],packet[1000]; size_t n; unsigned i;
    memset(jpeg,0xa5,sizeof(jpeg));
    CHECK(!direct_screen_packet(packet,0,jpeg,1,0));
    CHECK(!direct_screen_packet(packet,1,jpeg,DIRECT_SCREEN_MAX+1,0));
    CHECK(!direct_screen_packet(packet,1,jpeg,1,1));
    /* Reverse order; a duplicate never completes early. */
    for(i=DIRECT_SCREEN_PARTS;i>0;--i) {
        n=direct_screen_packet(packet,1,jpeg,sizeof(jpeg),i-1); CHECK(n>16 && n<=1000);
        CHECK(direct_screen_feed(&f,packet,n,10)==(i==1 ? 1 : 0));
        CHECK(direct_screen_feed(&f,packet,n,10)==0);
    }
    CHECK(f.total==sizeof(jpeg) && !memcmp(f.bytes,jpeg,sizeof(jpeg)));
    n=direct_screen_packet(packet,2,jpeg,2000,0); CHECK(!direct_screen_feed(&f,packet,n,100));
    n=direct_screen_packet(packet,2,jpeg,2000,1); CHECK(!direct_screen_feed(&f,packet,n,601));
    /* Stop supersedes incomplete frames and delayed packets cannot revive it. */
    n=direct_screen_packet(packet,3,NULL,0,0); CHECK(direct_screen_feed(&f,packet,n,602)==2);
    n=direct_screen_packet(packet,2,jpeg,2000,1); CHECK(!direct_screen_feed(&f,packet,n,603));
    n=direct_screen_packet(packet,4,jpeg,20,0);
    CHECK(!direct_screen_feed(&f,packet,n-1,604));
    packet[14]=1; CHECK(!direct_screen_feed(&f,packet,n,604)); packet[14]=0;
    CHECK(direct_screen_feed(&f,packet,n,604)==1);
    /* Fuzz malformed metadata with deterministic bytes; no allocations. */
    for(i=0;i<10000;++i) {
        packet[i%16]=(uint8_t)i; (void)direct_screen_feed(&f,packet,i%1001,605);
    }
    puts("Bounded screen reassembly, loss, reorder, duplicate, stop and malformed frames passed."); return 0;
}
