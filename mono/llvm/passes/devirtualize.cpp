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

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-init.h"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {
namespace {

/// Whether klass has a vtable for a slot to be read out of.
///
/// The layout is what fills the slots in, and a class that cannot have one
/// reaches vtable[index] with vtable null. Leaving a site alone puts the type
/// load back where the runtime raises it.
bool
laid_out (MonoClass *klass)
{
	mono_class_setup_vtable (klass);

	return !mono_class_has_failure (klass) && m_class_get_vtable (klass) != nullptr;
}

/// \p target as a caller can name it, or null where it cannot.
MonoMethod *
nameable (MonoMethod *target)
{
	// A class that leaves an inherited abstract method unimplemented has no
	// body to enter, and no instances either, so the site that raises stands.
	if (target == nullptr || (target->flags & METHOD_ATTRIBUTE_ABSTRACT) != 0)
		return nullptr;

	MonoMethodSignature *sig = mono_method_signature_internal (target);

	if (sig == nullptr)
		return nullptr;

	/*
	 * A virtual generic method's slot holds a trampoline that reads the
	 * asked-for instantiation out of the IMT register. So a site reading the
	 * slot alone enters no one method. A site that carries the key names an
	 * instantiation, and that is the method this answers with.
	 */
	if (sig->generic_param_count != 0) {
		MonoGenericContext *bound = mono_method_get_context (target);

		if (bound == nullptr || bound->method_inst == nullptr)
			return nullptr;
	}

	// An icall, a pinvoke or a runtime-implemented method is declared in the C
	// convention, which is not the shape the site calls with.
	if (implemented_outside_il (target))
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

} // namespace

MonoMethod *
slot_target (MonoClass *klass, int32_t index)
{
	if (!laid_out (klass))
		return nullptr;

	/*
	 * The word at vtable_size is the static field block rather than a method,
	 * so the bound is strict. A negative index is what a method with no slot of
	 * its own carries.
	 */
	if (index < 0 || index >= m_class_get_vtable_size (klass))
		return nullptr;

	return nameable (m_class_get_vtable (klass)[index]);
}

namespace {

/// The method \p klass implements \p asked with, or null where a caller cannot
/// name it.
///
/// The IMT slot a site reads is a hash bucket, so the answer comes from the
/// method the site asked for rather than from the slot. That is the same
/// question the runtime's thunk answers with the key in its register.
MonoMethod *
imt_target (MonoClass *klass, MonoMethod *asked)
{
	if (!mono_class_is_interface (asked->klass) || !laid_out (klass))
		return nullptr;

	/*
	 * The table is indexed from the receiver's offset for that interface, and
	 * the resolver asserts that the offset is there. Verifiable IL gives one,
	 * so a site that does not keeps its lookup rather than reading past the
	 * array.
	 */
	gboolean variance_used = FALSE;

	if (mono_class_interface_offset_with_variance (klass, asked->klass, &variance_used) <= 0)
		return nullptr;

	ERROR_DECL (error);
	MonoMethod *target = mono_class_get_virtual_method (klass, asked, FALSE, error);

	if (!is_ok (error)) {
		mono_error_cleanup (error);
		return nullptr;
	}

	/*
	 * A default implementation lives on an interface rather than on the class,
	 * and which one a class gets is not always a question with one answer: where
	 * two interfaces override a method and neither is more specific, the runtime
	 * raises AmbiguousImplementationException at the dispatch.
	 * mono_class_get_virtual_method () answers with a candidate instead of
	 * raising, so that verdict stays the dispatch's to give.
	 * mono/tests/dim-diamondshape.il is what asks for it.
	 */
	if (target != nullptr && mono_class_is_interface (target->klass))
		return nullptr;

	return nameable (target);
}

/// The method \p klass implements the virtual generic \p asked with, or null
/// where a caller cannot name it.
///
/// The slot holds a trampoline, because one slot serves every instantiation.
/// asked is the instantiation the call site named, so the two together settle
/// what that trampoline would have picked.
MonoMethod *
generic_virtual_target (MonoClass *klass, MonoMethod *asked)
{
	if (!laid_out (klass))
		return nullptr;

	ERROR_DECL (error);
	MonoMethod *target = mono_class_get_virtual_method (klass, asked, FALSE, error);

	if (!is_ok (error)) {
		mono_error_cleanup (error);
		return nullptr;
	}

	return nameable (target);
}

/// The shape every use of \p site calls the entry with, or null where the uses
/// do not agree or one of them is not a call.
///
/// Each use names the prototype the IL settled, so they agree in the ordinary
/// case. A use of another kind - the select a delegate's Invoke reads its entry
/// through is one - leaves the site alone, because what the entry is for there
/// is not this pass's to say.
///
/// A call with no arguments is refused as well. Only a method with a receiver
/// reaches a vtable slot, so one is a shape this pass does not understand.
FunctionType *
called_shape (CallBase *site)
{
	FunctionType *shape = nullptr;

	for (User *user : site->users ()) {
		auto *call = dyn_cast<CallBase> (user);

		if (call == nullptr || call->getCalledOperand () != site
		    || call->arg_size () < 1)
			return nullptr;
		if (shape != nullptr && shape != call->getFunctionType ())
			return nullptr;
		shape = call->getFunctionType ();
	}

	return shape;
}

/// Rewrites \p call so it enters \p entry.
///
/// A value type's slot holds the unbox entry, which steps the receiver past the
/// object header before it runs into the body. \p steps_receiver names an entry
/// that expects the step to have happened, so the call does here what that
/// prologue did - mono/mini/thunk.cpp asserts the same size the arch stub is
/// baked with.
///
/// \p drops_key names a site that carried the method it asked for in the IMT
/// register. The thunk that read it is gone, and the target never did
/// (emit_call () says so where it puts the key on), so the call is built again
/// without it. That is also what leaves this site holding the same prototype a
/// direct call to the same method holds anywhere else.
void
enter_at (CallBase *call, Function *entry, bool steps_receiver, bool drops_key)
{
	IRBuilder<> builder (call);
	SmallVector<Value *, 8> args (call->args ().begin (), call->args ().end ());

	if (steps_receiver)
		args[0] = builder.CreateGEP (builder.getInt8Ty (), args[0],
		                             builder.getInt64 (MONO_ABI_SIZEOF (MonoObject)));

	if (!drops_key) {
		call->setArgOperand (0, args[0]);
		call->setCalledFunction (entry);
		return;
	}

	args.pop_back ();

	CallBase *direct;

	if (auto *invoke = dyn_cast<InvokeInst> (call))
		direct = builder.CreateInvoke (entry, invoke->getNormalDest (),
		                               invoke->getUnwindDest (), args);
	else
		direct = builder.CreateCall (entry, args);

	// The key's own slot goes with it. Everything else the site said about its
	// arguments - which are extended, which are by value - still holds.
	direct->setAttributes (call->getAttributes ().removeParamAttributes (
		call->getContext (), call->arg_size () - 1));
	direct->setCallingConv (call->getCallingConv ());
	direct->setDebugLoc (call->getDebugLoc ());

	if (auto *from = dyn_cast<CallInst> (call))
		cast<CallInst> (direct)->setTailCallKind (from->getTailCallKind ());

	call->replaceAllUsesWith (direct);
	call->eraseFromParent ();
}

/// Points every call that reads \p site at \p entry instead.
void
enter_directly (CallBase *site, Function *entry, bool steps_receiver, bool drops_key)
{
	if (!steps_receiver && !drops_key) {
		site->replaceAllUsesWith (entry);
		return;
	}

	SmallVector<CallBase *, 2> calls;

	for (User *user : site->users ())
		calls.push_back (cast<CallBase> (user));

	for (CallBase *call : calls)
		enter_at (call, entry, steps_receiver, drops_key);
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

	/*
	 * decl is not this function's any more. Naming folds it into a declaration
	 * the module already held for the method and erases the loser, so reading
	 * it back - even to see whether it is worth erasing - is a read of freed
	 * memory. What is left standing when the publish fails is a declaration
	 * nothing calls, which no relocation and no code comes of.
	 */
	Function *entry = compile.publish (*decl, target);

	/*
	 * A declaration the module already held carries the shape its own site
	 * calls it with, and this site's shape is what the IL settled here. The two
	 * agree for an override, which has the signature it overrides. Where they
	 * do not, the site keeps its lookup rather than calling one prototype
	 * through the other.
	 */
	if (entry != nullptr && entry->getFunctionType () != shape)
		return nullptr;

	return entry;
}

/// Which declaration a set of sites calls, which decides where the method comes
/// from and whether the calls carry a key to drop.
enum class Lookup { vtable, imt, generic_virtual };

/// Answers what it can of the sites in \p f that call \p decl.
bool
answer_sites (Function &f, Function *decl, const CompileState &compile, Lookup lookup)
{
	if (decl == nullptr)
		return false;

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

		MonoMethod *target = nullptr;

		if (lookup == Lookup::vtable) {
			target = slot_target (klass,
			                      static_cast<int32_t> (index->getSExtValue ()));
		} else {
			auto *key = dyn_cast<GlobalValue> (site->getArgOperand (2));
			MonoMethod *asked =
				key != nullptr ? marked_method_pointer (*key) : nullptr;

			// The key is the last argument, and what the call enters is the
			// method's own prototype, which does not have it.
			if (asked == nullptr || shape->getNumParams () < 1)
				continue;

			target = lookup == Lookup::imt ? imt_target (klass, asked)
			                               : generic_virtual_target (klass, asked);
			shape = FunctionType::get (shape->getReturnType (),
			                           shape->params ().drop_back (),
			                           shape->isVarArg ());
		}

		if (target == nullptr)
			continue;

		Function *entry = entry_for (*f.getParent (), target, shape, compile);

		if (entry == nullptr)
			continue;

		/*
		 * The entry replaces the value the site answered rather than the calls
		 * that read it. Each of those is then a call of a constant, which the
		 * simplification behind this pass turns into a direct one. A receiver
		 * the entry wants stepped and a key it does not take are the two cases
		 * that have to reach the calls.
		 */
		enter_directly (site, entry, publishes_unbox_entry (target),
		                lookup != Lookup::vtable);
		site->eraseFromParent ();
		changed = true;
	}

	return changed;
}

} // namespace

PreservedAnalyses
DevirtualizePass::run (Function &f, FunctionAnalysisManager &)
{
	const CompileState &compile = current_compile ();
	Module &m = *f.getParent ();

	if (compile.domain == nullptr || !compile.publish)
		return PreservedAnalyses::all ();

	bool changed =
		answer_sites (f, m.getFunction (vtable_func_name), compile, Lookup::vtable);

	changed |= answer_sites (f, m.getFunction (imt_func_name), compile, Lookup::imt);
	changed |= answer_sites (f, m.getFunction (vtable_gfunc_name), compile,
	                         Lookup::generic_virtual);

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono
