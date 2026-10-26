#ifndef DIRECT_MEDIA_H
#define DIRECT_MEDIA_H
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include "link.h"

/* Images stay in a bounded, in-memory cache for this conversation only. */
#define DIRECT_MEDIA_MAX (2u * 1024u * 1024u)
int media_pick_and_send(HWND owner,uint32_t *id,unsigned *kind,wchar_t *label,size_t label_count);
int media_queue_bytes(const uint8_t *bytes,size_t size,uint32_t *id,unsigned *kind);
int media_pump(void);
void media_cancel(void);
int media_receive(const link_message *packet,uint32_t *id,unsigned *kind);
int media_get(uint32_t id,const uint8_t **bytes,size_t *size,const wchar_t **mime);
void media_close(void);
#endif
