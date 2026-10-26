#include "service_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)

static int origin(const char *text, int *secure, unsigned short *port) {
    WCHAR host[256];
    return service_link_parse_origin(text, secure, host, port);
}
static int wait_for(int kind, service_event *event, DWORD milliseconds) {
    ULONGLONG deadline = GetTickCount64() + milliseconds;
    while (GetTickCount64() < deadline) {
        if (service_link_next(event)) {
            if (event->kind == kind) return 1;
            free(event->text); event->text = NULL;
        } else Sleep(10);
    }
    return 0;
}
int main(void) {
    int secure = -1;
    unsigned short port = 0;
    WCHAR host[256];
    HWND window = CreateWindowW(L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    const char *url = getenv("MURMUR_SERVICE_URL");
    CHECK(window);
    /* Only HTTPS servers, or HTTP on this computer, are reachable. */
    CHECK(!origin("https://example.workers.dev", &secure, &port) && secure == 1 && port == 443);
    CHECK(!origin("https://example.workers.dev/", &secure, &port) && port == 443);
    CHECK(!origin("https://example.com:8443", &secure, &port) && port == 8443);
    CHECK(!origin("http://127.0.0.1:8788", &secure, &port) && secure == 0 && port == 8788);
    CHECK(!origin("http://localhost", &secure, &port) && port == 80);
    CHECK(origin("http://example.com", &secure, &port));
    CHECK(origin("http://192.168.1.5:8788", &secure, &port));
    CHECK(origin("ws://127.0.0.1:8788", &secure, &port));
    CHECK(origin("https://", &secure, &port));
    CHECK(origin("https://exa mple.com", &secure, &port));
    CHECK(origin("https://example.com:0", &secure, &port));
    CHECK(origin("https://example.com:99999", &secure, &port));
    CHECK(origin("https://example.com:", &secure, &port));
    CHECK(origin("https://example.com/path", &secure, &port));
    CHECK(origin("https://user@example.com", &secure, &port));
    CHECK(origin("https://-example.com", &secure, &port));
    CHECK(origin(NULL, &secure, &port));
    CHECK(service_link_parse_origin("https://example.com", &secure, host, &port) == 0 && !wcscmp(host, L"example.com"));
    /* Nothing opens before init, and bad slots or guild ids are refused. */
    CHECK(service_link_open(0, "https://example.com", "0123456789abcdef0123456789abcdef") < 0);
    service_link_init(window);
    CHECK(service_link_open(-1, "https://example.com", "0123456789abcdef0123456789abcdef") < 0);
    CHECK(service_link_open(SERVICE_SLOTS, "https://example.com", "0123456789abcdef0123456789abcdef") < 0);
    CHECK(service_link_open(0, "https://example.com", "short") < 0);
    CHECK(service_link_open(0, "https://example.com", "0123456789ABCDEF0123456789ABCDEF") < 0);
    CHECK(service_link_send(0, "x", 1) < 0);
    CHECK(service_link_send(0, "x", SERVICE_FRAME_MAX + 1) < 0);
    if (url) {
        /* Live check against a running service (npm run dev in server/). */
        service_event event;
        const char guild[] = "00112233445566778899aabbccddeeff";
        const char ping[] = "{\"t\":\"ping\",\"ref\":\"live\"}";
        int got_hello = 0, got_pong = 0;
        CHECK(!service_link_open(1, url, guild));
        CHECK(wait_for(SERVICE_EVENT_OPEN, &event, 8000));
        CHECK(!service_link_send(1, ping, sizeof(ping) - 1));
        while (!(got_hello && got_pong) && wait_for(SERVICE_EVENT_MESSAGE, &event, 4000)) {
            if (strstr(event.text, "\"t\":\"hello\"")) got_hello = 1;
            if (strstr(event.text, "\"t\":\"pong\"")) got_pong = 1;
            free(event.text);
        }
        CHECK(got_hello && got_pong);
        service_link_close(1);
        CHECK(wait_for(SERVICE_EVENT_CLOSE, &event, 8000));
        CHECK(!service_link_open(1, url, guild));   /* the slot can be reused after it closed */
        CHECK(wait_for(SERVICE_EVENT_OPEN, &event, 8000));
        puts("Live relay: open, text frames both ways, close and reuse passed.");
    }
    service_link_shutdown();
    puts("Server address validation and relay limits passed.");
    return 0;
}
