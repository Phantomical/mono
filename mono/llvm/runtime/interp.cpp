#include "runtime-error.hpp"

#include "interp.hpp"
#include "interp-entry.hpp"

#include "method-to-llvm.hpp"
#include "minimal-compile.hpp"
#include "naming.hpp"

#include <mono/mini/domain-method.hpp>

#include <llvm/IR/DerivedTypes.h>
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
 * lock of its own rather than an engine's: an interpreted call reads the layout
 * on every invocation and must never queue behind a compile.
 */
std::shared_mutex g_interp_mutex;
std::unordered_map<std::string, std::unique_ptr<arch::InterpEntryLayout>> g_layouts;

void
print_shape (Type *t, raw_ostream &os)
{
	if (auto *st = dyn_cast<StructType> (t)) {
		/*
		 * Type::print () writes a named struct as its name, and that name is the
		 * managed type's. Two assemblies can each define a type with one name and
		 * lay it out differently: mscorlib and System.Memory both define
		 * System.ReadOnlySpan`1, one with two fields and one with three. So the
		 * name does not say how many registers a value of it arrives in.
		 */
		if (st->isOpaque ()) {
			os << "opaque " << st->getName ();
			return;
		}

		os << (st->isPacked () ? "<{" : "{");
		for (unsigned i = 0; i < st->getNumElements (); ++i) {
			if (i != 0)
				os << ',';
			print_shape (st->getElementType (i), os);
		}
		os << (st->isPacked () ? "}>" : "}");
		return;
	}

	if (auto *at = dyn_cast<ArrayType> (t)) {
		os << '[' << at->getNumElements () << " x ";
		print_shape (at->getElementType (), os);
		os << ']';
		return;
	}

	t->print (os);
}

} // namespace

std::string
prototype_key (Function *f)
{
	std::string key;
	raw_string_ostream os (key);
	FunctionType *type = f->getFunctionType ();
	AttributeList attrs = f->getAttributes ();

	print_shape (type->getReturnType (), os);
	os << " (";
	for (unsigned i = 0; i < type->getNumParams (); ++i) {
		if (i != 0)
			os << ", ";
		print_shape (type->getParamType (i), os);
	}
	os << (type->isVarArg () ? ", ...)" : ")");

	os << "|cc" << f->getCallingConv () << "|ret "
	   << attrs.getRetAttrs ().getAsString ();
	for (unsigned i = 0; i < type->getNumParams (); ++i)
		os << "|p" << i << " " << attrs.getParamAttrs (i).getAsString ();
	os.flush ();
	return key;
}

namespace {

Expected<const arch::InterpEntryLayout *>
layout_for (MonoDomain *domain, MonoMethod *method)
{
	/*
	 * The layout comes from the prototype rather than the method. We declare
	 * one instead of compiling it, and the module exists only to hold its
	 * types.
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
	 * The prototype alone does not settle the layout. A byref parameter and an
	 * ordinary reference both lower to the same bare pointer, so the key must
	 * mark which one it is. Nor does the prototype say where the receiver stops
	 * and the arguments start.
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

arch::InterpEntryPoint
interp_entry_for (MonoMethod *method)
{
	/*
	 * This runs the method in the calling thread's domain, not the one that
	 * published the stub. A call that arrived here after a domain switch must
	 * run in the domain it switched to, since that domain owns the method's
	 * interpreter state.
	 */
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
