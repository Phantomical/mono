#include "method-to-llvm.hpp"
#include "mono/metadata/profiler-private.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

namespace mono {

/*
 * A profiler that installs a call-instrumentation filter is asking to be told
 * when a method is entered and when it is left, and it is told by code emitted
 * into the method itself: mono_profiler_raise_method_enter at the top, and
 * mono_profiler_raise_method_leave in front of each ret.
 *
 * The exceptional exit is not one of these. A method left by an exception is
 * left by the unwinder rather than by anything in its body, and
 * mono_handle_exception_internal raises method_exception_leave as it walks each
 * frame off - so what the front end owes is the enter that pairs with it.
 * That is why the enter goes ahead of the class initializer: a type
 * initializer that throws unwinds this frame like any other.
 */

/// Ask the profilers what they want instrumented in this method.
void
MethodLLVMEmitter::resolve_call_instrumentation ()
{
	/*
	 * A native-to-managed wrapper is entered from C on a thread the runtime may
	 * never have seen, and attaching it is the first thing its body does. A
	 * profiler callback ahead of that runs against a thread that does not exist
	 * yet, which is worse than the events it would have produced.
	 */
	if (method->wrapper_type == MONO_WRAPPER_NATIVE_TO_MANAGED)
		return;

	prof_flags = mono_profiler_get_call_instrumentation_flags (method);
}

bool
MethodLLVMEmitter::instrumented (MonoProfilerCallInstrumentationFlags flag) const
{
	/* A filter body is a helper over this frame, not an entry into the method. */
	return !filter_mode && (prof_flags & flag) != 0;
}

/// Call one of the runtime's mono_profiler_raise_* entry points, whose arguments
/// are all pointers.
void
MethodLLVMEmitter::emit_profiler_event (MonoIrBuilder &builder, const char *raise,
                                        void *address, llvm::ArrayRef<llvm::Value *> args)
{
	std::vector<llvm::Type *> params (args.size (), llvm::PointerType::get (context (), 0));
	llvm::CallInst *call = builder.CreateCall (
		llvm::FunctionCallee (
			llvm::FunctionType::get (llvm::Type::getVoidTy (context ()), params,
	                                         false),
			address_symbol (raise, address)),
		args);

	/*
	 * A callback is free to walk the stack, and the frame it wants to see is
	 * this one - so the leave in front of a void ret has to stay a call rather
	 * than become the jump tail-call elimination would make of it.
	 */
	call->setTailCallKind (llvm::CallInst::TCK_NoTail);
}

void
MethodLLVMEmitter::emit_profiler_enter (MonoIrBuilder &builder)
{
	if (!instrumented (MONO_PROFILER_CALL_INSTRUMENTATION_ENTER))
		return;

	emit_profiler_event (
		builder, "mono_profiler_raise_method_enter",
		reinterpret_cast<void *> (mono_profiler_raise_method_enter),
		{ method_symbol (method),
	          llvm::ConstantPointerNull::get (llvm::PointerType::get (context (), 0)) });
}

void
MethodLLVMEmitter::emit_profiler_leave (MonoIrBuilder &builder)
{
	if (!instrumented (MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE))
		return;

	emit_profiler_event (
		builder, "mono_profiler_raise_method_leave",
		reinterpret_cast<void *> (mono_profiler_raise_method_leave),
		{ method_symbol (method),
	          llvm::ConstantPointerNull::get (llvm::PointerType::get (context (), 0)) });
}

/// Report a site that gives this method's frame away to TARGET - an honored
/// tail. call, or a jmp - as the method's exit.
///
/// The report goes in front of the site rather than after it, since control does
/// not come back. A profiler that asked only about ordinary exits gets one here:
/// the frame is gone either way, and a leave it never heard about would leave its
/// idea of the stack one deep forever.
void
MethodLLVMEmitter::emit_profiler_frame_handover (MonoIrBuilder &builder, MonoMethod *target)
{
	if (instrumented (MONO_PROFILER_CALL_INSTRUMENTATION_TAIL_CALL)) {
		llvm::Constant *called =
			target != nullptr
				? method_symbol (target)
				: llvm::ConstantPointerNull::get (
					  llvm::PointerType::get (context (), 0));

		emit_profiler_event (
			builder, "mono_profiler_raise_method_tail_call",
			reinterpret_cast<void *> (mono_profiler_raise_method_tail_call),
			{ method_symbol (method), called });
		return;
	}

	emit_profiler_leave (builder);
}

} // namespace mono
