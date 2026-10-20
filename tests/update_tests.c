#include "murmur/update.h"
#include "update_fixture.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
    direct_update update; direct_version a,b; uint8_t changed[sizeof(fixture_manifest)], payload[sizeof(fixture_payload)]; size_t i;
    CHECK(!direct_version_parse("0.10.0",&a) && !direct_version_parse("0.9.99",&b) && direct_version_compare(a,b)>0);
    CHECK(direct_version_compare(a,a)==0 && direct_version_compare(b,a)<0);
    CHECK(direct_version_parse("0.1",&a) && direct_version_parse("0.01.0",&a) && direct_version_parse("4294967296.0.0",&a));
    CHECK(direct_version_parse("1.0.0-beta",&a) && direct_version_parse("v1.0.0",&a) && direct_version_parse("1.0.0\n",&a));
    CHECK(!direct_update_verify(fixture_manifest,sizeof(fixture_manifest),fixture_signature,sizeof(fixture_signature),&update));
    CHECK(update.version.major==9 && update.version.minor==8 && update.version.patch==7);
    CHECK(!direct_update_hash(&update,fixture_payload,sizeof(fixture_payload)));
    memcpy(payload,fixture_payload,sizeof(payload)); payload[2]^=1;
    CHECK(direct_update_hash(&update,payload,sizeof(payload)) && direct_update_hash(&update,fixture_payload,sizeof(fixture_payload)-1));
    memcpy(changed,fixture_manifest,sizeof(changed));
    for (i=0;i<sizeof(changed);++i) { changed[i]^=1; CHECK(direct_update_verify(changed,sizeof(changed),fixture_signature,sizeof(fixture_signature),&update)); changed[i]=fixture_manifest[i]; }
    CHECK(direct_update_verify(fixture_manifest,sizeof(fixture_manifest),fixture_signature,sizeof(fixture_signature)-1,&update));
    CHECK(direct_update_verify(fixture_wrong_platform,sizeof(fixture_wrong_platform),fixture_wrong_platform_sig,sizeof(fixture_wrong_platform_sig),&update));
    puts("Signed manifests, tampering, platform restrictions and version ordering passed."); return 0;
}
