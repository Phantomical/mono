/**
 * \file
 * \brief Writing a value's class beside it, and reading it back.
 */

#include "operand-class.hpp"

#include "compile-state.hpp"
#include "method-symbols.hpp"
#include "passes/strip-casts.hpp"

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
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

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
	MonoType *type = mono_field_get_type_internal (field);

	if ((mono_field_get_flags (field) & FIELD_ATTRIBUTE_INIT_ONLY) == 0
	    || !MONO_TYPE_IS_REFERENCE (type))
		return nullptr;

	// A special static lives per thread or per context, so what stands there
	// now says nothing about what a compiled body will read.
	if (field->offset < 0)
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
///
/// A shared body fetches its statics off the run-time context instead of naming
/// a block, so the marked global a match needs is the same thing that says the
/// class is closed.
MonoObject *
initonly_static_read (const Value *v)
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

	return field != nullptr ? initonly_static_value (current_compile ().domain, field)
	                        : nullptr;
}

/// The class \p v answers on its own, without looking through a `PHINode` or
/// a `SelectInst`. This is the class marked on \p v, the class an initonly
/// static read off it settles, or the class its parameter type bounds it
/// with.
std::pair<MonoClass *, bool>
leaf_operand_class (const Value *v, const Function &f)
{
	if (const auto *site = dyn_cast<Instruction> (v)) {
		if (MonoClass *marked = class_in (site->getMetadata (exact_class_md), 0))
			return { marked, true };

		if (MonoObject *held = initonly_static_read (site))
			return { settled_class_of (held), true };

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
 * Below is the walk that looks through a merge. operand_class () and
 * exact_class () are both one call into it, with the null rule set the way
 * each of them needs it.
 *
 * A phi that carries a value around a loop can name itself, directly or
 * through another phi. `visiting` names the values already on the call stack
 * for this walk. A value found there contributes nothing, and the walk does
 * not visit it again. This is sound by induction: if every incoming outside
 * the cycle names one class, the cycle itself never disagrees with that
 * class.
 *
 * This makes "found nothing" different from "found two classes that
 * disagree." Two disagreeing incomings settle the merge at no class. That
 * answer, unlike "found nothing", carries out to whatever merge called this
 * one, and settles it at no class too.
 *
 * "Found nothing" means the walk skipped every incoming, either by the cycle
 * rule above or by the null rule below. The merge that called this one can
 * still settle on a class from its own other incomings. `std::nullopt`
 * stands for "found nothing". Every other answer, no-class included, is
 * concrete. The two functions below this walk are the only place a
 * `std::nullopt` becomes a concrete answer. Their callers cannot read any
 * other kind of answer.
 */

/// Bounds how many merge nodes one walk visits, so a large phi web costs a
/// compile only a small, fixed amount of work.
constexpr unsigned merge_walk_budget = 24;

std::optional<std::pair<MonoClass *, bool>>
walk_operand_class (const Value *v, const Function &f, bool ignore_null,
                     SmallPtrSetImpl<const Value *> &visiting, unsigned &budget);

/// The class every value in \p arms agrees on. This answers no class,
/// concretely, when a contributing value answers no class itself, or when two
/// contributing values disagree. It answers "found nothing" when the walk
/// skipped every value in \p arms.
std::optional<std::pair<MonoClass *, bool>>
merged_class (ArrayRef<const Value *> arms, const Function &f, bool ignore_null,
              SmallPtrSetImpl<const Value *> &visiting, unsigned &budget)
{
	MonoClass *klass = nullptr;
	bool exact = true;
	bool any = false;

	for (const Value *arm : arms) {
		std::optional<std::pair<MonoClass *, bool>> got =
			walk_operand_class (arm, f, ignore_null, visiting, budget);

		if (!got)
			continue;

		if (got->first == nullptr || (any && got->first != klass))
			return std::make_pair (static_cast<MonoClass *> (nullptr), false);

		klass = got->first;
		exact = exact && got->second;
		any = true;
	}

	if (!any)
		return std::nullopt;

	return std::make_pair (klass, exact);
}

/// The class \p v answers with: \p v itself where it is not a phi or a
/// select, otherwise the merge of the values it names. The same outer walk
/// shares \p visiting and \p budget across every call it makes.
std::optional<std::pair<MonoClass *, bool>>
walk_operand_class (const Value *v, const Function &f, bool ignore_null,
                     SmallPtrSetImpl<const Value *> &visiting, unsigned &budget)
{
	v = strip_casts (v);

	if (visiting.count (v) || (ignore_null && isa<ConstantPointerNull> (v)))
		return std::nullopt;

	const auto *phi = dyn_cast<PHINode> (v);
	const auto *select = phi == nullptr ? dyn_cast<SelectInst> (v) : nullptr;

	if (phi == nullptr && select == nullptr)
		return leaf_operand_class (v, f);

	// A merge node this walk cannot afford to enter answers no class, not a
	// skip. Unlike the back edge of a cycle, nothing bounds what the
	// unexplored side of this merge can name.
	if (budget == 0)
		return std::make_pair (static_cast<MonoClass *> (nullptr), false);

	--budget;
	visiting.insert (v);

	std::optional<std::pair<MonoClass *, bool>> result;

	if (phi != nullptr) {
		SmallVector<const Value *, 4> incoming (
			phi->incoming_values ().begin (), phi->incoming_values ().end ());
		result = merged_class (incoming, f, ignore_null, visiting, budget);
	} else {
		const Value *arms[] = { select->getTrueValue (), select->getFalseValue () };
		result = merged_class (arms, f, ignore_null, visiting, budget);
	}

	visiting.erase (v);

	return result;
}

} // namespace

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
operand_class (const Value *v, const Function &f)
{
	SmallPtrSet<const Value *, 8> visiting;
	unsigned budget = merge_walk_budget;
	std::optional<std::pair<MonoClass *, bool>> found =
		walk_operand_class (v, f, /*ignore_null=*/false, visiting, budget);

	return found.value_or (std::make_pair (nullptr, false));
}

MonoClass *
exact_class (const Value *v, const Function &f)
{
	SmallPtrSet<const Value *, 8> visiting;
	unsigned budget = merge_walk_budget;
	std::optional<std::pair<MonoClass *, bool>> found =
		walk_operand_class (v, f, /*ignore_null=*/true, visiting, budget);
	auto [klass, exact] = found.value_or (std::make_pair (nullptr, false));

	if (klass == nullptr || exact)
		return klass;

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
	if (!m_class_is_sealed (klass) || m_class_get_rank (klass) != 0
	    || mono_class_is_marshalbyref (klass))
		return nullptr;

	return klass;
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
