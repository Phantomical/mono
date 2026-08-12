#include "runtime-error.hpp"

#include "thrower.hpp"

#include "debugging/perf/dump-method.hpp"
#include "jinfo.hpp"
#include "jit.hpp"
#include "naming.hpp"
#include "options.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>

#include "mono/metadata/class-internals.h"
#include "mono/utils/mono-error-internals.h"

using namespace llvm;
using namespace llvm::orc;

namespace mono {

Expected<Compiled>
compile_thrower (MonoJit &jit, MonoDomain *domain, MonoMethod *method,
                 MonoError *failure, RememberFn remember)
{
	MonoErrorBoxed *boxed =
		mono_error_box (failure, m_class_get_image (method->klass));

	if (boxed == nullptr)
		return runtime_error (failure);

	if (is_jit_trace_enabled ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] %s throws on call: %s\n", name,
		         mono_error_get_message (failure));
		g_free (name);
	}

	mono_error_cleanup (failure);

	/*
	 * The entry and the body are separate symbols the runtime redirects
	 * independently, and here they stand for the same three instructions - so
	 * this is that body, built twice under the two names.
	 */
	auto build = [&] (const std::string &name) -> Expected<void *> {
		auto context = std::make_unique<LLVMContext> ();
		auto module = std::make_unique<Module> (name, *context);
		LLVMContext &ctx = *context;
		Type *ptr = PointerType::get (ctx, 0);

		FunctionCallee load = module->getOrInsertFunction (
			"mono_llvm_load_error_exception",
			FunctionType::get (ptr, { ptr }, false));
		FunctionCallee raise = module->getOrInsertFunction (
			"mono_llvm_throw_exception",
			FunctionType::get (Type::getVoidTy (ctx), { ptr }, false));

		Function *function =
			Function::Create (FunctionType::get (Type::getVoidTy (ctx), false),
			                  GlobalValue::ExternalLinkage, name, module.get ());

		/* Mono walks this frame like any other, from its unwind record. */
		function->setUWTableKind (UWTableKind::Default);

		IRBuilder<> builder (BasicBlock::Create (ctx, "entry", function));
		Value *box = builder.CreateIntToPtr (
			builder.getInt64 ((uint64_t) (uintptr_t) boxed), ptr);

		builder.CreateCall (raise, { builder.CreateCall (load, { box }) });
		builder.CreateUnreachable ();

		if (Error err = bind_symbols (*module))
			return std::move (err);

		if (dumping (name.c_str ()))
			module->print (llvm::errs (), nullptr);

		Expected<CompiledMethod> compiled = jit.compile (
			ThreadSafeModule (std::move (module),
			                  ThreadSafeContext (std::move (context))),
			name);
		if (!compiled)
			return compiled.takeError ();

		perf::dump_method (method, *compiled);

		Expected<MonoJitInfo *> jinfo = register_jit_info (
			domain, method, nullptr, *compiled, CodeKind::Body);

		if (!jinfo)
			return jinfo.takeError ();
		remember (*compiled, *jinfo);

		return compiled->entry;
	};

	Expected<void *> body = build (stub_symbol (method, Entry::body));

	if (!body)
		return body.takeError ();

	/*
	 * One body under one name for every door. It takes no arguments it reads
	 * and never returns, so whichever entry a caller came for - the interop one,
	 * the unboxing one, the method itself - these three instructions answer for
	 * it, and publish_defs () points every stub the method has at this.
	 */
	return Compiled { *body, *body, *body };
}

/*
 * The metadata failures ECMA-335 raises where the thing is used rather than
 * where it is named: a method calling one that is missing gets to run until the
 * call, and its caller gets to catch what the call throws. Deferring one costs
 * nothing, because the name it could not resolve is only ever consulted by a
 * call.
 *
 * Invalid IL is not in the set. A body's validity is the answer to the question
 * "can this method be compiled", so whoever asked for the compile is entitled to
 * hear it: mini reports it through the MonoError, which is why creating a
 * delegate over a malformed DynamicMethod throws from Delegate.CreateDelegate
 * rather than from the first call through it. A call that arrives at a stub
 * without anyone having asked still gets the deferral it needs - raise_on_call ()
 * defers everything, that being the only answer available there.
 */
bool
raised_where_used (uint16_t code)
{
	switch (code) {
	case MONO_ERROR_MISSING_METHOD:
	case MONO_ERROR_MISSING_FIELD:
	case MONO_ERROR_TYPE_LOAD:
	case MONO_ERROR_FILE_NOT_FOUND:
	case MONO_ERROR_BAD_IMAGE:
	case MONO_ERROR_MEMBER_ACCESS:
		return true;
	default:
		return false;
	}
}

Expected<Compiled>
recover (MonoJit &jit, MonoDomain *domain, MonoMethod *method, Error failure,
         RememberFn remember)
{
	if (!failure.isA<RuntimeError> ())
		return std::move (failure);

	ERROR_DECL (metadata_error);

	handleAllErrors (std::move (failure),
	                 [&] (RuntimeError &runtime) { runtime.move_to (metadata_error); });

	if (!raised_where_used (mono_error_get_error_code (metadata_error)))
		return runtime_error (metadata_error);

	return compile_thrower (jit, domain, method, metadata_error, remember);
}

/*
 * A stub is the end of the line for a failure. The trampoline behind it has
 * already put the call's arguments back and there is no caller expecting a
 * miss, so a failure that gets this far either becomes an exception the program
 * can see or ends the process. Which means the choice recover () makes - defer
 * this one, report that one - is not available here: everything is deferred,
 * and a failure that never went through a MonoError becomes the
 * ExecutionEngineException managed code sees for an engine that gave up. A
 * symbol that failed to resolve then costs the one method that named it rather
 * than every method in the process.
 */
Expected<Compiled>
raise_on_call (MonoJit &jit, MonoDomain *domain, MonoMethod *method, Error failure,
               RememberFn remember)
{
	ERROR_DECL (call_error);

	if (failure.isA<RuntimeError> ())
		handleAllErrors (std::move (failure),
		                 [&] (RuntimeError &runtime) { runtime.move_to (call_error); });
	else
		mono_error_set_execution_engine (call_error, "%s",
		                                 toString (std::move (failure)).c_str ());

	return compile_thrower (jit, domain, method, call_error, remember);
}

} // namespace mono
