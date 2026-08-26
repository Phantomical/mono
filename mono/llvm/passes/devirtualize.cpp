/**
 * \file
 * \brief Resolving a dispatch site against the class its receiver turned out to
 * have.
 *
 * The slot the site reads is the one the runtime would have read, so what
 * stands there is the method a caller can name directly. The declaration this
 * writes is the one a direct call would have named all along: it carries the
 * MonoMethod, so the engine renames it to the published entry and the inliner
 * reads the method back off it like any other callee.
 */

#include "devirtualize.hpp"

#include "compile-state.hpp"
#include "method-symbols.hpp"
#include "runtime/naming.hpp"
#include "vtable-func.hpp"

#include "mini.h"

#include "mono/metadata/class-init.h"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/metadata.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {
namespace {

/// The method in slot \p index of \p klass's vtable, or null where a caller
/// cannot name what stands there.
MonoMethod *
slot_target (MonoClass *klass, int32_t index)
{
	/*
	 * The layout is what fills the slots in, and a class that cannot have one
	 * reaches vtable[index] with vtable null. Leaving the site alone puts the
	 * type load back where the runtime raises it.
	 */
	mono_class_setup_vtable (klass);
	if (mono_class_has_failure (klass) || m_class_get_vtable (klass) == nullptr)
		return nullptr;

	/*
	 * The word at vtable_size is the static field block rather than a method,
	 * so the bound is strict. A negative index is what a method with no slot of
	 * its own carries.
	 */
	if (index < 0 || index >= m_class_get_vtable_size (klass))
		return nullptr;

	MonoMethod *target = m_class_get_vtable (klass)[index];

	// A class that leaves an inherited abstract method unimplemented has no
	// body to enter, and no instances either, so the site that raises stands.
	if (target == nullptr || (target->flags & METHOD_ATTRIBUTE_ABSTRACT) != 0)
		return nullptr;

	MonoMethodSignature *sig = mono_method_signature_internal (target);

	if (sig == nullptr)
		return nullptr;

	/*
	 * A virtual generic method's slot holds a trampoline that reads the
	 * asked-for instantiation out of the IMT register, so no one method is what
	 * the site enters.
	 */
	if (sig->generic_param_count != 0)
		return nullptr;

	// An icall, a pinvoke or a runtime-implemented method is declared in the C
	// convention, which is not the shape the site calls with.
	if (implemented_outside_il (target))
		return nullptr;

	/*
	 * A boxed receiver arrives at the unbox entry, which steps it past the
	 * object header. Naming the body here hands it the box instead.
	 */
	if (publishes_unbox_entry (target))
		return nullptr;

	/*
	 * A method whose entry needs a context is one no caller enters directly: it
	 * would arrive with the receiver of some other instantiation, or none.
	 */
	if (mono_method_check_context_used (target) != 0)
		return nullptr;

	/*
	 * The runtime puts the wrapper in the slot, and dispatching is what found
	 * it. A direct call has to name the wrapper itself, or the body runs
	 * without its lock. The site calls what comes back with the prototype it
	 * already had, and a vararg wrapper is not that shape - emit_call ()
	 * refuses the same pair rather than building one.
	 */
	if ((target->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED) != 0)
		return sig->call_convention == MONO_CALL_VARARG
		               ? nullptr
		               : mono_marshal_get_synchronized_wrapper (target);

	return target;
}

/// The shape every use of \p site calls the entry with, or null where the uses
/// do not agree or one of them is not a call.
///
/// Each use names the prototype the IL settled, so they agree in the ordinary
/// case. A use of another kind - the select a delegate's Invoke reads its entry
/// through is one - leaves the site alone, because what the entry is for there
/// is not this pass's to say.
FunctionType *
called_shape (CallBase *site)
{
	FunctionType *shape = nullptr;

	for (User *user : site->users ()) {
		auto *call = dyn_cast<CallBase> (user);

		if (call == nullptr || call->getCalledOperand () != site)
			return nullptr;
		if (shape != nullptr && shape != call->getFunctionType ())
			return nullptr;
		shape = call->getFunctionType ();
	}

	return shape;
}

/// The entry a call of \p shape enters \p target through, declared in \p m.
Function *
entry_for (Module &m, MonoMethod *target, FunctionType *shape, const CompileState &compile)
{
	/*
	 * A placeholder name, the way the translator writes one: the engine reads
	 * the marker and renames this to whatever it publishes the entry under.
	 * Nothing has to agree with it in advance.
	 */
	Function *decl = Function::Create (shape, GlobalValue::ExternalLinkage,
	                                   "mono_devirt_" + identity_of (target), &m);

	mark_method_reference (*decl, target);

	Function *entry = compile.publish (*decl, target);

	/*
	 * The method's own metadata will not load. The site keeps its lookup, and
	 * the runtime raises where it always did.
	 */
	if (entry == nullptr && decl->use_empty ())
		decl->eraseFromParent ();

	return entry;
}

} // namespace

PreservedAnalyses
DevirtualizePass::run (Function &f, FunctionAnalysisManager &)
{
	const CompileState &compile = current_compile ();
	Function *decl = f.getParent ()->getFunction (vtable_func_name);

	if (decl == nullptr || compile.domain == nullptr || !compile.publish)
		return PreservedAnalyses::all ();

	SmallVector<CallBase *, 4> sites;

	for (User *user : decl->users ()) {
		auto *site = dyn_cast<CallBase> (user);

		if (site != nullptr && site->getFunction () == &f && !site->use_empty ())
			sites.push_back (site);
	}

	bool changed = false;

	for (CallBase *site : sites) {
		auto *vtable = dyn_cast<GlobalValue> (site->getArgOperand (0));
		auto *index = dyn_cast<ConstantInt> (site->getArgOperand (1));

		if (vtable == nullptr || index == nullptr)
			continue;

		MonoClass *klass = marked_class (*vtable);
		FunctionType *shape = called_shape (site);

		if (klass == nullptr || shape == nullptr)
			continue;

		MonoMethod *target = slot_target (klass, static_cast<int32_t> (index->getSExtValue ()));

		if (target == nullptr)
			continue;

		Function *entry = entry_for (*f.getParent (), target, shape, compile);

		if (entry == nullptr)
			continue;

		/*
		 * The entry replaces the value the site answered rather than the calls
		 * that read it. Each of those is then a call of a constant, which the
		 * simplification behind this pass turns into a direct one.
		 */
		site->replaceAllUsesWith (entry);
		site->eraseFromParent ();
		changed = true;
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono
