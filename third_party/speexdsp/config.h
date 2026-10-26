/* Build settings for the SpeexDSP files Murmur uses (echo canceller, noise suppression, automatic gain).
   Floating point, KISS FFT, stack allocation through the C runtime. */
#ifndef MURMUR_SPEEXDSP_CONFIG_H
#define MURMUR_SPEEXDSP_CONFIG_H
#include <malloc.h>
#define FLOATING_POINT 1
#define USE_KISS_FFT 1
#define USE_ALLOCA 1
#define EXPORT
#ifndef alloca
#define alloca _alloca
#endif
#endif
