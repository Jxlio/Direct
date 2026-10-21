#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MA_NO_NULL
#define MA_NO_DSOUND
#define MA_NO_WINMM
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#include <opus.h>
#include "murmur/voice.h"
#include <stdlib.h>
#include <string.h>

#define RING_SAMPLES (MURMUR_VOICE_SAMPLES * 4)
typedef struct {
    uint32_t sequence;
    size_t size;
    uint8_t opus[MURMUR_VOICE_PACKET - 4];
} jitter_packet;
struct murmur_voice {
    ma_device device;
    ma_pcm_rb capture, playback;
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    int device_ready, capture_ready, playback_ready, playing;
    ma_uint32 muted, input_volume, output_volume;
    uint32_t sequence, expected, last_played, last_real, history, highest;
    int initialized, scheduled, started, have_played, have_real, have_highest, missing;
    uint64_t deadline;
    jitter_packet jitter[MURMUR_VOICE_JITTER_PACKETS];
    murmur_voice_stats stats;
    int16_t capture_memory[RING_SAMPLES], playback_memory[RING_SAMPLES];
};
static size_t write_ring(ma_pcm_rb *ring, const int16_t *samples, size_t count) {
    size_t done = 0;
    while (done < count) {
        ma_uint32 available = (ma_uint32)(count - done);
        void *buffer;
        if (ma_pcm_rb_acquire_write(ring, &available, &buffer) != MA_SUCCESS || !available) break;
        memcpy(buffer, samples + done, available * sizeof(int16_t));
        ma_pcm_rb_commit_write(ring, available);
        done += available;
    }
    return done;
}
static size_t read_ring(ma_pcm_rb *ring, int16_t *samples, size_t count) {
    size_t done = 0;
    while (done < count) {
        ma_uint32 available = (ma_uint32)(count - done);
        void *buffer;
        if (ma_pcm_rb_acquire_read(ring, &available, &buffer) != MA_SUCCESS || !available) break;
        memcpy(samples + done, buffer, available * sizeof(int16_t));
        ma_pcm_rb_commit_read(ring, available);
        done += available;
    }
    return done;
}
void murmur_voice_capture(murmur_voice *v, const int16_t *samples, size_t count) {
    if (!v || !samples || ma_atomic_load_32(&v->muted)) return;
    /* Overflow drops incoming samples. The consumer skips stale queued capture. */
    {
        int16_t adjusted[256]; size_t offset=0; unsigned gain=ma_atomic_load_32(&v->input_volume);
        while (offset<count) {
            size_t i,n=count-offset; if(n>256) n=256;
            for(i=0;i<n;++i) adjusted[i]=(int16_t)((int)samples[offset+i]*(int)gain/100);
            write_ring(&v->capture,adjusted,n); offset+=n;
        }
    }
}
void murmur_voice_playback(murmur_voice *v, int16_t *samples, size_t count) {
    size_t done;
    if (!v || !samples) return;
    memset(samples, 0, count * sizeof(int16_t));
    if (!v->playing && ma_pcm_rb_available_read(&v->playback) >= MURMUR_VOICE_SAMPLES * 2)
        v->playing = 1;
    if (!v->playing) return;
    done = read_ring(&v->playback, samples, count);
    if (done < count) v->playing = 0;
    { size_t i; unsigned gain=ma_atomic_load_32(&v->output_volume);
      for(i=0;i<done;++i) samples[i]=(int16_t)((int)samples[i]*(int)gain/100); }
}
static void callback(ma_device *device, void *output, const void *input, ma_uint32 count) {
    murmur_voice *v = device->pUserData;
    if (input) murmur_voice_capture(v, input, count);
    if (output) murmur_voice_playback(v, output, count);
}
void murmur_voice_free(murmur_voice *v) {
    if (!v) return;
    if (v->device_ready) ma_device_uninit(&v->device);
    if (v->capture_ready) ma_pcm_rb_uninit(&v->capture);
    if (v->playback_ready) ma_pcm_rb_uninit(&v->playback);
    if (v->encoder) opus_encoder_destroy(v->encoder);
    if (v->decoder) opus_decoder_destroy(v->decoder);
    /* Use a volatile wipe for locally buffered PCM. */
    { volatile unsigned char *p = (volatile unsigned char *)v; size_t i;
      for (i = 0; i < sizeof(*v); ++i) p[i] = 0; }
    free(v);
}
murmur_voice *murmur_voice_create_config(int hardware, const murmur_audio_config *settings) {
    murmur_voice *v = calloc(1, sizeof(*v));
    int error;
    if (!v) return NULL;
    murmur_voice_volume(v, settings ? settings->input_volume : 100, settings ? settings->output_volume : 100);
    v->encoder = opus_encoder_create(MURMUR_VOICE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
    if (!v->encoder || error != OPUS_OK) goto fail;
    v->decoder = opus_decoder_create(MURMUR_VOICE_RATE, 1, &error);
    if (!v->decoder || error != OPUS_OK) goto fail;
    if (opus_encoder_ctl(v->encoder, OPUS_SET_BITRATE(24000)) != OPUS_OK ||
        opus_encoder_ctl(v->encoder, OPUS_SET_COMPLEXITY(5)) != OPUS_OK ||
        opus_encoder_ctl(v->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPUS_OK ||
        opus_encoder_ctl(v->encoder, OPUS_SET_DTX(1)) != OPUS_OK) goto fail;
    if (ma_pcm_rb_init(ma_format_s16, 1, RING_SAMPLES, v->capture_memory, NULL, &v->capture)) goto fail;
    v->capture_ready = 1;
    if (ma_pcm_rb_init(ma_format_s16, 1, RING_SAMPLES, v->playback_memory, NULL, &v->playback)) goto fail;
    v->playback_ready = 1;
    if (hardware) {
        ma_device_id input_id, output_id;
        ma_device_config config = ma_device_config_init(ma_device_type_duplex);
        if (settings && settings->input_selected) { memcpy(&input_id,settings->input_id,sizeof(input_id));
#ifdef _WIN32
            input_id.wasapi[63]=0;
#endif
            config.capture.pDeviceID=&input_id; }
        if (settings && settings->output_selected) { memcpy(&output_id,settings->output_id,sizeof(output_id));
#ifdef _WIN32
            output_id.wasapi[63]=0;
#endif
            config.playback.pDeviceID=&output_id; }
        config.capture.format = config.playback.format = ma_format_s16;
        config.capture.channels = config.playback.channels = 1;
        config.sampleRate = MURMUR_VOICE_RATE;
        config.periodSizeInMilliseconds = 10;
        config.dataCallback = callback;
        config.pUserData = v;
        if (ma_device_init(NULL, &config, &v->device) != MA_SUCCESS) goto fail;
        v->device_ready = 1;
        if (ma_device_start(&v->device) != MA_SUCCESS) goto fail;
    }
    return v;
fail:
    murmur_voice_free(v);
    return NULL;
}
void murmur_voice_mute(murmur_voice *v, int muted) {
    if (v) ma_atomic_store_32(&v->muted, muted ? 1u : 0u);
}
int murmur_voice_encode(murmur_voice *v, uint8_t packet[MURMUR_VOICE_PACKET]) {
    int16_t pcm[MURMUR_VOICE_SAMPLES];
    ma_uint32 available;
    int size;
    if (!v || !packet) return -1;
    available = ma_pcm_rb_available_read(&v->capture);
    if (ma_atomic_load_32(&v->muted)) {
        ma_pcm_rb_seek_read(&v->capture, available);
        return 0;
    }
    if (available < MURMUR_VOICE_SAMPLES) return 0;
    /* Only the capture consumer moves this cursor: preserve SPSC ownership. */
    if (available > MURMUR_VOICE_SAMPLES * 2)
        ma_pcm_rb_seek_read(&v->capture, available - MURMUR_VOICE_SAMPLES);
    if (read_ring(&v->capture, pcm, MURMUR_VOICE_SAMPLES) != MURMUR_VOICE_SAMPLES) return -1;
    size = opus_encode(v->encoder, pcm, MURMUR_VOICE_SAMPLES, packet + 4, MURMUR_VOICE_PACKET - 4);
    { volatile int16_t *p = pcm; size_t i; for (i = 0; i < MURMUR_VOICE_SAMPLES; ++i) p[i] = 0; }
    if (size < 0) return -1;
    packet[0] = (uint8_t)(v->sequence >> 24); packet[1] = (uint8_t)(v->sequence >> 16);
    packet[2] = (uint8_t)(v->sequence >> 8); packet[3] = (uint8_t)v->sequence;
    ++v->sequence;
    return size + 4;
}
int murmur_voice_decode(murmur_voice *v, const uint8_t *packet, size_t size) {
    uint32_t sequence, delta;
    jitter_packet *slot;
    if (!v || !packet || size < 5 || size > MURMUR_VOICE_PACKET ||
        opus_packet_get_nb_samples(packet + 4, (opus_int32)(size - 4), MURMUR_VOICE_RATE) != MURMUR_VOICE_SAMPLES) return 0;
    sequence = ((uint32_t)packet[0] << 24) | ((uint32_t)packet[1] << 16) |
        ((uint32_t)packet[2] << 8) | packet[3];
    if (v->have_played) {
        delta = sequence - v->last_played;
        if (!delta || delta >= UINT32_C(0x80000000)) {
            uint32_t age = v->last_played - sequence;
            if (age < 32 && (v->history & (UINT32_C(1) << age))) ++v->stats.duplicates;
            else ++v->stats.late;
            return 0;
        }
    }
    if (!v->initialized) {
        v->expected = sequence;
        v->initialized = 1;
    }
    delta = sequence - v->expected;
    if (delta >= UINT32_C(0x80000000)) {
        unsigned i;
        /* Before initial playout, an earlier packet can still fill the window. */
        if (v->started || v->expected - sequence >= MURMUR_VOICE_JITTER_PACKETS) {
            ++v->stats.late; return 0;
        }
        for (i = 0; i < MURMUR_VOICE_JITTER_PACKETS; ++i)
            if (v->jitter[i].size && v->jitter[i].sequence - sequence >= MURMUR_VOICE_JITTER_PACKETS) {
                ++v->stats.late; return 0;
            }
        v->expected = sequence;
    } else if (delta >= MURMUR_VOICE_JITTER_PACKETS) {
        /* A large jump must not generate seconds of old PLC or grow a queue. */
        memset(v->jitter, 0, sizeof(v->jitter));
        v->stats.buffered = 0;
        v->expected = sequence;
        v->scheduled = v->started = v->missing = 0;
        ++v->stats.resyncs;
        opus_decoder_ctl(v->decoder, OPUS_RESET_STATE);
    }
    slot = &v->jitter[sequence % MURMUR_VOICE_JITTER_PACKETS];
    if (slot->size) { ++v->stats.duplicates; return 0; }
    if (v->have_highest && sequence - v->highest >= UINT32_C(0x80000000)) ++v->stats.reordered;
    if (!v->have_highest || (sequence - v->highest && sequence - v->highest < UINT32_C(0x80000000))) {
        v->highest = sequence; v->have_highest = 1;
    }
    slot->sequence = sequence;
    slot->size = size - 4;
    memcpy(slot->opus, packet + 4, slot->size);
    ++v->stats.received; ++v->stats.buffered;
    return 1;
}
static void restart_playout(murmur_voice *v) {
    unsigned i;
    uint32_t earliest = 0, distance = UINT32_MAX;
    for (i = 0; i < MURMUR_VOICE_JITTER_PACKETS; ++i) {
        if (v->jitter[i].size && v->jitter[i].sequence - v->expected < distance) {
            distance = v->jitter[i].sequence - v->expected;
            earliest = v->jitter[i].sequence;
        }
    }
    v->initialized = v->stats.buffered != 0;
    if (v->initialized) v->expected = earliest;
    else if (v->have_real) {
        /* PLC has no sender sequence of its own. After silence, do not reject
           the first resumed frame merely because PLC advanced the local clock. */
        v->last_played = v->last_real;
        v->history = 1;
    }
    v->scheduled = v->started = v->missing = 0;
    ++v->stats.resyncs;
    opus_decoder_ctl(v->decoder, OPUS_RESET_STATE);
}
void murmur_voice_pump(murmur_voice *v, uint64_t now_ms) {
    int16_t pcm[MURMUR_VOICE_SAMPLES];
    unsigned produced = 0;
    if (!v || !v->initialized) return;
    if (!v->scheduled) { v->deadline = now_ms + 40; v->scheduled = 1; }
    if (now_ms > v->deadline && now_ms - v->deadline > 100) {
        /* Scheduler stall: retain only the newest queued packet, then prefill. */
        unsigned i;
        jitter_packet newest = {0};
        for (i = 0; i < MURMUR_VOICE_JITTER_PACKETS; ++i)
            if (v->jitter[i].size && (!newest.size ||
                (v->jitter[i].sequence - newest.sequence && v->jitter[i].sequence - newest.sequence < UINT32_C(0x80000000))))
                newest = v->jitter[i];
        memset(v->jitter, 0, sizeof(v->jitter));
        v->stats.buffered = newest.size ? 1u : 0u;
        if (newest.size) { v->jitter[newest.sequence % MURMUR_VOICE_JITTER_PACKETS] = newest; v->expected = newest.sequence; }
        restart_playout(v);
        return;
    }
    while (now_ms >= v->deadline && produced < 2) {
        jitter_packet *slot;
        int result, real = 0;
        uint32_t delta;
        if (ma_pcm_rb_available_write(&v->playback) < MURMUR_VOICE_SAMPLES) break;
        slot = &v->jitter[v->expected % MURMUR_VOICE_JITTER_PACKETS];
        if (slot->size && slot->sequence == v->expected) {
            result = opus_decode(v->decoder, slot->opus, (opus_int32)slot->size, pcm, MURMUR_VOICE_SAMPLES, 0);
            memset(slot, 0, sizeof(*slot)); --v->stats.buffered;
            real = result == MURMUR_VOICE_SAMPLES;
        }
        if (!real) {
            if (v->missing >= 3) { restart_playout(v); break; }
            result = opus_decode(v->decoder, NULL, 0, pcm, MURMUR_VOICE_SAMPLES, 0);
            ++v->missing; ++v->stats.concealed;
            if (result != MURMUR_VOICE_SAMPLES) memset(pcm, 0, sizeof(pcm));
        } else {
            v->missing = 0; ++v->stats.decoded;
            v->last_real = v->expected; v->have_real = 1;
        }
        write_ring(&v->playback, pcm, MURMUR_VOICE_SAMPLES);
        delta = v->have_played ? v->expected - v->last_played : 32;
        v->history = (delta >= 32 ? 0 : v->history << delta) | (real ? 1u : 0u);
        v->last_played = v->expected; v->have_played = v->started = 1;
        ++v->expected;
        v->deadline += 20;
        ++produced;
    }
    { volatile int16_t *p = pcm; size_t i; for (i = 0; i < MURMUR_VOICE_SAMPLES; ++i) p[i] = 0; }
}
void murmur_voice_get_stats(const murmur_voice *v, murmur_voice_stats *stats) {
    if (v && stats) *stats = v->stats;
}

void murmur_audio_defaults(murmur_audio_config *config) {
    if (!config) return;
    memset(config,0,sizeof(*config)); config->input_volume=config->output_volume=100;
}
murmur_voice *murmur_voice_create(int hardware) { return murmur_voice_create_config(hardware,NULL); }
void murmur_voice_volume(murmur_voice *v,unsigned input,unsigned output) {
    if(!v) return;
    ma_atomic_store_32(&v->input_volume,input>100?100:input);
    ma_atomic_store_32(&v->output_volume,output>100?100:output);
}
int murmur_audio_devices(int capture,murmur_audio_device *devices,unsigned capacity) {
    ma_context context; ma_device_info *inputs,*outputs,*list; ma_uint32 in_count,out_count,n,i;
    _Static_assert(sizeof(ma_device_id)<=256,"audio device ID capacity");
    if(!devices || ma_context_init(NULL,0,NULL,&context)!=MA_SUCCESS) return -1;
    if(ma_context_get_devices(&context,&outputs,&out_count,&inputs,&in_count)!=MA_SUCCESS) { ma_context_uninit(&context); return -1; }
    list=capture?inputs:outputs; n=capture?in_count:out_count; if(n>capacity) n=capacity;
    for(i=0;i<n;++i) { memset(&devices[i],0,sizeof(devices[i])); memcpy(devices[i].id,&list[i].id,sizeof(list[i].id)); memcpy(devices[i].name,list[i].name,sizeof(devices[i].name)-1); }
    ma_context_uninit(&context); return (int)n;
}
struct murmur_audio_preview { ma_device device; ma_uint32 level,volume; unsigned phase; int capture; };
static void preview_callback(ma_device *device,void *output,const void *input,ma_uint32 count) {
    murmur_audio_preview *p=device->pUserData; ma_uint32 i,peak=0,gain=ma_atomic_load_32(&p->volume);
    if(input) {
        const int16_t *pcm=input;
        for(i=0;i<count;++i) { unsigned value=(unsigned)abs((int)pcm[i])*gain/100; if(value>peak) peak=value; }
        ma_atomic_store_32(&p->level,peak*100/32768);
    }
    if(output) {
        int16_t *pcm=output;
        for(i=0;i<count;++i) { int value=(int)(p->phase%120); value=value<60?value:120-value; pcm[i]=(int16_t)((value-30)*100*(int)gain/100); ++p->phase; }
    }
}
murmur_audio_preview *murmur_audio_preview_start(int capture,const murmur_audio_config *settings) {
    murmur_audio_preview *p=calloc(1,sizeof(*p)); ma_device_id id;
    ma_device_config config=ma_device_config_init(capture?ma_device_type_capture:ma_device_type_playback);
    if(!p || !settings) { free(p); return NULL; }
    p->capture=!!capture; p->volume=capture?settings->input_volume:settings->output_volume; if(p->volume>100) p->volume=100;
    config.capture.format=config.playback.format=ma_format_s16;
    config.capture.channels=config.playback.channels=1; config.sampleRate=MURMUR_VOICE_RATE;
    config.dataCallback=preview_callback; config.pUserData=p;
    if(capture && settings->input_selected) { memcpy(&id,settings->input_id,sizeof(id));
#ifdef _WIN32
        id.wasapi[63]=0;
#endif
        config.capture.pDeviceID=&id; }
    if(!capture && settings->output_selected) { memcpy(&id,settings->output_id,sizeof(id));
#ifdef _WIN32
        id.wasapi[63]=0;
#endif
        config.playback.pDeviceID=&id; }
    if(ma_device_init(NULL,&config,&p->device)!=MA_SUCCESS) { free(p); return NULL; }
    if(ma_device_start(&p->device)!=MA_SUCCESS) { ma_device_uninit(&p->device); free(p); return NULL; }
    return p;
}
void murmur_audio_preview_stop(murmur_audio_preview *p) { if(p) { ma_device_uninit(&p->device); free(p); } }
unsigned murmur_audio_preview_level(murmur_audio_preview *p) { return p?ma_atomic_load_32(&p->level):0; }
void murmur_audio_preview_volume(murmur_audio_preview *p,unsigned input,unsigned output) {
    unsigned volume;
    if(!p) return; volume=p->capture?input:output; ma_atomic_store_32(&p->volume,volume>100?100:volume);
}
