#include "link.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "murmur/voice.h"
#include <juice/juice.h>
#ifdef DIRECT_SCREEN_SHARE
#include "screen.h"
#endif
#define QUEUE_SIZE 16
static struct {
    HWND window;
    HANDLE thread, stop, wake;
    CRITICAL_SECTION lock;
    int state, error, server;
    int test_audio;
    int voice_requested, voice_muted, voice_state;
    unsigned remote_voice;
    murmur_audio_config audio;
    unsigned voice_packets;
    uint64_t voice_decoded;
    int ice_mode, ice_phase, ice_relayed, answer_pending, signal_problem;
    murmur_ice_config ice_settings;
    char ice_input[MURMUR_ICE_TOKEN], ice_output[MURMUR_ICE_TOKEN], ice_answer[MURMUR_ICE_TOKEN];
    char host[16], port[6];
    uint8_t key[32];
    link_message out[QUEUE_SIZE], in[QUEUE_SIZE];
    unsigned out_head, out_count, in_head, in_count;
    unsigned level_self, level_peer;   /* 0..100, see murmur_voice_levels */
} transport;
/* Publishes who is speaking. The page is told only when a level moved enough to matter, so a quiet
   call costs no wake-ups; a missing voice (call ended) reads as silence. */
static void publish_levels(murmur_voice *voice) {
    unsigned self = 0, peer = 0, old_self, old_peer;
    int moved;
    if (voice) murmur_voice_levels(voice, GetTickCount64(), &self, &peer);
    EnterCriticalSection(&transport.lock);
    old_self = transport.level_self; old_peer = transport.level_peer;
    moved = (self > old_self ? self - old_self : old_self - self) >= 8 || (peer > old_peer ? peer - old_peer : old_peer - peer) >= 8 ||
            (self == 0) != (old_self == 0) || (peer == 0) != (old_peer == 0);
    if (moved) { transport.level_self = self; transport.level_peer = peer; }
    LeaveCriticalSection(&transport.lock);
    if (moved) PostMessageW(transport.window, WM_MURMUR_LEVELS, 0, 0);
}
void link_voice_levels(unsigned *self, unsigned *peer) {
    EnterCriticalSection(&transport.lock);
    if (self) *self = transport.level_self;
    if (peer) *peer = transport.level_peer;
    LeaveCriticalSection(&transport.lock);
}
static void voice_status(int state) {
    int changed;
    EnterCriticalSection(&transport.lock);
    changed = transport.voice_state != state;
    transport.voice_state = state;
    LeaveCriticalSection(&transport.lock);
    if (changed) PostMessageW(transport.window, WM_MURMUR_LINK, 0, 0);
}
static void remote_status(unsigned value) {
    int changed;
    EnterCriticalSection(&transport.lock);
    changed=transport.remote_voice!=value;
    /* Once a live peer leaves, end our participation too; no unexpected reopening. */
    if((transport.remote_voice&1) && !(value&1) && transport.voice_requested)
        transport.voice_requested=transport.voice_muted=0;
    transport.remote_voice=value;
    LeaveCriticalSection(&transport.lock);
    if(changed) PostMessageW(transport.window,WM_MURMUR_LINK,0,0);
}
static murmur_voice *configured_voice(int hardware) {
    murmur_audio_config config;
    EnterCriticalSection(&transport.lock); config=transport.audio; LeaveCriticalSection(&transport.lock);
    return murmur_voice_create_config(hardware,&config);
}
static void apply_volume(murmur_voice *voice) {
    EnterCriticalSection(&transport.lock);
    murmur_voice_volume(voice,transport.audio.input_volume,transport.audio.output_volume);
    murmur_voice_options(voice,transport.audio.echo,transport.audio.noise,transport.audio.agc,transport.audio.gate);
    LeaveCriticalSection(&transport.lock);
}
static void voice_failed(void) {
    EnterCriticalSection(&transport.lock);
    transport.voice_requested = transport.voice_muted = 0;
    LeaveCriticalSection(&transport.lock);
    voice_status(5);
}
static void update(int state, int error) {
    EnterCriticalSection(&transport.lock);
    transport.state = state;
    transport.error = error;
    LeaveCriticalSection(&transport.lock);
    PostMessageW(transport.window, WM_MURMUR_LINK, 0, 0);
}
static void ice_wakeup(void *user) { SetEvent((HANDLE)user); }
static void simulated_audio(murmur_voice *voice,ULONGLONG *last) {
    int16_t input[MURMUR_VOICE_SAMPLES],output[MURMUR_VOICE_SAMPLES]; unsigned i; ULONGLONG now=GetTickCount64();
    if (!transport.test_audio || now-*last<20) return;
    *last=now;
    for (i=0;i<MURMUR_VOICE_SAMPLES;++i) input[i]=(i%64)<32?2000:-2000;
    murmur_voice_capture(voice,input,MURMUR_VOICE_SAMPLES);
    murmur_voice_playback(voice,output,MURMUR_VOICE_SAMPLES);
}
static DWORD WINAPI run_ice(void *unused) {
    murmur_ice *peer;
    murmur_voice *voice = NULL;
    unsigned remote_voice = 0, sent_voice = 0;
    int exported = 0, connected = 0;
    DWORD wait = 0;
    ULONGLONG simulated_at=0;
    HANDLE events[2] = {transport.stop, transport.wake};
    (void)unused;
    transport.ice_settings.wakeup = ice_wakeup;
    transport.ice_settings.wakeup_user = transport.wake;
    peer = murmur_ice_create(transport.server, &transport.ice_settings, transport.server ? NULL : transport.ice_input);
    SecureZeroMemory(transport.ice_input, sizeof(transport.ice_input));
    if (!peer) {
        SecureZeroMemory(transport.ice_settings.password, sizeof(transport.ice_settings.password));
        update(3, -100); return 0;
    }
    while (WaitForMultipleObjects(2, events, FALSE, wait) != WAIT_OBJECT_0) {
        unsigned local_voice;
        int state, got, pending;
        link_message message = {0};
        murmur_kind kind;
        char token[MURMUR_ICE_TOKEN];
        EnterCriticalSection(&transport.lock);
        local_voice = transport.voice_requested ? (transport.voice_muted ? 3u : 1u) : 0u;
        pending = transport.answer_pending;
        if (pending) {
            memcpy(token, transport.ice_answer, sizeof(token));
            SecureZeroMemory(transport.ice_answer, sizeof(transport.ice_answer)); transport.answer_pending = 0;
        }
        LeaveCriticalSection(&transport.lock);
        if (pending) {
            got = murmur_ice_accept_answer(peer, token);
            EnterCriticalSection(&transport.lock); transport.signal_problem = !!got; LeaveCriticalSection(&transport.lock);
            PostMessageW(transport.window, WM_MURMUR_LINK, 0, 0);
        }
        if (!exported) {
            got = murmur_ice_local_token(peer, token, sizeof(token));
            if (got < 0) { update(3, -101); break; }
            if (got == 1) {
                EnterCriticalSection(&transport.lock); memcpy(transport.ice_output, token, sizeof(token)); LeaveCriticalSection(&transport.lock);
                exported = 1; PostMessageW(transport.window, WM_MURMUR_LINK, 0, 0);
            }
        }
        SecureZeroMemory(token, sizeof(token));
        state = murmur_ice_step(peer, GetTickCount64());
        if (state < 0) { update(3, murmur_ice_error(peer)); break; }
        wait = murmur_ice_wait_ms(peer, GetTickCount64());
        EnterCriticalSection(&transport.lock);
        got = transport.ice_phase != state;
        transport.ice_phase = state; transport.ice_relayed = murmur_ice_relayed(peer);
        LeaveCriticalSection(&transport.lock);
        if (got) PostMessageW(transport.window, WM_MURMUR_LINK, 0, 0);
        if (state != 4) continue;
        if (!connected) { connected = 1; update(2, 0); }
        if (local_voice != sent_voice && !murmur_ice_voice_state(peer, local_voice)) sent_voice = local_voice;
        EnterCriticalSection(&transport.lock);
        got = transport.out_count != 0;
        if (got) message = transport.out[transport.out_head];
        LeaveCriticalSection(&transport.lock);
        if (got && !murmur_ice_send(peer, message.room, message.text, message.size)) {
            EnterCriticalSection(&transport.lock);
            SecureZeroMemory(&transport.out[transport.out_head], sizeof(link_message));
            transport.out_head = (transport.out_head + 1) % QUEUE_SIZE; --transport.out_count;
            LeaveCriticalSection(&transport.lock);
        }
        EnterCriticalSection(&transport.lock); got = transport.in_count < QUEUE_SIZE; LeaveCriticalSection(&transport.lock);
        if (got && murmur_ice_receive(peer, &kind, &message.room, message.text, sizeof(message.text), &message.size) == 1) {
            if (kind == MURMUR_CONTROL) { remote_voice = message.text[0]; remote_status(remote_voice); }
            else {
                EnterCriticalSection(&transport.lock);
                transport.in[(transport.in_head + transport.in_count) % QUEUE_SIZE] = message;
                ++transport.in_count; LeaveCriticalSection(&transport.lock);
                PostMessageW(transport.window, WM_MURMUR_LINK, 0, 0);
            }
        }
        SecureZeroMemory(&message, sizeof(message));
        if (!(local_voice & 1) || !(remote_voice & 1)) {
            murmur_voice_free(voice); voice = NULL; publish_levels(NULL);
            if (local_voice) voice_status(1); else if (link_voice_state() != 5) voice_status(0);
        } else {
            uint8_t packet[MURMUR_VOICE_PACKET];
            int encoded;
            if (!voice) {
#ifdef MURMUR_HEADLESS_AUDIO_TEST
                voice = murmur_voice_create(0);
#else
                voice = configured_voice(!transport.test_audio);
#endif
                if (!voice) { voice_failed(); continue; }
            }
            apply_volume(voice);
            murmur_voice_mute(voice, (local_voice & 2) != 0);
            simulated_audio(voice,&simulated_at);
            voice_status(local_voice & 2 ? 4 : 3);
            encoded = murmur_voice_encode(voice, packet);
            if (encoded > 0) murmur_ice_send_audio(peer, packet, (size_t)encoded);
            SecureZeroMemory(packet, sizeof(packet));
        }
        /* Always drain UDP audio, even when the user declined the call. */
        {
            uint8_t packet[MURMUR_VOICE_PACKET];
            size_t size;
            int n;
            for (n = 0; n < 16 && murmur_ice_receive_audio(peer, packet, sizeof(packet), &size) == 1; ++n) {
                if (voice && murmur_voice_decode(voice, packet, size)) {
                    EnterCriticalSection(&transport.lock); ++transport.voice_packets; LeaveCriticalSection(&transport.lock);
                }
            }
            SecureZeroMemory(packet, sizeof(packet));
        }
        if (voice) {
            murmur_voice_stats stats;
            murmur_voice_pump(voice, GetTickCount64()); murmur_voice_get_stats(voice, &stats); publish_levels(voice);
            EnterCriticalSection(&transport.lock); transport.voice_decoded = stats.decoded; LeaveCriticalSection(&transport.lock);
        }
#ifdef DIRECT_SCREEN_SHARE
        screen_pump(peer,1);
        wait = (voice || screen_sharing()) ? 5 : murmur_ice_wait_ms(peer, GetTickCount64());
#else
        wait = voice ? 5 : murmur_ice_wait_ms(peer, GetTickCount64());
#endif
        EnterCriticalSection(&transport.lock); got = transport.in_count < QUEUE_SIZE; LeaveCriticalSection(&transport.lock);
        if (got && murmur_ice_pending_message(peer)) wait = 0;
    }
#ifdef DIRECT_SCREEN_SHARE
    screen_disconnect();
#endif
    murmur_voice_free(voice); publish_levels(NULL); murmur_ice_free(peer);
    EnterCriticalSection(&transport.lock);
    transport.voice_requested = transport.voice_muted = 0;
    SecureZeroMemory(transport.ice_output, sizeof(transport.ice_output));
    SecureZeroMemory(transport.ice_answer, sizeof(transport.ice_answer)); transport.answer_pending = 0;
    SecureZeroMemory(transport.ice_settings.password, sizeof(transport.ice_settings.password));
    LeaveCriticalSection(&transport.lock);
    remote_status(0);
    voice_status(0);
    if (WaitForSingleObject(transport.stop, 0) == WAIT_OBJECT_0) update(0, 0);
    PostMessageW(transport.window, WM_MURMUR_LINK, 0, 0);
    return 0;
}
static DWORD WINAPI run(void *unused) {
    murmur_peer *peer;
    murmur_peer *media = NULL;
    murmur_voice *voice = NULL;
    uint8_t media_key[32];
    unsigned remote_voice = 0, sent_voice = 0;
    ULONGLONG media_started = 0;
    int connected = 0;
    (void)unused;
    peer = murmur_peer_open(transport.server, transport.host, transport.port, transport.key);
    if (murmur_media_key(transport.key, media_key)) {
        murmur_peer_free(peer); peer = NULL;
    }
    SecureZeroMemory(transport.key, sizeof(transport.key));
    if (!peer) { SecureZeroMemory(media_key, sizeof(media_key)); update(3, -1); return 0; }
    while (WaitForSingleObject(transport.stop, 0) == WAIT_TIMEOUT) {
        unsigned local_voice;
        murmur_peer_state state;
        link_message message = {0};
        murmur_kind kind;
        int got = 0;
        EnterCriticalSection(&transport.lock);
        local_voice = transport.voice_requested ? (transport.voice_muted ? 3u : 1u) : 0u;
        LeaveCriticalSection(&transport.lock);
        state = murmur_peer_step(peer, local_voice && remote_voice ? 5 : 40);
        if (state == MURMUR_PEER_FAILED || state == MURMUR_PEER_CLOSED) {
            update(state == MURMUR_PEER_FAILED ? 3 : 0, murmur_peer_error(peer));
            break;
        }
        if (state != MURMUR_PEER_CONNECTED) continue;
        if (!connected) { connected = 1; update(2, 0); }
        /* Prioritize consent transitions over queued text. These controls are TLS-authenticated. */
        if (local_voice != sent_voice && murmur_peer_send_voice_state(peer, local_voice) == 0)
            sent_voice = local_voice;
        EnterCriticalSection(&transport.lock);
        if (transport.out_count) {
            message = transport.out[transport.out_head];
            got = 1;
        }
        LeaveCriticalSection(&transport.lock);
        if (got && murmur_peer_send(peer, message.room, message.text, message.size) == 0) {
            EnterCriticalSection(&transport.lock);
            SecureZeroMemory(&transport.out[transport.out_head], sizeof(link_message));
            transport.out_head = (transport.out_head + 1) % QUEUE_SIZE;
            --transport.out_count;
            LeaveCriticalSection(&transport.lock);
        }
        SecureZeroMemory(&message, sizeof(message));
        /* Full inbound queue backpressures the TLS stream rather than dropping messages. */
        EnterCriticalSection(&transport.lock);
        got = transport.in_count < QUEUE_SIZE;
        LeaveCriticalSection(&transport.lock);
        if (got) {
            got = murmur_peer_receive_packet(peer, &kind, &message.room, message.text, sizeof(message.text), &message.size);
            if (got == 1 && kind == MURMUR_CONTROL) { remote_voice = message.text[0]; remote_status(remote_voice); }
            else if (got == 1 && kind == MURMUR_TEXT) {
                EnterCriticalSection(&transport.lock);
                transport.in[(transport.in_head + transport.in_count) % QUEUE_SIZE] = message;
                ++transport.in_count;
                LeaveCriticalSection(&transport.lock);
                PostMessageW(transport.window, WM_MURMUR_LINK, 0, 0);
            }
        }
        SecureZeroMemory(&message, sizeof(message));
        if (!(local_voice & 1) || !(remote_voice & 1)) {
            murmur_voice_free(voice); voice = NULL; publish_levels(NULL);
            murmur_peer_free(media); media = NULL;
            if (local_voice) voice_status(1);
            else if (link_voice_state() != 5) voice_status(0);
            continue;
        }
        if (!media) {
            unsigned port_number = (unsigned)strtoul(transport.port, NULL, 10);
            char media_port[6];
            if (port_number == 65535) { voice_failed(); continue; }
            sprintf_s(media_port, sizeof(media_port), "%u", port_number + 1);
            media = murmur_media_open(transport.server, transport.host, media_port, media_key);
            if (!media) { voice_failed(); continue; }
            media_started = GetTickCount64();
            voice_status(2);
        }
        state = murmur_peer_step(media, 0);
        if (state == MURMUR_PEER_FAILED || state == MURMUR_PEER_CLOSED ||
            (state != MURMUR_PEER_CONNECTED && GetTickCount64() - media_started > 15000)) {
            murmur_voice_free(voice); voice = NULL; publish_levels(NULL);
            murmur_peer_free(media); media = NULL;
            voice_failed(); continue;
        }
        if (state == MURMUR_PEER_CONNECTED) {
            uint8_t packet[MURMUR_VOICE_PACKET];
            size_t size;
            unsigned room;
            int encoded, n;
            if (!voice) {
#ifdef MURMUR_HEADLESS_AUDIO_TEST
                voice = murmur_voice_create(0);
                if (voice) {
                    int16_t synthetic[MURMUR_VOICE_SAMPLES * 2];
                    unsigned i;
                    for (i = 0; i < MURMUR_VOICE_SAMPLES * 2; ++i) synthetic[i] = (i % 64) < 32 ? 4000 : -4000;
                    murmur_voice_capture(voice, synthetic, MURMUR_VOICE_SAMPLES * 2);
                }
#else
                /* Hardware is opened only after both opt-ins and DTLS authentication. */
                voice = configured_voice(1);
#endif
                if (!voice) { murmur_peer_free(media); media = NULL; voice_failed(); continue; }
            }
            apply_volume(voice);
            murmur_voice_mute(voice, (local_voice & 2) != 0);
            voice_status(local_voice & 2 ? 4 : 3);
            encoded = murmur_voice_encode(voice, packet);
            if (encoded > 0) murmur_peer_send_audio(media, packet, (size_t)encoded);
            /* Bounded drain of authenticated UDP packets; no unbounded work per tick. */
            for (n = 0; n < 8; ++n) {
                got = murmur_peer_receive_packet(media, &kind, &room, packet, sizeof(packet), &size);
                if (got != 1) break;
                if (kind == MURMUR_AUDIO && murmur_voice_decode(voice, packet, size)) {
                    EnterCriticalSection(&transport.lock); ++transport.voice_packets; LeaveCriticalSection(&transport.lock);
                }
                if (murmur_peer_step(media, 0) != MURMUR_PEER_CONNECTED) break;
            }
            SecureZeroMemory(packet, sizeof(packet));
            murmur_voice_pump(voice, GetTickCount64()); publish_levels(voice);
            {
                murmur_voice_stats stats;
                murmur_voice_get_stats(voice, &stats);
                EnterCriticalSection(&transport.lock); transport.voice_decoded = stats.decoded; LeaveCriticalSection(&transport.lock);
            }
        }
    }
    murmur_voice_free(voice); publish_levels(NULL);
    murmur_peer_free(media);
    murmur_peer_free(peer);
    SecureZeroMemory(media_key, sizeof(media_key));
    EnterCriticalSection(&transport.lock);
    transport.voice_requested = transport.voice_muted = 0;
    LeaveCriticalSection(&transport.lock);
    remote_status(0);
    voice_status(0);
    if (WaitForSingleObject(transport.stop, 0) == WAIT_OBJECT_0) update(0, 0);
    return 0;
}
void link_init(HWND window) {
    juice_set_log_level(JUICE_LOG_LEVEL_NONE);
    transport.window = window;
    murmur_audio_defaults(&transport.audio);
    InitializeCriticalSection(&transport.lock);
    transport.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    transport.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
}
int link_set_test_audio(int enabled) {
    if (link_busy()) return -1;
    transport.test_audio=!!enabled; return 0;
}
int link_busy(void) {
    return transport.thread && WaitForSingleObject(transport.thread, 0) == WAIT_TIMEOUT;
}
int link_start(int server, const char *host, const char *port, const uint8_t key[32]) {
    if (link_busy() || !transport.stop || !host || !port || !key || strlen(host) > 15 || strlen(port) > 5) return -1;
    if (transport.thread) { CloseHandle(transport.thread); transport.thread = NULL; }
    ResetEvent(transport.stop);
    transport.server = server;
    transport.ice_mode = 0;
    strcpy_s(transport.host, sizeof(transport.host), host);
    strcpy_s(transport.port, sizeof(transport.port), port);
    memcpy(transport.key, key, 32);
    EnterCriticalSection(&transport.lock);
    SecureZeroMemory(transport.out, sizeof(transport.out));
    SecureZeroMemory(transport.in, sizeof(transport.in));
    transport.out_head = transport.out_count = transport.in_head = transport.in_count = 0;
    transport.state = 1; transport.error = 0;
    transport.remote_voice=0;
    transport.voice_requested = transport.voice_muted = transport.voice_state = 0;
    transport.voice_packets = 0;
    transport.voice_decoded = 0;
    LeaveCriticalSection(&transport.lock);
    transport.thread = CreateThread(NULL, 0, run, NULL, 0, NULL);
    if (!transport.thread) { SecureZeroMemory(transport.key, 32); update(3, -1); return -1; }
    return 0;
}
void link_stop(void) {
    if (link_busy()) { update(4, 0); SetEvent(transport.stop); }
}
int link_send(unsigned room, const uint8_t *text, size_t size) {
    link_message *message;
    int result = -1;
    if (!text || !size || size > MURMUR_CHAT_LIMIT || room > 2) return -1;
    EnterCriticalSection(&transport.lock);
    if (transport.state == 2 && transport.out_count < QUEUE_SIZE) {
        message = &transport.out[(transport.out_head + transport.out_count) % QUEUE_SIZE];
        message->room = room; message->size = size;
        memcpy(message->text, text, size);
        ++transport.out_count;
        result = 0;
    }
    LeaveCriticalSection(&transport.lock);
    if (!result) SetEvent(transport.wake);
    return result;
}
int link_receive(link_message *message) {
    int result = 0;
    EnterCriticalSection(&transport.lock);
    if (transport.in_count) {
        *message = transport.in[transport.in_head];
        SecureZeroMemory(&transport.in[transport.in_head], sizeof(link_message));
        transport.in_head = (transport.in_head + 1) % QUEUE_SIZE;
        --transport.in_count; result = 1;
    }
    LeaveCriticalSection(&transport.lock);
    if (result) SetEvent(transport.wake);
    return result;
}
int link_state(void) {
    int state;
    EnterCriticalSection(&transport.lock); state = transport.state; LeaveCriticalSection(&transport.lock);
    return state;
}
int link_error(void) {
    int error;
    EnterCriticalSection(&transport.lock); error = transport.error; LeaveCriticalSection(&transport.lock);
    return error;
}
int link_voice_toggle(void) {
    int result = -1;
    EnterCriticalSection(&transport.lock);
    if (transport.state == 2) {
        transport.voice_requested = !transport.voice_requested;
        transport.voice_muted = 0;
        if (transport.voice_requested) transport.voice_state = 1;
        result = 0;
    }
    LeaveCriticalSection(&transport.lock);
    SetEvent(transport.wake);
    PostMessageW(transport.window, WM_MURMUR_LINK, 0, 0);
    return result;
}
void link_voice_mute_toggle(void) {
    EnterCriticalSection(&transport.lock);
    if (transport.voice_requested) transport.voice_muted = !transport.voice_muted;
    LeaveCriticalSection(&transport.lock);
    SetEvent(transport.wake);
}
int link_voice_state(void) {
    int value;
    EnterCriticalSection(&transport.lock); value = transport.voice_state; LeaveCriticalSection(&transport.lock);
    return value;
}
int link_voice_requested(void) {
    int value;
    EnterCriticalSection(&transport.lock); value = transport.voice_requested; LeaveCriticalSection(&transport.lock);
    return value;
}
unsigned link_voice_received_packets(void) {
    unsigned value;
    EnterCriticalSection(&transport.lock); value = transport.voice_packets; LeaveCriticalSection(&transport.lock);
    return value;
}
uint64_t link_voice_decoded_frames(void) {
    uint64_t value;
    EnterCriticalSection(&transport.lock); value = transport.voice_decoded; LeaveCriticalSection(&transport.lock);
    return value;
}
int link_ice_start(int offerer, const murmur_ice_config *settings, const char *offer) {
    if (link_busy() || !settings || !transport.stop || !transport.wake || (!offerer && (!offer || strlen(offer) >= MURMUR_ICE_TOKEN))) return -1;
    if (transport.thread) { CloseHandle(transport.thread); transport.thread = NULL; }
    ResetEvent(transport.stop);
    transport.ice_settings = *settings; transport.server = !!offerer; transport.ice_mode = 1;
    SecureZeroMemory(transport.ice_input, sizeof(transport.ice_input));
    if (!offerer) strcpy_s(transport.ice_input, sizeof(transport.ice_input), offer);
    EnterCriticalSection(&transport.lock);
    transport.state = 1; transport.error = 0; transport.ice_phase = transport.ice_relayed = transport.signal_problem = 0;
    transport.remote_voice=0;
    transport.voice_requested = transport.voice_muted = transport.voice_state = 0;
    transport.voice_packets = 0; transport.voice_decoded = 0;
    transport.in_head = transport.in_count = transport.out_head = transport.out_count = 0;
    SecureZeroMemory(transport.in, sizeof(transport.in)); SecureZeroMemory(transport.out, sizeof(transport.out));
    SecureZeroMemory(transport.ice_output, sizeof(transport.ice_output));
    SecureZeroMemory(transport.ice_answer, sizeof(transport.ice_answer)); transport.answer_pending = 0;
    LeaveCriticalSection(&transport.lock);
    transport.thread = CreateThread(NULL, 0, run_ice, NULL, 0, NULL);
    if (!transport.thread) {
        SecureZeroMemory(transport.ice_input, sizeof(transport.ice_input));
        SecureZeroMemory(transport.ice_settings.password, sizeof(transport.ice_settings.password));
        update(3, -1); return -1;
    }
    return 0;
}
int link_ice_answer(const char *answer) {
    int result = -1;
    if (!answer || strlen(answer) >= MURMUR_ICE_TOKEN) return -1;
    EnterCriticalSection(&transport.lock);
    if (transport.ice_mode && transport.server && transport.state == 1 && !transport.answer_pending) {
        strcpy_s(transport.ice_answer, sizeof(transport.ice_answer), answer); transport.answer_pending = 1; result = 0;
    }
    LeaveCriticalSection(&transport.lock);
    if (!result) SetEvent(transport.wake);
    return result;
}
int link_ice_token(char *token, size_t capacity) {
    int result = 0;
    if (!token || !capacity) return -1;
    EnterCriticalSection(&transport.lock);
    if (capacity > strlen(transport.ice_output)) { strcpy_s(token, capacity, transport.ice_output); result = !!token[0]; }
    else result = -1;
    LeaveCriticalSection(&transport.lock); return result;
}
int link_ice_mode(void) { return transport.ice_mode; }
int link_ice_phase(void) {
    int result;
    EnterCriticalSection(&transport.lock); result = transport.signal_problem ? -2 : transport.ice_phase; LeaveCriticalSection(&transport.lock); return result;
}
int link_ice_relayed(void) {
    int result;
    EnterCriticalSection(&transport.lock); result = transport.ice_relayed; LeaveCriticalSection(&transport.lock); return result;
}

unsigned link_voice_remote(void) {
    unsigned value; EnterCriticalSection(&transport.lock); value=transport.remote_voice; LeaveCriticalSection(&transport.lock); return value;
}
void link_audio_configure(const murmur_audio_config *config) {
    if(!config) return;
    EnterCriticalSection(&transport.lock); transport.audio=*config;
    if(transport.audio.input_volume>100) transport.audio.input_volume=100;
    if(transport.audio.output_volume>100) transport.audio.output_volume=100;
    LeaveCriticalSection(&transport.lock); SetEvent(transport.wake);
}
