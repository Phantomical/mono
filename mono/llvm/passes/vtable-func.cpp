/**
 * \file
 * \brief The `mono.vtable.func` declaration, and the load it lowers to.
 *
 * The layout comes from mono's own headers: a slot is a word into
 * `MonoVTable.vtable`. What the declaration carries is what no header states,
 * which is that the two operands settle the callee between them.
 */

#include "vtable-func.hpp"

#include "mini-runtime.h"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {

namespace {

/// Puts on decl the attributes every one of these declarations carries.
///
/// `memory(none)` rather than a read, and the slot does change under it: it
/// holds a trampoline until the first call patches the entry in, and an
/// override moves it again. Every value it takes enters the method the site
/// named, so a reader that keeps an older one calls the same method.
///
/// `speculatable` holds because the vtable operand is a load off the receiver,
/// which stands under the null check on it. A slot read off a vtable that
/// exists cannot fault.
Function *
describe_slot_read (Function *decl)
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
	if (Function *existing = m.getFunction (vtable_func_name))
		return existing;

	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);

	return describe_slot_read (Function::Create (
		FunctionType::get (ptr, { ptr, Type::getInt32Ty (c) }, false),
		GlobalValue::ExternalLinkage, vtable_func_name, m));
}

Function *
imt_func_decl (Module &m)
{
	if (Function *existing = m.getFunction (imt_func_name))
		return existing;

	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);

	return describe_slot_read (Function::Create (
		FunctionType::get (ptr, { ptr, Type::getInt32Ty (c), ptr }, false),
		GlobalValue::ExternalLinkage, imt_func_name, m));
}

namespace {

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
	/*
	 * The declaration cannot unwind, so the translator writes a call. An invoke
	 * has destinations this rewrite does not carry over, and erasing one leaves
	 * a block with no terminator.
	 */
	if (isa<InvokeInst> (site))
		report_fatal_error (Twine (site->getCalledFunction ()->getName ())
		                    + " was called by an invoke");

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

/// Lowers every call to the declaration \p name holds in m, and erases it.
void
lower_all (Module &m, StringRef name, int64_t first_word, int64_t slot_bias)
{
	Function *decl = m.getFunction (name);

	if (decl == nullptr)
		return;

	SmallVector<CallBase *, 8> sites;

	for (User *user : decl->users ())
		if (auto *site = dyn_cast<CallBase> (user))
			sites.push_back (site);

	for (CallBase *site : sites)
		lower (site, first_word, slot_bias);

	/* Anything left is a use this lowering does not understand. */
	if (!decl->use_empty ())
		report_fatal_error (Twine ("unlowered use of ") + name);
	decl->eraseFromParent ();
}

} // namespace

PreservedAnalyses
LowerVTableFuncPass::run (Module &m, ModuleAnalysisManager &)
{
	if (m.getFunction (vtable_func_name) == nullptr
	    && m.getFunction (imt_func_name) == nullptr)
		return PreservedAnalyses::all ();

	lower_all (m, vtable_func_name, MONO_STRUCT_OFFSET (MonoVTable, vtable), 0);

	// The table sits in the words before the MonoVTable, so a slot counts back
	// from the base rather than on from the method array.
	lower_all (m, imt_func_name, 0, -MONO_IMT_SIZE);

	return PreservedAnalyses::none ();
}

} // namespace mono
