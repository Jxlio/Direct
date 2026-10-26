#ifndef DIRECT_SETTINGS_H
#define DIRECT_SETTINGS_H
#include <windows.h>
void settings_init(int sandbox);
void settings_close(void);
typedef struct {
    WCHAR inputs[33][300], outputs[33][300];
    int input_count, output_count, input_choice, output_choice;
    unsigned input_volume, output_volume, preview_level;
    int preview_active, locked, sandbox;
    unsigned echo, noise, agc, gate; int cleanup_available;
    const WCHAR *notice;
} direct_audio_web_state;
const direct_audio_web_state *settings_web_state(void);
void settings_web_enumerate(void);
int settings_web_select(int input,int choice);
void settings_web_volume(int input,unsigned value);
void settings_web_preview(HWND owner,int input);
void settings_web_cleanup(unsigned echo,unsigned noise,unsigned agc,unsigned gate);
void settings_web_tick(void);
#endif
