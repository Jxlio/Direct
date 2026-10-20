#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#endif
#include "murmur/peer.h"
#include "murmur/frame.h"
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/timing.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <psa/crypto.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define WIRE_SIZE (MURMUR_CHAT_LIMIT + MURMUR_HEADER_SIZE + 1)
struct murmur_peer {
    mbedtls_net_context listener, socket;
    mbedtls_ssl_context tls;
    mbedtls_ssl_config config;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_ssl_cookie_ctx cookie;
    mbedtls_timing_delay_context timer;
    murmur_peer_state state;
    int server, accepted, connecting, error, datagram;
#ifdef _WIN32
    int winsock_started;
#endif
    time_t deadline;
    uint8_t tx[WIRE_SIZE], rx[WIRE_SIZE];
    size_t tx_size, tx_offset, rx_size;
};
static const int suites[] = { MBEDTLS_TLS_ECDHE_PSK_WITH_CHACHA20_POLY1305_SHA256, 0 };
static const unsigned char identity[] = "murmur-session-v1";
int murmur_media_key(const uint8_t session[32], uint8_t media[32]) {
    static const unsigned char info[] = "murmur/dtls/voice/v1";
    if (!session || !media) return -1;
    return mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), NULL, 0,
        session, 32, info, sizeof(info) - 1, media, 32);
}
static int valid_address(const char *host) {
    int part;
    const char *cursor = host;
    if (!host || strlen(host) > 15) return 0;
    for (part = 0; part < 4; ++part) {
        unsigned value = 0, count = 0;
        const char *start = cursor;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10 + (unsigned)(*cursor++ - '0');
            if (++count > 3 || value > 255) return 0;
        }
        if (!count || (count > 1 && *start == '0')) return 0;
        if (part < 3) { if (*cursor++ != '.') return 0; }
        else if (*cursor) return 0;
    }
    return 1;
}
static int valid_port(const char *port) {
    unsigned long value;
    size_t i;
    if (!port || !*port || strlen(port) > 5) return 0;
    for (i = 0; port[i]; ++i) if (port[i] < '0' || port[i] > '9') return 0;
    value = strtoul(port, NULL, 10);
    return value > 0 && value <= 65535;
}
int murmur_random_key(uint8_t key[MURMUR_KEY_SIZE]) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    int result;
    if (!key) return -1;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&random);
    result = mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy,
                                  (const unsigned char *)"murmur-invitation", 17);
    if (!result) result = mbedtls_ctr_drbg_random(&random, key, MURMUR_KEY_SIZE);
    mbedtls_ctr_drbg_free(&random);
    mbedtls_entropy_free(&entropy);
    return result;
}
int murmur_invite_format(char *out, size_t capacity, const char *host, const char *port,
                         const uint8_t key[MURMUR_KEY_SIZE]) {
    char hex[65];
    static const char digits[] = "0123456789abcdef";
    size_t i;
    int n;
    if (!out || !key || !valid_address(host) || !valid_port(port)) return -1;
    for (i = 0; i < 32; ++i) { hex[i * 2] = digits[key[i] >> 4]; hex[i * 2 + 1] = digits[key[i] & 15]; }
    hex[64] = 0;
    n = snprintf(out, capacity, "murmur1|%s|%s|%s", host, port, hex);
    mbedtls_platform_zeroize(hex, sizeof(hex));
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}
int murmur_invite_parse(const char *text, char host[16], char port[6], uint8_t key[32]) {
    const char *a, *b;
    size_t i, host_size, port_size;
    uint8_t temp[32];
    if (!text || !host || !port || !key || strlen(text) > 96 || strncmp(text, "murmur1|", 8)) return -1;
    a = strchr(text + 8, '|');
    if (!a || !(b = strchr(a + 1, '|')) || strlen(b + 1) != 64) return -1;
    host_size = (size_t)(a - text - 8);
    port_size = (size_t)(b - a - 1);
    if (!host_size || host_size > 15 || !port_size || port_size > 5) return -1;
    memcpy(host, text + 8, host_size); host[host_size] = 0;
    memcpy(port, a + 1, port_size); port[port_size] = 0;
    if (!valid_address(host) || !valid_port(port)) return -1;
    for (i = 0; i < 64; ++i) {
        char ch = b[1 + i];
        unsigned value;
        if (ch >= '0' && ch <= '9') value = (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') value = (unsigned)(ch - 'a') + 10;
        else { mbedtls_platform_zeroize(temp, sizeof(temp)); return -1; }
        if (!(i & 1)) temp[i / 2] = (uint8_t)(value << 4);
        else temp[i / 2] |= (uint8_t)value;
    }
    memcpy(key, temp, 32);
    mbedtls_platform_zeroize(temp, sizeof(temp));
    return 0;
}
void murmur_peer_free(murmur_peer *p) {
    if (!p) return;
    mbedtls_net_free(&p->socket);
    mbedtls_net_free(&p->listener);
    mbedtls_ssl_free(&p->tls);
    mbedtls_ssl_config_free(&p->config);
    mbedtls_ssl_cookie_free(&p->cookie);
    mbedtls_ctr_drbg_free(&p->random);
    mbedtls_entropy_free(&p->entropy);
#ifdef _WIN32
    if (p->winsock_started) WSACleanup();
#endif
    mbedtls_platform_zeroize(p, sizeof(*p));
    free(p);
}
static murmur_peer *open_peer(int server, const char *host, const char *port, const uint8_t key[32], int datagram) {
    murmur_peer *p;
    int r;
    if (!key || !valid_port(port) || (!server && !valid_address(host))) return NULL;
    p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    mbedtls_net_init(&p->listener); mbedtls_net_init(&p->socket);
    mbedtls_ssl_init(&p->tls); mbedtls_ssl_config_init(&p->config);
    mbedtls_entropy_init(&p->entropy); mbedtls_ctr_drbg_init(&p->random);
    mbedtls_ssl_cookie_init(&p->cookie);
    p->server = server;
    p->datagram = datagram;
    if (psa_crypto_init() != PSA_SUCCESS) goto fail;
    r = mbedtls_ctr_drbg_seed(&p->random, mbedtls_entropy_func, &p->entropy,
                              (const unsigned char *)"murmur-tls", 10);
    if (r) goto fail;
    r = mbedtls_ssl_config_defaults(&p->config, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                    datagram ? MBEDTLS_SSL_TRANSPORT_DATAGRAM : MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r) goto fail;
    mbedtls_ssl_conf_min_tls_version(&p->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&p->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_ciphersuites(&p->config, suites);
    mbedtls_ssl_conf_rng(&p->config, mbedtls_ctr_drbg_random, &p->random);
    if (datagram) {
        mbedtls_ssl_conf_handshake_timeout(&p->config, 250, 4000);
        if (server) {
            if (mbedtls_ssl_cookie_setup(&p->cookie, mbedtls_ctr_drbg_random, &p->random)) goto fail;
            mbedtls_ssl_conf_dtls_cookies(&p->config, mbedtls_ssl_cookie_write,
                mbedtls_ssl_cookie_check, &p->cookie);
        }
    }
    /* Authentication is PSK-only. No certificates or permissive verify callback. */
    r = mbedtls_ssl_conf_psk(&p->config, key, 32, identity, sizeof(identity) - 1);
    if (r || mbedtls_ssl_setup(&p->tls, &p->config)) goto fail;
    if (datagram) {
        mbedtls_ssl_set_mtu(&p->tls, 1200);
        mbedtls_ssl_set_timer_cb(&p->tls, &p->timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay);
    }
    if (server) {
        if (mbedtls_net_bind(&p->listener, "0.0.0.0", port, datagram ? MBEDTLS_NET_PROTO_UDP : MBEDTLS_NET_PROTO_TCP) ||
            mbedtls_net_set_nonblock(&p->listener)) goto fail;
    } else {
        struct sockaddr_in address;
#ifdef _WIN32
        WSADATA data;
        SOCKET socket_handle;
        if (WSAStartup(MAKEWORD(2, 2), &data)) goto fail;
        p->winsock_started = 1;
        socket_handle = socket(AF_INET, datagram ? SOCK_DGRAM : SOCK_STREAM, datagram ? IPPROTO_UDP : IPPROTO_TCP);
        if (socket_handle == INVALID_SOCKET) goto fail;
        p->socket.fd = (int)socket_handle;
#else
        p->socket.fd = socket(AF_INET, datagram ? SOCK_DGRAM : SOCK_STREAM, 0);
        if (p->socket.fd < 0) goto fail;
#endif
        if (mbedtls_net_set_nonblock(&p->socket)) goto fail;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons((unsigned short)strtoul(port, NULL, 10));
        if (inet_pton(AF_INET, host, &address.sin_addr) != 1) goto fail;
        r = connect(p->socket.fd, (struct sockaddr *)&address, sizeof(address));
        if (r) {
#ifdef _WIN32
            if (WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINPROGRESS) goto fail;
#else
            if (errno != EINPROGRESS) goto fail;
#endif
            p->connecting = 1;
        }
        p->accepted = 1;
    }
    mbedtls_ssl_set_bio(&p->tls, &p->socket, mbedtls_net_send, mbedtls_net_recv, NULL);
    p->deadline = time(NULL) + 15;
    return p;
fail:
    murmur_peer_free(p);
    return NULL;
}
murmur_peer *murmur_peer_open(int server, const char *host, const char *port, const uint8_t key[32]) {
    return open_peer(server, host, port, key, 0);
}
murmur_peer *murmur_media_open(int server, const char *host, const char *port, const uint8_t key[32]) {
    return open_peer(server, host, port, key, 1);
}
static int valid_frame(const murmur_peer *p, const murmur_frame *frame) {
    if (p->datagram) return frame->kind == MURMUR_AUDIO && frame->payload_size >= 5 && frame->payload_size <= MURMUR_AUDIO_LIMIT;
    if (frame->kind == MURMUR_CONTROL)
        return frame->payload_size == 1 && (frame->payload[0] == 0 || frame->payload[0] == 1 || frame->payload[0] == 3);
    return frame->kind == MURMUR_TEXT && frame->payload_size >= 2 &&
        frame->payload_size <= MURMUR_CHAT_LIMIT + 1 && frame->payload[0] <= 2;
}
static murmur_peer_state failed(murmur_peer *p, int r) {
    p->error = r; p->state = MURMUR_PEER_FAILED;
    return p->state;
}
murmur_peer_state murmur_peer_step(murmur_peer *p, unsigned timeout_ms) {
    int r;
    if (!p) return MURMUR_PEER_FAILED;
    if (p->state == MURMUR_PEER_FAILED || p->state == MURMUR_PEER_CLOSED) return p->state;
    if (p->connecting) {
        int error = 0;
#ifdef _WIN32
        int length = sizeof(error);
#else
        socklen_t length = sizeof(error);
#endif
        if (time(NULL) > p->deadline) return failed(p, MBEDTLS_ERR_SSL_TIMEOUT);
        r = mbedtls_net_poll(&p->socket, MBEDTLS_NET_POLL_WRITE, timeout_ms);
        if (r < 0) return failed(p, r);
        if (!r) return p->state;
        if (getsockopt(p->socket.fd, SOL_SOCKET, SO_ERROR, (char *)&error, &length) || error)
            return failed(p, MBEDTLS_ERR_NET_CONNECT_FAILED);
        p->connecting = 0;
    }
    if (!p->accepted) {
        unsigned char client_id[16];
        size_t client_id_size = 0;
        r = mbedtls_net_poll(&p->listener, MBEDTLS_NET_POLL_READ, timeout_ms);
        if (r < 0) return failed(p, r);
        if (!r) return p->state;
        r = mbedtls_net_accept(&p->listener, &p->socket, client_id, sizeof(client_id), &client_id_size);
        if (r == MBEDTLS_ERR_SSL_WANT_READ) return p->state;
        if (r || mbedtls_net_set_nonblock(&p->socket)) return failed(p, r ? r : -1);
        p->accepted = 1; p->deadline = time(NULL) + 15;
        if (p->datagram && mbedtls_ssl_set_client_transport_id(&p->tls, client_id, client_id_size)) return failed(p, -1);
        if (!p->datagram) mbedtls_net_free(&p->listener);
    }
    if (p->state == MURMUR_PEER_WAITING) {
        if (time(NULL) > p->deadline) return failed(p, MBEDTLS_ERR_SSL_TIMEOUT);
        r = mbedtls_ssl_handshake(&p->tls);
        if (!r) { p->state = MURMUR_PEER_CONNECTED; mbedtls_net_free(&p->listener); }
        else if (p->datagram && p->server && r == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
            mbedtls_net_free(&p->socket);
            if (mbedtls_ssl_session_reset(&p->tls)) return failed(p, -1);
            p->accepted = 0;
            return p->state;
        }
        else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return failed(p, r);
        else {
            r = mbedtls_net_poll(&p->socket, r == MBEDTLS_ERR_SSL_WANT_READ ?
                MBEDTLS_NET_POLL_READ : MBEDTLS_NET_POLL_WRITE, timeout_ms);
            if (r < 0) return failed(p, r);
            return p->state;
        }
    }
    if (p->tx_size) {
        r = mbedtls_ssl_write(&p->tls, p->tx + p->tx_offset, p->tx_size - p->tx_offset);
        if (r > 0) {
            p->tx_offset += (size_t)r;
            if (p->tx_offset == p->tx_size) {
                mbedtls_platform_zeroize(p->tx, p->tx_size);
                p->tx_size = p->tx_offset = 0;
            }
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return failed(p, r);
    }
    if (p->rx_size == sizeof(p->rx) || (p->datagram && p->rx_size)) return p->state;
    r = mbedtls_net_poll(&p->socket, MBEDTLS_NET_POLL_READ, timeout_ms);
    if (r < 0) return failed(p, r);
    if (!r && !mbedtls_ssl_check_pending(&p->tls)) return p->state;
    r = mbedtls_ssl_read(&p->tls, p->rx + p->rx_size, sizeof(p->rx) - p->rx_size);
    if (r > 0) {
        murmur_frame frame;
        size_t consumed;
        murmur_result decoded;
        p->rx_size += (size_t)r;
        if (p->rx_size >= MURMUR_HEADER_SIZE) {
            uint32_t declared = ((uint32_t)p->rx[8] << 24) | ((uint32_t)p->rx[9] << 16) |
                ((uint32_t)p->rx[10] << 8) | (uint32_t)p->rx[11];
            if (declared > MURMUR_CHAT_LIMIT + 1) return failed(p, -2);
        }
        decoded = murmur_frame_decode(p->rx, p->rx_size, &frame, &consumed);
        if (decoded == MURMUR_INVALID || (decoded == MURMUR_OK && !valid_frame(p, &frame)) ||
            (p->datagram && (decoded != MURMUR_OK || consumed != p->rx_size)))
            return failed(p, -2);
    } else if (!r || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) p->state = MURMUR_PEER_CLOSED;
    else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return failed(p, r);
    return p->state;
}
int murmur_peer_send(murmur_peer *p, unsigned room, const uint8_t *utf8, size_t size) {
    uint8_t payload[MURMUR_CHAT_LIMIT + 1];
    if (!p || p->datagram || p->state != MURMUR_PEER_CONNECTED || p->tx_size || !utf8 ||
        !size || size > MURMUR_CHAT_LIMIT || room > 2) return -1;
    payload[0] = (uint8_t)room;
    memcpy(payload + 1, utf8, size);
    if (murmur_frame_encode(MURMUR_TEXT, payload, size + 1, p->tx, sizeof(p->tx), &p->tx_size) != MURMUR_OK) return -1;
    mbedtls_platform_zeroize(payload, sizeof(payload));
    return 0;
}
int murmur_peer_send_voice_state(murmur_peer *p, unsigned flags) {
    uint8_t payload = (uint8_t)flags;
    if (!p || p->datagram || p->state != MURMUR_PEER_CONNECTED || p->tx_size ||
        (flags != 0 && flags != 1 && flags != 3)) return -1;
    return murmur_frame_encode(MURMUR_CONTROL, &payload, 1, p->tx, sizeof(p->tx), &p->tx_size) == MURMUR_OK ? 0 : -1;
}
int murmur_peer_send_audio(murmur_peer *p, const uint8_t *packet, size_t size) {
    if (!p || !p->datagram || p->state != MURMUR_PEER_CONNECTED || p->tx_size || !packet ||
        size < 5 || size > MURMUR_AUDIO_LIMIT) return -1;
    return murmur_frame_encode(MURMUR_AUDIO, packet, size, p->tx, sizeof(p->tx), &p->tx_size) == MURMUR_OK ? 0 : -1;
}
int murmur_peer_receive_packet(murmur_peer *p, murmur_kind *kind, unsigned *room, uint8_t *utf8, size_t capacity, size_t *size) {
    murmur_frame frame;
    size_t consumed;
    murmur_result r;
    if (size) *size = 0;
    size_t offset;
    if (!p || !kind || !room || !utf8 || !size) return -1;
    r = murmur_frame_decode(p->rx, p->rx_size, &frame, &consumed);
    if (r == MURMUR_INCOMPLETE) return 0;
    if (r != MURMUR_OK || !valid_frame(p, &frame)) { failed(p, -2); return -1; }
    offset = frame.kind == MURMUR_TEXT ? 1u : 0u;
    if (capacity < frame.payload_size - offset) return -1;
    *kind = frame.kind;
    *room = offset ? frame.payload[0] : 0;
    *size = frame.payload_size - offset;
    memcpy(utf8, frame.payload + offset, *size);
    memmove(p->rx, p->rx + consumed, p->rx_size - consumed);
    p->rx_size -= consumed;
    mbedtls_platform_zeroize(p->rx + p->rx_size, consumed);
    return 1;
}
int murmur_peer_receive(murmur_peer *p, unsigned *room, uint8_t *utf8, size_t capacity, size_t *size) {
    murmur_kind kind;
    int result = murmur_peer_receive_packet(p, &kind, room, utf8, capacity, size);
    return result == 1 && kind != MURMUR_TEXT ? -1 : result;
}
int murmur_peer_error(const murmur_peer *p) { return p ? p->error : -1; }
