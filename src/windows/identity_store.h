#ifndef MURMUR_IDENTITY_STORE_H
#define MURMUR_IDENTITY_STORE_H
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include "murmur/identity.h"

/* Loads the user's identity from a DPAPI-protected file, creating it on first
 * use. The secret never leaves this module; callers sign through it.
 * A damaged file is kept untouched and reported as an error. */
int identity_store_open(const WCHAR *path);
/* Default location: %LOCALAPPDATA%\Murmur\identity.dat */
int identity_store_open_default(void);
const uint8_t *identity_store_public(void);
const char *identity_store_fingerprint(void);
int identity_store_sign(const uint8_t *message, size_t size, uint8_t signature[MURMUR_ID_SIGNATURE]);
/* The encryption public key (see murmur/identity.h) and ECDH with a peer's encryption key. */
const uint8_t *identity_store_enc_public(void);
int identity_store_ecdh(const uint8_t peer[MURMUR_ID_PUBLIC], uint8_t shared[32]);
void identity_store_close(void);
#endif
