#include "media.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"check failed at line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static link_message sent;
static int sends;
int link_state(void) { return 2; }
int link_send(unsigned room,const uint8_t *bytes,size_t size) {
    if(room || !bytes || size>MURMUR_CHAT_LIMIT) return -1;
    sent.room=room; sent.size=size; memcpy(sent.text,bytes,size); ++sends; return 0;
}
static void number(uint8_t *p,uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void packet(link_message *m,uint32_t id,uint32_t total,uint32_t offset,const uint8_t *bytes,size_t size) {
    memset(m,0,sizeof(*m)); m->size=17+size;
    m->text[1]='D'; m->text[2]='M'; m->text[3]=1; m->text[4]=2;
    number(m->text+5,id); number(m->text+9,total); number(m->text+13,offset);
    memcpy(m->text+17,bytes,size);
}
int main(void) {
    static const uint8_t png[68]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,4,0,0,0,181,28,12,2,0,0,0,11,73,68,65,84,120,218,99,252,255,31,0,3,3,2,0,239,191,249,119,0,0,0,0,73,69,78,68,174,66,96,130};
    static const uint8_t gif[111]={71,73,70,56,57,97,2,0,2,0,129,0,0,255,0,0,0,0,0,0,0,0,0,0,0,33,255,11,78,69,84,83,67,65,80,69,50,46,48,3,1,0,0,0,33,249,4,0,10,0,0,0,44,0,0,0,0,2,0,2,0,0,8,6,0,1,8,4,16,16,0,33,249,4,1,10,0,1,0,44,0,0,0,0,2,0,2,0,129,0,0,255,0,0,0,0,0,0,0,0,0,8,6,0,1,8,4,16,16,0,59};
    link_message m; uint32_t id=0; unsigned kind=0;
    const uint8_t *content=NULL; size_t size=0; const wchar_t *mime=NULL;
    packet(&m,42,68,0,png,32); CHECK(media_receive(&m,&id,&kind)==0);
    packet(&m,42,68,33,png+32,35); CHECK(media_receive(&m,&id,&kind)==-1);
    packet(&m,42,68,32,png+32,36); CHECK(media_receive(&m,&id,&kind)==1);
    CHECK(id==42 && kind==2);
    CHECK(media_get(42,&content,&size,&mime)==1 && size==68 && !memcmp(content,png,68));
    CHECK(!wcscmp(mime,L"image/png"));
    packet(&m,43,DIRECT_MEDIA_MAX+1,0,png,10); CHECK(media_receive(&m,&id,&kind)==-1);
    packet(&m,44,68,0,png,68); m.text[4]=3; CHECK(media_receive(&m,&id,&kind)==-1);
    media_close();
    CHECK(!media_get(42,NULL,NULL,NULL));
    CHECK(media_queue_bytes(png,sizeof(png),&id,&kind)==1 && kind==2);
    CHECK(media_pump()==1 && sends==1 && sent.size==sizeof(png)+17);
    media_close();
    CHECK(media_receive(&sent,&id,&kind)==1 && kind==2);
    CHECK(media_get(id,&content,&size,&mime)==1 && size==sizeof(png) && !memcmp(content,png,size));
    media_close();
    CHECK(media_queue_bytes(gif,sizeof(gif),&id,&kind)==1 && kind==3);
    CHECK(media_pump()==1 && sends==2);
    media_close();
    CHECK(media_receive(&sent,&id,&kind)==1 && kind==3);
    CHECK(media_get(id,&content,&size,&mime)==1 && size==sizeof(gif) && !memcmp(content,gif,size));
    CHECK(!wcscmp(mime,L"image/gif"));
    media_close();
    puts("Bounded image reassembly, ordering, type validation and cache cleanup passed.");
    return 0;
}
