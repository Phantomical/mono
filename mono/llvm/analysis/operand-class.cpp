/**
 * \file
 * \brief Writing a value's class beside it, and reading it back.
 */

#include "operand-class.hpp"

#include "compile-state.hpp"
#include "method-symbols.hpp"
#include "passes/alloc-func.hpp"
#include "strip-casts.hpp"
#include "constant-values.hpp"

#include "mono/metadata/class.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalObject.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/ModRef.h>

#include <cstdint>
#include <optional>

using namespace llvm;

namespace mono {
namespace {

Metadata *
as_metadata (LLVMContext &c, const void *pointer)
{
	return ConstantAsMetadata::get (ConstantInt::get (
		Type::getInt64Ty (c), reinterpret_cast<uintptr_t> (pointer)));
}

/// The pointer at \p at in \p node, or null where the node holds none there.
void *
pointer_in (const MDNode *node, unsigned at)
{
	if (node == nullptr || at >= node->getNumOperands ())
		return nullptr;

	auto *held = mdconst::dyn_extract<ConstantInt> (node->getOperand (at));

	if (held == nullptr)
		return nullptr;

	return reinterpret_cast<void *> (static_cast<uintptr_t> (held->getZExtValue ()));
}

MonoClass *
class_in (const MDNode *node, unsigned at)
{
	return static_cast<MonoClass *> (pointer_in (node, at));
}

/// The method \p site was marked with under \p kind, or null where it carries
/// no such mark.
MonoMethod *
marked_with (const Instruction &site, StringRef kind)
{
	return static_cast<MonoMethod *> (pointer_in (site.getMetadata (kind), 0));
}

/*
 * Below is the second channel: a read of an initonly static, recognized by its
 * shape rather than by a mark.
 *
 * The translator marks the load it writes, and that mark does not survive.
 * InstCombine folds the address GEP into the load's pointer operand, which
 * builds a new load, and only a field at offset 0 has no GEP to fold. What the
 * shape carries instead is a marked global and a constant offset, and both
 * outlive every pass because no instruction-level transform rebuilds a global.
 *
 * Reading the field here rather than at translation is also what lets a load
 * answer that the translator could not: one a pass rematerialized, and one
 * inlined from a body compiled before the class initializer had run.
 */

/// The class \p held is an instance of, or null where a compile cannot state one.
///
/// A transparent proxy stands in for another class and carries a vtable that is
/// not that class's.
MonoClass *
settled_class_of (MonoObject *held)
{
	MonoClass *klass = mono_object_class (held);

	return mono_class_is_marshalbyref (klass) ? nullptr : klass;
}

/// The method the delegate \p held calls, or null where \p held is not a
/// delegate whose target this compile can name.
///
/// MonoDelegate::method is the field to read. mini_init_delegate () writes it
/// once, at construction, and no other path writes it at all. method_ptr is not
/// an alternative: the delegate trampoline replaces it on the first call and can
/// put an unbox entry there, so a value read now is not the one a later call
/// uses.
///
/// Two delegates name a method they do not enter. One built from `ldvirtftn`
/// resolves an override when it is called, and a multicast one runs an
/// invocation list instead of a target.
MonoMethod *
settled_delegate_target_of (MonoObject *held)
{
	if (m_class_get_parent (mono_object_class (held))
	    != mono_defaults.multicastdelegate_class)
		return nullptr;

	auto *delegate = (MonoDelegate *) held;

	if (delegate->method_is_virtual
	    || ((MonoMulticastDelegate *) delegate)->delegates != nullptr)
		return nullptr;

	return delegate->method;
}

/// The static field of \p klass that lives at \p offset in its statics block,
/// or null where no field of that class is declared there.
MonoClassField *
static_field_at (MonoClass *klass, int offset)
{
	gpointer iter = nullptr;

	while (MonoClassField *field = mono_class_get_fields_internal (klass, &iter)) {
		uint32_t flags = mono_field_get_flags (field);

		// A literal is a metadata constant with no storage, so it holds an
		// offset that says nothing and can stand where a real field is.
		if ((flags & FIELD_ATTRIBUTE_STATIC) == 0
		    || (flags & FIELD_ATTRIBUTE_LITERAL) != 0)
			continue;

		if (m_field_get_offset (field) == offset)
			return field;
	}

	return nullptr;
}

/// The object \p field holds, or null where this compile cannot read one.
///
/// `initonly` makes the class initializer the only writer that IL has, so the
/// value read once that initializer has run is the value the field keeps. What
/// a caller may state from it is what stays right while the collector moves the
/// object, so no address is written down.
///
/// Reflection is the writer IL does not have.
/// `mono_field_static_set_value_internal ()` refuses a literal and nothing else,
/// and no compiled body is taken back when a field is written that way. A
/// program that does it reads a stale object here.
MonoObject *
initonly_static_value (MonoDomain *domain, MonoClassField *field)
{
	if (!MONO_TYPE_IS_REFERENCE (mono_field_get_type_internal (field)))
		return nullptr;

	ERROR_DECL (vtable_error);
	MonoVTable *vtable = mono_class_vtable_checked (domain, field->parent, vtable_error);

	if (vtable == nullptr) {
		mono_error_cleanup (vtable_error);
		return nullptr;
	}

	/*
	 * The flag goes on once the initializer has run. A body compiled before
	 * that reads whatever the field holds part way through, which the rest of
	 * the initializer is free to replace.
	 */
	if (!vtable->initialized)
		return nullptr;

	return *(MonoObject **) ((char *) mono_vtable_get_static_field_data (vtable)
	                         + field->offset);
}

/// The object \p v reads out of an initonly static, or null where \p v is not
/// such a read or this compile cannot answer for it.
MonoObject *
initonly_static_read (const Value *v)
{
	MonoClassField *field = initonly_static_field (v);

	return field != nullptr ? initonly_static_value (current_compile ().domain, field)
	                        : nullptr;
}

/// The class an allocation site makes, read off the vtable its first operand
/// names, or null where \p site is not one of the object-allocation builtins,
/// that operand names no class this compile can read, or the vtable does not
/// settle what comes back.
///
/// `mono.exact.class` is the ordinary way an allocation states its class, and
/// it can go missing: `changeToInvokeAndSplitBasicBlock ()`
/// (`llvm/lib/Transforms/Utils/Local.cpp`) is what `InlineFunction ()` calls
/// to turn a folded call into an invoke when the call it was folded into sits
/// in a protected region, and it copies only the debug location, the calling
/// convention, the attributes and `MD_prof` onto the new instruction. The
/// vtable operand is not metadata, so it survives that rewrite, and it names
/// the same class the missing mark would have.
///
/// A marshalbyref or COM class is the one case where the vtable operand does
/// not settle it: `mono_object_new_specific_checked ()` can answer such an
/// allocation with a transparent proxy instead, and a proxy carries a vtable
/// of its own. `MethodLLVMEmitter::allocation_can_be_a_proxy ()`
/// (`method-to-llvm/call.cpp`) is the same rule, read here without the
/// translator: it is two calls into class metadata, not a fact this compile
/// only knows while it is translating.
MonoClass *
allocation_class (const CallBase &site)
{
	const Function *callee = site.getCalledFunction ();

	if (callee == nullptr)
		return nullptr;

	StringRef name = callee->getName ();

	if (name != alloc_object_name && name != alloc_object_kept_name)
		return nullptr;

	const auto *vtable =
		dyn_cast<GlobalObject> (site.getArgOperand (0)->stripPointerCasts ());

	if (vtable == nullptr)
		return nullptr;

	MonoClass *klass = marked_class (*vtable);

	if (klass == nullptr
	    || mono_class_is_marshalbyref (klass) || mono_class_is_com_object (klass))
		return nullptr;

	return klass;
}

std::pair<MonoClass *, bool>
leaf_operand_class (const Value *v, const Function &f)
{
	if (const auto *site = dyn_cast<Instruction> (v)) {
		if (MonoClass *marked = class_in (site->getMetadata (exact_class_md), 0))
			return { marked, true };

		if (MonoObject *held = initonly_static_read (site))
			return { settled_class_of (held), true };

		if (const auto *call = dyn_cast<CallBase> (site))
			if (MonoClass *klass = allocation_class (*call))
				return { klass, true };

		return { nullptr, true };
	}

	const auto *arg = dyn_cast<Argument> (v);

	if (arg == nullptr || arg->getParent () != &f)
		return { nullptr, false };

	const MDNode *listed = f.getMetadata (param_classes_md);

	if (listed == nullptr)
		return { nullptr, false };

	for (const MDOperand &entry : listed->operands ()) {
		const auto *pair = dyn_cast<MDNode> (entry);

		if (pair == nullptr || pair->getNumOperands () != 2)
			continue;

		auto *index = mdconst::dyn_extract<ConstantInt> (pair->getOperand (0));

		if (index == nullptr || index->getZExtValue () != arg->getArgNo ())
			continue;

		// A declared type is an upper bound. Every class the slot admits is
		// assignable to it, and none of them has to be it.
		return { class_in (pair, 1), false };
	}

	return { nullptr, false };
}

/*
 * Below are the three class queries. Each folds `leaf_operand_class ()` over
 * the values `MonoConstantValues` says reach the operand, and they differ only
 * in how they read a null source and an incomplete set.
 */

/// How strong an answer the caller needs, which is what decides how the rule
/// below reads a path it cannot see the end of.
enum class ClassRule {
	/// The answer has to hold on every path, and a null is one of the values
	/// a path can carry. `operand_class ()`.
	settled,

	/// The caller dereferences the answer, so a null path faults in front of
	/// the use and the answer never has to cover it. `exact_class ()`.
	dereferenced,

	/// The caller compares against the answer before it acts on it, so a path
	/// this was wrong about costs it nothing. `guessed ()`.
	guessed,
};

/// Reads the class \p v holds under \p rule, and whether that is the class it
/// is rather than a bound on it. No class where nothing settles one.
std::pair<MonoClass *, bool>
class_of (Value *v, const Function &f, ClassRule rule, const ConstantValues &values)
{
	const ValueSources &from = values.sources (v);
	std::pair<MonoClass *, bool> agreed { nullptr, false };
	bool constrained = false;

	for (const Value *source : from.sources) {
		if (rule != ClassRule::settled && isa<ConstantPointerNull> (source))
			continue;

		std::pair<MonoClass *, bool> held = leaf_operand_class (source, f);

		// A guess names a class this function saw an object made under. A
		// parameter's declared class is a bound instead, so a compare
		// against it misses every subclass the slot admits.
		if (rule == ClassRule::guessed && !held.second)
			continue;

		if (held.first == nullptr) {
			// A guess is one arm of a compare the caller writes, so a
			// source with no class is one more path that compare covers.
			if (rule == ClassRule::guessed)
				continue;

			return { nullptr, false };
		}

		// Two sources that name two classes settle nothing. One compare
		// picks one class, and picking either leaves the other's whole count
		// on the dispatch.
		if (constrained && agreed.first != held.first)
			return { nullptr, false };

		agreed = { held.first, (!constrained || agreed.second) && held.second };
		constrained = true;
	}

	return agreed;
}

} // namespace

MonoClassField *
initonly_static_field (const Value *v)
{
	const auto *load = dyn_cast<LoadInst> (v);

	if (load == nullptr || current_compile ().domain == nullptr)
		return nullptr;

	const DataLayout &layout = load->getModule ()->getDataLayout ();
	const Value *address = load->getPointerOperand ();
	APInt offset (layout.getIndexTypeSizeInBits (address->getType ()), 0);
	const auto *block = dyn_cast<GlobalValue> (address->stripAndAccumulateConstantOffsets (
		layout, offset, /*AllowNonInbounds=*/true));

	if (block == nullptr || offset.isNegative () || !offset.isSignedIntN (32))
		return nullptr;

	MonoClass *klass = marked_statics_class (*block);

	if (klass == nullptr)
		return nullptr;

	MonoClassField *field =
		static_field_at (klass, static_cast<int> (offset.getSExtValue ()));

	if (field == nullptr || (mono_field_get_flags (field) & FIELD_ATTRIBUTE_INIT_ONLY) == 0)
		return nullptr;

	// A special static lives per thread or per context, so what stands there
	// now says nothing about what a compiled body will read.
	return field->offset >= 0 ? field : nullptr;
}

void
mark_exact_class (Instruction &site, MonoClass *klass)
{
	LLVMContext &c = site.getContext ();

	site.setMetadata (exact_class_md, MDNode::get (c, { as_metadata (c, klass) }));
}

void
mark_parameter_classes (Function &f, ArrayRef<std::pair<unsigned, MonoClass *>> classes)
{
	if (classes.empty ())
		return;

	LLVMContext &c = f.getContext ();
	SmallVector<Metadata *, 8> pairs;

	for (const auto &entry : classes)
		pairs.push_back (MDNode::get (
			c, { ConstantAsMetadata::get (
				     ConstantInt::get (Type::getInt32Ty (c), entry.first)),
		             as_metadata (c, entry.second) }));

	f.setMetadata (param_classes_md, MDNode::get (c, pairs));
}

std::pair<MonoClass *, bool>
stated_class (const Value *v, const Function &f)
{
	return leaf_operand_class (strip_casts (v), f);
}

std::pair<MonoClass *, bool>
operand_class (Value *v, const Function &f, const ConstantValues &values)
{
	return class_of (v, f, ClassRule::settled, values);
}

MonoClass *
exact_class (Value *v, const Function &f, const ConstantValues &values)
{
	auto [klass, exact] = class_of (v, f, ClassRule::dereferenced, values);

	if (klass == nullptr || exact)
		return klass;

	return bound_is_exact (klass) ? klass : nullptr;
}

bool
bound_is_exact (MonoClass *klass)
{
	/*
	 * An array is marked sealed and is still not exact. A slot admits every
	 * array of that rank with the same cast class, and each of those carries a
	 * vtable of its own, so `int[]`, `uint[]` and an array of an enum over int
	 * reach three different interface slots.
	 *
	 * Narrowing this to the elements the width fold leaves alone does not work
	 * either. An enum's cast class is its underlying type, and the loader takes
	 * that type off the first instance field without a check -
	 * `mono_class_is_valid_enum ()` has one caller, in `sre.c`. So an image can
	 * declare an enum over any type at all and put its array in the set.
	 * `mono/tests/array-devirt.cs` gates the answer.
	 */
	return m_class_is_sealed (klass) && m_class_get_rank (klass) == 0
	       && !mono_class_is_marshalbyref (klass);
}

MonoClass *
guessed_class (Value *v, const Function &f, const ConstantValues &values)
{
	// Every leaf this rule reads answers exactly, so a merge of them does too.
	return class_of (v, f, ClassRule::guessed, values).first;
}

void
mark_delegate_target (Instruction &site, MonoMethod *target)
{
	LLVMContext &c = site.getContext ();

	site.setMetadata (delegate_target_md, MDNode::get (c, { as_metadata (c, target) }));
}

MonoMethod *
delegate_target (const Value *v)
{
	const auto *site = dyn_cast<Instruction> (strip_casts (v));

	if (site == nullptr)
		return nullptr;

	if (MonoMethod *marked = marked_with (*site, delegate_target_md))
		return marked;

	MonoObject *held = initonly_static_read (site);

	return held != nullptr ? settled_delegate_target_of (held) : nullptr;
}

} // namespace mono
