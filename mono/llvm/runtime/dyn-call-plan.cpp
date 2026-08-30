#include "runtime-error.hpp"

#include "dyn-call-plan.hpp"

#include "interp.hpp"
#include "method-to-llvm.hpp"
#include "minimal-compile.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mini.h"

using namespace llvm;

namespace mono {

namespace {

/*
 * One plan per prototype, shared by every method with that prototype - see
 * interp.cpp's g_layouts, which this mirrors for the outgoing direction.
 * Behind a lock of its own rather than an engine's: an interpreted caller
 * reads a plan on every jit call and must never queue behind a compile.
 */
std::shared_mutex g_dyn_call_mutex;
std::unordered_map<std::string, std::unique_ptr<arch::DynCallPlan>> g_dyn_call_plans;

} // namespace

Expected<const arch::DynCallPlan *>
dyn_call_plan_for (MonoMethod *method)
{
	/*
	 * As in interp.cpp's layout_for (): declared rather than compiled, and the
	 * module exists only to hold the declaration's types.
	 */
	ERROR_DECL (metadata_error);
	MinimalCompile cfg (method, mono_domain_get (), metadata_error);

	if (cfg.get ()->header == nullptr)
		return runtime_error (metadata_error);

	LLVMContext context;
	Module module ("mono.dyn.call", context);

	std::vector<ExternalSymbol> externals;
	MethodLLVMEmitter declarer (&module, cfg.get (), method, &externals);
	Expected<Function *> shape = declarer.declare (method);

	if (!shape)
		return shape.takeError ();

	std::string key = prototype_key (*shape);

	{
		std::shared_lock<std::shared_mutex> lock (g_dyn_call_mutex);
		auto it = g_dyn_call_plans.find (key);

		if (it != g_dyn_call_plans.end ())
			return it->second.get ();
	}

	Expected<std::unique_ptr<arch::DynCallPlan>> planned =
		arch::plan_dyn_call (*shape, mono_method_signature_internal (method));

	if (!planned)
		return planned.takeError ();

	std::unique_lock<std::shared_mutex> lock (g_dyn_call_mutex);
	std::unique_ptr<arch::DynCallPlan> &slot = g_dyn_call_plans[key];

	/* Two threads racing on one prototype agree, so the loser's is dropped. */
	if (slot == nullptr)
		slot = std::move (*planned);
	return slot.get ();
}

} // namespace mono
