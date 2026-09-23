#include "receiver-profile.hpp"

#include "builtins.hpp"
#include "compile-state.hpp"
#include "il-line-table.hpp"
#include "runtime/options.hpp"
#include "tier-counter.hpp"
#include "vtable-func.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/ProfileData/InstrProf.h>

#include <algorithm>

using namespace llvm;

namespace mono {
namespace {

/// `void @mono.profile.receiver (ptr record, ptr vtable)` counts vtable into
/// record. ReceiverProfilePass writes it and LowerReceiverProfilePass lowers it.
constexpr StringRef count_receiver_name = "mono.profile.receiver";

constexpr StringRef record_receiver_helper = "mono_llvm_jit_record_receiver";

constexpr unsigned record_words = 2 * ReceiverRecord::entries + 1;

/// A dispatch site's call, the vtable it dispatches on, and the key naming
/// where its IL wrote it.
struct Dispatch {
	CallBase *call;
	Value *vtable;
	ReceiverSiteKey key;
};

/// Appends every call \p f makes through a dispatch site to \p into, each with
/// the vtable it dispatches on and the site's key, \p ids naming its origin.
void
collect_dispatches (Function &f, const DenseMap<const DISubprogram *, uint64_t> &ids,
                    SmallVectorImpl<Dispatch> &into)
{
	for (StringRef name : { vtable_func_name, imt_func_name, vtable_gfunc_name }) {
		for (CallBase *site : builtin_sites (f, name)) {
			CallBase *call = dispatch_call (site);

			if (call == nullptr)
				continue;

			std::optional<std::pair<uint64_t, uint32_t>> origin =
				il_debug_origin (*call, ids);

			if (!origin)
				continue;

			into.push_back ({ call, site->getArgOperand (0),
			                  { origin->first, origin->second } });
		}
	}
}

} // namespace

std::vector<ReceiverSites> &
receiver_sites ()
{
	static thread_local std::vector<ReceiverSites> sites;

	return sites;
}

void
ReceiverCounts::add (const ReceiverRecord &record)
{
	for (const ReceiverRecord::Entry &entry : record.seen) {
		uint64_t vtable = entry.vtable.load (std::memory_order_relaxed);
		uint64_t count = entry.count.load (std::memory_order_relaxed);

		if (vtable == 0 || count == 0)
			continue;

		auto known = std::find_if (seen.begin (), seen.end (),
		                           [&] (const auto &s) { return s.first == vtable; });

		if (known != seen.end ())
			known->second += count;
		else
			seen.emplace_back (vtable, count);
	}

	other += record.other.load (std::memory_order_relaxed);
}

uint64_t
ReceiverCounts::total () const
{
	uint64_t sum = other;

	for (const auto &[vtable, count] : seen)
		sum += count;

	return sum;
}

PreservedAnalyses
ReceiverProfilePass::run (Module &m, ModuleAnalysisManager &)
{
	if (!receiver_profile ())
		return PreservedAnalyses::all ();

	DenseMap<const DISubprogram *, uint64_t> ids = il_debug_subprogram_ids (m);
	std::vector<ReceiverSites> &recorded = receiver_sites ();
	SmallVector<Dispatch, 16> dispatches;

	for (Function &f : m) {
		if (f.isDeclaration () || !f.hasFnAttribute (tier_counter_attribute))
			continue;

		size_t first = dispatches.size ();

		collect_dispatches (f, ids, dispatches);
		if (dispatches.size () == first)
			continue;

		ReceiverSites sites { f.getName ().str (), (uint32_t) first, {} };

		for (size_t i = first; i < dispatches.size (); i++)
			sites.keys.push_back (dispatches[i].key);
		recorded.push_back (std::move (sites));
	}

	if (dispatches.empty ())
		return PreservedAnalyses::all ();

	LLVMContext &c = m.getContext ();
	Type *i64 = Type::getInt64Ty (c);
	Type *ptr = PointerType::get (c, 0);
	auto *records = ArrayType::get (ArrayType::get (i64, record_words), dispatches.size ());

	// One array for the module, so the link has one block to report whichever
	// batch member it belongs to.
	auto *table = new GlobalVariable (m, records, /*isConstant=*/false,
	                                  GlobalValue::PrivateLinkage,
	                                  ConstantAggregateZero::get (records), "mono_receivers");

	table->setSection (receiver_section);
	table->setAlignment (Align (alignof (ReceiverRecord)));

	Function *count = builtin_decl (m, count_receiver_name,
	                                FunctionType::get (Type::getVoidTy (c), { ptr, ptr }, false));

	count->setDoesNotThrow ();
	count->setWillReturn ();
	count->setMemoryEffects (MemoryEffects::argMemOnly ());

	for (size_t i = 0; i < dispatches.size (); i++) {
		const Dispatch &at = dispatches[i];
		// In front of the call rather than the site, which LICM can hoist out
		// of the loop the call stays in.
		IRBuilder<> b (at.call);

		b.CreateCall (count, { b.CreateConstInBoundsGEP2_64 (records, table, 0, i), at.vtable });
	}

	return PreservedAnalyses::none ();
}

PreservedAnalyses
LowerReceiverProfilePass::run (Module &m, ModuleAnalysisManager &)
{
	SmallVector<CallBase *, 8> sites = builtin_sites (m, count_receiver_name);

	if (sites.empty ())
		return PreservedAnalyses::all ();

	LLVMContext &c = m.getContext ();
	Type *i64 = Type::getInt64Ty (c);
	Type *ptr = PointerType::get (c, 0);
	FunctionCallee helper = m.getOrInsertFunction (
		record_receiver_helper, FunctionType::get (Type::getVoidTy (c), { ptr, ptr }, false));

	cast<Function> (helper.getCallee ())->setDoesNotThrow ();

	MDNode *mostly_hit = MDBuilder (c).createLikelyBranchWeights ();

	for (CallBase *site : sites) {
		Value *record = site->getArgOperand (0);
		Value *vtable = site->getArgOperand (1);
		BasicBlock *head = site->getParent ();
		Function *f = head->getParent ();
		BasicBlock *done = head->splitBasicBlock (site->getIterator (), "receiver_done");
		BasicBlock *hit = BasicBlock::Create (c, "receiver_hit", f, done);
		BasicBlock *miss = BasicBlock::Create (c, "receiver_miss", f, done);

		head->getTerminator ()->eraseFromParent ();

		IRBuilder<> b (head);

		b.SetCurrentDebugLocation (site->getDebugLoc ());

		// Monotonic, because mono_llvm_jit_record_receiver () claims an entry
		// on another thread. Two threads bumping one count lose an increment,
		// which only skews the shares.
		LoadInst *first = b.CreateAlignedLoad (i64, record, Align (8));

		first->setAtomic (AtomicOrdering::Monotonic);
		b.CreateCondBr (b.CreateICmpEQ (first, b.CreatePtrToInt (vtable, i64)), hit, miss,
		                mostly_hit);

		b.SetInsertPoint (hit);

		Value *slot = b.CreateConstInBoundsGEP1_64 (i64, record, 1);
		LoadInst *counted = b.CreateAlignedLoad (i64, slot, Align (8));

		counted->setAtomic (AtomicOrdering::Monotonic);
		b.CreateAlignedStore (b.CreateAdd (counted, b.getInt64 (1)), slot, Align (8))
			->setAtomic (AtomicOrdering::Monotonic);
		b.CreateBr (done);

		b.SetInsertPoint (miss);
		b.CreateCall (helper, { record, vtable });
		b.CreateBr (done);

		site->eraseFromParent ();
	}

	erase_builtin (m, count_receiver_name);
	return PreservedAnalyses::none ();
}

PreservedAnalyses
AnnotateReceiversPass::run (Module &m, ModuleAnalysisManager &)
{
	const CompileState &compile = current_compile ();

	if (!compile.receivers)
		return PreservedAnalyses::all ();

	DenseMap<const DISubprogram *, uint64_t> ids = il_debug_subprogram_ids (m);
	bool changed = false;

	for (Function &f : m) {
		if (f.isDeclaration ())
			continue;

		SmallVector<Dispatch, 16> dispatches;

		collect_dispatches (f, ids, dispatches);

		for (const Dispatch &at : dispatches) {
			std::optional<ReceiverCounts> counts = compile.receivers (at.key);

			if (!counts || counts->total () == 0)
				continue;

			SmallVector<InstrProfValueData, 4> values;

			for (const auto &[vtable, count] : counts->seen)
				values.push_back ({ vtable, count });

			std::sort (values.begin (), values.end (),
			           [] (const auto &a, const auto &b) { return a.Count > b.Count; });

			// On the site rather than the call, which the guard writes again.
			auto *site = cast<CallBase> (at.call->getCalledOperand ());

			annotateValueSite (m, *site, values, counts->total (), IPVK_VTableTarget,
			                   values.size ());
			changed = true;
		}
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono
