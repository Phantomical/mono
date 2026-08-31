/**
 * \file
 * \brief The live half of `class_has_no_cctor ()`'s old job.
 *
 * Two things in the IR name a class-init check: the call `emit_class_init ()`
 * always emits now, tagged `class_init_attribute`, and the reload
 * `push_guarded_static_read ()` always emits beside it to re-read
 * `MonoVTable::initialized` (the front end never trusts the call above to
 * mean the flag is set - see that function for why). Both used to be left out
 * by the front end itself when the class was already warm; both are always
 * there now, and this pass is where a warm class still gets them taken back
 * out, once removing them can no longer change what either tier's hash
 * covers.
 */

#include "class-init-warm.hpp"
#include "class-init.hpp"

#include "analysis/strip-casts.hpp"
#include "compile-state.hpp"
#include "method-symbols.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"

using namespace llvm;

namespace mono {

bool
class_is_initialized (MonoDomain *domain, MonoClass *klass)
{
	MonoVTable *vtable = mono_class_try_get_vtable (domain, klass);

	return vtable != nullptr && vtable->initialized;
}

namespace {

/// The class a value naming a vtable stands for, or null where this is not
/// that shape: a symbol `class_symbol ()` marked, seen through whatever casts
/// carry it from an operand to the symbol itself.
MonoClass *
vtable_class (const Value *v)
{
	const auto *global = dyn_cast<GlobalVariable> (strip_casts (v));

	return global != nullptr ? marked_class (*global) : nullptr;
}

/// Removes site, which must return nothing - the same call every user of
/// class_init_attribute makes of it. If site is an invoke, its normal edge
/// becomes an unconditional branch, the way a call's would already be one.
void
erase_class_init_call (CallBase *site)
{
	if (auto *invoke = dyn_cast<InvokeInst> (site)) {
		invoke->getUnwindDest ()->removePredecessor (invoke->getParent ());
		IRBuilder<> (invoke).CreateBr (invoke->getNormalDest ());
	}

	site->eraseFromParent ();
}

/// Whether load is push_guarded_static_read ()'s reload of
/// MonoVTable::initialized, off a vtable this compile can name a class for.
///
/// The base is a global rather than an instruction: a getelementptr with a
/// constant base and a constant index is itself a constant, never a separate
/// instruction, in every pipeline this runs behind.
MonoClass *
cctor_finished_read_class (const LoadInst &load)
{
	if (!load.getType ()->isIntegerTy (8))
		return nullptr;

	const auto *gep = dyn_cast<GEPOperator> (load.getPointerOperand ());

	if (gep == nullptr || !gep->hasAllConstantIndices () || gep->getNumIndices () != 1)
		return nullptr;

	auto *offset = cast<ConstantInt> (gep->idx_begin ()->get ());

	if (offset->getSExtValue () != MONO_STRUCT_OFFSET (MonoVTable, initialized))
		return nullptr;

	return vtable_class (gep->getPointerOperand ());
}

} // namespace

PreservedAnalyses
ClassInitWarmPass::run (Function &f, FunctionAnalysisManager &)
{
	MonoDomain *domain = current_compile ().domain;

	if (domain == nullptr)
		return PreservedAnalyses::all ();

	SmallVector<CallBase *, 4> dead_calls;

	for (Function &decl : f.getParent ()->functions ()) {
		if (!decl.isDeclaration () || !decl.hasFnAttribute (class_init_attribute))
			continue;

		for (User *user : decl.users ()) {
			auto *site = dyn_cast<CallBase> (user);

			if (site == nullptr || site->getFunction () != &f
			    || site->getCalledFunction () != &decl || site->arg_size () < 1)
				continue;

			MonoClass *klass = vtable_class (site->getArgOperand (0));

			if (klass != nullptr && class_is_initialized (domain, klass))
				dead_calls.push_back (site);
		}
	}

	SmallVector<LoadInst *, 4> warm_reads;

	for (Instruction &i : instructions (f))
		if (auto *load = dyn_cast<LoadInst> (&i)) {
			MonoClass *klass = cctor_finished_read_class (*load);

			if (klass != nullptr && class_is_initialized (domain, klass))
				warm_reads.push_back (load);
		}

	if (dead_calls.empty () && warm_reads.empty ())
		return PreservedAnalyses::all ();

	for (CallBase *site : dead_calls)
		erase_class_init_call (site);

	// The flag is a byte, and any nonzero value reads as "finished" - see the
	// icmp push_guarded_static_read () builds beside this load. Later passes
	// take it from here: SimplifyCFG folds a branch this makes constant, and
	// the arm it drops takes llvm.invariant.start's block with it.
	for (LoadInst *load : warm_reads) {
		load->replaceAllUsesWith (ConstantInt::get (load->getType (), 1));
		load->eraseFromParent ();
	}

	return PreservedAnalyses::none ();
}

} // namespace mono
