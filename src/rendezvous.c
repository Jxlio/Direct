#include <stddef.h>
#include "murmur/rendezvous.h"
#include "murmur/friend.h"
#include <mbedtls/hkdf.h>
#include <mbedtls/platform_util.h>
#include <string.h>
#include <stdio.h>
int direct_service_valid(const direct_service *s) {
    size_t n,i;
    if(!s || !memchr(s->origin,0,sizeof(s->origin)) || !memchr(s->access,0,sizeof(s->access))) return 0;
    n=strlen(s->origin);
    if(n<24 || n>240 || strncmp(s->origin,"https://",8) || strcmp(s->origin+n-12,".workers.dev") || strlen(s->access)!=64) return 0;
    for(i=8;i<n;++i) if(!((s->origin[i]>='a'&&s->origin[i]<='z') || (s->origin[i]>='0'&&s->origin[i]<='9') || s->origin[i]=='-' || s->origin[i]=='.')) return 0;
    for(i=0;i<64;++i) if(!((s->access[i]>='0'&&s->access[i]<='9') || (s->access[i]>='a'&&s->access[i]<='f'))) return 0;
    return 1;
}
int direct_invite_create(const uint8_t secret[32],const direct_service *s,char out[DIRECT_INVITE_MAX]) {
    char code[MURMUR_FRIEND_CODE]; int n;
    if(!secret || !out || murmur_friend_code(secret,code)) return -1;
    if(!s || !s->origin[0]) { memcpy(out,code,sizeof(code)); return 0; }
    if(!direct_service_valid(s)) return -1;
    n=snprintf(out,DIRECT_INVITE_MAX,"direct2|%s|%s|%s",code,s->origin,s->access);
    mbedtls_platform_zeroize(code,sizeof(code)); return n>0 && n<DIRECT_INVITE_MAX?0:-1;
}
int direct_invite_parse(const char *code,uint8_t secret[32],direct_service *s) {
    char legacy[MURMUR_FRIEND_CODE]; direct_service candidate={{0},{0}}; uint8_t key[32];
    const char *split; size_t n; int result=-1;
    if(!code || !secret || !s || strlen(code)>=DIRECT_INVITE_MAX) return -1;
    if(!strncmp(code,"direct2|",8)) {
        if(strlen(code)<8+72+1 || code[80]!='|') return -1;
        memcpy(legacy,code+8,72); legacy[72]=0;
        split=strchr(code+81,'|'); if(!split) return -1;
        n=(size_t)(split-(code+81)); if(n>=sizeof(candidate.origin) || strlen(split+1)!=64) return -1;
        memcpy(candidate.origin,code+81,n); memcpy(candidate.access,split+1,65);
        if(!direct_service_valid(&candidate)) goto done;
        if(murmur_friend_parse(legacy,key)) goto done;
    } else if(murmur_friend_parse(code,key)) goto done;
    memcpy(secret,key,32); *s=candidate; result=0;
done:
    mbedtls_platform_zeroize(key,sizeof(key)); mbedtls_platform_zeroize(legacy,sizeof(legacy));
    mbedtls_platform_zeroize(&candidate,sizeof(candidate)); return result;
}
int direct_friend_route(const uint8_t secret[32],uint8_t route[32]) {
    static const unsigned char domain[]="direct/rendezvous/route/v1";
    if(!secret || !route) return -1;
    return mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),NULL,0,secret,32,domain,sizeof(domain)-1,route,32);
}
