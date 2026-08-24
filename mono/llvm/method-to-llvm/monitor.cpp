/**
 * \file
 * \brief Compiling System.Threading.Monitor with its fast paths in front.
 */

#include "method-to-llvm.hpp"

// class-internals.h brings in jit-icall-reg.h, which has no include guard.
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>

#include <optional>
#include <string_view>

namespace mono {

/*
 * A call to Enter or Exit reaches the runtime through a managed-to-native
 * wrapper. That wrapper saves five callee-saved registers, pushes an LMF, calls
 * the C function through a pointer and tests the interruption flag. An
 * uncontended lock is one compare-and-swap on the object's lock word. The
 * transition therefore costs more than the work.
 *
 * mono_monitor_enter_fast (), mono_monitor_enter_v4_fast () and
 * mono_monitor_exit_fast () (mono/metadata/monitor.c) are that compare-and-swap
 * alone. mini_init () (mono/mini/mini-runtime.c) registers each with no
 * wrapper, so a call site reaches the C function itself.
 *
 * Each helper returns false for every case it does not answer. So the call the
 * site named stays. It sits on the edge the helper did not take, and it raises
 * what each of those cases is owed.
 *
 * A site that takes a helper's edge does not run the wrapper's interruption
 * test. The thread reaches the next interruption point instead.
 */

/// Whether method is the named static method on System.Threading.Monitor.
static bool
is_monitor_method (MonoMethod *method, MonoMethodSignature *sig, std::string_view what)
{
	if (sig == nullptr || sig->hasthis || sig->call_convention == MONO_CALL_VARARG)
		return false;
	if (sig->ret->byref || sig->ret->type != MONO_TYPE_VOID)
		return false;

	MonoClass *klass = method->klass;

	if (m_class_get_image (klass) != mono_get_corlib ())
		return false;
	if (std::string_view (m_class_get_name_space (klass)) != "System.Threading")
		return false;
	if (std::string_view (m_class_get_name (klass)) != "Monitor")
		return false;

	return std::string_view (method->name) == what;
}

/// Whether the first parameter is an object passed by value.
static bool
takes_the_lock_object (MonoMethodSignature *sig)
{
	return sig->param_count >= 1 && !sig->params[0]->byref
	       && sig->params[0]->type == MONO_TYPE_OBJECT;
}

std::optional<MonoJitICallId>
monitor_enter_fast_icall (MonoMethod *method, MonoMethodSignature *sig)
{
	// The tests below pick one overload each, so a signature that is not
	// exactly one of the two keeps its call.
	if (!is_monitor_method (method, sig, "Enter") || !takes_the_lock_object (sig))
		return std::nullopt;

	if (sig->param_count == 1)
		return MONO_JIT_ICALL_mono_monitor_enter_fast;

	// C# compiles `lock` to this overload, so it is the one a program locks
	// through. The helper writes lockTaken itself.
	if (sig->param_count == 2 && sig->params[1]->byref
	    && sig->params[1]->type == MONO_TYPE_BOOLEAN)
		return MONO_JIT_ICALL_mono_monitor_enter_v4_fast;

	return std::nullopt;
}

std::optional<MonoJitICallId>
monitor_exit_fast_icall (MonoMethod *method, MonoMethodSignature *sig)
{
	if (!is_monitor_method (method, sig, "Exit") || !takes_the_lock_object (sig))
		return std::nullopt;
	if (sig->param_count != 1)
		return std::nullopt;

	return MONO_JIT_ICALL_mono_monitor_exit_fast;
}

/// Compiles a call to Monitor.Enter or Monitor.Exit as a call to helper, with
/// the call the site named on the edge helper declined. The arguments come off
/// the evaluation stack.
///
/// callee_method is the method the IL named, taken before the icall wrapper
/// swap. Every one of these overloads is static, and none is virtual, generic
/// or synchronized. The declined edge therefore needs that swap, and none of
/// the other work an ordinary call site does.
llvm::Error
MethodLLVMEmitter::emit_monitor_fast_path (MonoIrBuilder &builder, MonoMethod *callee_method,
                                           MonoMethodSignature *sig, MonoJitICallId helper)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::Expected<llvm::Function *> slow_decl =
		create_method_decl (icall_wrapper_target (callee_method));
	if (!slow_decl)
		return slow_decl.takeError ();

	llvm::Expected<llvm::FunctionCallee> fast =
		raw_icall_decl (mono_find_jit_icall_info (helper));
	if (!fast)
		return fast.takeError ();

	llvm::CallInst *answered = builder.CreateCall (*fast, *args, "monitor_answered");

	// The helper is a C function, so the site takes the C convention. It never
	// unwinds: a case it does not answer comes back as false. A plain call is
	// therefore right inside a protected region as well.
	mark_mono_call (answered);
	answered->setDoesNotThrow ();

	llvm::BasicBlock *declined =
		llvm::BasicBlock::Create (context (), "monitor_declined", function);
	llvm::BasicBlock *done =
		llvm::BasicBlock::Create (context (), "monitor_done", function);
	llvm::BranchInst *branch =
		builder.CreateCondBr (builder.CreateIsNotNull (answered), done, declined);

	// Most of these calls are on an uncontended lock, which is the case the
	// helpers answer. The two edges look alike without this, and the layout
	// then puts the wrapper call in the fallthrough.
	llvm::MDBuilder md (context ());
	branch->setMetadata (llvm::LLVMContext::MD_prof, md.createBranchWeights (1000, 1));

	builder.SetInsertPoint (declined);
	emit_protected_call (builder, *slow_decl, adapt_to_callee (builder, *slow_decl, *args));

	// emit_protected_call () continues in a block of its own when it makes an
	// invoke. This branch therefore leaves whichever block it left the builder
	// in, rather than declined.
	builder.CreateBr (done);
	builder.SetInsertPoint (done);

	pop_stack (sig->param_count);
	return llvm::Error::success ();
}

} // namespace mono
