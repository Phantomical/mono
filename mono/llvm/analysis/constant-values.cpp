/**
 * \file
 * \brief The walk that settles a function's values, and the map it fills.
 */

#include "constant-values.hpp"

#include "escape.hpp"
#include "passes/alloc-func.hpp"
#include "passes/gc-barrier.hpp"
#include "strip-casts.hpp"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
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
#include <optional>
#include <queue>
#include <utility>
#include <vector>

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

/// \p ptr as (base, byte offset), or nothing where a non-constant index
/// leaves the offset unknowable.
///
/// Two pointers name the same address only where both halves agree, which is
/// what lets a store reach a load with no alias query: the walk below reads
/// this off each side and compares.
std::optional<std::pair<Value *, int64_t>>
normalize_address (Value *ptr, const DataLayout &dl)
{
	ptr = const_cast<Value *> (strip_casts (ptr));

	APInt offset (dl.getIndexTypeSizeInBits (ptr->getType ()), 0);
	Value *base = ptr->stripAndAccumulateConstantOffsets (dl, offset,
	                                                       /*AllowNonInbounds=*/true);

	// A non-constant GEP index stops the peel with the GEP itself standing
	// as the base, which is what says the offset did not fully settle.
	if (isa<GEPOperator> (base))
		return std::nullopt;

	return std::make_pair (const_cast<Value *> (strip_casts (base)), offset.getSExtValue ());
}

/// Whether \p call may write to memory a normalized address could name.
///
/// An allocation writes only its own, not yet visible memory
/// (`inaccessiblemem`), so it answers false: nothing already reachable
/// through a pointer could alias what such a call touches.
bool
may_clobber_tracked_memory (const CallBase &call)
{
	MemoryEffects effects = call.getMemoryEffects ();
	ModRefInfo elsewhere =
		effects.getWithoutLoc (MemoryEffects::Location::InaccessibleMem).getModRef ();

	return isModSet (elsewhere);
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

	/// A store's address, its own base with any constant GEP offsets folded
	/// in. Two addresses name the same location only where both halves
	/// match, which trades the precision an alias query would answer for
	/// not asking one.
	using AddrKey = std::pair<Value *, int64_t>;

	/// What a block's own writes settle one key to, over every path this
	/// walk has combined into it so far.
	struct KeyState {
		/// The values a store here might leave, merged the same way any
		/// other value's sources are: past max_sources the walk gives up
		/// on this key rather than keep growing it.
		ValueSources sources;

		/// The width \c sources was recorded at. A load of a different
		/// width answers opaque instead of misreading a wider or narrower
		/// store's bytes.
		uint64_t width = 0;

		/// Whether a path reaches here having never written this key, so
		/// the allocation's own fill is one more value a load might read.
		bool maybe_unwritten = false;

		/// Whether a write neither a normalized address nor
		/// may_clobber_tracked_memory () could rule out has reached here.
		/// Left standing alongside \c sources rather than clearing them:
		/// a load already folds an opaque flag and a known value together,
		/// the same as it does for a merge of disagreeing arms.
		bool opaque = false;

		/// Folds \p other's answer for this key into this one.
		///
		/// \returns whether this changed anything.
		bool merge (const KeyState &other)
		{
			bool changed = false;

			if (other.opaque && !opaque) {
				opaque = true;
				changed = true;
			}

			if (other.maybe_unwritten && !maybe_unwritten) {
				maybe_unwritten = true;
				changed = true;
			}

			if (other.sources.is_empty () && !other.sources.is_widened ())
				return changed;

			if (sources.is_empty () && !sources.is_widened ())
				width = other.width;

			if (width == other.width) {
				changed |= sources.insert (other.sources);
			} else if (!opaque) {
				// Two different widths landed at the same address: no
				// width left to answer a load with, so the key is opaque
				// from here rather than half of one value's bytes.
				opaque = true;
				changed = true;
			}

			return changed;
		}
	};

	llvm::DenseMap<llvm::Value *, ValueSources> sources;

	/// Whether this walk forwards a store to the load that reads it.
	bool reads_memory;

public:
	ConstantValuesSolver (llvm::Function &f, bool reads_memory)
		: f (f), dl (f.getParent ()->getDataLayout ()), reads_memory (reads_memory)
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

		// Every simple load or store's address, normalized once so
		// solve_key () below does not repeat that walk once per key it
		// asks about. A store this walk cannot place is left out on
		// purpose: walk_block_for_key () reads its absence as the barrier
		// it is. A write neither a store nor a placeable address, settled
		// here too: what it clobbers does not depend on which key is being
		// solved, so asking once serves every key instead of one each.
		DenseMap<Instruction *, AddrKey> keys;
		DenseSet<Instruction *> barriers;
		SmallVector<AddrKey, 8> loaded_keys;
		DenseSet<AddrKey> seen_loaded_keys;

		for (Instruction &inst : instructions (f)) {
			std::optional<AddrKey> key;
			bool is_load = false;

			if (auto *load = dyn_cast<LoadInst> (&inst)) {
				if (!load->isSimple ())
					continue;
				is_load = true;
				key = normalize_address (load->getPointerOperand (), dl);
			} else if (auto *store = dyn_cast<StoreInst> (&inst)) {
				if (!store->isSimple ())
					continue;
				key = normalize_address (store->getPointerOperand (), dl);
			} else if (auto *call = dyn_cast<CallBase> (&inst)) {
				if (may_clobber_tracked_memory (*call))
					barriers.insert (&inst);
				continue;
			} else {
				// A fence, an atomic RMW or cmpxchg, or anything else this
				// walk does not otherwise recognize as a write.
				if (inst.mayWriteToMemory ())
					barriers.insert (&inst);
				continue;
			}

			if (!key.has_value ())
				continue;

			keys.try_emplace (&inst, *key);

			if (is_load && seen_loaded_keys.insert (*key).second)
				loaded_keys.push_back (*key);
		}

		// A key nothing loads is nothing any consumer of this analysis
		// reads back, so settling it would cost this walk a solve for an
		// answer nobody asks.
		if (loaded_keys.empty ())
			return;

		SmallVector<SCC, 4> sccs;

		for (auto it = scc_begin (&f.getEntryBlock ()); it != scc_end (&f.getEntryBlock ()); ++it)
			sccs.emplace_back (it);

		// Reused rather than rebuilt per key: a fresh DenseMap per key costs
		// more, over as many keys as a large root can have, than the walk
		// solving each key spends.
		DenseMap<BasicBlock *, KeyState> block_in;
		DenseMap<BasicBlock *, KeyState> block_out;

		for (const AddrKey &key : loaded_keys) {
			block_in.clear ();
			block_out.clear ();
			solve_key (key, keys, barriers, sccs, block_in, block_out);
		}
	}

	/// Records what \p load reads where \p key names no tracked store: the
	/// allocation's zero fill where that is provably the whole story,
	/// opaque otherwise.
	///
	/// A phi or select base lets some other key name the same runtime
	/// address, so a store filed under that key leaves this one looking
	/// untouched. getUnderlyingObjects () returning more than one object
	/// marks that case, and only then does crossing it risk a store this
	/// key's own tracking missed.
	void reached_unwritten (LoadInst &load, const AddrKey &key, MemoryDeps &deps)
	{
		SmallVector<const Value *, 4> objects;

		getUnderlyingObjects (load.getPointerOperand (), objects);

		if (objects.size () != 1 || objects.front () != key.first) {
			deps.opaque = true;
			return;
		}

		const Value *object = objects.front ();

		// Loading through a null pointer is UB.
		if (isa<ConstantPointerNull> (object))
			return;

		CallBase *alloc = allocation_behind (const_cast<Value *> (object));

		if (alloc != nullptr && is_zeroinit (*alloc))
			deps.zeroed = true;
		else
			deps.opaque = true;
	}

	/// Settles what \p load reads, from \p entry as \p key stands at the
	/// load: a value a store settled it to, the allocation behind it where
	/// a path reaches it unwritten, or otherwise nothing settled.
	void settle_load (LoadInst &load, const AddrKey &key, const KeyState &entry)
	{
		MemoryDeps deps;
		uint64_t width = dl.getTypeStoreSize (load.getType ());

		if (entry.opaque)
			deps.opaque = true;

		if (entry.maybe_unwritten)
			reached_unwritten (load, key, deps);

		if (entry.sources.is_widened ())
			deps.opaque = true;
		else if (!entry.sources.is_empty ()) {
			if (entry.width == width)
				deps.stored.assign (entry.sources.sources.begin (), entry.sources.sources.end ());
			else
				deps.opaque = true;
		}

		for (Value *stored : deps.stored)
			dependents[stored].insert (&load);

		memory_deps.try_emplace (&load, std::move (deps));
	}

	/// Applies one block's own writes to \p key's state, in place, settling
	/// what each of the block's own loads of \p key read along the way
	/// where \p settle asks for that.
	///
	/// Run once per block during the fixed point below with \p settle
	/// false, and once more after it converges with \p settle true: a load
	/// visited before the point has converged would settle on a state this
	/// walk has not finished growing.
	void walk_block_for_key (BasicBlock &block, const AddrKey &key,
	                         const DenseMap<Instruction *, AddrKey> &keys,
	                         const DenseSet<Instruction *> &barriers, KeyState &state,
	                         bool settle)
	{
		for (Instruction &inst : block) {
			if (auto *load = dyn_cast<LoadInst> (&inst)) {
				if (settle) {
					auto found = keys.find (&inst);

					if (found != keys.end () && found->second == key)
						settle_load (*load, key, state);
				}
				continue;
			}

			if (auto *store = dyn_cast<StoreInst> (&inst)) {
				auto found = keys.find (&inst);

				if (found == keys.end ()) {
					// A non-simple store, or one an address the walk
					// cannot place, could still land on this key.
					state.opaque = true;
					continue;
				}

				if (found->second == key)
					state = KeyState { ValueSources (store->getValueOperand ()),
					                   dl.getTypeStoreSize (
						                   store->getValueOperand ()->getType ()),
					                   /*maybe_unwritten=*/false, /*opaque=*/false };
				continue;
			}

			if (barriers.contains (&inst))
				state.opaque = true;
		}
	}

	/// Settles \p key for every block, the same shape solve () below runs
	/// at instruction level: a block outside a cycle settles in one pass
	/// over its predecessors, one inside a cycle by a worklist, so a value
	/// a loop carries across its back edge still converges.
	///
	/// \p block_in and \p block_out arrive empty. The caller clears and
	/// reuses them for the next key, rather than paying for a fresh pair
	/// per key.
	void solve_key (const AddrKey &key, const DenseMap<Instruction *, AddrKey> &keys,
	                const DenseSet<Instruction *> &barriers, const SmallVectorImpl<SCC> &sccs,
	                DenseMap<BasicBlock *, KeyState> &block_in,
	                DenseMap<BasicBlock *, KeyState> &block_out)
	{
		block_in.try_emplace (&f.getEntryBlock (),
		                      KeyState { {}, 0, /*maybe_unwritten=*/true, false });

		auto merge_predecessors = [&] (BasicBlock &block, KeyState &in) {
			bool changed = false;

			for (BasicBlock *pred : predecessors (&block)) {
				auto found = block_out.find (pred);

				if (found != block_out.end ())
					changed |= in.merge (found->second);
			}

			return changed;
		};

		// scc_begin () enumerates in reverse topological order, entry block
		// last.
		for (const SCC &scc : reverse (sccs)) {
			if (!scc.has_cycle) {
				BasicBlock *block = *scc.blocks.begin ();
				KeyState &in = block_in.try_emplace (block).first->second;

				if (block != &f.getEntryBlock ())
					merge_predecessors (*block, in);

				KeyState out = in;
				walk_block_for_key (*block, key, keys, barriers, out, /*settle=*/false);
				block_out.try_emplace (block, std::move (out));
				continue;
			}

			llvm::SmallVector<BasicBlock *, 8> worklist (scc.blocks.begin (), scc.blocks.end ());
			llvm::SmallPtrSet<BasicBlock *, 8> queued (scc.blocks.begin (), scc.blocks.end ());

			for (BasicBlock *block : scc.blocks) {
				block_in.try_emplace (block);
				block_out.try_emplace (block);
			}

			while (!worklist.empty ()) {
				BasicBlock *block = worklist.pop_back_val ();
				queued.erase (block);

				KeyState &in = block_in.find (block)->second;
				bool changed = block == &f.getEntryBlock () ? false : merge_predecessors (*block, in);

				if (!changed)
					continue;

				KeyState out = in;
				walk_block_for_key (*block, key, keys, barriers, out, /*settle=*/false);
				block_out.find (block)->second = std::move (out);

				for (BasicBlock *succ : successors (block)) {
					if (scc.blocks.contains (succ) && queued.insert (succ).second)
						worklist.push_back (succ);
				}
			}
		}

		// Every block's IN has converged now, so a second pass over each of
		// them settles what its own loads of this key read without growing
		// it any further.
		for (auto &entry : block_in)
			walk_block_for_key (*entry.first, key, keys, barriers, entry.second, /*settle=*/true);
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
ConstantValues::invalidate (Function &, const PreservedAnalyses &pa,
                            FunctionAnalysisManager::Invalidator &)
{
	return !pa.getChecker (built_by).preserved ();
}

AnalysisKey MonoConstantValues::Key;
AnalysisKey MonoMemoryValues::Key;

namespace {

ConstantValues
settle (Function &f, AnalysisKey *built_by, bool reads_memory)
{
	ConstantValues values;
	ConstantValuesSolver solver (f, reads_memory);

	solver.solve (values, built_by);

	return values;
}

} // namespace

ConstantValues
MonoConstantValues::run (Function &f, FunctionAnalysisManager &)
{
	return settle (f, ID (), /*reads_memory=*/false);
}

ConstantValues
MonoMemoryValues::run (Function &f, FunctionAnalysisManager &)
{
	return settle (f, ID (), /*reads_memory=*/true);
}

} // namespace mono
