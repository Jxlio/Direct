#define COBJMACROS
#include "screen.h"
#include "murmur/screen_frame.h"
#include <wincodec.h>
#include <objbase.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#define WIDTH 960
#define HEIGHT 540
#define RELEASE(p) do { if(p) { (p)->lpVtbl->Release(p); (p)=NULL; } } while(0)
static struct {
    HWND owner,viewer;
    int com_initialized;
    CRITICAL_SECTION lock;
    HANDLE stop,thread;
    RECT source,monitors[16];
    unsigned monitor_count;
    int test_mode,synthetic;
    LONG sharing, viewing, lost;
    uint8_t latest[DIRECT_SCREEN_MAX],received[DIRECT_SCREEN_MAX];
    size_t latest_size,received_size;
    uint32_t generation;
    uint64_t remote_at;
    /* ICE thread only. */
    direct_screen_frame assembly;
    uint8_t sending[DIRECT_SCREEN_MAX];
    size_t sending_size;
    uint32_t sending_id;
    unsigned part,stops,was_sharing;
    uint64_t sent_at;
    /* UI only. */
    HBITMAP image;
    unsigned width,height;
    HANDLE record_timing;
    WCHAR record_dir[MAX_PATH];
    ULONGLONG record_started;
    unsigned record_count;
    LONG capture_attempts, capture_encoded, capture_oversize, send_packets, received_frames;
    DWORD last_jpeg_bytes;
    DWORD last_unbounded_bytes;
    float last_quality;
    int record_sender;
} share;

static HRESULT factory_create(IWICImagingFactory **factory) {
    return CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void **)factory);
}
/* Only built-in Windows JPEG codec, no disk files, fixed output allocation. */
static size_t encode(IWICImagingFactory *factory,HBITMAP bitmap,uint8_t *out,size_t *unbounded,float quality) {
    IWICBitmap *source=NULL;
    IWICFormatConverter *converter=NULL;
    IWICBitmapEncoder *encoder=NULL;
    IWICBitmapFrameEncode *frame=NULL;
    IPropertyBag2 *options=NULL;
    IStream *stream=NULL;
    HGLOBAL memory=NULL;
    uint8_t *bytes=NULL;
    STATSTG stat;
    size_t result=0;
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;
    PROPBAG2 prop={0}; VARIANT value;
    HRESULT hr=IWICImagingFactory_CreateBitmapFromHBITMAP(factory,bitmap,NULL,WICBitmapIgnoreAlpha,&source);
    if(FAILED(hr)) goto done;
    hr=IWICImagingFactory_CreateFormatConverter(factory,&converter); if(FAILED(hr)) goto done;
    hr=IWICFormatConverter_Initialize(converter,(IWICBitmapSource *)source,&format,WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom);
    if(FAILED(hr)) goto done;
    hr=CreateStreamOnHGlobal(NULL,TRUE,&stream); if(FAILED(hr)) goto done;
    hr=IWICImagingFactory_CreateEncoder(factory,&GUID_ContainerFormatJpeg,NULL,&encoder); if(FAILED(hr)) goto done;
    hr=IWICBitmapEncoder_Initialize(encoder,stream,WICBitmapEncoderNoCache); if(FAILED(hr)) goto done;
    hr=IWICBitmapEncoder_CreateNewFrame(encoder,&frame,&options); if(FAILED(hr)) goto done;
    memset(&value,0,sizeof(value)); value.vt=VT_R4; value.fltVal=quality; prop.pstrName=L"ImageQuality";
    hr=IPropertyBag2_Write(options,1,&prop,&value); if(FAILED(hr)) goto done;
    hr=IWICBitmapFrameEncode_Initialize(frame,options); if(FAILED(hr)) goto done;
    hr=IWICBitmapFrameEncode_SetSize(frame,WIDTH,HEIGHT); if(FAILED(hr)) goto done;
    hr=IWICBitmapFrameEncode_SetPixelFormat(frame,&format); if(FAILED(hr)) goto done;
    hr=IWICBitmapFrameEncode_WriteSource(frame,(IWICBitmapSource *)converter,NULL); if(FAILED(hr)) goto done;
    if(FAILED(IWICBitmapFrameEncode_Commit(frame)) || FAILED(IWICBitmapEncoder_Commit(encoder))) goto done;
    if(FAILED(IStream_Stat(stream,&stat,STATFLAG_NONAME)) || stat.cbSize.HighPart || !stat.cbSize.LowPart) goto done;
    if(unbounded) *unbounded=stat.cbSize.LowPart;
    if(stat.cbSize.LowPart>DIRECT_SCREEN_MAX) goto done;
    if(FAILED(GetHGlobalFromStream(stream,&memory))) goto done;
    bytes=GlobalLock(memory); if(!bytes) goto done;
    result=stat.cbSize.LowPart; memcpy(out,bytes,result); GlobalUnlock(memory);
done:
    RELEASE(options); RELEASE(frame); RELEASE(encoder); RELEASE(stream); RELEASE(converter); RELEASE(source);
    return result;
}
static DWORD WINAPI capture(void *unused) {
    IWICImagingFactory *factory=NULL;
    HDC desktop=NULL,dc=NULL;
    HBITMAP bitmap=NULL; HGDIOBJ old=NULL;
    uint8_t *bytes=NULL;
    RECT r=share.source;
    float quality=0.35f;
    HRESULT com=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    (void)unused;
    if(FAILED(com) || FAILED(factory_create(&factory))) goto done;
    desktop=GetDC(NULL); if(!desktop) goto done;
    dc=CreateCompatibleDC(desktop); bitmap=CreateCompatibleBitmap(desktop,WIDTH,HEIGHT);
    bytes=HeapAlloc(GetProcessHeap(),0,DIRECT_SCREEN_MAX);
    if(!dc || !bitmap || !bytes) goto done;
    old=SelectObject(dc,bitmap); SetStretchBltMode(dc,HALFTONE); SetBrushOrgEx(dc,0,0,NULL);
    while(WaitForSingleObject(share.stop,0)!=WAIT_OBJECT_0) {
        ULONGLONG start=GetTickCount64();
        int sw=r.right-r.left,sh=r.bottom-r.top,w=WIDTH,h=HEIGHT;
        size_t size,unbounded=0;
        /* Preserve aspect ratio and never read outside the chosen monitor. */
        if((int64_t)sw*HEIGHT>(int64_t)sh*WIDTH) h=(int)((int64_t)sh*WIDTH/sw);
        else w=(int)((int64_t)sw*HEIGHT/sh);
        PatBlt(dc,0,0,WIDTH,HEIGHT,BLACKNESS);
        if(share.synthetic) {
            RECT text={70,80,900,180},box; HBRUSH brush=CreateSolidBrush(RGB(92,104,230));
            FillRect(dc,&(RECT){0,0,WIDTH,HEIGHT},brush); DeleteObject(brush);
            SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255));
            DrawTextW(dc,L"Direct · Test du partage d’écran\nAucun écran ni microphone capturé.",-1,&text,DT_LEFT);
            box.left=80+(LONG)((start/10)%600); box.top=260; box.right=box.left+100; box.bottom=360;
            FillRect(dc,&box,(HBRUSH)GetStockObject(WHITE_BRUSH));
        } else if(!StretchBlt(dc,(WIDTH-w)/2,(HEIGHT-h)/2,w,h,desktop,r.left,r.top,sw,sh,SRCCOPY|CAPTUREBLT)) break;
        InterlockedIncrement(&share.capture_attempts);
        size=encode(factory,bitmap,bytes,&unbounded,quality);
        share.last_unbounded_bytes=(DWORD)unbounded;
        share.last_quality=quality;
        if(unbounded>DIRECT_SCREEN_MAX) {
            InterlockedIncrement(&share.capture_oversize);
            if(quality>0.06f) quality-=0.04f;
        } else if(size && size<DIRECT_SCREEN_MAX/2 && quality<0.50f) quality+=0.01f;
        if(size) {
            share.last_jpeg_bytes=(DWORD)size;
            InterlockedIncrement(&share.capture_encoded);
            EnterCriticalSection(&share.lock);
            memcpy(share.latest,bytes,size); share.latest_size=size;
            LeaveCriticalSection(&share.lock);
        }
        /* At most 5 fps; codec work never blocks audio or network threads. */
        { ULONGLONG elapsed=GetTickCount64()-start;
          if(elapsed<200 && WaitForSingleObject(share.stop,(DWORD)(200-elapsed))==WAIT_OBJECT_0) break; }
    }
done:
    if(bytes) { SecureZeroMemory(bytes,DIRECT_SCREEN_MAX); HeapFree(GetProcessHeap(),0,bytes); }
    if(old) SelectObject(dc,old);
    if(bitmap) DeleteObject(bitmap); if(dc) DeleteDC(dc); if(desktop) ReleaseDC(NULL,desktop);
    RELEASE(factory); if(SUCCEEDED(com)) CoUninitialize();
    InterlockedExchange(&share.sharing,0);
    PostMessageW(share.owner,WM_APP+31,0,0);
    return 0;
}
static void stop_capture(void) {
    InterlockedExchange(&share.sharing,0);
    if(share.thread) { SetEvent(share.stop); WaitForSingleObject(share.thread,INFINITE); CloseHandle(share.thread); share.thread=NULL; }
    EnterCriticalSection(&share.lock); SecureZeroMemory(share.latest,sizeof(share.latest)); share.latest_size=0; LeaveCriticalSection(&share.lock);
}
static int start_capture(RECT source,int synthetic) {
    stop_capture();
    share.synthetic=synthetic; share.source=source; ResetEvent(share.stop);
    InterlockedExchange(&share.sharing,1);
    share.thread=CreateThread(NULL,0,capture,NULL,0,NULL);
    if(!share.thread) { InterlockedExchange(&share.sharing,0); return -1; }
    PostMessageW(share.owner,WM_APP+31,0,0);
    return 0;
}
int screen_record_game_begin(const wchar_t *directory,int sender) {
    if(!directory || !*directory || wcslen(directory)>=MAX_PATH-32) return -1;
    wcscpy_s(share.record_dir,MAX_PATH,directory);
    share.record_sender=sender;
    share.capture_attempts=share.capture_encoded=share.capture_oversize=0;
    share.send_packets=share.received_frames=0; share.last_jpeg_bytes=share.last_unbounded_bytes=0; share.last_quality=0;
    if(sender) {
        HWND game=FindWindowW(NULL,L"Path of Exile 2");
        MONITORINFO monitor={sizeof(monitor)};
        if(!game || IsIconic(game) || !GetMonitorInfoW(MonitorFromWindow(game,MONITOR_DEFAULTTONEAREST),&monitor)) return -1;
        return start_capture(monitor.rcMonitor,0);
    }
    CreateDirectoryW(directory,NULL);
    if(GetFileAttributesW(directory)==INVALID_FILE_ATTRIBUTES) return -1;
    wcscpy_s(share.record_dir,MAX_PATH,directory);
    {
        WCHAR path[MAX_PATH]; DWORD written;
        swprintf_s(path,MAX_PATH,L"%ls\\timing.csv",directory);
        share.record_timing=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if(share.record_timing==INVALID_HANDLE_VALUE) { share.record_timing=NULL; return -1; }
        WriteFile(share.record_timing,"frame,elapsed_ms,bytes\r\n",24,&written,NULL);
    }
    share.record_count=0; share.record_started=GetTickCount64();
    InterlockedExchange(&share.viewing,1);
    return 0;
}
void screen_record_tick(void) {
    uint8_t bytes[DIRECT_SCREEN_MAX]; size_t size; unsigned index;
    WCHAR path[MAX_PATH]; HANDLE file; DWORD written;
    if(!share.record_timing) return;
    EnterCriticalSection(&share.lock);
    size=share.received_size;
    if(size) memcpy(bytes,share.received,size);
    share.received_size=0;
    LeaveCriticalSection(&share.lock);
    if(!size) return;
    index=share.record_count+1;
    swprintf_s(path,MAX_PATH,L"%ls\\frame-%04u.jpg",share.record_dir,index);
    file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file!=INVALID_HANDLE_VALUE) {
        if(WriteFile(file,bytes,(DWORD)size,&written,NULL) && written==size) {
            char row[80]; int count=snprintf(row,sizeof(row),"%u,%llu,%zu\r\n",index,
                (unsigned long long)(GetTickCount64()-share.record_started),size);
            if(count>0) WriteFile(share.record_timing,row,(DWORD)count,&written,NULL);
            share.record_count=index;
        }
        CloseHandle(file);
    }
    SecureZeroMemory(bytes,sizeof(bytes));
}
int screen_record_count(void) { return (int)share.record_count; }
void screen_record_end(void) {
    if(share.record_dir[0]) {
        WCHAR path[MAX_PATH]; char report[256]; DWORD written;
        HANDLE file;
        swprintf_s(path,MAX_PATH,L"%ls\\%ls",share.record_dir,share.record_sender?L"sender.txt":L"receiver.txt");
        file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file!=INVALID_HANDLE_VALUE) {
            int count=snprintf(report,sizeof(report),"captured=%ld\nencoded=%ld\noversize=%ld\nlast_jpeg_bytes=%lu\nlast_unbounded_bytes=%lu\nlast_quality=%.2f\nsent_packets=%ld\nreceived_frames=%ld\nrecorded_frames=%u\n",
                share.capture_attempts,share.capture_encoded,share.capture_oversize,(unsigned long)share.last_jpeg_bytes,
                (unsigned long)share.last_unbounded_bytes,(double)share.last_quality,
                share.send_packets,share.received_frames,share.record_count);
            if(count>0) WriteFile(file,report,(DWORD)count,&written,NULL);
            CloseHandle(file);
        }
        share.record_dir[0]=0;
    }
    if(share.record_timing) { CloseHandle(share.record_timing); share.record_timing=NULL; }
    if(!share.viewer) InterlockedExchange(&share.viewing,0);
    if(screen_sharing()) stop_capture();
}
static HBITMAP decode(const uint8_t *bytes,size_t size,unsigned *width,unsigned *height) {
    IWICImagingFactory *factory=NULL; IWICStream *stream=NULL;
    IWICBitmapDecoder *decoder=NULL; IWICBitmapFrameDecode *frame=NULL; IWICFormatConverter *convert=NULL;
    HBITMAP bitmap=NULL; BITMAPINFO info={0}; void *pixels=NULL;
    UINT w=0,h=0; HRESULT hr;
    /* Only explicitly recognized JPEG, with dimensions checked before pixel allocation. */
    if(size<4 || size>DIRECT_SCREEN_MAX || bytes[0]!=0xff || bytes[1]!=0xd8) return NULL;
    hr=factory_create(&factory); if(FAILED(hr)) goto done;
    hr=IWICImagingFactory_CreateStream(factory,&stream); if(FAILED(hr)) goto done;
    hr=IWICStream_InitializeFromMemory(stream,(BYTE *)bytes,(DWORD)size); if(FAILED(hr)) goto done;
    hr=IWICImagingFactory_CreateDecoderFromStream(factory,(IStream *)stream,&GUID_VendorMicrosoft,WICDecodeMetadataCacheOnDemand,&decoder); if(FAILED(hr)) goto done;
    hr=IWICBitmapDecoder_GetFrame(decoder,0,&frame); if(FAILED(hr)) goto done;
    hr=IWICBitmapFrameDecode_GetSize(frame,&w,&h); if(FAILED(hr) || !w || !h || w>WIDTH || h>HEIGHT) goto done;
    hr=IWICImagingFactory_CreateFormatConverter(factory,&convert); if(FAILED(hr)) goto done;
    hr=IWICFormatConverter_Initialize(convert,(IWICBitmapSource *)frame,&GUID_WICPixelFormat32bppBGR,WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom); if(FAILED(hr)) goto done;
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=(LONG)w; info.bmiHeader.biHeight=-(LONG)h;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    bitmap=CreateDIBSection(NULL,&info,DIB_RGB_COLORS,&pixels,NULL,0); if(!bitmap) goto done;
    hr=IWICFormatConverter_CopyPixels(convert,NULL,w*4,w*h*4,pixels);
    if(FAILED(hr)) { DeleteObject(bitmap); bitmap=NULL; } else { *width=w; *height=h; }
done:
    RELEASE(convert); RELEASE(frame); RELEASE(decoder); RELEASE(stream); RELEASE(factory); return bitmap;
}
static LRESULT CALLBACK viewer_proc(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: SetTimer(window,1,100,NULL); return 0;
    case WM_TIMER: {
        uint8_t bytes[DIRECT_SCREEN_MAX]; size_t size; uint64_t at;
        EnterCriticalSection(&share.lock); size=share.received_size; at=share.remote_at;
        if(size) memcpy(bytes,share.received,size); share.received_size=0;
        LeaveCriticalSection(&share.lock);
        if(size) {
            HBITMAP image=decode(bytes,size,&share.width,&share.height);
            if(image) { if(share.image) DeleteObject(share.image); share.image=image; InvalidateRect(window,NULL,FALSE); }
            SecureZeroMemory(bytes,sizeof(bytes));
        }
        if(share.image && (!at || GetTickCount64()-at>2000)) { DeleteObject(share.image); share.image=NULL; InvalidateRect(window,NULL,FALSE); }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(window,&ps); RECT r; GetClientRect(window,&r);
        FillRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        if(share.image) {
            HDC source=CreateCompatibleDC(dc); HGDIOBJ old=SelectObject(source,share.image);
            int w=r.right,h=(int)((int64_t)w*share.height/share.width);
            if(h>r.bottom) { h=r.bottom; w=(int)((int64_t)h*share.width/share.height); }
            SetStretchBltMode(dc,HALFTONE); StretchBlt(dc,(r.right-w)/2,(r.bottom-h)/2,w,h,source,0,0,(int)share.width,(int)share.height,SRCCOPY);
            SelectObject(source,old); DeleteDC(source);
        } else {
            SetTextColor(dc,RGB(230,233,240)); SetBkMode(dc,TRANSPARENT);
            DrawTextW(dc,L"En attente du partage de votre ami…",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
        EndPaint(window,&ps); return 0;
    }
    case WM_SIZE: InvalidateRect(window,NULL,FALSE); return 0;
    case WM_DESTROY:
        KillTimer(window,1); InterlockedExchange(&share.viewing,0); share.viewer=NULL;
        if(share.image) { DeleteObject(share.image); share.image=NULL; } return 0;
    default: return DefWindowProcW(window,msg,wp,lp);
    }
}
static BOOL CALLBACK monitor_enum(HMONITOR monitor,HDC dc,LPRECT rect,LPARAM data) {
    (void)monitor; (void)dc; (void)data;
    if(share.monitor_count<16) share.monitors[share.monitor_count++]=*rect;
    return TRUE;
}
void screen_init(HWND owner) {
    WNDCLASSW wc={0};
    share.owner=owner; InitializeCriticalSection(&share.lock); share.stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    share.com_initialized=SUCCEEDED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED));
    wc.lpfnWndProc=viewer_proc; wc.hInstance=GetModuleHandleW(NULL); wc.lpszClassName=L"DirectScreenViewer";
    wc.hCursor=LoadCursor(NULL,IDC_ARROW); RegisterClassW(&wc);
}
void screen_menu(HWND owner) {
    HMENU menu=CreatePopupMenu(); POINT pt; UINT choice; unsigned i;
    share.monitor_count=0; EnumDisplayMonitors(NULL,NULL,monitor_enum,0);
    if(screen_sharing()) AppendMenuW(menu,MF_STRING,1,L"Arrêter mon partage d’écran");
    else if(share.test_mode) AppendMenuW(menu,MF_STRING,3,L"Partager une mire de test · sans capture");
    else for(i=0;i<share.monitor_count;++i) {
        WCHAR label[120]; RECT r=share.monitors[i];
        swprintf_s(label,120,L"Partager l’écran %u · %ld × %ld",i+1,r.right-r.left,r.bottom-r.top);
        AppendMenuW(menu,MF_STRING,10+i,label);
    }
    AppendMenuW(menu,MF_SEPARATOR,0,NULL);
    AppendMenuW(menu,MF_STRING,2,L"Voir l’écran de mon ami");
    GetCursorPos(&pt); choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,pt.x,pt.y,0,owner,NULL); DestroyMenu(menu);
    if(choice==1) stop_capture();
    if(choice==3 || (choice>=10 && choice<10+share.monitor_count)) {
        if(choice!=3 && MessageBoxW(owner,L"Tout ce qui est visible sur cet écran sera partagé avec votre ami, y compris les notifications.\n\nContinuer ?",L"Partager mon écran",MB_OKCANCEL|MB_ICONINFORMATION)!=IDOK) return;
        start_capture(choice==3 ? (RECT){0,0,WIDTH,HEIGHT} : share.monitors[choice-10],choice==3);
    }
    if(choice==2) {
        if(!share.viewer) {
            RECT r; int w,h;
            GetWindowRect(owner,&r); w=r.right-r.left; h=r.bottom-r.top;
            if(w>980) w=980; if(h>600) h=600;
            InterlockedExchange(&share.viewing,1);
            share.viewer=CreateWindowExW(WS_EX_APPWINDOW,L"DirectScreenViewer",L"Direct · Écran de votre ami",WS_OVERLAPPEDWINDOW|WS_VISIBLE,r.left+20,r.top+40,w,h,NULL,NULL,GetModuleHandleW(NULL),NULL);
            if(!share.viewer) InterlockedExchange(&share.viewing,0);
        } else { ShowWindow(share.viewer,SW_RESTORE); SetForegroundWindow(share.viewer); }
    }
    PostMessageW(owner,WM_APP+31,0,0);
}
int screen_sharing(void) { return InterlockedCompareExchange(&share.sharing,0,0)!=0; }
void screen_test_mode(int enabled) { share.test_mode=!!enabled; }
int screen_remote(void) {
    uint64_t at; EnterCriticalSection(&share.lock); at=share.remote_at; LeaveCriticalSection(&share.lock);
    return at && GetTickCount64()-at<2000;
}
void screen_pump(murmur_ice *peer,int connected) {
    uint8_t packet[1000]; size_t size; unsigned n; uint64_t now=GetTickCount64();
    if(!connected) return;
    if(!screen_sharing()) {
        if(share.was_sharing) {
            share.sending_size=0; share.part=0; share.stops=3;
            if(!++share.generation) ++share.generation;
            share.was_sharing=0;
        }
        EnterCriticalSection(&share.lock); share.latest_size=0; LeaveCriticalSection(&share.lock);
    } else if(!share.sending_size && now-share.sent_at>=200) {
        EnterCriticalSection(&share.lock);
        if(share.latest_size) {
            memcpy(share.sending,share.latest,share.latest_size); share.sending_size=share.latest_size; share.latest_size=0;
            share.sending_id=++share.generation; if(!share.sending_id) share.sending_id=++share.generation;
            share.part=0; share.sent_at=now; share.stops=0; share.was_sharing=1;
        }
        LeaveCriticalSection(&share.lock);
    }
    if(share.sending_size) {
        for(n=0;n<3 && share.sending_size;++n) {
            size=direct_screen_packet(packet,share.sending_id,share.sending,share.sending_size,share.part);
            if(murmur_ice_send_screen(peer,packet,size)) break;
            InterlockedIncrement(&share.send_packets);
            if(++share.part*DIRECT_SCREEN_CHUNK>=share.sending_size) share.sending_size=0;
        }
    } else if(!screen_sharing() && share.stops) {
        size=direct_screen_packet(packet,share.generation,NULL,0,0);
        if(!murmur_ice_send_screen(peer,packet,size)) --share.stops;
    }
    for(n=0;n<16 && murmur_ice_receive_screen(peer,packet,sizeof(packet),&size)==1;++n) {
        int result=direct_screen_feed(&share.assembly,packet,size,now);
        if(result==1 || result==2) {
            if(result==1) InterlockedIncrement(&share.received_frames);
            int changed;
            EnterCriticalSection(&share.lock); changed=!share.remote_at || result==2; share.remote_at=result==1 ? now : 0;
            if(result==1 && InterlockedCompareExchange(&share.viewing,0,0)) {
                memcpy(share.received,share.assembly.bytes,share.assembly.total); share.received_size=share.assembly.total;
            } else if(result==2) { SecureZeroMemory(share.received,sizeof(share.received)); share.received_size=0; }
            LeaveCriticalSection(&share.lock);
            if(changed) PostMessageW(share.owner,WM_APP+31,0,0);
        }
    }
}
void screen_disconnect(void) {
    /* Worker asks UI to stop capture; no UI waits or codec work on transport. */
    InterlockedExchange(&share.sharing,0); SetEvent(share.stop);
    SecureZeroMemory(&share.assembly,sizeof(share.assembly)); SecureZeroMemory(share.sending,sizeof(share.sending));
    share.sending_size=0; share.stops=0; share.was_sharing=0;
    EnterCriticalSection(&share.lock); share.remote_at=0; share.received_size=0; share.latest_size=0;
    SecureZeroMemory(share.received,sizeof(share.received)); SecureZeroMemory(share.latest,sizeof(share.latest));
    LeaveCriticalSection(&share.lock);
    PostMessageW(share.owner,WM_APP+31,0,0);
}
void screen_shutdown(void) {
    screen_record_end(); stop_capture(); if(share.viewer) DestroyWindow(share.viewer); CloseHandle(share.stop); DeleteCriticalSection(&share.lock);
    if(share.com_initialized) CoUninitialize();
}
#ifdef DIRECT_SCREEN_TEST
int screen_codec_test(void) {
    IWICImagingFactory *factory=NULL; BITMAPINFO info={0}; HBITMAP bitmap=NULL,image=NULL;
    uint8_t bytes[DIRECT_SCREEN_MAX]; void *pixels=NULL; size_t size; unsigned w=0,h=0;
    int result=1; HRESULT com=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    if(FAILED(com) || FAILED(factory_create(&factory))) goto done;
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=WIDTH; info.bmiHeader.biHeight=-HEIGHT;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    bitmap=CreateDIBSection(NULL,&info,DIB_RGB_COLORS,&pixels,NULL,0); if(!bitmap) goto done;
    memset(pixels,120,WIDTH*HEIGHT*4);
    size=encode(factory,bitmap,bytes,NULL,0.55f); if(!size || size>DIRECT_SCREEN_MAX) goto done;
    image=decode(bytes,size,&w,&h); if(!image || w!=WIDTH || h!=HEIGHT) goto done;
    DeleteObject(image); image=NULL;
    if(decode((const uint8_t *)"invalid",7,&w,&h)) goto done;
    /* JPEG SOF dimensions are untrusted: reject before allocating pixels. */
    { size_t i;
      for(i=2;i+9<size;++i) if(bytes[i]==0xff && bytes[i+1]==0xc0) {
          bytes[i+7]=0x7f; bytes[i+8]=0xff; break;
      }
      if(i+9>=size || (image=decode(bytes,size,&w,&h))!=NULL) goto done;
    }
    result=0;
done:
    if(bitmap) DeleteObject(bitmap); if(image) DeleteObject(image); RELEASE(factory);
    if(SUCCEEDED(com)) CoUninitialize(); return result;
}
#endif
