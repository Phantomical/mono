/**
 * \file
 */

#include "compile.h"

/* Dummy versions of some arch specific functions to avoid ifdefs at call sites */

#ifndef MONO_ARCH_HAVE_DECOMPOSE_OPTS
void
mono_arch_decompose_opts (MonoCompile *cfg, MonoInst *ins)
{
}
#endif

#ifndef MONO_ARCH_HAVE_OPCODE_NEEDS_EMULATION
gboolean
mono_arch_opcode_needs_emulation (MonoCompile *cfg, int opcode)
{
	return TRUE;
}
#endif
