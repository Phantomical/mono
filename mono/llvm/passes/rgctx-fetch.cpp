/**
 * \file
 * \brief Lowering a generic-context fetch into a load with the fill call behind it.
 *
 * A shared body reads the metadata it was entered with out of the runtime
 * generic context, and the context holds each entry in a slot. The translator
 * emits every fetch as a call to the icall that fills the slot. The first fetch
 * of a slot has to build its value, which can create a vtable and run a class
 * initializer. Each fetch after it finds the value already there.
 *
 * So this puts the load in front of the call:
 *
 *     %rgctx = load ptr, ptr %context + 40    ; the vtable's context, or the
 *     %hit   = icmp ne ptr %rgctx, null       ;   MRGCTX itself
 *     br i1 %hit, label %step, label %fill
 *   step:
 *     %info = load ptr, ptr %rgctx + 24
 *     %have = icmp ne ptr %info, null
 *     br i1 %have, label %done, label %fill
 *   fill:
 *     %filled = call ptr @fill_rgctx (ptr %context, i32 2)
 *     br label %done
 *   done:
 *     %slot = phi ptr [ %info, %step ], [ %filled, %fill ]
 *
 * A null slot is an empty slot, because the runtime never stores a null value.
 * A read that loses the race with the thread that fills the slot therefore sees
 * null and takes the call, which is the answer the site had before.
 *
 * How many loads reach the slot, and at which offsets, is decided while
 * translating and travels on the site's attribute. That keeps mono's context
 * layout out of this file.
 */

#include "rgctx-fetch.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {
namespace {

struct FetchSpec {
	/// Which operand of the site the context arrives in.
	unsigned context = 0;
	/// The byte offsets the walk loads through, the slot itself last.
	SmallVector<uint64_t, 4> walk;
};

FetchSpec
parse_spec (const CallBase *site)
{
	FetchSpec spec;
	StringRef text = site->getFnAttr (rgctx_walk_attribute).getValueAsString ();
	auto malformed = [&] () {
		report_fatal_error (Twine ("malformed ") + rgctx_walk_attribute
		                    + " attribute in "
		                    + site->getFunction ()->getName ());
	};

	while (!text.empty ()) {
		auto [pair, rest] = text.split (',');
		auto [key, value] = pair.split ('=');

		if (key == "ctx") {
			if (value.getAsInteger (10, spec.context))
				malformed ();
		} else if (key == "walk") {
			while (!value.empty ()) {
				auto [first, others] = value.split (':');
				uint64_t offset = 0;

				if (first.getAsInteger (10, offset))
					malformed ();
				spec.walk.push_back (offset);
				value = others;
			}
		} else {
			malformed ();
		}
		text = rest;
	}

	return spec;
}

/// Weights the edge to the fill call as unlikely, matching what the translator
/// puts on its own guards.
void
mark_unlikely (BranchInst *branch)
{
	MDBuilder md (branch->getContext ());

	branch->setMetadata (LLVMContext::MD_prof, md.createBranchWeights (1, 1000));
}

/// Whether the pass understands this site well enough to rewrite it.
bool
is_fetch (const CallBase *site, const Function *decl)
{
	return site->getCalledFunction () == decl && !site->getType ()->isVoidTy ()
	       && site->hasFnAttr (rgctx_walk_attribute);
}

void
lower_site (CallBase *site, const FetchSpec &spec)
{
	Function *fn = site->getFunction ();
	LLVMContext &ctx = fn->getContext ();
	Type *ptr = PointerType::get (ctx, 0);
	Value *context = site->getArgOperand (spec.context);

	/*
	 * The call keeps a block of its own, so an invoke keeps its unwind edge and
	 * the walk grows as a chain of blocks in front of it.
	 */
	BasicBlock *head = site->getParent ();
	BasicBlock *fill = head->splitBasicBlock (site, "rgctx_fill");

	head->getTerminator ()->eraseFromParent ();

	BasicBlock *done = nullptr;
	auto *invoke = dyn_cast<InvokeInst> (site);

	if (invoke != nullptr) {
		BasicBlock *normal = invoke->getNormalDest ();

		done = BasicBlock::Create (ctx, "rgctx_done", fn, normal);
		invoke->setNormalDest (done);
		IRBuilder<> (done).CreateBr (normal);
		for (PHINode &phi : normal->phis ())
			phi.replaceIncomingBlockWith (fill, done);
	} else {
		done = fill->splitBasicBlock (site->getNextNode (), "rgctx_done");
	}

	IRBuilder<> b (head);

	// The walk stands where the fetch did, so it carries the fetch's own line.
	b.SetCurrentDebugLocation (site->getDebugLoc ());

	// The icall takes the context as an integer, which is what its signature
	// says. The walk needs an address.
	Value *base = context->getType ()->isPointerTy ()
	                      ? context
	                      : b.CreateIntToPtr (context, ptr);
	Value *found = nullptr;

	for (unsigned step = 0; step < spec.walk.size (); ++step) {
		bool last = step + 1 == spec.walk.size ();
		Value *at = b.CreateInBoundsGEP (b.getInt8Ty (), base,
		                                 b.getInt64 (spec.walk[step]));

		found = b.CreateAlignedLoad (last ? site->getType () : ptr, at,
		                             Align (sizeof (void *)));

		BasicBlock *next =
			last ? done : BasicBlock::Create (ctx, "rgctx_step", fn, fill);

		mark_unlikely (b.CreateCondBr (b.CreateIsNull (found), fill, next));
		if (last)
			break;
		b.SetInsertPoint (next);
		base = found;
	}

	IRBuilder<> db (done, done->begin ());

	db.SetCurrentDebugLocation (site->getDebugLoc ());

	PHINode *slot = db.CreatePHI (site->getType (), 2, "rgctx_slot");

	site->replaceAllUsesWith (slot);
	slot->addIncoming (found, b.GetInsertBlock ());
	slot->addIncoming (site, fill);
}

} // namespace

PreservedAnalyses
RgctxFetchPass::run (Module &m, ModuleAnalysisManager &)
{
	SmallVector<Function *, 2> decls;

	for (Function &f : m)
		if (f.isDeclaration () && f.hasFnAttribute (rgctx_fetch_attribute))
			decls.push_back (&f);

	bool changed = false;

	for (Function *decl : decls) {
		SmallVector<CallBase *, 8> sites;

		for (User *user : decl->users ())
			if (auto *site = dyn_cast<CallBase> (user))
				if (is_fetch (site, decl))
					sites.push_back (site);

		for (CallBase *site : sites) {
			FetchSpec spec = parse_spec (site);

			// A site with no walk keeps the call it had.
			if (spec.walk.empty () || spec.context >= site->arg_size ())
				continue;

			lower_site (site, spec);
			changed = true;
		}
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono
