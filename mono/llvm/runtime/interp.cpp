#include "runtime-error.hpp"

#include "interp.hpp"
#include "interp-entry.hpp"

#include "method-to-llvm.hpp"
#include "minimal-compile.hpp"
#include "naming.hpp"

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
 * One layout per prototype, shared by every method with that prototype, and one
 * entry per method per domain. Behind a lock of their own rather than an
 * engine's: an interpreted call reads these on every invocation and must never
 * queue behind a compile.
 */
std::shared_mutex g_interp_mutex;
std::unordered_map<std::string, std::unique_ptr<arch::InterpEntryLayout>> g_layouts;
std::unordered_map<MonoDomain *, std::unordered_map<MonoMethod *, arch::InterpEntryPoint>>
	g_interp_entries;

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

Expected<const arch::InterpEntryPoint *>
interp_entry (MonoDomain *domain, MonoMethod *method)
{
	{
		std::shared_lock<std::shared_mutex> lock (g_interp_mutex);
		auto domain_entries = g_interp_entries.find (domain);

		if (domain_entries != g_interp_entries.end ()) {
			auto it = domain_entries->second.find (method);

			if (it != domain_entries->second.end ())
				return &it->second;
		}
	}

	Expected<const arch::InterpEntryLayout *> layout =
		layout_for (domain, method);

	if (!layout)
		return layout.takeError ();

	ERROR_DECL (interp_error);
	void *imethod = mini_get_interp_callbacks ()->get_imethod (method, interp_error);

	if (imethod == nullptr)
		return runtime_error (interp_error);

	std::unique_lock<std::shared_mutex> lock (g_interp_mutex);
	arch::InterpEntryPoint &entry = g_interp_entries[domain][method];

	entry.layout = *layout;
	entry.imethod = imethod;
	return &entry;
}

/*
 * The thread's own domain, not the one that published the stub: a call that
 * arrived here having switched domains has to run the method as the domain it
 * switched to, which is the one holding that method's interpreter state.
 */
const arch::InterpEntryPoint *
interp_entry_for (MonoMethod *method)
{
	Expected<const arch::InterpEntryPoint *> entry =
		interp_entry (mono_domain_get (), method);

	if (!entry) {
		consumeError (entry.takeError ());
		return nullptr;
	}

	return *entry;
}

void
forget_interp_entries (MonoDomain *domain)
{
	std::unique_lock<std::shared_mutex> lock (g_interp_mutex);

	g_interp_entries.erase (domain);
}

/*
 * Freeing a method hands its MonoMethod back to the allocator, and the next one
 * allocated can land on that address - so an entry left here is one that method
 * would find and take for its own, pointing at interpreter state that died with
 * the method before it.
 */
void
forget_interp_entry (MonoMethod *method)
{
	std::unique_lock<std::shared_mutex> lock (g_interp_mutex);

	for (auto &domain : g_interp_entries)
		domain.second.erase (method);
}

} // namespace mono
