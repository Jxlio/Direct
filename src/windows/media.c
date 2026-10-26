#define COBJMACROS
#include "media.h"
#include <commdlg.h>
#include <rpc.h>
#include <wincodec.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define MEDIA_SLOTS 8
#define MEDIA_HEADER 17u
#define MEDIA_PROTOCOL 1u
#define MEDIA_TIMEOUT 30000u

typedef struct {
    uint32_t id;
    unsigned kind;
    size_t size;
    uint8_t *bytes;
    ULONGLONG used;
} MediaEntry;
static struct {
    MediaEntry cache[MEDIA_SLOTS];
    uint32_t sending_id;
    size_t sending_offset;
    uint32_t receiving_id;
    unsigned receiving_kind;
    size_t receiving_size,receiving_offset;
    uint8_t *receiving_bytes;
    ULONGLONG receiving_at;
} media;

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void write32(uint8_t *p,uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static unsigned image_kind(const uint8_t *p,size_t n) {
    static const uint8_t png[]={137,80,78,71,13,10,26,10};
    if(n>=3 && p[0]==0xff && p[1]==0xd8 && p[2]==0xff) return 1;
    if(n>=8 && !memcmp(p,png,8)) return 2;
    if(n>=6 && (!memcmp(p,"GIF87a",6) || !memcmp(p,"GIF89a",6))) return 3;
    return 0;
}
static const wchar_t *image_mime(unsigned kind) {
    static const wchar_t *types[]={L"",L"image/jpeg",L"image/png",L"image/gif"};
    return kind<=3?types[kind]:types[0];
}
static int valid_image(const uint8_t *bytes,size_t size,unsigned kind) {
    IWICImagingFactory *factory=NULL;
    IWICStream *stream=NULL;
    IWICBitmapDecoder *decoder=NULL;
    IWICBitmapFrameDecode *frame=NULL;
    GUID format; UINT width=0,height=0,frames=0;
    HRESULT com=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED),hr;
    int result=0;
    if(FAILED(com) && com!=RPC_E_CHANGED_MODE) return 0;
    hr=CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void **)&factory);
    if(FAILED(hr)) goto done;
    hr=IWICImagingFactory_CreateStream(factory,&stream); if(FAILED(hr)) goto done;
    hr=IWICStream_InitializeFromMemory(stream,(BYTE *)bytes,(DWORD)size); if(FAILED(hr)) goto done;
    hr=IWICImagingFactory_CreateDecoderFromStream(factory,(IStream *)stream,NULL,WICDecodeMetadataCacheOnDemand,&decoder);
    if(FAILED(hr)) goto done;
    hr=IWICBitmapDecoder_GetContainerFormat(decoder,&format); if(FAILED(hr)) goto done;
    if((kind==1 && !IsEqualGUID(&format,&GUID_ContainerFormatJpeg)) ||
       (kind==2 && !IsEqualGUID(&format,&GUID_ContainerFormatPng)) ||
       (kind==3 && !IsEqualGUID(&format,&GUID_ContainerFormatGif))) goto done;
    hr=IWICBitmapDecoder_GetFrame(decoder,0,&frame); if(FAILED(hr)) goto done;
    hr=IWICBitmapFrameDecode_GetSize(frame,&width,&height);
    if(FAILED(hr)) goto done;
    hr=IWICBitmapDecoder_GetFrameCount(decoder,&frames);
    if(SUCCEEDED(hr) && width && height && width<=4096 && height<=4096 &&
       (uint64_t)width*height<=(kind==3?2100000u:8000000u) &&
       frames && frames<=(kind==3?100u:1u)) result=1;
done:
    if(frame) IWICBitmapFrameDecode_Release(frame);
    if(decoder) IWICBitmapDecoder_Release(decoder);
    if(stream) IWICStream_Release(stream);
    if(factory) IWICImagingFactory_Release(factory);
    if(SUCCEEDED(com)) CoUninitialize();
    return result;
}
static MediaEntry *lookup(uint32_t id) {
    unsigned i;
    for(i=0;i<MEDIA_SLOTS;++i) if(media.cache[i].id==id && media.cache[i].bytes) return &media.cache[i];
    return NULL;
}
static void discard(MediaEntry *entry) {
    if(entry->bytes) { SecureZeroMemory(entry->bytes,entry->size); HeapFree(GetProcessHeap(),0,entry->bytes); }
    ZeroMemory(entry,sizeof(*entry));
}
static int store(uint32_t id,unsigned kind,uint8_t *bytes,size_t size) {
    MediaEntry *slot=NULL;
    unsigned i;
    if(lookup(id)) return -1;
    for(i=0;i<MEDIA_SLOTS;++i) {
        if(!media.cache[i].bytes) { slot=&media.cache[i]; break; }
        if(media.cache[i].id!=media.sending_id && (!slot || media.cache[i].used<slot->used)) slot=&media.cache[i];
    }
    if(!slot) return -1;
    discard(slot);
    slot->id=id; slot->kind=kind; slot->bytes=bytes; slot->size=size; slot->used=GetTickCount64();
    return 0;
}
static int begin_owned(uint8_t *bytes,size_t size,unsigned type,uint32_t *id,unsigned *kind) {
    UUID guid; RPC_STATUS status; uint32_t key;
    status=UuidCreate(&guid);
    if(status!=RPC_S_OK && status!=RPC_S_UUID_LOCAL_ONLY) return -1;
    key=guid.Data1 ^ ((uint32_t)guid.Data2<<16 | guid.Data3);
    if(!key) key=1;
    while(lookup(key)) ++key;
    if(store(key,type,bytes,size)) return -1;
    if(id) *id=key; if(kind) *kind=type;
    media.sending_id=key; media.sending_offset=0;
    return 1;
}
int media_queue_bytes(const uint8_t *bytes,size_t size,uint32_t *id,unsigned *kind) {
    uint8_t *copy; unsigned type; int result;
    if(media.sending_id || link_state()!=2) return -2;
    if(!bytes || size<12 || size>DIRECT_MEDIA_MAX) return -3;
    type=image_kind(bytes,size);
    if(!type || !valid_image(bytes,size,type)) return -4;
    copy=HeapAlloc(GetProcessHeap(),0,size); if(!copy) return -1;
    memcpy(copy,bytes,size);
    result=begin_owned(copy,size,type,id,kind);
    if(result<0) HeapFree(GetProcessHeap(),0,copy);
    return result;
}
int media_pick_and_send(HWND owner,uint32_t *id,unsigned *kind,wchar_t *label,size_t label_count) {
    WCHAR path[MAX_PATH]={0};
    OPENFILENAMEW dialog={0};
    HANDLE file=INVALID_HANDLE_VALUE;
    LARGE_INTEGER length;
    uint8_t *bytes=NULL;
    DWORD read=0;
    int result;
    const WCHAR *name;
    unsigned type;
    if(media.sending_id || link_state()!=2) return -2;
    dialog.lStructSize=sizeof(dialog); dialog.hwndOwner=owner;
    dialog.lpstrFilter=L"Images et GIF\0*.jpg;*.jpeg;*.png;*.gif\0Tous les fichiers\0*.*\0\0";
    dialog.lpstrFile=path; dialog.nMaxFile=MAX_PATH;
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_HIDEREADONLY|OFN_NOCHANGEDIR;
    if(!GetOpenFileNameW(&dialog)) return CommDlgExtendedError()? -1:0;
    file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return -1;
    if(!GetFileSizeEx(file,&length) || length.QuadPart<12 || length.QuadPart>DIRECT_MEDIA_MAX) { CloseHandle(file); return -3; }
    bytes=HeapAlloc(GetProcessHeap(),0,(SIZE_T)length.QuadPart);
    if(!bytes || !ReadFile(file,bytes,(DWORD)length.QuadPart,&read,NULL) || read!=(DWORD)length.QuadPart) {
        if(bytes) HeapFree(GetProcessHeap(),0,bytes);
        CloseHandle(file); return -1;
    }
    CloseHandle(file);
    type=image_kind(bytes,(size_t)length.QuadPart);
    if(!type || !valid_image(bytes,(size_t)length.QuadPart,type)) { SecureZeroMemory(bytes,(SIZE_T)length.QuadPart); HeapFree(GetProcessHeap(),0,bytes); return -4; }
    result=begin_owned(bytes,(size_t)length.QuadPart,type,id,kind);
    if(result<0) {
        SecureZeroMemory(bytes,(SIZE_T)length.QuadPart); HeapFree(GetProcessHeap(),0,bytes); return -1;
    }
    name=wcsrchr(path,L'\\'); name=name?name+1:path;
    if(label && label_count) wcsncpy_s(label,label_count,name,_TRUNCATE);
    return 1;
}
int media_pump(void) {
    MediaEntry *entry;
    uint8_t packet[MURMUR_CHAT_LIMIT];
    size_t n;
    if(!media.sending_id) return 1;
    if(link_state()!=2) { media_cancel(); return -1; }
    entry=lookup(media.sending_id);
    if(!entry) { media_cancel(); return -1; }
    n=entry->size-media.sending_offset;
    if(n>MURMUR_CHAT_LIMIT-MEDIA_HEADER) n=MURMUR_CHAT_LIMIT-MEDIA_HEADER;
    packet[0]=0; packet[1]='D'; packet[2]='M'; packet[3]=MEDIA_PROTOCOL; packet[4]=(uint8_t)entry->kind;
    write32(packet+5,entry->id); write32(packet+9,(uint32_t)entry->size);
    write32(packet+13,(uint32_t)media.sending_offset);
    memcpy(packet+MEDIA_HEADER,entry->bytes+media.sending_offset,n);
    if(link_send(0,packet,n+MEDIA_HEADER)) return 0;
    media.sending_offset+=n;
    if(media.sending_offset==entry->size) { media.sending_id=0; media.sending_offset=0; return 1; }
    return 0;
}
void media_cancel(void) { media.sending_id=0; media.sending_offset=0; }
int media_receive(const link_message *packet,uint32_t *id,unsigned *kind) {
    uint32_t key,total,offset;
    unsigned type;
    size_t chunk;
    const uint8_t *p=packet->text;
    if(packet->size<=MEDIA_HEADER || p[0] || p[1]!='D' || p[2]!='M' || p[3]!=MEDIA_PROTOCOL) return -1;
    type=p[4]; key=read32(p+5); total=read32(p+9); offset=read32(p+13); chunk=packet->size-MEDIA_HEADER;
    if(!key || !type || type>3 || !total || total>DIRECT_MEDIA_MAX || offset>total || chunk>total-offset) return -1;
    if(media.receiving_bytes && GetTickCount64()-media.receiving_at>MEDIA_TIMEOUT) {
        HeapFree(GetProcessHeap(),0,media.receiving_bytes); media.receiving_bytes=NULL;
    }
    if(offset==0) {
        if(media.receiving_bytes) HeapFree(GetProcessHeap(),0,media.receiving_bytes);
        media.receiving_bytes=HeapAlloc(GetProcessHeap(),0,total);
        if(!media.receiving_bytes) return -1;
        media.receiving_id=key; media.receiving_kind=type; media.receiving_size=total; media.receiving_offset=0;
    }
    if(!media.receiving_bytes || key!=media.receiving_id || type!=media.receiving_kind || total!=media.receiving_size || offset!=media.receiving_offset) return -1;
    memcpy(media.receiving_bytes+offset,p+MEDIA_HEADER,chunk);
    media.receiving_offset+=chunk; media.receiving_at=GetTickCount64();
    if(media.receiving_offset==media.receiving_size) {
        uint8_t *bytes=media.receiving_bytes;
        media.receiving_bytes=NULL;
        if(image_kind(bytes,total)!=type || !valid_image(bytes,total,type) || store(key,type,bytes,total)) { HeapFree(GetProcessHeap(),0,bytes); return -1; }
        if(id) *id=key; if(kind) *kind=type;
        return 1;
    }
    return 0;
}
int media_get(uint32_t id,const uint8_t **bytes,size_t *size,const wchar_t **mime) {
    MediaEntry *entry=lookup(id);
    if(!entry) return 0;
    entry->used=GetTickCount64();
    if(bytes) *bytes=entry->bytes;
    if(size) *size=entry->size;
    if(mime) *mime=image_mime(entry->kind);
    return 1;
}
void media_close(void) {
    unsigned i;
    media_cancel();
    if(media.receiving_bytes) HeapFree(GetProcessHeap(),0,media.receiving_bytes);
    for(i=0;i<MEDIA_SLOTS;++i) discard(&media.cache[i]);
    ZeroMemory(&media,sizeof(media));
}
