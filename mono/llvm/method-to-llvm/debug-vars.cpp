/**
 * \file
 * \brief Giving every argument and local one frame home, for the debugger.
 *
 * A MonoDebugVarInfo names exactly one place a variable lives, for the whole
 * method. That is not a description an optimized body can be given honestly - a
 * value moves between registers and is dead over the stretches where nothing
 * needs it - so the only way to answer the question truthfully is to make the
 * answer true: keep each argument and local in the entry-block alloca it starts
 * out in, and hand the debugger that slot.
 *
 * The stackmap emitted here does both halves at once. It is a use of the alloca
 * that is neither a load nor a store, so mem2reg and SROA leave the slot alone
 * and every ldloc/stloc stays a real access of it; and its operand is resolved
 * against the laid-out frame during codegen, so the record LLVM writes into
 * `.llvm_stackmaps` says which register each slot is addressed off and at what
 * displacement - which is exactly what a MONO_DEBUG_VAR_ADDRESS_MODE_REGOFFSET
 * variable is. jinfo.cpp reads the record back and publishes it.
 *
 * This costs real code quality, so it only happens when something is going to
 * read it: a debugger is attached, or a profiler asked for call contexts.
 */

#include "method-to-llvm.hpp"
#include "sidetables.hpp"

#include "mini-runtime.h"

#include <llvm/IR/Intrinsics.h>

namespace mono {

/// Whether this method's arguments and locals should be pinned to frame slots.
bool
MethodLLVMEmitter::debug_var_slots_wanted () const
{
	/*
	 * A filter body reaches the parent frame's slots through llvm.localrecover
	 * rather than holding any of its own, and a wrapper with a native signature
	 * spills its arguments in the marshalled layout - neither is a frame the
	 * debugger's picture of the method describes.
	 */
	if (filter_mode || native_signature ())
		return false;

	/*
	 * mdb_optimizations is what the runtime turns on when a debugger attaches;
	 * it is the same switch mini's own variable info hangs off. The profiler
	 * flags are the other reader: a call-context callback asks for a frame's
	 * locals through the same MonoDebugMethodJitInfo.
	 */
	return mini_get_debug_options ()->mdb_optimizations
	       || (prof_flags & (MONO_PROFILER_CALL_INSTRUMENTATION_ENTER_CONTEXT
	                         | MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE_CONTEXT))
	                  != 0;
}

/// Pin this frame's arguments and locals and record where they landed, and say
/// whether anything was pinned.
///
/// One marker for the whole frame, arguments first and then locals, which is the
/// order the reader joins its locations back against the method's signature and
/// header.
bool
MethodLLVMEmitter::emit_debug_var_marker (MonoIrBuilder &builder)
{
	if (!debug_var_slots_wanted () || (args.empty () && locals.empty ()))
		return false;

	std::vector<llvm::Value *> operands = {
		builder.getInt64 (vars_stackmap_id),
		builder.getInt32 (0),
	};

	for (const Entry &arg : args)
		operands.push_back (arg.alloca);
	for (const Entry &local : locals)
		operands.push_back (local.alloca);

	builder.CreateIntrinsic (llvm::Intrinsic::experimental_stackmap, {}, operands);
	return true;
}

} // namespace mono
