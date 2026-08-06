/**
 * \file
 */

#ifndef __MONO_MINI_ARCH_H__
#define __MONO_MINI_ARCH_H__

#ifdef TARGET_AMD64
#include "mini-amd64.h"
#else
#error amd64 is the only architecture this runtime targets
#endif

#ifdef TARGET_WIN32
#include "mini-windows.h"
#endif

#if (MONO_ARCH_FRAME_ALIGNMENT == 4)
#define MONO_ARCH_LOCALLOC_ALIGNMENT 8
#else
#define MONO_ARCH_LOCALLOC_ALIGNMENT MONO_ARCH_FRAME_ALIGNMENT
#endif

#endif /* __MONO_MINI_ARCH_H__ */  
