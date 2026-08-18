#include "runtime-error.hpp"

#include "interp.hpp"
#include "interp-entry.hpp"

#include "method-to-llvm.hpp"
#include "minimal-compile.hpp"
#include "naming.hpp"

#include <mono/mini/domain-method.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/marshal.h"

using namespace llvm;

namespace mono {

namespace {

/*
 * One layout per prototype, shared by every method with that prototype. Behind a
 * lock of its own rather than an engine's: an interpreted call reads this on
 * every invocation and must never queue behind a compile.
 */
std::shared_mutex g_interp_mutex;
std::unordered_map<std::string, std::unique_ptr<arch::InterpEntryLayout>> g_layouts;

/// A key naming everything about a prototype that decides how a call to it is
/// laid out - the types and the attributes that move a value. Two methods that
/// agree on this can share one thunk between them, and nothing narrower is safe
/// to share on.
std::string
prototype_key (Function *f)
{
	std::string key;
	raw_string_ostream os (key);
	AttributeList attrs = f->getAttributes ();

	f->getFunctionType ()->print (os);
	os << "|cc" << f->getCallingConv () << "|ret "
	   << attrs.getRetAttrs ().getAsString ();
	for (unsigned i = 0; i < f->getFunctionType ()->getNumParams (); ++i)
		os << "|p" << i << " " << attrs.getParamAttrs (i).getAsString ();
	os.flush ();
	return key;
}

Expected<const arch::InterpEntryLayout *>
layout_for (MonoDomain *domain, MonoMethod *method)
{
	/*
	 * The layout is worked out from the prototype rather than from the method,
	 * so it is built out of a declaration of one - nothing here compiles
	 * anything, and the module exists only to hold the types.
	 */
	ERROR_DECL (metadata_error);
	MinimalCompile cfg (method, domain, metadata_error);

	if (cfg.get ()->header == nullptr)
		return runtime_error (metadata_error);

	LLVMContext context;
	Module module ("mono.interp.entry", context);

	std::vector<ExternalSymbol> externals;
	MethodLLVMEmitter declarer (&module, cfg.get (), method, &externals);
	Expected<Function *> shape = declarer.declare (method);

	if (!shape)
		return shape.takeError ();

	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (method->string_ctor)
		sig = mono_marshal_get_string_ctor_signature (method);

	/*
	 * The prototype alone does not settle the layout: a byref parameter hands
	 * the interpreter the pointer itself where an ordinary reference hands it
	 * the slot holding one, and the two are the same bare ptr here. Nor does it
	 * settle where the receiver stops and the arguments start.
	 */
	std::string key = prototype_key (*shape);

	key += sig->hasthis ? "|this" : "|static";
	for (int i = 0; i < sig->param_count; ++i)
		key += sig->params[i]->byref ? '&' : '.';

	{
		std::shared_lock<std::shared_mutex> lock (g_interp_mutex);
		auto it = g_layouts.find (key);

		if (it != g_layouts.end ())
			return it->second.get ();
	}

	Expected<arch::InterpEntryLayout> planned = arch::plan_interp_entry (*shape, sig);

	if (!planned)
		return planned.takeError ();

	std::unique_lock<std::shared_mutex> lock (g_interp_mutex);
	std::unique_ptr<arch::InterpEntryLayout> &slot = g_layouts[key];

	/* Two threads racing on one prototype agree, so the loser's is dropped. */
	if (slot == nullptr)
		slot = std::make_unique<arch::InterpEntryLayout> (std::move (*planned));
	return slot.get ();
}

} // namespace

Expected<arch::InterpEntryPoint>
interp_entry (MonoDomainMethod &dm)
{
	const arch::InterpEntryLayout *layout = dm.interp_layout.load (std::memory_order_acquire);

	if (layout == nullptr) {
		Expected<const arch::InterpEntryLayout *> planned = layout_for (dm.domain, dm.method);

		if (!planned)
			return planned.takeError ();

		layout = *planned;
		dm.interp_layout.store (layout, std::memory_order_release);
	}

	void *imethod = dm.interp_method ();

	if (imethod == nullptr) {
		ERROR_DECL (interp_error);

		imethod = mini_get_interp_callbacks ()->get_imethod (dm.method, interp_error);

		if (imethod == nullptr)
			return runtime_error (interp_error);
	}

	return arch::InterpEntryPoint { layout, imethod };
}

/*
 * The thread's own domain, not the one that published the stub: a call that
 * arrived here having switched domains has to run the method as the domain it
 * switched to, which is the one holding that method's interpreter state.
 */
arch::InterpEntryPoint
interp_entry_for (MonoMethod *method)
{
	MonoDomainMethod *dm = domain_method_find (mono_domain_get (), method);

	if (dm == nullptr)
		return {};

	Expected<arch::InterpEntryPoint> entry = interp_entry (*dm);

	if (!entry) {
		consumeError (entry.takeError ());
		return {};
	}

	return *entry;
}

} // namespace mono
