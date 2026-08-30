/**
 * \file
 * \brief The walk that settles a function's values, and the map it fills.
 */

#include "constant-values.hpp"

#include "escape.hpp"
#include "passes/alloc-func.hpp"
#include "passes/gc-barrier.hpp"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ModRef.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <utility>

using namespace llvm;

namespace mono {
namespace {

/// Whether \p alloc hands back memory that reads as zero.
bool
is_zeroinit (const CallBase &alloc)
{
	const Function *callee = alloc.getCalledFunction ();

	// getAllocKind () asserts the attribute is there.
	if (!callee->hasFnAttribute (Attribute::AllocKind))
		return false;

	AllocFnKind kind = callee->getFnAttribute (Attribute::AllocKind).getAllocKind ();

	return (kind & AllocFnKind::Zeroed) != AllocFnKind::Unknown;
}

/// The range a value copy writes, or nothing where its width is not a constant.
///
/// Alias analysis reads a memory intrinsic's length off the call itself. A value
/// copy is an ordinary call, so its width is read here.
std::optional<MemoryLocation>
value_copy_target (const CallBase &call)
{
	const Function *callee = call.getCalledFunction ();

	if (callee == nullptr || callee->getName () != gc_value_copy_name)
		return std::nullopt;

	const auto *count = dyn_cast<ConstantInt> (call.getArgOperand (2));
	const auto *width = dyn_cast<ConstantInt> (call.getArgOperand (3));

	if (count == nullptr || width == nullptr)
		return std::nullopt;

	bool overflowed = false;
	APInt bytes = count->getValue ().zextOrTrunc (64).umul_ov (width->getValue ().zextOrTrunc (64),
	                                                           overflowed);

	if (overflowed)
		return std::nullopt;

	return MemoryLocation (call.getArgOperand (0), LocationSize::precise (bytes.getZExtValue ()));
}

/// Returns \p held as a constant of type \p want, or null where no such
/// constant exists.
Constant *
as_type (Constant *held, Type *want, const DataLayout &dl)
{
	if (held->getType () == want)
		return held;

	if (held->getType ()->isPointerTy () && want->isIntegerTy ())
		return ConstantFoldCastOperand (Instruction::PtrToInt, held, want, dl);

	if (held->getType ()->isIntegerTy () && want->isPointerTy ())
		return ConstantFoldCastOperand (Instruction::IntToPtr, held, want, dl);

	return nullptr;
}

} // namespace

/// Drives the walk over one function and hands back what it settled.
class ConstantValuesSolver : public InstVisitor<ConstantValuesSolver, bool> {
	Function &f;
	const DataLayout &dl;
	FunctionAnalysisManager &fam;

	/// Null until gather_memory_deps () finds a load to forward. Building one
	/// costs more than every other part of this walk together.
	MemorySSA *mssa = nullptr;

	/// The loads that read each stored value.
	DenseMap<Value *, SmallPtrSet<Value *, 2>> dependents;

	/// What one load is built from.
	struct MemoryDeps {
		/// The value operand of each store that reaches the load.
		SmallVector<Value *, 4> stored;

		/// Whether a path reaches an allocation's zero fill with no store
		/// over it.
		bool zeroed = false;

		bool opaque = false;
	};

	DenseMap<LoadInst *, MemoryDeps> memory_deps;

	/// Built once for the pre-pass. BatchAAResults caches its queries, and one
	/// per load discards that cache.
	std::optional<BatchAAResults> aa;

	llvm::DenseMap<llvm::Value *, ValueSources> sources;

	/// Whether this walk forwards a store to the load that reads it.
	bool reads_memory;

public:
	ConstantValuesSolver (llvm::Function &f, llvm::FunctionAnalysisManager &fam,
	                      bool reads_memory)
		: f (f), dl (f.getParent ()->getDataLayout ()), fam (fam),
		  reads_memory (reads_memory)
	{
	}

	struct SCC {
		llvm::SmallPtrSet<llvm::BasicBlock *, 1> blocks;
		bool has_cycle;

		SCC (llvm::scc_iterator<llvm::BasicBlock *> it)
		{
			blocks.reserve (it->size ());
			blocks.insert (it->begin (), it->end ());
			has_cycle = it.hasCycle ();
		}
	};

	void solve (ConstantValues &result, llvm::AnalysisKey *built_by)
	{
		result.built_by = built_by;
		sources = std::move (result.lookup);

		gather_memory_deps ();

		std::deque<llvm::Instruction *> queue;
		llvm::SmallPtrSet<llvm::Value *, 16> dirty;
		llvm::SmallVector<SCC, 4> sccs;

		for (auto it = llvm::scc_begin (&f.getEntryBlock ());
		     it != llvm::scc_end (&f.getEntryBlock ()); ++it) {
			sccs.emplace_back (it);
		}

		for (llvm::Argument &arg : f.args ())
			sources.try_emplace (&arg, ValueSources (&arg));

		// scc_begin () enumerates in reverse topological order, entry block
		// last.
		for (const SCC &scc : llvm::reverse (sccs)) {
			const auto &blocks = scc.blocks;

			// One block with no back edge settles in a single pass.
			if (!scc.has_cycle) {
				visit (*blocks.begin ());
				continue;
			}

			dirty.clear ();

			for (llvm::BasicBlock *block : blocks) {
				for (llvm::Instruction &inst : *block) {
					queue.push_back (&inst);
					dirty.insert (&inst);
				}
			}

			while (!queue.empty ()) {
				auto inst = queue.front ();
				queue.pop_front ();
				dirty.erase (inst);

				if (!visit (inst))
					continue;

				for (llvm::Use &use : inst->uses ()) {
					auto inst = llvm::dyn_cast_or_null<llvm::Instruction> (use.getUser ());
					if (!inst)
						continue;

					if (!blocks.contains (inst->getParent ()))
						continue;

					if (!dirty.insert (inst).second)
						continue;

					queue.push_back (inst);
				}

				// The loop above misses a load, which is not a use of the
				// value operand it reads.
				auto reads = dependents.find (inst);
				if (reads == dependents.end ())
					continue;

				for (llvm::Value *reader : reads->second) {
					auto load = llvm::cast<llvm::Instruction> (reader);

					if (!blocks.contains (load->getParent ()))
						continue;

					if (!dirty.insert (load).second)
						continue;

					queue.push_back (load);
				}
			}
		}

		result.lookup = std::move (sources);
		result.read_memory = mssa != nullptr;
	}

	bool visitPHINode (llvm::PHINode &phi)
	{
		bool changed = false;

		for (llvm::Value *incoming : phi.incoming_values ())
			changed |= uses (&phi, incoming);

		return changed;
	}

	bool visitSelectInst (llvm::SelectInst &pick)
	{
		bool changed = false;
		changed |= uses (&pick, pick.getTrueValue ());
		changed |= uses (&pick, pick.getFalseValue ());
		return changed;
	}

	bool visitPtrToIntInst (llvm::PtrToIntInst &inst)
	{
		return casts (inst, llvm::Instruction::PtrToInt);
	}

	bool visitIntToPtrInst (llvm::IntToPtrInst &inst)
	{
		return casts (inst, llvm::Instruction::IntToPtr);
	}

	bool visitBitCastInst (llvm::BitCastInst &inst)
	{
		return casts (inst, llvm::Instruction::BitCast);
	}

	bool visitFreezeInst (llvm::FreezeInst &inst)
	{
		// A freeze of undef takes one value of its own choosing.
		return uses (&inst, inst.getOperand (0), [&] (llvm::Value *v) -> llvm::Value * {
			if (llvm::isa<llvm::UndefValue> (v))
				return nullptr;

			return v;
		});
	}

	void gather_memory_deps ()
	{
		if (!reads_memory)
			return;

		SmallVector<LoadInst *, 16> loads;

		for (Instruction &at : instructions (f)) {
			auto *load = dyn_cast<LoadInst> (&at);

			// An atomic or volatile load can change outside this function.
			if (load != nullptr && load->isSimple ())
				loads.push_back (load);
		}

		if (loads.empty ())
			return;

		mssa = &fam.getResult<MemorySSAAnalysis> (f).getMSSA ();
		aa.emplace (mssa->getAA ());

		for (LoadInst *load : loads) {
			MemoryDeps deps = walk_memory (*load);

			for (Value *stored : deps.stored)
				dependents[stored].insert (load);

			memory_deps.try_emplace (load, std::move (deps));
		}
	}

	/// Records what \p load reads where no store in this function reaches it.
	void reached_unwritten (LoadInst &load, MemoryDeps &deps)
	{
		SmallVector<const Value *, 4> objects;

		// The plural form crosses a phi, where getUnderlyingObject () stops.
		getUnderlyingObjects (load.getPointerOperand (), objects);

		for (const Value *object : objects) {
			// Loading through a null pointer is UB.
			if (isa<ConstantPointerNull> (object))
				continue;

			CallBase *alloc = allocation_behind (const_cast<Value *> (object));

			if (alloc != nullptr && is_zeroinit (*alloc))
				deps.zeroed = true;
			else
				deps.opaque = true;
		}
	}

	/// The writes that reach \p load.
	MemoryDeps walk_memory (LoadInst &load)
	{
		MemoryLocation where = MemoryLocation::get (&load);
		MemorySSAWalker *walker = mssa->getWalker ();

		SmallPtrSet<MemoryAccess *, 8> seen;
		SmallVector<MemoryAccess *, 8> work{walker->getClobberingMemoryAccess (&load, *aa)};
		MemoryDeps deps;

		while (!work.empty ()) {
			MemoryAccess *at = work.pop_back_val ();

			if (!seen.insert (at).second)
				continue;

			if (mssa->isLiveOnEntryDef (at)) {
				reached_unwritten (load, deps);
				continue;
			}

			if (auto *merge = dyn_cast<MemoryPhi> (at)) {
				for (Use &incoming : merge->incoming_values ()) {
					auto *from = cast<MemoryAccess> (incoming.get ());

					work.push_back (walker->getClobberingMemoryAccess (from, where, *aa));
				}
				continue;
			}

			auto step_past = [&] {
				work.push_back (walker->getClobberingMemoryAccess (
					cast<MemoryDef> (at)->getDefiningAccess (), where, *aa));
			};

			Instruction *wrote = cast<MemoryUseOrDef> (at)->getMemoryInst ();

			if (auto *copy = dyn_cast<CallBase> (wrote)) {
				std::optional<MemoryLocation> writes = value_copy_target (*copy);

				if (writes.has_value () && aa->alias (*writes, where) == AliasResult::NoAlias) {
					step_past ();
					continue;
				}
			}

			auto *store = dyn_cast<StoreInst> (wrote);

			// An unknown write. The walk goes on, because a store behind it
			// can reach the load too.
			if (store == nullptr) {
				deps.opaque = true;
				step_past ();
				continue;
			}

			AliasResult alias = aa->alias (MemoryLocation::get (store), where);
			if (alias == AliasResult::NoAlias) {
				step_past ();
				continue;
			}

			// This walk reads a whole store or nothing, so a store wider
			// than the load names no value either.
			if (store->isSimple ()
			    && dl.getTypeStoreSize (store->getValueOperand ()->getType ())
			           == dl.getTypeStoreSize (load.getType ()))
				deps.stored.push_back (store->getValueOperand ());
			else
				deps.opaque = true;

			// MustAlias means we stop walking backwards here.
			if (alias == AliasResult::MustAlias)
				continue;

			step_past ();
		}

		// A cyclic MemoryPhi can end the walk before it records a store. An
		// empty set here reads as a field with no values in it.
		if (deps.stored.empty () && !deps.zeroed)
			deps.opaque = true;

		return deps;
	}

	bool visitLoadInst (LoadInst &load)
	{
		auto found = memory_deps.find (&load);

		// gather_memory_deps () passes over an atomic or volatile load.
		if (found == memory_deps.end ())
			return unknown (&load);

		const MemoryDeps &deps = found->second;
		bool changed = false;

		if (deps.opaque)
			changed |= unknown (&load);

		if (deps.zeroed)
			changed |= forwards (load, Constant::getNullValue (load.getType ()));

		for (Value *stored : deps.stored)
			changed |= forwards (load, stored);

		return changed;
	}

	bool visitInstruction (llvm::Instruction &inst)
	{
		// A void instruction is nobody's operand.
		if (inst.getType ()->isVoidTy ())
			return false;

		if (inst.mayHaveSideEffects () || inst.mayReadFromMemory () || inst.isTerminator ())
			return unknown (&inst);

		llvm::SmallVector<llvm::Constant *, 4> operands;
		for (const llvm::Use &use : inst.operands ()) {
			auto constant = value_of (use.get ());
			if (!constant)
				return unknown (&inst);

			operands.push_back (constant);
		}

		llvm::Constant *folded = ConstantFoldInstOperands (&inst, operands, dl);
		if (!folded)
			return unknown (&inst);

		return uses (&inst, folded);
	}

private:
	/// Update \p inst's source set to include all the sources of \p value
	///
	/// \returns whether this changed \p inst's source set.
	bool uses (llvm::Instruction *inst, llvm::Value *value)
	{
		ValueSources &set = sources.try_emplace (inst).first->getSecond ();

		if (auto c = llvm::dyn_cast_or_null<llvm::Constant> (value))
			return set.insert (c);

		auto it = sources.find (value);
		if (it == sources.end ())
			return false;

		return set.insert (it->getSecond ());
	}

	template<typename F>
	bool uses (llvm::Instruction *inst, llvm::Value *value, F transform)
	{
		ValueSources &set = sources.try_emplace (inst).first->getSecond ();

		if (auto c = llvm::dyn_cast_or_null<llvm::Constant> (value)) {
			llvm::Value *held = transform (c);

			if (held == nullptr)
				held = inst;

			return set.insert (held);
		}

		auto it = sources.find (value);
		if (it == sources.end ())
			return false;

		return set.insert (it->getSecond (), inst, std::move (transform));
	}

	/// Looks through a cast that keeps the address and changes only the type.
	bool casts (llvm::Instruction &inst, llvm::Instruction::CastOps op)
	{
		return uses (&inst, inst.getOperand (0), [&] (llvm::Value *v) -> llvm::Value * {
			auto c = llvm::dyn_cast<llvm::Constant> (v);

			if (c == nullptr)
				return nullptr;

			return llvm::ConstantFoldCastOperand (op, c, inst.getType (), dl);
		});
	}

	/// Records \p inst as its own source, for a path the walk did not settle.
	bool unknown (llvm::Instruction *inst)
	{
		ValueSources &set = sources.try_emplace (inst).first->getSecond ();

		return set.insert (inst);
	}

	/// Update \p load's source set to include what \p stored settled to, under
	/// the type \p load reads with.
	///
	/// \returns whether this changed \p load's source set.
	bool forwards (llvm::LoadInst &load, llvm::Value *stored)
	{
		return uses (&load, stored, [&] (llvm::Value *held) -> llvm::Value * {
			// Only a constant is written back into the IR under the
			// load's type.
			auto c = llvm::dyn_cast<llvm::Constant> (held);

			if (c == nullptr)
				return held;

			return as_type (c, load.getType (), dl);
		});
	}

	llvm::Constant *value_of (llvm::Value *value)
	{
		if (auto c = llvm::dyn_cast_or_null<llvm::Constant> (value))
			return c;

		auto inst = llvm::dyn_cast_or_null<llvm::Instruction> (value);
		if (!inst)
			return nullptr;

		auto it = sources.find (inst);
		if (it == sources.end ())
			return nullptr;

		auto &set = it->getSecond ();
		if (set.sources.size () != 1)
			return nullptr;

		return llvm::dyn_cast<llvm::Constant> (*set.sources.begin ());
	}
};

static const ValueSources nothing;

const ValueSources &
ConstantValues::sources (llvm::Value *v) const
{
	auto found = lookup.find (v);
	if (found == lookup.end ())
		return nothing;

	return found->second;
}

llvm::Constant *
ConstantValues::value (llvm::Value *v) const
{
	if (auto constant = llvm::dyn_cast_or_null<llvm::Constant> (v))
		return constant;

	auto inst = llvm::dyn_cast_or_null<llvm::Instruction> (v);
	if (!inst)
		return nullptr;

	const ValueSources &src = sources (inst);
	if (src.sources.size () != 1)
		return nullptr;

	return llvm::dyn_cast<llvm::Constant> (*src.sources.begin ());
}

bool
ConstantValues::invalidate (Function &f, const PreservedAnalyses &pa,
                            FunctionAnalysisManager::Invalidator &inv)
{
	if (!pa.getChecker (built_by).preserved ())
		return true;

	return read_memory && inv.invalidate<MemorySSAAnalysis> (f, pa);
}

AnalysisKey MonoConstantValues::Key;
AnalysisKey MonoMemoryValues::Key;

namespace {

ConstantValues
settle (Function &f, FunctionAnalysisManager &fam, AnalysisKey *built_by,
        bool reads_memory)
{
	ConstantValues values;
	ConstantValuesSolver solver (f, fam, reads_memory);

	solver.solve (values, built_by);

	return values;
}

} // namespace

ConstantValues
MonoConstantValues::run (Function &f, FunctionAnalysisManager &fam)
{
	return settle (f, fam, ID (), /*reads_memory=*/false);
}

ConstantValues
MonoMemoryValues::run (Function &f, FunctionAnalysisManager &fam)
{
	return settle (f, fam, ID (), /*reads_memory=*/true);
}

} // namespace mono
