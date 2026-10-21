#include "murmur/rendezvous.h"
#include "murmur/friend.h"
#include <string.h>
#include <stdio.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    direct_service profile={"https://example.private.workers.dev","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},parsed;
    uint8_t secret[32]={1},result[32],route[32],other[32],challenge[32]={2},decoded[32],packet[6064];
    char code[DIRECT_INVITE_MAX],token[6000]; size_t size=sizeof(packet); unsigned role;
    CHECK(direct_service_valid(&profile)); CHECK(!direct_invite_create(secret,&profile,code));
    CHECK(!direct_invite_parse(code,result,&parsed) && !memcmp(secret,result,32) && !memcmp(&profile,&parsed,sizeof(profile)));
    CHECK(!direct_friend_route(secret,route)); CHECK(!direct_friend_route(challenge,other)); CHECK(memcmp(route,other,32)); CHECK(memcmp(route,secret,32));
    CHECK(!direct_signal_seal(secret,0,challenge,"private description",packet,&size));
    CHECK(!direct_signal_open(secret,packet,size,&role,decoded,token) && role==0 && !memcmp(decoded,challenge,32) && !strcmp(token,"private description"));
    CHECK(murmur_friend_open(secret,packet,size,&role,decoded,token));
    packet[size-1]^=1; CHECK(direct_signal_open(secret,packet,size,&role,decoded,token));
    strcpy(profile.origin,"http://example.private.workers.dev"); CHECK(!direct_service_valid(&profile));
    strcpy(profile.origin,"https://example.private.workers.dev@evil.example"); CHECK(!direct_service_valid(&profile));
    CHECK(!direct_invite_create(secret,NULL,code)); CHECK(!direct_invite_parse(code,result,&parsed) && !parsed.origin[0]);
    CHECK(direct_invite_parse("direct2|broken",result,&parsed));
    puts("Invitations, service validation, opaque routes and separate authenticated encryption domain passed."); return 0;
}
