/**
 * \file
 * \brief The `mono.vtable.func` declaration, and the load it lowers to.
 *
 * The layout comes from mono's own headers: a slot is a word into
 * `MonoVTable.vtable`. What the declaration carries is what no header states,
 * which is that the two operands settle the callee between them.
 */

#include "vtable-func.hpp"

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

Function *
vtable_func_decl (Module &m)
{
	if (Function *existing = m.getFunction (vtable_func_name))
		return existing;

	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);
	Function *decl = Function::Create (
		FunctionType::get (ptr, { ptr, Type::getInt32Ty (c) }, false),
		GlobalValue::ExternalLinkage, vtable_func_name, m);

	/*
	 * `memory(none)` rather than a read, and the slot does change under it: it
	 * holds a vcall trampoline until the first call patches the entry in, and an
	 * override moves it again. Every value it takes enters the method the site
	 * named, so a reader that keeps an older one calls the same method.
	 *
	 * `speculatable` holds because the vtable operand is a load off the
	 * receiver, which stands under the null check on it. A slot read off a
	 * vtable that exists cannot fault.
	 */
	decl->setDoesNotAccessMemory ();
	decl->setDoesNotThrow ();
	decl->addFnAttr (Attribute::WillReturn);
	decl->addFnAttr (Attribute::Speculatable);
	return decl;
}

namespace {

void
lower (CallBase *site)
{
	/*
	 * The declaration cannot unwind, so the translator writes a call. An invoke
	 * has destinations this rewrite does not carry over, and erasing one leaves
	 * a block with no terminator.
	 */
	if (isa<InvokeInst> (site))
		report_fatal_error (Twine (vtable_func_name) + " was called by an invoke");

	IRBuilder<> b (site);

	/*
	 * The translator writes a constant slot, and the slot reaching here is a
	 * value all the same: SimplifyCFG sinks two calls that differ in one operand
	 * into their common successor and gives that operand a phi. The builder
	 * folds the arithmetic back to a constant offset wherever the slot still is
	 * one.
	 *
	 * Signed, because mono_method_get_vtable_index () answers -1 for a method
	 * that has no slot. A caller that asks anyway gets the read it wrote, which
	 * is what it got when this site was arithmetic.
	 */
	Value *index = b.CreateSExt (site->getArgOperand (1), b.getInt64Ty ());
	Value *offset = b.CreateAdd (
		b.CreateMul (index, b.getInt64 (sizeof (void *))),
		b.getInt64 (MONO_STRUCT_OFFSET (MonoVTable, vtable)));
	Value *slot = b.CreateGEP (b.getInt8Ty (), site->getArgOperand (0), offset);
	Value *entry = b.CreateAlignedLoad (PointerType::get (site->getContext (), 0), slot,
	                                    Align (sizeof (void *)));

	site->replaceAllUsesWith (entry);
	site->eraseFromParent ();
}

} // namespace

PreservedAnalyses
LowerVTableFuncPass::run (Module &m, ModuleAnalysisManager &)
{
	Function *decl = m.getFunction (vtable_func_name);

	if (decl == nullptr)
		return PreservedAnalyses::all ();

	SmallVector<CallBase *, 8> sites;

	for (User *user : decl->users ())
		if (auto *site = dyn_cast<CallBase> (user))
			sites.push_back (site);

	for (CallBase *site : sites)
		lower (site);

	/* Anything left is a use this lowering does not understand. */
	if (!decl->use_empty ())
		report_fatal_error (Twine ("unlowered use of ") + vtable_func_name);
	decl->eraseFromParent ();

	return PreservedAnalyses::none ();
}

} // namespace mono
