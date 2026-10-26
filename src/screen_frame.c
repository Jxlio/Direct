#include "murmur/screen_frame.h"
#include <string.h>
static uint32_t read32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void write32(uint8_t *p,uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
size_t direct_screen_packet(uint8_t packet[1000],uint32_t id,const uint8_t *jpeg,size_t total,unsigned part) {
    size_t n; unsigned count;
    if (!packet || !id || total>DIRECT_SCREEN_MAX || (!jpeg && total)) return 0;
    count=(unsigned)((total+DIRECT_SCREEN_CHUNK-1)/DIRECT_SCREEN_CHUNK);
    if ((total && part>=count) || (!total && part)) return 0;
    n=total ? total-part*DIRECT_SCREEN_CHUNK : 0;
    if(n>DIRECT_SCREEN_CHUNK) n=DIRECT_SCREEN_CHUNK;
    memcpy(packet,"DSC1",4); write32(packet+4,id); write32(packet+8,(uint32_t)total);
    packet[12]=0; packet[13]=(uint8_t)part; packet[14]=0; packet[15]=(uint8_t)count;
    if(n) memcpy(packet+16,jpeg+part*DIRECT_SCREEN_CHUNK,n);
    return n+16;
}
int direct_screen_feed(direct_screen_frame *f,const uint8_t *p,size_t size,uint64_t now) {
    uint32_t id,total; unsigned part,count; size_t n;
    if(!f || !p || size<16 || size>1000 || memcmp(p,"DSC1",4) || p[12] || p[14]) return 0;
    id=read32(p+4); total=read32(p+8); part=p[13]; count=p[15];
    if(!id || total>DIRECT_SCREEN_MAX || count!=(total+DIRECT_SCREEN_CHUNK-1)/DIRECT_SCREEN_CHUNK) return 0;
    if(f->id && id!=f->id && (uint32_t)(id-f->id)>UINT32_C(0x7fffffff)) return 0;
    if(!total) {
        if(size!=16 || part || id==f->id) return 0;
        memset(f,0,sizeof(*f)); f->id=id; return 2;
    }
    if(part>=count) return 0;
    n=total-part*DIRECT_SCREEN_CHUNK; if(n>DIRECT_SCREEN_CHUNK) n=DIRECT_SCREEN_CHUNK;
    if(size!=16+n) return 0;
    if(id!=f->id) {
        memset(f,0,sizeof(*f)); f->id=id; f->total=total; f->count=count; f->started=now;
    }
    if(f->total!=total || f->count!=count || now-f->started>500 || f->seen[part]) return 0;
    memcpy(f->bytes+part*DIRECT_SCREEN_CHUNK,p+16,n); f->seen[part]=1;
    return ++f->received==count ? 1 : 0;
}
