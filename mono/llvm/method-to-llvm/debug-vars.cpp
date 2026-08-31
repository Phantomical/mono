/**
 * \file
 * \brief Pins arguments and locals to frame slots so a debugger can find them.
 *
 * A MonoDebugVarInfo names one place where a variable lives, for the whole
 * method. An optimized body cannot answer that question honestly. A value
 * moves between registers, and it is dead over the stretches where nothing
 * needs it. So this file makes the answer true instead. It keeps each
 * argument and local in the entry-block alloca it starts in, and it hands
 * that slot to the debugger.
 *
 * One stackmap intrinsic call does both halves of the job. The intrinsic
 * uses the alloca directly. It is neither a load nor a store, so mem2reg and
 * SROA leave the slot alone. Every ldloc or stloc on that variable stays a
 * real memory access. LLVM also resolves the intrinsic's operand against the
 * laid-out frame during codegen. The `.llvm_stackmaps` record it writes says
 * which register each slot sits at, and at what displacement. That is
 * exactly what a MONO_DEBUG_VAR_ADDRESS_MODE_REGOFFSET variable needs.
 * jinfo.cpp reads that record and publishes it.
 *
 * This costs code quality, so it happens only when something will read the
 * result. That is true when a debugger is attached, or when a profiler
 * asked for call contexts.
 */

#include "method-to-llvm.hpp"
#include "sidetables.hpp"

#include "mini-runtime.h"

namespace mono {

bool
MethodLLVMEmitter::debug_var_slots_wanted () const
{
	/*
	 * A filter body has no allocas of its own. It reaches the parent frame's
	 * slots through llvm.localrecover. A wrapper with a native signature
	 * spills its arguments in the marshalled layout instead. Neither shape
	 * matches the frame the debugger's picture of the method describes.
	 */
	if (filter_mode || native_signature ())
		return false;

	/*
	 * mdb_optimizations turns on once the debugger agent starts. The profiler
	 * flags are the other reader: a call-context callback asks for a frame's
	 * locals through the same MonoDebugMethodJitInfo.
	 */
	return mini_get_debug_options ()->mdb_optimizations
	       || (prof_flags & (MONO_PROFILER_CALL_INSTRUMENTATION_ENTER_CONTEXT
	                         | MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE_CONTEXT))
	                  != 0;
}

/// Emits one stackmap marker naming every pinned argument and local slot in
/// this frame, and returns whether it emitted one.
///
/// Arguments come first, then locals, in the order jinfo.cpp joins each slot
/// back against the method's signature and header.
bool
MethodLLVMEmitter::emit_debug_var_marker (MonoIrBuilder &builder)
{
	if (!debug_var_slots_wanted () || (args.empty () && locals.empty ()))
		return false;

	std::vector<llvm::Value *> vars;

	for (const Entry &arg : args)
		vars.push_back (arg.alloca);
	for (const Entry &local : locals)
		vars.push_back (local.alloca);

	emit_stackmap_marker (builder, vars_stackmap_id, vars);
	return true;
}

} // namespace mono
