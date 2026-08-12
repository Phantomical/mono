#include "method-to-llvm.hpp"
#include "mono/metadata/profiler-private.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

namespace mono {

// A profiler that installs a call-instrumentation filter wants to know when a
// method starts and when it exits. The method's own body tells it. A call to
// mono_profiler_raise_method_enter sits at the top. A call to
// mono_profiler_raise_method_leave sits in front of each ret.
//
// An exception exit is different. The unwinder leaves the frame, not the
// method's own code, and mono_handle_exception_internal raises
// method_exception_leave as it walks the frame off. So the front end only
// owes the enter event that pairs with it. That is why the enter comes
// before the class initializer. A type initializer that throws still unwinds
// this frame.

/// Ask the profilers what they want instrumented in this method.
void
MethodLLVMEmitter::resolve_call_instrumentation ()
{
	// A native-to-managed wrapper enters from C, on a thread that can still
	// be unattached. Attaching the thread is the first thing the wrapper's
	// body does. A profiler callback before that point runs on a thread
	// that does not exist yet, which is worse than skipping the event.
	if (method->wrapper_type == MONO_WRAPPER_NATIVE_TO_MANAGED)
		return;

	prof_flags = mono_profiler_get_call_instrumentation_flags (method);
}

bool
MethodLLVMEmitter::instrumented (MonoProfilerCallInstrumentationFlags flag) const
{
	// A filter body is a helper over this frame, not a new entry into the method.
	return !filter_mode && (prof_flags & flag) != 0;
}

/// Call one of the runtime's mono_profiler_raise_* entry points. Every
/// argument is a pointer.
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

	// A callback can walk the stack and expects to find this frame still
	// there. The call stays an ordinary call, not a tail call, so tail-call
	// elimination cannot turn it into a jump that skips over this frame.
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

/// Report an honored tail. call, or a jmp, as the method's exit. The site
/// hands this method's frame to target.
///
/// Control does not come back after the site runs. So the report must go in
/// front of it, not after. A profiler that did not ask for tail-call events
/// still needs a leave report here. The frame is gone either way, and
/// without a leave, the profiler's idea of the stack stays one frame too
/// deep.
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
