#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "murmur/peer.h"
#include <juice/juice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#define pause_server() Sleep(100)
#else
#include <time.h>
static void pause_server(void) { struct timespec delay = {0, 100000000}; nanosleep(&delay, NULL); }
#endif
static volatile sig_atomic_t stopped;
static void stop_server(int signal_number) { (void)signal_number; stopped = 1; }
static int setting(char *out, size_t capacity, const char *name, const char *fallback) {
    const char *value = getenv(name);
    size_t size;
    if (!value || !*value) value = fallback;
    size = strlen(value);
    if (size >= capacity) return -1;
    memcpy(out, value, size + 1);
    return 0;
}
static int port_setting(const char *name, unsigned fallback, uint16_t *port) {
    const char *value = getenv(name);
    char *end;
    unsigned long result;
    if (!value || !*value) { *port = (uint16_t)fallback; return 0; }
    if (strlen(value) > 5) return -1;
    result = strtoul(value, &end, 10);
    if (*end || result == 0 || result > 65535 || *value < '0' || *value > '9') return -1;
    *port = (uint16_t)result; return 0;
}
int main(int argc, char **argv) {
    char bind[64], external[64], username[128], password[128];
    juice_server_config_t config = {0};
    juice_server_credentials_t credentials = {0};
    juice_server_t *server;
    int check = argc == 2 && !strcmp(argv[1], "--check");
    if (argc == 2 && !strcmp(argv[1], "--new-secret")) {
        uint8_t key[32]; size_t i;
        if (murmur_random_key(key)) return 1;
        for (i = 0; i < sizeof(key); ++i) printf("%02x", (unsigned)key[i]);
        putchar('\n');
        { volatile uint8_t *p = key; for (i = 0; i < sizeof(key); ++i) p[i] = 0; }
        return 0;
    }
    if (argc != 1 && !check) {
        puts("murmur-relay [--check | --new-secret]\nConfiguration: voir RELAY.md. Aucun service n'est installe automatiquement.");
        return 1;
    }
    if (setting(bind, sizeof(bind), "MURMUR_RELAY_BIND", "127.0.0.1") ||
        setting(external, sizeof(external), "MURMUR_RELAY_EXTERNAL", "") ||
        setting(username, sizeof(username), "MURMUR_RELAY_USERNAME", "") ||
        setting(password, sizeof(password), "MURMUR_RELAY_PASSWORD", "") ||
        port_setting("MURMUR_RELAY_PORT", 3478, &config.port) ||
        port_setting("MURMUR_RELAY_MIN_PORT", 50000, &config.relay_port_range_begin) ||
        port_setting("MURMUR_RELAY_MAX_PORT", 50099, &config.relay_port_range_end) ||
        !*username || strlen(password) < 32 || config.relay_port_range_begin > config.relay_port_range_end ||
        ((!strcmp(bind, "0.0.0.0") || !strcmp(bind, "::")) && !*external)) {
        fputs("Configuration invalide : identifiant requis, secret >=32 caracteres, ports valides,\n"
            "et adresse externe requise avec une ecoute sur toutes les interfaces. Voir RELAY.md.\n", stderr);
        return 1;
    }
    credentials.username = username; credentials.password = password; credentials.allocations_quota = 8;
    config.credentials = &credentials; config.credentials_count = 1;
    config.bind_address = bind; config.external_address = *external ? external : NULL;
    config.realm = "Murmur private relay"; config.max_allocations = 8; config.max_peers = 16;
    if (check) { puts("Configuration syntaxically valid; no sockets opened."); memset(password, 0, sizeof(password)); return 0; }
    juice_set_log_level(JUICE_LOG_LEVEL_NONE);
    server = juice_server_create(&config);
    if (!server) { fputs("Unable to start relay. Check bind address and UDP ports.\n", stderr); return 1; }
    { volatile char *p = password; size_t i; for (i = 0; i < sizeof(password); ++i) p[i] = 0; }
    signal(SIGINT, stop_server); signal(SIGTERM, stop_server);
    printf("Murmur STUN/TURN relay listening on %s:%u/UDP. Ctrl+C to stop.\n", bind, juice_server_get_port(server));
    while (!stopped) pause_server();
    juice_server_destroy(server);
    return 0;
}
