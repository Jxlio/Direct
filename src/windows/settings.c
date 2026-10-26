#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include "settings.h"
#include "datadir.h"
#include "link.h"
#define DEVICES 32
static struct {
    murmur_audio_device inputs[DEVICES],outputs[DEVICES];
    int input_count,output_count,sandbox;
    murmur_audio_config config;
    murmur_audio_preview *preview;
    ULONGLONG deadline;
    WCHAR path[MAX_PATH];
    direct_audio_web_state web;
    HWND web_owner;
    int web_enumerated;
} prefs;
static void persist(void) {
    HANDLE file; DWORD written;
    if(prefs.sandbox || !prefs.path[0]) return;
    file=CreateFileW(prefs.path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file!=INVALID_HANDLE_VALUE) { WriteFile(file,&prefs.config,sizeof(prefs.config),&written,NULL); CloseHandle(file); }
}
void settings_init(int sandbox) {
    WCHAR folder[MAX_PATH]; HANDLE file; DWORD read;
    prefs.sandbox=sandbox; murmur_audio_defaults(&prefs.config);
    if(!sandbox && SUCCEEDED(murmur_local_appdata(folder))) {
        swprintf_s(prefs.path,MAX_PATH,L"%ls\\Murmur",folder); CreateDirectoryW(prefs.path,NULL);
        wcscat_s(prefs.path,MAX_PATH,L"\\audio-v1.dat");
        file=CreateFileW(prefs.path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file!=INVALID_HANDLE_VALUE) {
            murmur_audio_config config;
            murmur_audio_defaults(&config);
            if(ReadFile(file,&config,sizeof(config),&read,NULL) && (read==sizeof(config) || read==MURMUR_AUDIO_CONFIG_BASE_SIZE) &&
               config.input_selected<=1 && config.output_selected<=1 && config.input_volume<=100 && config.output_volume<=100 &&
               (read==MURMUR_AUDIO_CONFIG_BASE_SIZE || (config.echo<=1 && config.noise<=1 && config.agc<=1 && config.gate<=100))) {
                if(read==MURMUR_AUDIO_CONFIG_BASE_SIZE) { murmur_audio_config old=config; murmur_audio_defaults(&config); memcpy(&config,&old,MURMUR_AUDIO_CONFIG_BASE_SIZE); config.echo=config.noise=1; config.agc=config.gate=0; }
                prefs.config=config;
            }
            CloseHandle(file);
        }
    }
    link_audio_configure(&prefs.config);
}
static void stop_preview(void) {
    murmur_audio_preview_stop(prefs.preview); prefs.preview=NULL;
    if (prefs.web_owner) { KillTimer(prefs.web_owner,905); prefs.web_owner=NULL; }
}
void settings_close(void) { if(prefs.preview) stop_preview(); }
void settings_web_enumerate(void) {
    int i;
    if(prefs.web_enumerated) return;
    prefs.web_enumerated=1;
    wcscpy_s(prefs.web.inputs[0],300,L"Périphérique par défaut du système");
    wcscpy_s(prefs.web.outputs[0],300,L"Périphérique par défaut du système");
    prefs.input_count=prefs.sandbox?0:murmur_audio_devices(1,prefs.inputs,DEVICES);
    prefs.output_count=prefs.sandbox?0:murmur_audio_devices(0,prefs.outputs,DEVICES);
    if(prefs.input_count<0) prefs.input_count=0;
    if(prefs.output_count<0) prefs.output_count=0;
    prefs.web.input_count=prefs.input_count+1; prefs.web.output_count=prefs.output_count+1;
    for(i=0;i<prefs.input_count;++i) {
        MultiByteToWideChar(CP_UTF8,0,prefs.inputs[i].name,-1,prefs.web.inputs[i+1],300);
        if(prefs.config.input_selected && !memcmp(prefs.inputs[i].id,prefs.config.input_id,256)) prefs.web.input_choice=i+1;
    }
    for(i=0;i<prefs.output_count;++i) {
        MultiByteToWideChar(CP_UTF8,0,prefs.outputs[i].name,-1,prefs.web.outputs[i+1],300);
        if(prefs.config.output_selected && !memcmp(prefs.outputs[i].id,prefs.config.output_id,256)) prefs.web.output_choice=i+1;
    }
}
const direct_audio_web_state *settings_web_state(void) {
    prefs.web.input_volume=prefs.config.input_volume;
    prefs.web.output_volume=prefs.config.output_volume;
    prefs.web.locked=link_voice_requested() || (link_voice_remote()&1);
    prefs.web.sandbox=prefs.sandbox;
    prefs.web.preview_active=prefs.preview!=NULL;
    prefs.web.preview_level=prefs.preview?murmur_audio_preview_level(prefs.preview):0;
    prefs.web.echo=prefs.config.echo; prefs.web.noise=prefs.config.noise; prefs.web.agc=prefs.config.agc; prefs.web.gate=prefs.config.gate;
    prefs.web.cleanup_available=murmur_voice_cleanup_available();
    if(!prefs.web.notice) prefs.web.notice=prefs.sandbox?L"Test solo : audio simulé, aucun périphérique ouvert.":L"Choisissez vos périphériques et volumes.";
    return &prefs.web;
}
int settings_web_select(int input,int choice) {
    murmur_audio_device *devices=input?prefs.inputs:prefs.outputs;
    int count=input?prefs.input_count:prefs.output_count;
    if(!prefs.web_enumerated || prefs.sandbox || link_voice_requested() || (link_voice_remote()&1) || choice<0 || choice>count) return -1;
    stop_preview();
    if(input) { prefs.config.input_selected=choice!=0; if(choice) memcpy(prefs.config.input_id,devices[choice-1].id,256); prefs.web.input_choice=choice; }
    else { prefs.config.output_selected=choice!=0; if(choice) memcpy(prefs.config.output_id,devices[choice-1].id,256); prefs.web.output_choice=choice; }
    link_audio_configure(&prefs.config); persist(); return 0;
}
void settings_web_volume(int input,unsigned value) {
    if(value>100) return;
    if(input) prefs.config.input_volume=value; else prefs.config.output_volume=value;
    link_audio_configure(&prefs.config);
    murmur_audio_preview_volume(prefs.preview,prefs.config.input_volume,prefs.config.output_volume);
    persist();
}
void settings_web_preview(HWND owner,int input) {
    int was_active=prefs.preview!=NULL;
    stop_preview();
    if(was_active || prefs.sandbox || link_voice_requested() || (link_voice_remote()&1)) return;
    prefs.preview=murmur_audio_preview_start(input,&prefs.config);
    if(!prefs.preview) { prefs.web.notice=L"Impossible d’ouvrir ce périphérique. Vérifiez le branchement et les autorisations Windows."; return; }
    prefs.web_owner=owner; prefs.deadline=GetTickCount64()+5000;
    SetTimer(owner,905,80,NULL);
    prefs.web.notice=input?L"Test du microphone en cours…":L"Test de la sortie audio en cours…";
}
/* Echo, noise and gain clean-up on or off, and the microphone gate: each person's own choice, saved with the rest. */
void settings_web_cleanup(unsigned echo,unsigned noise,unsigned agc,unsigned gate) {
    if(echo>1 || noise>1 || agc>1 || gate>100) return;
    prefs.config.echo=echo; prefs.config.noise=noise; prefs.config.agc=agc; prefs.config.gate=gate;
    link_audio_configure(&prefs.config); persist();
}
void settings_web_tick(void) {
    if(prefs.preview && GetTickCount64()>=prefs.deadline) {
        stop_preview(); prefs.web.notice=L"Test terminé.";
    }
}
