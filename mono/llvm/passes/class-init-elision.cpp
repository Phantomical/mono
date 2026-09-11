#include "class-init-elision.hpp"
#include "compile-state.hpp"

#include "method-symbols.hpp"
#include "mono/llvm/analysis/strip-casts.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/MapVector.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>

namespace mono {
namespace {

bool
is_same_class (const llvm::CallBase *a, const llvm::CallBase *b)
{
	if (a->arg_size () != b->arg_size ())
		return false;

	for (unsigned i = 0; i < a->arg_size (); ++i)
		if (strip_casts (a->getArgOperand (i)) != strip_casts (b->getArgOperand (i)))
			return false;

	return true;
}

void
erase_call (llvm::CallBase *call)
{
	// If we have an invoke then replace it with an unconditional branch to its
	// normal successor.
	if (auto *invoke = llvm::dyn_cast<llvm::InvokeInst> (call)) {
		invoke->getUnwindDest ()->removePredecessor (invoke->getParent ());
		llvm::IRBuilder<> (invoke).CreateBr (invoke->getNormalDest ());
	}

	call->eraseFromParent ();
}

MonoClass *
get_vtable_class (llvm::Value *v)
{
	auto global = llvm::dyn_cast<llvm::GlobalVariable> (strip_casts (v));
	if (!global)
		return nullptr;

	return get_class (*global);
}

MonoClass *
get_init_check_class (llvm::LoadInst &load)
{
	if (!load.getType ()->isIntegerTy (8))
		return nullptr;

	auto *gep = llvm::dyn_cast<llvm::GEPOperator> (load.getPointerOperand ());
	if (gep == nullptr || !gep->hasAllConstantIndices () || gep->getNumIndices () != 1)
		return nullptr;

	auto *offset = llvm::cast<llvm::ConstantInt> (gep->idx_begin ()->get ());
	if (offset->getSExtValue () != MONO_STRUCT_OFFSET (MonoVTable, initialized))
		return nullptr;

	return get_vtable_class (gep->getPointerOperand ());
}

} // namespace

bool
is_class_initialized (MonoDomain *domain, MonoClass *klass)
{
	MonoVTable *vtable = mono_class_try_get_vtable (domain, klass);
	if (!vtable)
		return false;

	return vtable->initialized;
}

llvm::PreservedAnalyses
ClassInitDominatedElisionPass::run (llvm::Function &f, llvm::FunctionAnalysisManager &fam)
{
	llvm::DominatorTree *dt = nullptr;
	llvm::SmallMapVector<const llvm::Value *, llvm::SmallVector<llvm::CallBase *, 4>, 4> inits;
	unsigned total = 0;

	for (auto &decl : f.getParent ()->functions ()) {
		if (!decl.isDeclaration ())
			continue;
		if (!decl.hasFnAttribute (class_init_attribute))
			continue;

		for (auto user : decl.users ()) {
			auto site = llvm::dyn_cast<llvm::CallBase> (user);
			if (site == nullptr)
				continue;
			if (site->getFunction () != &f || site->getCalledFunction () != &decl)
				continue;
			if (site->arg_size () < 1 || !site->use_empty ())
				continue;

			if (!dt)
				dt = &fam.getResult<llvm::DominatorTreeAnalysis> (f);

			// Let DCE handle unreachable code.
			if (!dt->isReachableFromEntry (site->getParent ()))
				continue;

			inits[strip_casts (site->getArgOperand (0))].push_back (site);
			total += 1;
		}
	}

	if (total < 2)
		return llvm::PreservedAnalyses::all ();

	llvm::SmallVector<llvm::CallBase *, 4> dead;
	for (auto &[_, sites] : inits) {
		if (sites.size () < 2)
			continue;

		for (llvm::CallBase *site : sites) {
			for (llvm::CallBase *earlier : sites) {
				if (earlier == site || !is_same_class (earlier, site))
					continue;
				if (!dt->dominates (earlier, site))
					continue;

				dead.push_back (site);
				break;
			}
		}
	}

	if (dead.empty ())
		return llvm::PreservedAnalyses::all ();

	for (llvm::CallBase *call : dead)
		erase_call (call);

	return llvm::PreservedAnalyses::none ();
}

llvm::PreservedAnalyses
ClassInitCompleteElisionPass::run (llvm::Function &f, llvm::FunctionAnalysisManager &fam)
{
	MonoDomain *domain = current_compile ().domain;
	if (!domain)
		return llvm::PreservedAnalyses::all ();

	llvm::SmallVector<llvm::CallBase *, 8> dead;

	for (auto &decl : f.getParent ()->functions ()) {
		if (!decl.isDeclaration () || !decl.hasFnAttribute (class_init_attribute))
			continue;

		for (auto user : decl.users ()) {
			auto call = llvm::dyn_cast<llvm::CallBase> (user);
			if (!call)
				continue;
			if (call->getFunction () != &f || call->getCalledFunction () != &decl)
				continue;
			if (call->arg_size () < 1)
				continue;

			MonoClass *klass = get_vtable_class (call->getArgOperand (0));
			if (klass == nullptr || !is_class_initialized (domain, klass))
				continue;

			dead.push_back (call);
		}
	}

	llvm::SmallVector<llvm::LoadInst *, 8> dead_reads;
	for (auto &i : llvm::instructions (f)) {
		auto load = llvm::dyn_cast<llvm::LoadInst> (&i);
		if (!load)
			continue;

		MonoClass *klass = get_init_check_class (*load);
		if (klass == nullptr || !is_class_initialized (domain, klass))
			continue;

		dead_reads.push_back (load);
	}

	if (dead.empty () && dead_reads.empty ())
		return llvm::PreservedAnalyses::all ();

	for (auto *call : dead)
		erase_call (call);

	for (auto *load : dead_reads) {
		load->replaceAllUsesWith (llvm::ConstantInt::get (load->getType (), 1));
		load->eraseFromParent ();
	}

	return llvm::PreservedAnalyses::none ();
}

} // namespace mono
