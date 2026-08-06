/**
 * \file
 */

#ifndef __MONO_MINI_LLVM_H__
#define __MONO_MINI_LLVM_H__

#include "mini.h"
#include "aot-runtime.h"

/*
 * The personality routine JIT'd code names in its landing pads. Defined in
 * mini-runtime.c.
 */
MONO_API void mono_personality (void);

#endif
