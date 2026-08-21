/**
 * \file
 * \brief What the C runtime owes the perf jit dump.
 *
 * A C header because the runtime plants its stubs and trampolines from C.
 * Everything else the dump is written through is C++.
 */

#ifndef MONO_LLVM_DEBUGGING_PERF_PERF_H
#define MONO_LLVM_DEBUGGING_PERF_PERF_H

#include <glib.h>

#include "mono/utils/mono-publib.h"

MONO_BEGIN_DECLS

/**
 * Names [code, code + code_size) in the jit dump, together with the frame
 * description that lets a profile unwind out of it.
 *
 * cfi is the DWARF call frame program mono keeps for the stub
 * (mono_unwind_ops_encode), counted from the target's entry state. Pass NULL and
 * the stub is named but a stack walk stops at it. Does nothing unless a dump is
 * open.
 */
void mono_llvm_perf_dump_stub (const char *name, gpointer code, guint32 code_size,
                               const guint8 *cfi, guint32 cfi_size);

/**
 * How many bytes past its end a code allocation has to keep free while a dump is
 * open. Zero when no dump is open.
 *
 * perf maps an image of its own over each range of code it is told about, longer
 * than the code by the frame description. A second range inside that takes it,
 * and the description is then read out of the wrong image. So code the dump
 * describes has to be spaced out.
 */
guint32 mono_llvm_perf_code_slack (void);

MONO_END_DECLS

#endif /* MONO_LLVM_DEBUGGING_PERF_PERF_H */
