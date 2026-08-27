/**
 * \file
 * \brief The declarations a vtable is read through, and the loads they lower to.
 *
 * The layout comes from mono's own headers: a slot is a word into
 * `MonoVTable.vtable`, and each field sits where `MonoVTable` puts it. What a
 * declaration carries is what no header states, which is that its operands
 * settle what the read gives between them.
 */

#include "vtable-func.hpp"

#include "builtins.hpp"

#include "mini-runtime.h"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {

namespace {

/// Puts on decl the attributes every read off a vtable carries.
///
/// `memory(none)` rather than a read. A slot does change under it: it holds a
/// trampoline until the first call patches the entry in, and an override moves
/// it again. Every value it takes enters the method the site named, so a reader
/// that keeps an older one calls the same method. The class, the type and the
/// rank take their values while `mono_class_create_runtime_vtable ()` builds the
/// vtable. That is before compiled code can hold the vtable to read them.
///
/// `speculatable` holds because the vtable operand is a load off the receiver,
/// which stands under the null check on it. A read off a vtable that exists
/// cannot fault.
Function *
describe_vtable_read (Function *decl)
{
	decl->setDoesNotAccessMemory ();
	decl->setDoesNotThrow ();
	decl->addFnAttr (Attribute::WillReturn);
	decl->addFnAttr (Attribute::Speculatable);
	return decl;
}

} // namespace

Function *
vtable_func_decl (Module &m)
{
	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);

	return describe_vtable_read (builtin_decl (
		m, vtable_func_name,
		FunctionType::get (ptr, { ptr, Type::getInt32Ty (c) }, false)));
}

Function *
imt_func_decl (Module &m)
{
	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);

	return describe_vtable_read (builtin_decl (
		m, imt_func_name,
		FunctionType::get (ptr, { ptr, Type::getInt32Ty (c), ptr }, false)));
}

Function *
vtable_gfunc_decl (Module &m)
{
	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);

	return describe_vtable_read (builtin_decl (
		m, vtable_gfunc_name,
		FunctionType::get (ptr, { ptr, Type::getInt32Ty (c), ptr }, false)));
}

Function *
vtable_klass_decl (Module &m)
{
	Type *ptr = PointerType::get (m.getContext (), 0);

	return describe_vtable_read (
		builtin_decl (m, vtable_klass_name, FunctionType::get (ptr, { ptr }, false)));
}

Function *
vtable_type_decl (Module &m)
{
	Type *ptr = PointerType::get (m.getContext (), 0);

	return describe_vtable_read (
		builtin_decl (m, vtable_type_name, FunctionType::get (ptr, { ptr }, false)));
}

Function *
vtable_rank_decl (Module &m)
{
	LLVMContext &c = m.getContext ();

	return describe_vtable_read (builtin_decl (
		m, vtable_rank_name,
		FunctionType::get (Type::getInt8Ty (c), { PointerType::get (c, 0) }, false)));
}

namespace {

/// Fails the process where site is an invoke.
///
/// Every declaration here is nounwind, so the translator writes a call. An
/// invoke has destinations a rewrite does not carry over, and erasing one leaves
/// a block with no terminator.
void
refuse_invoke (CallBase *site)
{
	if (isa<InvokeInst> (site))
		report_fatal_error (Twine (site->getCalledFunction ()->getName ())
		                    + " was called by an invoke");
}

/// Rewrites site into the load it stands for, \p first_word bytes from the
/// vtable and \p slot_words words on from there.
///
/// The translator writes a constant slot, and the slot reaching here is a value
/// all the same: SimplifyCFG sinks two calls that differ in one operand into
/// their common successor and gives that operand a phi. The builder folds the
/// arithmetic back to a constant offset wherever the slot still is one.
///
/// Signed, because a vtable index of -1 is what a method with no slot of its
/// own carries, and an IMT slot counts back from the vtable. A caller that asks
/// either way gets the read it wrote, which is what it got when this site was
/// arithmetic.
void
lower (CallBase *site, int64_t first_word, int64_t slot_bias)
{
	refuse_invoke (site);

	IRBuilder<> b (site);
	Value *index = b.CreateSExt (site->getArgOperand (1), b.getInt64Ty ());
	Value *offset = b.CreateAdd (
		b.CreateMul (b.CreateAdd (index, b.getInt64 (slot_bias)),
	                     b.getInt64 (sizeof (void *))),
		b.getInt64 (first_word));
	Value *slot = b.CreateGEP (b.getInt8Ty (), site->getArgOperand (0), offset);
	Value *entry = b.CreateAlignedLoad (PointerType::get (site->getContext (), 0), slot,
	                                    Align (sizeof (void *)));

	site->replaceAllUsesWith (entry);
	site->eraseFromParent ();
}

/// Rewrites site into the load of the field at \p offset, as wide as the site's
/// own result.
///
/// The load is `!invariant.load`, which is the same claim the declaration's
/// `memory(none)` makes: each field takes its value while the vtable is built.
void
lower_field (CallBase *site, int64_t offset)
{
	refuse_invoke (site);

	IRBuilder<> b (site);
	Type *held = site->getType ();
	Value *at =
		b.CreateGEP (b.getInt8Ty (), site->getArgOperand (0), b.getInt64 (offset));
	LoadInst *value = b.CreateAlignedLoad (
		held, at, site->getModule ()->getDataLayout ().getABITypeAlign (held));

	value->setMetadata (LLVMContext::MD_invariant_load,
	                    MDNode::get (site->getContext (), {}));
	site->replaceAllUsesWith (value);
	site->eraseFromParent ();
}

/// Lowers every call to the declaration \p name holds in m with \p rewrite,
/// erases the declaration, and says whether it was there.
bool
lower_all (Module &m, StringRef name, function_ref<void (CallBase *)> rewrite)
{
	for (CallBase *site : builtin_sites (m, name))
		rewrite (site);

	return erase_builtin (m, name);
}

} // namespace

bool
lower_vtable_reads (Module &m)
{
	bool changed = lower_all (m, vtable_func_name, [] (CallBase *site) {
		lower (site, MONO_STRUCT_OFFSET (MonoVTable, vtable), 0);
	});

	// The table sits in the words before the MonoVTable, so a slot counts back
	// from the base rather than on from the method array.
	changed |= lower_all (m, imt_func_name,
	                      [] (CallBase *site) { lower (site, 0, -MONO_IMT_SIZE); });

	// The key is the trampoline's to read out of its own register, so the load
	// this leaves is the plain slot read.
	changed |= lower_all (m, vtable_gfunc_name, [] (CallBase *site) {
		lower (site, MONO_STRUCT_OFFSET (MonoVTable, vtable), 0);
	});

	changed |= lower_all (m, vtable_klass_name, [] (CallBase *site) {
		lower_field (site, MONO_STRUCT_OFFSET (MonoVTable, klass));
	});

	changed |= lower_all (m, vtable_type_name, [] (CallBase *site) {
		lower_field (site, MONO_STRUCT_OFFSET (MonoVTable, type));
	});

	changed |= lower_all (m, vtable_rank_name, [] (CallBase *site) {
		lower_field (site, MONO_STRUCT_OFFSET (MonoVTable, rank));
	});

	return changed;
}

} // namespace mono
