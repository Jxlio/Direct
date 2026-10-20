# Third-party components

The release-note JSON tokenizer is jsmn, revision
`25647e692c7906b96ffd2b05ca54c097948e879c` from
https://github.com/zserge/jsmn, under the MIT license. The original header and
license are preserved in include/third_party/ and copied into binary packages.
The update verifier uses the already linked Mbed TLS implementation of standard
ECDSA P-256 and SHA-256. No private publishing key is embedded in the application.

Direct 0.6 embeds third-party notices inside the executable as well, accessible
through Versions > Licences integrees, so an executable-only update carries its
current notices. Source SVG rendering tools (Pillow and resvg-js) and the Python
cryptography signing tool are development tools; they are not shipped or linked
into the native client.

Mbed TLS 3.6.7, commit `068ff080b369adfac81509f9b57b2afabaf82dc5`.
Source: https://github.com/Mbed-TLS/mbedtls

Murmur uses Mbed TLS under the Apache-2.0 license option. The upstream source
contains LICENSE, license texts, copyright and third-party notices. Preserve
these notices with source distributions and include the corresponding license
texts and notices when packaging a binary. The build places the upstream license,
Everest and p256-m notices, and this file in a licenses/ folder next to murmur.
Keep that folder with the executable when sharing a build. An installer is not
yet implemented.

The source checkout is locally in vendor/mbedtls (ignored by Git); CMake fetches
the pinned commit and its framework submodule if the local checkout is absent.
No runtime dependency download or network bootstrap service is used.

Opus 1.6.1, commit `22244de5a79bd1d6d623c32e72bf1954b56235be`.
Source: https://github.com/xiph/opus
The upstream COPYING file is copied to licenses/Opus-LICENSE.txt. DRED, OSCE,
deep PLC and upstream programs/tests are disabled in this build. Standard Opus
packet loss concealment and runtime CPU dispatch remain enabled.

miniaudio 0.11.25, commit `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d`.
Source: https://github.com/mackron/miniaudio
Murmur chooses the MIT No Attribution license alternative. The full upstream
license is copied to licenses/miniaudio-LICENSE.txt. Only device I/O, conversion
and PCM ring buffers are used; the engine, resource manager, file codecs and
generators are disabled. Windows uses WASAPI; other platforms are not validated.

libjuice 1.7.4, commit `b89c792e3612faf2f12cf35bcc56857313a06be3`, MPL-2.0.
Source: https://github.com/paullouisageneau/libjuice/tree/b89c792e3612faf2f12cf35bcc56857313a06be3
Corresponding unmodified source is available at that exact public revision.
Preserve its source availability notice and MPL license with binary distributions.
Murmur uses the static library with STUN/TURN enabled and upstream tests disabled;
consent freshness remains enabled. No upstream files have been modified.
The license is included as licenses/libjuice-LICENSE.txt.

KCP, commit `b1a7a2101dcbb96017681a500d6b82bbe5a88766`, MIT license.
Source: https://github.com/skywind3000/kcp/tree/b1a7a2101dcbb96017681a500d6b82bbe5a88766
Only ikcp.c is compiled; no scripts, examples or executables are used. It operates
strictly inside authenticated DTLS, never on unauthenticated Internet input.
The license is included as licenses/KCP-LICENSE.txt. No upstream files modified.
# Lucide icons

The selected SVG icons are from [Lucide](https://lucide.dev/), revision
`c29c276f16590cf017126671535ad8c792be64d8` of
[lucide-icons/lucide](https://github.com/lucide-icons/lucide).
The upstream ISC license, including the MIT notice for Feather-derived icons,
is preserved in `src/windows/assets/lucide/LICENSE` and distributed as
`licenses/Lucide-LICENSE.txt`. The original SVG sources are retained; build-time
renderings in several sizes and colors are embedded as Windows icon resources.
No icon font, JavaScript runtime or SVG renderer is shipped with the application.
