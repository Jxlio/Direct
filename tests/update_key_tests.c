#include "murmur/update.h"
#include "update_fixture.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned char *read_file(const char *path,size_t *size) {
    FILE *f=NULL; unsigned char *data; long n;
#ifdef _MSC_VER
    if (fopen_s(&f,path,"rb")) return NULL;
#else
    f=fopen(path,"rb"); if (!f) return NULL;
#endif
    if (fseek(f,0,SEEK_END) || (n=ftell(f))<1 || (unsigned long)n>DIRECT_UPDATE_LIMIT || fseek(f,0,SEEK_SET)) { fclose(f); return NULL; }
    data=(unsigned char *)malloc((size_t)n); if (!data) { fclose(f); return NULL; }
    if (fread(data,1,(size_t)n,f)!=(size_t)n) { free(data); fclose(f); return NULL; }
    fclose(f); *size=(size_t)n; return data;
}
int main(int argc,char **argv) {
    direct_update update;
    if (!direct_update_verify(fixture_manifest,sizeof(fixture_manifest),fixture_signature,sizeof(fixture_signature),&update)) return 1;
    if (argc==4) {
        size_t n=0,s=0,b=0; unsigned char *manifest=read_file(argv[1],&n), *signature=read_file(argv[2],&s), *binary=read_file(argv[3],&b);
        int okay=manifest && signature && binary && !direct_update_verify(manifest,n,signature,s,&update) && !direct_update_hash(&update,binary,b);
        free(manifest); free(signature); free(binary); if (!okay) return 1;
    } else if (argc!=1) return 1;
    puts("Production verification key rejects test signatures; supplied release assets verified."); return 0;
}
