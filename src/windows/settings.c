#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include "settings.h"
#include "link.h"
#include "local_test.h"
#include "updates.h"
#include "panel_style.h"
#define DEVICES 32
static struct {
    HWND window,owner,input,output,in_volume,out_volume,meter,status,mic,tone,solo,versions,in_label,out_label;
    HFONT font,heading;
    murmur_audio_device inputs[DEVICES],outputs[DEVICES];
    int input_count,output_count,sandbox;
    murmur_audio_config config;
    murmur_audio_preview *preview;
    ULONGLONG deadline;
    UINT dpi;
    WCHAR path[MAX_PATH];
} prefs;
static int scale(int n) { return MulDiv(n,(int)prefs.dpi,96); }
static void persist(void) {
    HANDLE file; DWORD written;
    if(prefs.sandbox || !prefs.path[0]) return;
    file=CreateFileW(prefs.path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file!=INVALID_HANDLE_VALUE) { WriteFile(file,&prefs.config,sizeof(prefs.config),&written,NULL); CloseHandle(file); }
}
void settings_init(int sandbox) {
    WCHAR folder[MAX_PATH]; HANDLE file; DWORD read;
    prefs.sandbox=sandbox; murmur_audio_defaults(&prefs.config);
    if(!sandbox && SUCCEEDED(SHGetFolderPathW(NULL,CSIDL_LOCAL_APPDATA,NULL,SHGFP_TYPE_CURRENT,folder))) {
        swprintf_s(prefs.path,MAX_PATH,L"%ls\\Murmur",folder); CreateDirectoryW(prefs.path,NULL);
        wcscat_s(prefs.path,MAX_PATH,L"\\audio-v1.dat");
        file=CreateFileW(prefs.path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file!=INVALID_HANDLE_VALUE) {
            murmur_audio_config config;
            if(ReadFile(file,&config,sizeof(config),&read,NULL) && read==sizeof(config) && config.input_selected<=1 && config.output_selected<=1 && config.input_volume<=100 && config.output_volume<=100) prefs.config=config;
            CloseHandle(file);
        }
    }
    link_audio_configure(&prefs.config);
}
static HWND control(HWND parent,const WCHAR *type,const WCHAR *text,DWORD style,int x,int y,int w,int h,int id) {
    HWND child;
    if(!wcscmp(type,L"BUTTON")) style=(style & ~BS_TYPEMASK)|BS_OWNERDRAW;
    child=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,scale(x),scale(y),scale(w),scale(h),parent,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(child,WM_SETFONT,(WPARAM)prefs.font,TRUE); return child;
}
static void stop_preview(void) {
    KillTimer(prefs.window,1); murmur_audio_preview_stop(prefs.preview); prefs.preview=NULL;
    if(prefs.window) { SendMessageW(prefs.meter,PBM_SETPOS,0,0); SetWindowTextW(prefs.mic,L"Tester le micro"); SetWindowTextW(prefs.tone,L"Écouter un son"); }
}
static void update_volumes(void) {
    WCHAR label[80];
    prefs.config.input_volume=(unsigned)SendMessageW(prefs.in_volume,TBM_GETPOS,0,0);
    prefs.config.output_volume=(unsigned)SendMessageW(prefs.out_volume,TBM_GETPOS,0,0);
    swprintf_s(label,80,L"Volume du micro · %u %%",prefs.config.input_volume); SetWindowTextW(prefs.in_label,label);
    swprintf_s(label,80,L"Volume de sortie · %u %%",prefs.config.output_volume); SetWindowTextW(prefs.out_label,label);
    link_audio_configure(&prefs.config);
    murmur_audio_preview_volume(prefs.preview,prefs.config.input_volume,prefs.config.output_volume);
}
static void fill_devices(HWND combo,murmur_audio_device *devices,int count,int selected,const uint8_t *id) {
    int i,choice=0; WCHAR name[300];
    SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)L"Périphérique par défaut du système");
    for(i=0;i<count;++i) {
        MultiByteToWideChar(CP_UTF8,0,devices[i].name,-1,name,300);
        SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)name);
        if(selected && !memcmp(devices[i].id,id,256)) choice=i+1;
    }
    if(selected && !choice) {
        SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)L"Périphérique enregistré indisponible — choisissez-en un autre");
        choice=count+1;
    }
    SendMessageW(combo,CB_SETCURSEL,(WPARAM)choice,0);
}
void settings_refresh(void) {
    int busy=link_voice_requested() || (link_voice_remote()&1);
    if(!prefs.window) return;
    EnableWindow(prefs.input,!busy && !prefs.sandbox); EnableWindow(prefs.output,!busy && !prefs.sandbox);
    EnableWindow(prefs.mic,!busy && !prefs.sandbox); EnableWindow(prefs.tone,!busy && !prefs.sandbox);
    EnableWindow(prefs.solo,!prefs.sandbox && !link_busy());
    if(busy && prefs.preview) stop_preview();
    SetWindowTextW(prefs.status,prefs.sandbox?L"Test solo : audio simulé, aucun micro ni haut-parleur ouvert.":busy?L"Raccrochez pour changer de périphérique. Les volumes restent réglables.":L"Les tests ouvrent un seul périphérique et s’arrêtent après 5 secondes.");
}
static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    switch(message) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX common={sizeof(common),ICC_BAR_CLASSES|ICC_PROGRESS_CLASS}; InitCommonControlsEx(&common);
        prefs.window=window; prefs.dpi=GetDpiForWindow(window);
        prefs.font=CreateFontW(-scale(14),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        prefs.heading=CreateFontW(-scale(24),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        SendMessageW(control(window,L"STATIC",L"Son et préférences",0,28,22,460,34,0),WM_SETFONT,(WPARAM)prefs.heading,TRUE);
        control(window,L"STATIC",L"MICROPHONE",0,28,80,460,22,0);
        prefs.input=control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,28,106,464,200,10);
        prefs.in_label=control(window,L"STATIC",L"",0,28,148,464,22,0);
        prefs.in_volume=control(window,TRACKBAR_CLASSW,L"Volume du microphone",TBS_AUTOTICKS|WS_TABSTOP,22,174,476,34,12);
        control(window,L"STATIC",L"SORTIE AUDIO",0,28,228,460,22,0);
        prefs.output=control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,28,254,464,200,11);
        prefs.out_label=control(window,L"STATIC",L"",0,28,296,464,22,0);
        prefs.out_volume=control(window,TRACKBAR_CLASSW,L"Volume de sortie",TBS_AUTOTICKS|WS_TABSTOP,22,322,476,34,13);
        prefs.mic=control(window,L"BUTTON",L"Tester le micro",WS_TABSTOP,28,380,220,36,14);
        prefs.tone=control(window,L"BUTTON",L"Écouter un son",WS_TABSTOP,272,380,220,36,15);
        control(window,L"STATIC",L"Niveau du micro",0,28,428,464,22,0);
        prefs.meter=control(window,PROGRESS_CLASSW,L"Niveau du microphone",0,28,454,464,14,0);
        prefs.status=control(window,L"STATIC",L"",0,28,480,464,48,0);
        prefs.solo=control(window,L"BUTTON",L"Test solo · ouvrir deux clients",WS_TABSTOP,28,544,288,36,16);
        prefs.versions=control(window,L"BUTTON",L"Versions",WS_TABSTOP,332,544,160,36,17);
        control(window,L"STATIC",L"Les réglages sont enregistrés à la fermeture de ce panneau.",0,28,598,464,40,0);
        control(window,L"BUTTON",L"Fermer",WS_TABSTOP|BS_DEFPUSHBUTTON,332,648,160,36,IDCANCEL);
        SendMessageW(prefs.in_volume,TBM_SETRANGE,TRUE,MAKELPARAM(0,100)); SendMessageW(prefs.out_volume,TBM_SETRANGE,TRUE,MAKELPARAM(0,100));
        SendMessageW(prefs.in_volume,TBM_SETPOS,TRUE,prefs.config.input_volume); SendMessageW(prefs.out_volume,TBM_SETPOS,TRUE,prefs.config.output_volume);
        prefs.input_count=prefs.sandbox?0:murmur_audio_devices(1,prefs.inputs,DEVICES); if(prefs.input_count<0) prefs.input_count=0;
        prefs.output_count=prefs.sandbox?0:murmur_audio_devices(0,prefs.outputs,DEVICES); if(prefs.output_count<0) prefs.output_count=0;
        fill_devices(prefs.input,prefs.inputs,prefs.input_count,prefs.config.input_selected,prefs.config.input_id);
        fill_devices(prefs.output,prefs.outputs,prefs.output_count,prefs.config.output_selected,prefs.config.output_id);
        EnableWindow(prefs.versions,!prefs.sandbox); update_volumes(); settings_refresh(); return 0;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        return direct_panel_color(message,wp,lp);
    case WM_DRAWITEM: return direct_panel_button((DRAWITEMSTRUCT *)lp,prefs.font);
    case WM_HSCROLL: update_volumes(); return 0;
    case WM_COMMAND: {
        int id=LOWORD(wp);
        if((id==10 || id==11) && HIWORD(wp)==CBN_SELCHANGE) {
            int index=(int)SendMessageW(id==10?prefs.input:prefs.output,CB_GETCURSEL,0,0);
            int count=id==10?prefs.input_count:prefs.output_count;
            stop_preview();
            if(index>=0 && index<=count) {
                if(id==10) { prefs.config.input_selected=index!=0; if(index) memcpy(prefs.config.input_id,prefs.inputs[index-1].id,256); }
                else { prefs.config.output_selected=index!=0; if(index) memcpy(prefs.config.output_id,prefs.outputs[index-1].id,256); }
                link_audio_configure(&prefs.config);
            }
        }
        if((id==14 || id==15) && HIWORD(wp)==BN_CLICKED) {
            int was_active=prefs.preview!=NULL; stop_preview();
            if(!was_active && !prefs.sandbox && !link_voice_requested() && !(link_voice_remote()&1)) {
                prefs.preview=murmur_audio_preview_start(id==14,&prefs.config);
                if(!prefs.preview) MessageBoxW(window,L"Impossible d’ouvrir ce périphérique. Vérifiez le branchement et les autorisations audio de Windows.",L"Périphérique indisponible",MB_OK|MB_ICONWARNING);
                else { prefs.deadline=GetTickCount64()+5000; SetTimer(window,1,80,NULL); SetWindowTextW(id==14?prefs.mic:prefs.tone,L"Arrêter le test"); }
            }
        }
        if(id==16 && HIWORD(wp)==BN_CLICKED && !prefs.sandbox && !link_busy()) {
            HWND owner=prefs.owner; DestroyWindow(window);
            if(local_test_launch(owner)) MessageBoxW(owner,L"Impossible de démarrer le test local.",L"Direct",MB_OK|MB_ICONWARNING);
        }
        if(id==17 && HIWORD(wp)==BN_CLICKED && !prefs.sandbox) updates_show(prefs.owner);
        if(id==IDCANCEL) DestroyWindow(window);
        return 0;
    }
    case WM_TIMER:
        SendMessageW(prefs.meter,PBM_SETPOS,murmur_audio_preview_level(prefs.preview),0);
        if(GetTickCount64()>=prefs.deadline) stop_preview(); return 0;
    case WM_DPICHANGED: {
        UINT old_dpi=prefs.dpi; RECT *suggested=(RECT *)lp; HWND child;
        prefs.dpi=HIWORD(wp);
        DeleteObject(prefs.font); DeleteObject(prefs.heading);
        prefs.font=CreateFontW(-scale(14),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        prefs.heading=CreateFontW(-scale(24),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        for(child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
            RECT r; WCHAR type[32]; GetWindowRect(child,&r); MapWindowPoints(NULL,window,(POINT *)&r,2);
            GetClassNameW(child,type,32);
            SetWindowPos(child,NULL,MulDiv(r.left,(int)prefs.dpi,(int)old_dpi),MulDiv(r.top,(int)prefs.dpi,(int)old_dpi),MulDiv(r.right-r.left,(int)prefs.dpi,(int)old_dpi),!wcscmp(type,L"ComboBox")?scale(200):MulDiv(r.bottom-r.top,(int)prefs.dpi,(int)old_dpi),SWP_NOZORDER|SWP_NOACTIVATE);
            SendMessageW(child,WM_SETFONT,(WPARAM)(child==GetWindow(window,GW_CHILD)?prefs.heading:prefs.font),TRUE);
        }
        SetWindowPos(window,NULL,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        stop_preview(); persist(); prefs.window=NULL; DeleteObject(prefs.font); DeleteObject(prefs.heading); return 0;
    default: return DefWindowProcW(window,message,wp,lp);
    }
}
void settings_show(HWND owner) {
    WNDCLASSW wc={0}; RECT bounds={0,0,520,712}; UINT dpi=GetDpiForWindow(owner);
    if(prefs.window) { ShowWindow(prefs.window,SW_RESTORE); SetForegroundWindow(prefs.window); return; }
    prefs.owner=owner; wc.lpfnWndProc=proc; wc.hInstance=GetModuleHandleW(NULL); wc.lpszClassName=L"DirectPreferences";
    wc.hCursor=LoadCursorW(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); RegisterClassW(&wc);
    bounds.right=MulDiv(bounds.right,(int)dpi,96); bounds.bottom=MulDiv(bounds.bottom,(int)dpi,96);
    AdjustWindowRectExForDpi(&bounds,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE,0,dpi);
    prefs.window=CreateWindowExW(0,wc.lpszClassName,L"Direct · Paramètres",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,owner,NULL,wc.hInstance,NULL);
    ShowWindow(prefs.window,SW_SHOW);
}
int settings_translate(MSG *message) { return prefs.window && IsDialogMessageW(prefs.window,message); }
void settings_close(void) { if(prefs.window) DestroyWindow(prefs.window); }
