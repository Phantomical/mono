#include "runtime-error.hpp"

#include "dispatcher.hpp"

#include "debugging/perf/dump-method.hpp"
#include "jit.hpp"
#include "method-to-llvm.hpp"
#include "minimal-compile.hpp"
#include "naming.hpp"
#include "options.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>
#include <vector>

#include "mono/metadata/class-internals.h"
#include "mono/metadata/domain-internals.h"
#include "mono/metadata/marshal.h"

using namespace llvm;
using namespace llvm::orc;

namespace mono {

bool
bindable (MonoDomain *owner, MonoMethod *method)
{
	return mono_domain_get () == owner || implemented_outside_il (method);
}

Expected<void *>
build_dispatcher (MonoJit &jit, MonoDomain *domain, MonoMethod *method,
                  RememberFn remember)
{
	/*
	 * The dispatcher borrows the fastcc body's exact type - signature,
	 * convention, attributes - from a declaration; the accessor substitution
	 * mirrors translate_and_compile (), whose compiled body is what the helper
	 * will return. The declaration itself goes back out of the module: the
	 * forward is through the helper's answer, never through the stub, or the
	 * dispatcher would bounce off its own binding forever.
	 */
	MonoMethod *declared = method;

	if (m_class_get_rank (declared->klass) > 0
	    && (declared->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
	    && (declared->iflags & METHOD_IMPL_ATTRIBUTE_NATIVE))
		declared = mono_marshal_get_array_accessor_wrapper (declared);

	ERROR_DECL (metadata_error);
	MinimalCompile cfg (declared, domain, metadata_error);

	if (cfg.get ()->header == nullptr)
		return runtime_error (metadata_error);

	auto context = std::make_unique<LLVMContext> ();
	std::string name = stub_symbol (method, Entry::body) + "$dispatch";
	auto module = std::make_unique<Module> (name, *context);

	std::vector<ExternalSymbol> externals;
	MethodLLVMEmitter declarer (module.get (), cfg.get (), declared, &externals);
	Expected<Function *> target = declarer.declare (declared);

	if (!target)
		return target.takeError ();

	FunctionType *type = (*target)->getFunctionType ();
	CallingConv::ID conv = (*target)->getCallingConv ();
	AttributeList attrs = (*target)->getAttributes ();

	if ((*target)->use_empty ())
		(*target)->eraseFromParent ();

	Function *disp =
		Function::Create (type, Function::ExternalLinkage, name, module.get ());

	disp->setCallingConv (conv);
	disp->setAttributes (attrs);

	IRBuilder<> builder (BasicBlock::Create (*context, "", disp));
	PointerType *ptr = PointerType::getUnqual (*context);
	FunctionCallee helper = module->getOrInsertFunction (
		"mono_llvm_jit_body_for_current_domain",
		FunctionType::get (ptr, { ptr }, false));
	Value *self = builder.CreateIntToPtr (
		builder.getInt64 ((uint64_t) (uintptr_t) method), ptr);
	Value *body = builder.CreateCall (helper, { self });

	std::vector<Value *> args;

	for (Argument &arg : disp->args ())
		args.push_back (&arg);

	CallInst *forward = builder.CreateCall (type, body, args);

	forward->setCallingConv (conv);
	forward->setAttributes (attrs);
	forward->setTailCallKind (CallInst::TCK_MustTail);

	if (type->getReturnType ()->isVoidTy ())
		builder.CreateRetVoid ();
	else
		builder.CreateRet (forward);

	if (Error err = bind_symbols (*module))
		return std::move (err);

	Expected<CompiledMethod> compiled = jit.compile (
		ThreadSafeModule (std::move (module),
		                  ThreadSafeContext (std::move (context))),
		name);
	if (!compiled)
		return compiled.takeError ();

	perf::dump_method (method, *compiled);
	remember (*compiled, nullptr);

	if (is_jit_trace_enabled ())
		fprintf (stderr,
		         "[llvm-jit] %s dispatches per call (owner %s, first reached "
		         "from %s)\n",
		         name.c_str (), domain->friendly_name,
		         mono_domain_get ()->friendly_name);

	return compiled->entry;
}

} // namespace mono
