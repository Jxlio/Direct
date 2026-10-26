#include "murmur/ice.h"
#include <juice/juice.h>
#include <ikcp.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/timing.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/base64.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/constant_time.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION ice_mutex;
#define mutex_init(m) InitializeCriticalSection(m)
#define mutex_lock(m) EnterCriticalSection(m)
#define mutex_unlock(m) LeaveCriticalSection(m)
#define mutex_free(m) DeleteCriticalSection(m)
#else
#include <pthread.h>
typedef pthread_mutex_t ice_mutex;
#define mutex_init(m) ((void)pthread_mutex_init(m, NULL))
#define mutex_lock(m) ((void)pthread_mutex_lock(m))
#define mutex_unlock(m) ((void)pthread_mutex_unlock(m))
#define mutex_free(m) ((void)pthread_mutex_destroy(m))
#endif
#define DATAGRAM_SIZE 1600
#define QUEUE 16
#define KCP_MTU 1000
#define MESSAGE_SIZE (MURMUR_CHAT_LIMIT + MURMUR_HEADER_SIZE + 1)
typedef struct { size_t size; uint8_t bytes[DATAGRAM_SIZE]; } datagram;
struct murmur_ice {
    juice_agent_t *agent;
    ice_mutex lock;
    juice_state_t ice_state;
    int gathered, offerer, remote_set, state, error, tls_ready, relayed;
    murmur_ice_config settings;
    uint8_t key[32], transport_id[32];
    datagram incoming[QUEUE], reliable[QUEUE], audio_in[QUEUE], audio_out, pending;
    unsigned in_head, in_count, out_head, out_count, audio_head, audio_count;
    datagram screen_in[QUEUE], screen_out[QUEUE];
    unsigned screen_head, screen_count, screen_out_head, screen_out_count;
    mbedtls_ssl_context tls;
    mbedtls_ssl_config config;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_ssl_cookie_ctx cookie;
    mbedtls_timing_delay_context timer;
    ikcpcb *kcp;
    uint64_t handshake_started, last_rx, last_heartbeat, checks_started;
};
static const int suites[] = {MBEDTLS_TLS_ECDHE_PSK_WITH_CHACHA20_POLY1305_SHA256, 0};
static const unsigned char psk_id[] = "murmur-ice-session-v1";
static int fail(murmur_ice *p, int error) { p->state = -1; p->error = error; return -1; }
static int token_mac(const uint8_t key[32], const uint8_t *bytes, size_t size, uint8_t mac[32]) {
    uint8_t derived[32];
    static const unsigned char domain[] = "murmur/ice/signaling/v1";
    const mbedtls_md_info_t *hash = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int result = mbedtls_hkdf(hash, NULL, 0, key, 32, domain, sizeof(domain) - 1, derived, 32);
    if (!result) result = mbedtls_md_hmac(hash, derived, 32, bytes, size, mac);
    mbedtls_platform_zeroize(derived, sizeof(derived));
    return result;
}
static int unpack(const char *text, int role, uint8_t key[32], char sdp[JUICE_MAX_SDP_STRING_LEN]) {
    char compact[MURMUR_ICE_TOKEN];
    uint8_t binary[68 + JUICE_MAX_SDP_STRING_LEN], mac[32], supplied[32];
    size_t i, length = 0, decoded = 0, sdp_size;
    int result = -1;
    if (!text) return -1;
    for (i = 0; text[i] && i < MURMUR_ICE_TOKEN; ++i) {
        if (text[i] != ' ' && text[i] != '\n' && text[i] != '\r' && text[i] != '\t') {
            if (length + 1 >= sizeof(compact)) goto done;
            compact[length++] = text[i];
        }
    }
    if (i == MURMUR_ICE_TOKEN || length < 8 || memcmp(compact, "murmur2:", 8)) goto done;
    if (mbedtls_base64_decode(binary, sizeof(binary), &decoded, (unsigned char *)compact + 8, length - 8) || decoded < 69) goto done;
    if (binary[0] != 1 || binary[1] != role) goto done;
    sdp_size = ((size_t)binary[66] << 8) | binary[67];
    if (!sdp_size || sdp_size >= JUICE_MAX_SDP_STRING_LEN || decoded != sdp_size + 68 || memchr(binary + 68, 0, sdp_size)) goto done;
    if (!role) memcpy(key, binary + 2, 32);
    else { uint8_t zeros[32] = {0}; if (memcmp(binary + 2, zeros, 32)) goto done; }
    memcpy(supplied, binary + 34, 32);
    memset(binary + 34, 0, 32);
    if (token_mac(key, binary, decoded, mac) || mbedtls_ct_memcmp(supplied, mac, 32)) goto done;
    memcpy(sdp, binary + 68, sdp_size); sdp[sdp_size] = 0;
    result = 0;
done:
    mbedtls_platform_zeroize(compact, sizeof(compact));
    mbedtls_platform_zeroize(binary, sizeof(binary));
    mbedtls_platform_zeroize(mac, sizeof(mac));
    mbedtls_platform_zeroize(supplied, sizeof(supplied));
    return result;
}
static int relay_description(char sdp[JUICE_MAX_SDP_STRING_LEN]) {
    char filtered[JUICE_MAX_SDP_STRING_LEN];
    const char *cursor = sdp;
    size_t used = 0;
    int found = 0;
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        size_t size = end ? (size_t)(end - cursor) + 1 : strlen(cursor);
        int candidate = size >= 12 && !memcmp(cursor, "a=candidate:", 12);
        const char *relay = strstr(cursor, " typ relay");
        int keep = !candidate || (relay && relay < cursor + size);
        if (keep) { memcpy(filtered + used, cursor, size); used += size; if (candidate) found = 1; }
        cursor += size;
    }
    filtered[used] = 0;
    memcpy(sdp, filtered, used + 1);
    mbedtls_platform_zeroize(filtered, sizeof(filtered));
    return found ? 0 : -1;
}
static void state_changed(juice_agent_t *agent, juice_state_t state, void *user) {
    murmur_ice *p = user; (void)agent;
    mutex_lock(&p->lock); p->ice_state = state; mutex_unlock(&p->lock);
    if (p->settings.wakeup) p->settings.wakeup(p->settings.wakeup_user);
}
static void gathered(juice_agent_t *agent, void *user) {
    murmur_ice *p = user; (void)agent;
    mutex_lock(&p->lock); p->gathered = 1; mutex_unlock(&p->lock);
    if (p->settings.wakeup) p->settings.wakeup(p->settings.wakeup_user);
}
static void received(juice_agent_t *agent, const char *data, size_t size, void *user) {
    murmur_ice *p = user; (void)agent;
    if (!size || size > DATAGRAM_SIZE) return;
    mutex_lock(&p->lock);
    if (p->in_count < QUEUE) {
        datagram *d = &p->incoming[(p->in_head + p->in_count) % QUEUE];
        memcpy(d->bytes, data, size); d->size = size; ++p->in_count;
    }
    mutex_unlock(&p->lock);
    if (p->settings.wakeup) p->settings.wakeup(p->settings.wakeup_user);
}
static int bio_send(void *context, const unsigned char *data, size_t size) {
    murmur_ice *p = context;
    int result = juice_send(p->agent, (const char *)data, size);
    if (!result) return (int)size;
    return result == JUICE_ERR_AGAIN || result == JUICE_ERR_NOT_AVAIL ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}
static int bio_recv(void *context, unsigned char *data, size_t capacity) {
    murmur_ice *p = context;
    size_t size;
    mutex_lock(&p->lock);
    if (!p->in_count) { mutex_unlock(&p->lock); return MBEDTLS_ERR_SSL_WANT_READ; }
    size = p->incoming[p->in_head].size;
    if (size <= capacity) memcpy(data, p->incoming[p->in_head].bytes, size);
    mbedtls_platform_zeroize(&p->incoming[p->in_head], sizeof(datagram));
    p->in_head = (p->in_head + 1) % QUEUE; --p->in_count;
    mutex_unlock(&p->lock);
    return size <= capacity ? (int)size : MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
}
static int reliable_output(const char *data, int length, ikcpcb *kcp, void *user) {
    murmur_ice *p = user;
    datagram *d; (void)kcp;
    if (length < 0 || length > KCP_MTU || p->out_count == QUEUE) return -1;
    d = &p->reliable[(p->out_head + p->out_count) % QUEUE];
    d->bytes[0] = 1; memcpy(d->bytes + 1, data, (size_t)length); d->size = (size_t)length + 1;
    ++p->out_count;
    return 0;
}
void murmur_ice_free(murmur_ice *p) {
    if (!p) return;
    if (p->state == 4) (void)mbedtls_ssl_close_notify(&p->tls);
    if (p->agent) juice_destroy(p->agent);
    if (p->kcp) ikcp_release(p->kcp);
    mbedtls_ssl_free(&p->tls); mbedtls_ssl_config_free(&p->config);
    mbedtls_ssl_cookie_free(&p->cookie); mbedtls_ctr_drbg_free(&p->random); mbedtls_entropy_free(&p->entropy);
    mutex_free(&p->lock);
    mbedtls_platform_zeroize(p, sizeof(*p)); free(p);
}
static int set_remote(murmur_ice *p, char *sdp) {
    if (p->settings.require_relay && relay_description(sdp)) return -1;
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (uint8_t *)sdp, strlen(sdp), p->transport_id) ||
        juice_set_remote_description(p->agent, sdp)) return -1;
    p->remote_set = 1;
    return 0;
}
murmur_ice *murmur_ice_create(int offerer, const murmur_ice_config *settings, const char *offer) {
    murmur_ice *p = calloc(1, sizeof(*p));
    juice_config_t config = {0};
    juice_turn_server_t turn = {0};
    char sdp[JUICE_MAX_SDP_STRING_LEN] = {0};
    if (!p) return NULL;
    mutex_init(&p->lock);
    mbedtls_ssl_init(&p->tls); mbedtls_ssl_config_init(&p->config); mbedtls_ssl_cookie_init(&p->cookie);
    mbedtls_entropy_init(&p->entropy); mbedtls_ctr_drbg_init(&p->random);
    p->offerer = !!offerer;
    if (settings) p->settings = *settings;
#define CHECK_STRING(field) if (!memchr(p->settings.field, 0, sizeof(p->settings.field))) goto error
    CHECK_STRING(stun_host); CHECK_STRING(turn_host); CHECK_STRING(username); CHECK_STRING(password); CHECK_STRING(bind_address);
#undef CHECK_STRING
    if (psa_crypto_init() != PSA_SUCCESS || (offerer ? murmur_random_key(p->key) : unpack(offer, 0, p->key, sdp))) goto error;
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
    config.bind_address = *p->settings.bind_address ? p->settings.bind_address : NULL;
    if (*p->settings.stun_host) { config.stun_server_host = p->settings.stun_host; config.stun_server_port = p->settings.stun_port ? p->settings.stun_port : 3478; }
    if (*p->settings.turn_host) {
        if (!*p->settings.username || !*p->settings.password) goto error;
        turn.host = p->settings.turn_host; turn.port = p->settings.turn_port ? p->settings.turn_port : 3478;
        turn.username = p->settings.username; turn.password = p->settings.password;
        config.turn_servers = &turn; config.turn_servers_count = 1;
    }
    config.cb_recv = received; config.cb_state_changed = state_changed; config.cb_gathering_done = gathered; config.user_ptr = p;
    p->agent = juice_create(&config);
    if (!p->agent || (!offerer && set_remote(p, sdp)) || juice_gather_candidates(p->agent)) goto error;
    mbedtls_platform_zeroize(sdp, sizeof(sdp));
    return p;
error:
    mbedtls_platform_zeroize(sdp, sizeof(sdp)); murmur_ice_free(p); return NULL;
}
int murmur_ice_local_token(murmur_ice *p, char *out, size_t capacity) {
    char sdp[JUICE_MAX_SDP_STRING_LEN];
    uint8_t binary[68 + JUICE_MAX_SDP_STRING_LEN] = {0}, mac[32];
    size_t length, written;
    int ready, result = -1;
    if (!p || !out || capacity < 9) return -1;
    mutex_lock(&p->lock); ready = p->gathered; mutex_unlock(&p->lock);
    if (!ready) return 0;
    if (juice_get_local_description(p->agent, sdp, sizeof(sdp)) ||
        (p->settings.require_relay && relay_description(sdp))) goto done;
    length = strlen(sdp);
    binary[0] = 1; binary[1] = p->offerer ? 0 : 1;
    if (p->offerer) memcpy(binary + 2, p->key, 32);
    binary[66] = (uint8_t)(length >> 8); binary[67] = (uint8_t)length;
    memcpy(binary + 68, sdp, length);
    if (token_mac(p->key, binary, length + 68, mac)) goto done;
    memcpy(binary + 34, mac, 32);
    memcpy(out, "murmur2:", 8);
    if (!mbedtls_base64_encode((unsigned char *)out + 8, capacity - 8, &written, binary, length + 68)) result = 1;
done:
    mbedtls_platform_zeroize(binary, sizeof(binary)); mbedtls_platform_zeroize(mac, sizeof(mac)); mbedtls_platform_zeroize(sdp, sizeof(sdp));
    return result;
}
int murmur_ice_accept_answer(murmur_ice *p, const char *answer) {
    char sdp[JUICE_MAX_SDP_STRING_LEN];
    int result;
    if (!p || !p->offerer || p->remote_set) return -1;
    result = unpack(answer, 1, p->key, sdp);
    if (!result) result = set_remote(p, sdp);
    mbedtls_platform_zeroize(sdp, sizeof(sdp));
    return result;
}
static int setup_tls(murmur_ice *p) {
    if (mbedtls_ctr_drbg_seed(&p->random, mbedtls_entropy_func, &p->entropy, psk_id, sizeof(psk_id) - 1) ||
        mbedtls_ssl_config_defaults(&p->config, p->offerer ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT)) return -1;
    mbedtls_ssl_conf_min_tls_version(&p->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&p->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_ciphersuites(&p->config, suites);
    mbedtls_ssl_conf_rng(&p->config, mbedtls_ctr_drbg_random, &p->random);
    /* The answerer may wait while a human copies its answer back to the caller. */
    mbedtls_ssl_conf_handshake_timeout(&p->config, p->offerer ? 250 : 500, p->offerer ? 4000 : 60000);
    if (p->offerer) {
        if (mbedtls_ssl_cookie_setup(&p->cookie, mbedtls_ctr_drbg_random, &p->random)) return -1;
        mbedtls_ssl_conf_dtls_cookies(&p->config, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check, &p->cookie);
    }
    if (mbedtls_ssl_conf_psk(&p->config, p->key, 32, psk_id, sizeof(psk_id) - 1) || mbedtls_ssl_setup(&p->tls, &p->config)) return -1;
    mbedtls_ssl_set_mtu(&p->tls, 1200);
    mbedtls_ssl_set_bio(&p->tls, p, bio_send, bio_recv, NULL);
    mbedtls_ssl_set_timer_cb(&p->tls, &p->timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay);
    if (p->offerer && mbedtls_ssl_set_client_transport_id(&p->tls, p->transport_id, 32)) return -1;
    p->tls_ready = 1;
    return 0;
}
static int input_reliable(murmur_ice *p, const uint8_t *bytes, size_t size) {
    const uint8_t *cursor = bytes;
    size_t remaining = size;
    unsigned segments = 0;
    if (size > KCP_MTU) return -1;
    while (remaining) {
        uint32_t length;
        if (remaining < 24 || ++segments > 42 || cursor[5] >= 16) return -1;
        length = (uint32_t)cursor[20] | ((uint32_t)cursor[21] << 8) | ((uint32_t)cursor[22] << 16) | ((uint32_t)cursor[23] << 24);
        if (length > remaining - 24 || (cursor[4] == 81 && length > KCP_MTU - 24)) return -1;
        cursor += 24 + length; remaining -= 24 + length;
    }
    if (p->kcp->ackcount + segments > 256) return 0; /* bounded ACK storage; dropped input is retransmitted */
    return ikcp_input(p->kcp, (const char *)bytes, (long)size);
}
int murmur_ice_step(murmur_ice *p, uint64_t now) {
    juice_state_t state;
    unsigned i;
    int r;
    char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN], remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
    if (!p || p->state < 0) return -1;
    mutex_lock(&p->lock); state = p->ice_state; mutex_unlock(&p->lock);
    if (state == JUICE_STATE_FAILED) return fail(p, -10);
    if (!p->remote_set) { p->state = 1; return p->state; }
    if (!p->checks_started) p->checks_started = now ? now : 1;
    if (state != JUICE_STATE_CONNECTED && state != JUICE_STATE_COMPLETED) {
        if (now > p->checks_started && now - p->checks_started > (p->offerer ? 45000u : 120000u)) return fail(p, -11);
        p->state = 2; return p->state;
    }
    if (juice_get_selected_candidates(p->agent, local, sizeof(local), remote, sizeof(remote))) { p->state = 2; return p->state; }
    p->relayed = strstr(local, " typ relay") || strstr(remote, " typ relay");
    if (p->settings.require_relay && state != JUICE_STATE_COMPLETED) {
        if (now > p->checks_started && now - p->checks_started > (p->offerer ? 45000u : 120000u)) return fail(p, -11);
        p->state = 2; return p->state;
    }
    if (p->settings.require_relay && !p->relayed) return fail(p, -12);
    if (!p->tls_ready) { if (setup_tls(p)) return fail(p, -20); p->handshake_started = now; }
    if (!p->kcp) {
        p->state = 3;
        if (now - p->handshake_started > (p->offerer ? 15000u : 120000u)) return fail(p, -21);
        r = mbedtls_ssl_handshake(&p->tls);
        if (r == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED && p->offerer) {
            if (mbedtls_ssl_session_reset(&p->tls) || mbedtls_ssl_set_client_transport_id(&p->tls, p->transport_id, 32)) return fail(p, -22);
            return p->state;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return p->state;
        if (r) return fail(p, r);
        p->kcp = ikcp_create(UINT32_C(0x4d555232), p);
        if (!p->kcp || ikcp_setmtu(p->kcp, KCP_MTU)) return fail(p, -30);
        ikcp_wndsize(p->kcp, 16, 32);
        /* Upstream enforces a 128-fragment default. Our validated fragment
           count is <16, so an explicitly smaller advertised receive window is safe. */
        p->kcp->rcv_wnd = 32;
        ikcp_nodelay(p->kcp, 1, 10, 2, 0);
        ikcp_setoutput(p->kcp, reliable_output);
        p->state = 4; p->last_rx = p->last_heartbeat = now;
    }
    /* An ICE path change must not allocate a second KCP session or discard
       already queued messages on the existing authenticated DTLS association. */
    p->state = 4;
    for (i = 0; i < QUEUE; ++i) {
        uint8_t bytes[DATAGRAM_SIZE];
        r = mbedtls_ssl_read(&p->tls, bytes, sizeof(bytes));
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (r <= 0) return fail(p, r ? r : -31);
        p->last_rx = now;
        if (bytes[0] == 1) { if (input_reliable(p, bytes + 1, (size_t)r - 1)) return fail(p, -32); }
        else if (bytes[0] == 2 && r >= 6 && r <= MURMUR_AUDIO_LIMIT + 1) {
            if (p->audio_count < QUEUE) {
                datagram *d = &p->audio_in[(p->audio_head + p->audio_count) % QUEUE];
                d->size = (size_t)r - 1; memcpy(d->bytes, bytes + 1, d->size); ++p->audio_count;
            }
        } else if (bytes[0] == 4 && r > 1 && r <= MURMUR_SCREEN_PACKET + 1) {
            if (p->screen_count < QUEUE) {
                datagram *d = &p->screen_in[(p->screen_head + p->screen_count) % QUEUE];
                d->size = (size_t)r - 1; memcpy(d->bytes, bytes + 1, d->size); ++p->screen_count;
            }
        } else if (bytes[0] != 3 || r != 1) return fail(p, -33);
        mbedtls_platform_zeroize(bytes, sizeof(bytes));
    }
    if (now - p->last_rx > 15000) return fail(p, -34);
    ikcp_update(p->kcp, (IUINT32)now);
    if (p->kcp->state == (IUINT32)-1) return fail(p, -35);
    for (i = 0; i < 8; ++i) {
        if (!p->pending.size) {
            if (p->audio_out.size) { p->pending = p->audio_out; mbedtls_platform_zeroize(&p->audio_out, sizeof(datagram)); }
            else if (p->out_count) {
                p->pending = p->reliable[p->out_head];
                mbedtls_platform_zeroize(&p->reliable[p->out_head], sizeof(datagram));
                p->out_head = (p->out_head + 1) % QUEUE; --p->out_count;
            } else if (p->screen_out_count) {
                p->pending = p->screen_out[p->screen_out_head];
                mbedtls_platform_zeroize(&p->screen_out[p->screen_out_head], sizeof(datagram));
                p->screen_out_head = (p->screen_out_head + 1) % QUEUE; --p->screen_out_count;
            } else if (now - p->last_heartbeat >= 1000) { p->pending.bytes[0] = 3; p->pending.size = 1; p->last_heartbeat = now; }
            else break;
        }
        r = mbedtls_ssl_write(&p->tls, p->pending.bytes, p->pending.size);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (r < 0 || (size_t)r != p->pending.size) return fail(p, r < 0 ? r : -36);
        mbedtls_platform_zeroize(&p->pending, sizeof(datagram));
    }
    return p->state;
}
static int send_frame(murmur_ice *p, murmur_kind kind, const uint8_t *payload, size_t size) {
    uint8_t bytes[MESSAGE_SIZE];
    size_t written;
    int result;
    if (!p || p->state != 4 || ikcp_waitsnd(p->kcp) + (int)((size + 12 + 975) / 976) > 32 ||
        murmur_frame_encode(kind, payload, size, bytes, sizeof(bytes), &written) != MURMUR_OK) return -1;
    result = ikcp_send(p->kcp, (const char *)bytes, (int)written);
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    return result < 0 ? -1 : 0;
}
int murmur_ice_send(murmur_ice *p, unsigned room, const uint8_t *data, size_t size) {
    uint8_t payload[MURMUR_CHAT_LIMIT + 1];
    int result;
    if (!data || !size || size > MURMUR_CHAT_LIMIT || room > 2) return -1;
    payload[0] = (uint8_t)room; memcpy(payload + 1, data, size);
    result = send_frame(p, MURMUR_TEXT, payload, size + 1);
    mbedtls_platform_zeroize(payload, sizeof(payload)); return result;
}
int murmur_ice_voice_state(murmur_ice *p, unsigned flags) {
    uint8_t payload = (uint8_t)flags;
    if (flags != 0 && flags != 1 && flags != 3) return -1;
    return send_frame(p, MURMUR_CONTROL, &payload, 1);
}
int murmur_ice_receive(murmur_ice *p, murmur_kind *kind, unsigned *room, uint8_t *data, size_t capacity, size_t *size) {
    uint8_t bytes[MESSAGE_SIZE];
    murmur_frame frame;
    size_t consumed, offset;
    int peek, result = -1;
    if (size) *size = 0;
    if (!p || p->state != 4 || !kind || !room || !data || !size) return -1;
    peek = ikcp_peeksize(p->kcp);
    if (peek < 0) return 0;
    if ((size_t)peek > sizeof(bytes)) return fail(p, -40);
    if (ikcp_recv(p->kcp, (char *)bytes, -(int)sizeof(bytes)) != peek ||
        murmur_frame_decode(bytes, (size_t)peek, &frame, &consumed) != MURMUR_OK || consumed != (size_t)peek) goto invalid;
    if (frame.kind == MURMUR_TEXT) {
        if (frame.payload_size < 2 || frame.payload_size > MURMUR_CHAT_LIMIT + 1 || frame.payload[0] > 2) goto invalid;
    } else if (frame.kind == MURMUR_CONTROL) {
        if (frame.payload_size != 1 || (frame.payload[0] != 0 && frame.payload[0] != 1 && frame.payload[0] != 3)) goto invalid;
    } else goto invalid;
    offset = frame.kind == MURMUR_TEXT ? 1u : 0u;
    if (capacity < frame.payload_size - offset) goto done;
    if (ikcp_recv(p->kcp, NULL, sizeof(bytes)) != peek) goto invalid;
    *kind = frame.kind; *room = offset ? frame.payload[0] : 0;
    *size = frame.payload_size - offset; memcpy(data, frame.payload + offset, *size);
    result = 1; goto done;
invalid:
    fail(p, -41);
done:
    mbedtls_platform_zeroize(bytes, sizeof(bytes)); return result;
}
int murmur_ice_send_audio(murmur_ice *p, const uint8_t *data, size_t size) {
    if (!p || p->state != 4 || !data || size < 5 || size > MURMUR_AUDIO_LIMIT) return -1;
    p->audio_out.bytes[0] = 2; memcpy(p->audio_out.bytes + 1, data, size); p->audio_out.size = size + 1;
    return 0;
}
int murmur_ice_receive_audio(murmur_ice *p, uint8_t *data, size_t capacity, size_t *size) {
    datagram *d;
    if (size) *size = 0;
    if (!p || !data || !size) return -1;
    if (!p->audio_count) return 0;
    d = &p->audio_in[p->audio_head];
    if (capacity < d->size) return -1;
    memcpy(data, d->bytes, d->size); *size = d->size;
    mbedtls_platform_zeroize(d, sizeof(*d));
    p->audio_head = (p->audio_head + 1) % QUEUE; --p->audio_count;
    return 1;
}
int murmur_ice_send_screen(murmur_ice *p, const uint8_t *data, size_t size) {
    datagram *d;
    if (!p || p->state != 4 || !data || !size || size > MURMUR_SCREEN_PACKET) return -1;
    if (p->screen_out_count == QUEUE) return 1;
    d = &p->screen_out[(p->screen_out_head + p->screen_out_count) % QUEUE];
    d->bytes[0] = 4; memcpy(d->bytes + 1, data, size); d->size = size + 1;
    ++p->screen_out_count; return 0;
}
int murmur_ice_receive_screen(murmur_ice *p, uint8_t *data, size_t capacity, size_t *size) {
    datagram *d;
    if (size) *size = 0;
    if (!p || !data || !size) return -1;
    if (!p->screen_count) return 0;
    d = &p->screen_in[p->screen_head];
    if (capacity < d->size) return -1;
    memcpy(data, d->bytes, d->size); *size = d->size;
    mbedtls_platform_zeroize(d, sizeof(*d));
    p->screen_head = (p->screen_head + 1) % QUEUE; --p->screen_count; return 1;
}
int murmur_ice_error(const murmur_ice *p) { return p ? p->error : -1; }
int murmur_ice_relayed(const murmur_ice *p) { return p ? p->relayed : 0; }
unsigned murmur_ice_wait_ms(const murmur_ice *p, uint64_t now) {
    uint64_t remaining;
    if (!p || p->state < 0) return 1000;
    if (!p->remote_set) return 1000;
    if (p->state != 4) return 10;
    if (p->pending.size || p->out_count || p->audio_out.size || p->screen_out_count || p->kcp->ackcount ||
        p->kcp->nsnd_buf || p->kcp->nsnd_que) return 10;
    remaining = now - p->last_heartbeat;
    return remaining >= 1000 ? 0 : (unsigned)(1000 - remaining);
}
int murmur_ice_pending_message(const murmur_ice *p) {
    return p && p->kcp && ikcp_peeksize(p->kcp) >= 0;
}
