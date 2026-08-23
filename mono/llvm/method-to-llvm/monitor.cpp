/**
 * \file
 * \brief Compiling System.Threading.Monitor.Enter with its fast path in front.
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
 * Both Enter overloads reach the runtime through a managed-to-native wrapper.
 * That wrapper saves five callee-saved registers, pushes an LMF, calls the C
 * function through a pointer and tests the interruption flag. An uncontended
 * lock is one compare-and-swap on the object's lock word. The transition
 * therefore costs more than the work.
 *
 * mono_monitor_enter_fast () and mono_monitor_enter_v4_fast ()
 * (mono/metadata/monitor.c) are that compare-and-swap alone. mini_init ()
 * (mono/mini/mini-runtime.c) registers both with no wrapper, so a call site
 * reaches the C function itself.
 *
 * Each returns false for every case it does not answer: a null object, a
 * lockTaken that is already true, a lock another thread holds, and an
 * interruption. So the call the site named stays. It sits on the edge the
 * helper did not take, and it raises what each of those cases is owed.
 */

std::optional<MonoJitICallId>
monitor_enter_fast_icall (MonoMethod *method, MonoMethodSignature *sig)
{
	// The tests below pick one overload each, so a signature that is not
	// exactly one of the two keeps its call.
	if (sig == nullptr || sig->hasthis || sig->call_convention == MONO_CALL_VARARG)
		return std::nullopt;
	if (sig->ret->byref || sig->ret->type != MONO_TYPE_VOID)
		return std::nullopt;

	MonoClass *klass = method->klass;

	if (m_class_get_image (klass) != mono_get_corlib ())
		return std::nullopt;
	if (std::string_view (m_class_get_name_space (klass)) != "System.Threading")
		return std::nullopt;
	if (std::string_view (m_class_get_name (klass)) != "Monitor")
		return std::nullopt;
	if (std::string_view (method->name) != "Enter")
		return std::nullopt;

	if (sig->param_count < 1 || sig->params[0]->byref
	    || sig->params[0]->type != MONO_TYPE_OBJECT)
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

/// Compiles a call to Monitor.Enter as a call to helper, with the call the site
/// named on the edge helper declined. The arguments come off the evaluation
/// stack.
///
/// callee_method is the method the IL named, taken before the icall wrapper
/// swap. Both overloads are static, and neither is virtual, generic or
/// synchronized. The contended edge therefore needs that swap, and none of the
/// other work an ordinary call site does.
llvm::Error
MethodLLVMEmitter::emit_monitor_enter (MonoIrBuilder &builder, MonoMethod *callee_method,
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

	llvm::CallInst *acquired = builder.CreateCall (*fast, *args, "lock_taken");

	// The helper is a C function, so the site takes the C convention. It never
	// unwinds: a lock it could not take comes back as false. A plain call is
	// therefore right inside a protected region as well.
	mark_mono_call (acquired);
	acquired->setDoesNotThrow ();

	llvm::BasicBlock *contended =
		llvm::BasicBlock::Create (context (), "monitor_contended", function);
	llvm::BasicBlock *entered =
		llvm::BasicBlock::Create (context (), "monitor_entered", function);
	llvm::BranchInst *branch =
		builder.CreateCondBr (builder.CreateIsNotNull (acquired), entered, contended);

	// Most locks a program takes are locks no other thread holds, which is the
	// case the helper answers. The two edges look alike without this, and the
	// layout then puts the wrapper call in the fallthrough.
	llvm::MDBuilder md (context ());
	branch->setMetadata (llvm::LLVMContext::MD_prof, md.createBranchWeights (1000, 1));

	builder.SetInsertPoint (contended);
	emit_protected_call (builder, *slow_decl, adapt_to_callee (builder, *slow_decl, *args));

	// emit_protected_call () continues in a block of its own when it makes an
	// invoke. This branch therefore leaves whichever block it left the builder
	// in, rather than contended.
	builder.CreateBr (entered);
	builder.SetInsertPoint (entered);

	pop_stack (sig->param_count);
	return llvm::Error::success ();
}

} // namespace mono
