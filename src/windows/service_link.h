#ifndef MURMUR_SERVICE_LINK_H
#define MURMUR_SERVICE_LINK_H
#include <windows.h>
#include <stddef.h>

/* WebSocket relay between the page and Murmur servers (protocol v2, see docs/protocole.md).
 * The page speaks the protocol; this module only opens the socket, moves text frames and
 * enforces limits, so the page needs no network permission beyond its own origin. */
#define WM_SERVICE_LINK (WM_APP + 35)
#define SERVICE_SLOTS 4
#define SERVICE_FRAME_MAX 72000   /* file chunks are the large ones */
enum { SERVICE_EVENT_OPEN = 1, SERVICE_EVENT_MESSAGE = 2, SERVICE_EVENT_CLOSE = 3 };
typedef struct { int slot, kind, code; char *text; } service_event;

void service_link_init(HWND owner);
/* origin: https://host[:port], or http://127.0.0.1|localhost[:port] for local development.
 * guild: 32 lowercase hex digits. Returns 0 when the connection attempt has started. */
int service_link_open(int slot, const char *origin, const char *guild);
int service_link_send(int slot, const char *utf8, size_t size);
void service_link_close(int slot);
/* Returns 1 and fills *event (free event->text) when an event is waiting. */
int service_link_next(service_event *event);
void service_link_shutdown(void);
/* Exposed for tests: validates and splits an origin. */
int service_link_parse_origin(const char *origin, int *secure, WCHAR host[256], unsigned short *port);
#endif
