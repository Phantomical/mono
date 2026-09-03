/**
 * \file
 */

#ifndef __MONO_MINI_TIER0_ARCH_H__
#define __MONO_MINI_TIER0_ARCH_H__

#ifdef TARGET_AMD64
#include "arch/amd64/tier0-amd64.h"
#else
#error amd64 is the only architecture this runtime targets
#endif

#endif /* __MONO_MINI_TIER0_ARCH_H__ */
