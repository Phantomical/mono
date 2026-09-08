/**
 * \file
 * \brief The C-callable half of jit-dump.hpp, for the classic tier-0 compiler.
 *
 * mono/mini/tier0 is C, and jit-dump.hpp is C++ throughout, so this is the one
 * boundary tier0's own disassembly dump crosses to reach it.
 */

#ifndef MONO_MINI_JIT_DUMP_TIER0_H
#define MONO_MINI_JIT_DUMP_TIER0_H

#include <stdio.h>

#include <mono/utils/mono-publib.h>

typedef struct _MonoMethod MonoMethod;

MONO_BEGIN_DECLS

/// An open `tier0-asm` dump. Opaque to C, and never defined: jit-dump.cpp
/// casts it straight to and from the C++ object behind it.
typedef struct MonoTier0AsmDump MonoTier0AsmDump;

/// Opens a `tier0-asm` dump for this method. Returns NULL, and the caller
/// disassembles nothing, when MONO_JIT_DUMP does not name the point,
/// MONO_JIT_DUMP_FILTER excludes the method, or the destination fails to
/// open.
MonoTier0AsmDump *mono_tier0_asm_dump_open (MonoMethod *method);

/// The stream a dump opened with mono_tier0_asm_dump_open () writes to.
FILE *mono_tier0_asm_dump_stream (MonoTier0AsmDump *dump);

/// Closes a dump opened with mono_tier0_asm_dump_open ().
void mono_tier0_asm_dump_close (MonoTier0AsmDump *dump);

MONO_END_DECLS

#endif
