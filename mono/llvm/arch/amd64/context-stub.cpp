/**
 * \file
 * \brief The stub that hands a shared body the context of one instantiation.
 *
 * A body shared between reference instantiations that has no receiver to read
 * its context out of is entered with the context in a register instead.
 *
 * Every caller reaches a method through its own thunk, so the instantiation's
 * thunk is pointed at a context stub rather than at the shared body. The stub
 * writes the context that instantiation was published with, then jumps into
 * the shared method's own thunk. That covers a compiled caller, an
 * interpreted one, reflection and a delegate alike, because all four go
 * through the thunk.
 */

#include "arch/arch.hpp"

#include <llvm/Support/ErrorHandling.h>

#include <cstdint>
#include <cstring>

namespace mono::arch {

void
write_context_stub (char *at, void *context, void *target)
{
	/*
	 * %r10 is MONO_ARCH_RGCTX_REG, and it is what LLVM pins a `nest` parameter
	 * to. That is how the shared body declares the context it is entered
	 * with. A thunk in front of this one leaves the register alone.
	 */
	static const uint8_t movabs_r10[] = { 0x49, 0xBA };
	int64_t displacement = (int64_t) ((char *) target - (at + context_stub_size));

	if (displacement < INT32_MIN || displacement > INT32_MAX)
		llvm::report_fatal_error ("a context stub cannot reach the body it enters",
		                          false);

	uint64_t key = (uint64_t) (uintptr_t) context;
	int32_t rel = (int32_t) displacement;

	memcpy (at, movabs_r10, sizeof (movabs_r10));
	memcpy (at + 2, &key, sizeof (key));
	at[10] = (char) 0xE9;
	memcpy (at + 11, &rel, sizeof (rel));
}

} // namespace mono::arch
