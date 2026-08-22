#include "runtime-error.hpp"

#include "thrower.hpp"

#include "debugging/perf/dump-method.hpp"
#include "dump.hpp"
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

	std::string name = stub_symbol (method);
	auto context = std::make_unique<LLVMContext> ();
	auto module = std::make_unique<Module> (name, *context);
	LLVMContext &ctx = *context;
	Type *ptr = PointerType::get (ctx, 0);

	FunctionCallee load = module->getOrInsertFunction (
		"mono_llvm_load_error_exception", FunctionType::get (ptr, { ptr }, false));
	FunctionCallee raise = module->getOrInsertFunction (
		"mono_llvm_throw_exception",
		FunctionType::get (Type::getVoidTy (ctx), { ptr }, false));

	Function *function =
		Function::Create (FunctionType::get (Type::getVoidTy (ctx), false),
	                          GlobalValue::ExternalLinkage, name, module.get ());

	/* Mono walks this frame like any other, from its unwind record. */
	function->setUWTableKind (UWTableKind::Default);

	IRBuilder<> builder (BasicBlock::Create (ctx, "entry", function));
	Value *box =
		builder.CreateIntToPtr (builder.getInt64 ((uint64_t) (uintptr_t) boxed), ptr);

	builder.CreateCall (raise, { builder.CreateCall (load, { box }) });
	builder.CreateUnreachable ();

	if (Error err = bind_symbols (*module))
		return std::move (err);

	std::string dumped = any_dump_point_enabled () ? dump_name (method)
	                                               : std::string ();

	if (!dumped.empty ())
		set_dump_name (*function, dumped);

	auto dump_thrower = [&] (DumpPoint point) {
		if (!dumping (point, dumped.c_str ()))
			return;

		cantFail (with_dump_stream (point, dumped, [&] (raw_ostream &out) {
			function->print (out);
			return Error::success ();
		}));
	};

	dump_thrower (DumpPoint::unopt_ir);

	MonoJit::optimize (*module, JitTier::tier1);

	dump_thrower (DumpPoint::tier1_ir);

	Expected<CompiledMethod> compiled =
		jit.compile (ThreadSafeModule (std::move (module),
	                                       ThreadSafeContext (std::move (context))),
	                     name);

	if (!compiled)
		return compiled.takeError ();

	perf::dump_method (method, *compiled);

	Expected<MonoJitInfo *> jinfo =
		register_jit_info (domain, method, nullptr, *compiled, CodeKind::Body);

	if (!jinfo)
		return jinfo.takeError ();
	remember (*compiled, *jinfo);

	return Compiled { compiled->entry };
}

/*
 * Deferring a failure in this set costs nothing: the name that failed to
 * resolve is only ever consulted by a call.
 *
 * Invalid IL is not in the set. Whether a body can be compiled is the answer
 * the requester asked for, and translation reports it through that request's
 * MonoError. That is why a delegate over a malformed DynamicMethod throws from
 * Delegate.CreateDelegate and not from the first call through it.
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
 * A thunk is the end of the line for a failure. The choice recover ()
 * makes - defer this one, report that one - is not available here:
 * everything is deferred.
 *
 * A failure that never went through a MonoError becomes the
 * ExecutionEngineException managed code sees for an engine that gave up. A
 * symbol that failed to resolve then costs the one method that named it
 * rather than every method in the process.
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
