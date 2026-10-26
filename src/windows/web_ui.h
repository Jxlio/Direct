#ifndef DIRECT_WEB_UI_H
#define DIRECT_WEB_UI_H
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const wchar_t *text, *author;
    uint32_t uid; /* stable identity: lets the bridge send only what changed */
    int room, remote, hour, minute;
    uint32_t media_id;
    unsigned media_kind;
    int media_pending;
} WebUiMessage;
typedef struct { const wchar_t *name; int online; } WebUiFriend;
typedef struct {
    const wchar_t *inputs[33], *outputs[33], *notice;
    int input_count, output_count, input_choice, output_choice;
    int input_volume, output_volume, preview_active, preview_level, locked, sandbox;
    int echo, noise, agc, gate, cleanup_available;
} WebUiAudio;
typedef struct {
    const wchar_t *peer, *self, *connection, *voice;
    const WebUiMessage *messages;
    const WebUiFriend *friends;
    const WebUiAudio *audio;
    const wchar_t *friends_notice, *identity, *identity_public, *identity_enc, *network_token, *invite_code, *network_notice, *update_status;
    size_t message_count;
    size_t friend_count;
    int room, connected, connecting, call_requested, call_incoming, muted;
    int screen_enabled, sharing, remote_screen, test_mode;
    int internet_enabled, network_phase, network_error;
    int update_available, update_busy;
    int media_busy;
} WebUiState;
typedef void (*WebUiAction)(const wchar_t *action, const wchar_t *text);
typedef int (*WebUiMediaProvider)(uint32_t id,const uint8_t **bytes,size_t *size,const wchar_t **mime);
void web_ui_init(HWND window, WebUiAction action);
void web_ui_set_media_provider(WebUiMediaProvider provider);
int web_ui_active(void);
void web_ui_resize(HWND window);
void web_ui_update(const WebUiState *state);
/* Forget what the page already has, so the next update is sent in full. */
void web_ui_forget_sent(void);
/* Minimized window: lower memory, and suspend the page when allow_suspend is set. */
void web_ui_set_background(int hidden, int allow_suspend);
int web_ui_suspended(void);
void web_ui_screen_signal(const wchar_t *signal);
/* Who is speaking: loudness 0..100 of our microphone and of the other side. */
void web_ui_levels(int self, int peer);
/* Server relay events and signatures for the page. Text is UTF-8; a NULL signature reports a refusal. */
void web_ui_service_event(int slot, const char *event, int code, const char *utf8);
void web_ui_signature(const char *id, const char *hex);
void web_ui_ecdh(const char *id, const char *hex);
void web_ui_close(void);
#ifdef __cplusplus
}
#endif
#endif
