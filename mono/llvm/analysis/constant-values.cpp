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
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/Analysis/MemorySSA.h>
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
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ModRef.h>

#include <cstdint>
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

/// A pointer's own base with any constant GEP offsets folded in.
using AddrKey = std::pair<Value *, int64_t>;

/// \p ptr as (base, byte offset), or nothing where a non-constant index
/// leaves the offset unknowable.
///
/// Two pointers name the same address only where both halves agree, which is
/// what lets apply_store () below forward a store to a load with no alias
/// query: it compares this against the same call on the load's own pointer.
std::optional<AddrKey>
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
/// through a pointer could alias what such a call touches. A write barrier
/// answers false the same way for the card it marks - its declared effects
/// name only the field argument and the table, never a third address - which
/// is what lets it stand between a store and a load with neither losing the
/// other.
bool
may_clobber_tracked_memory (const CallBase &call)
{
	MemoryEffects effects = call.getMemoryEffects ();
	ModRefInfo elsewhere =
		effects.getWithoutLoc (MemoryEffects::Location::InaccessibleMem).getModRef ();

	return isModSet (elsewhere);
}

/// The range a bulk write - a value copy, or a plain `llvm.mem{cpy,move,set}`
/// the translator or a stock pass left standing - writes, or nothing where
/// its target or length does not settle to a constant.
///
/// Such a call's own declared effects cover the whole of its destination
/// argument, so apply_call () below reads the range here instead of asking
/// an alias query to place it more precisely.
std::optional<std::pair<AddrKey, uint64_t>>
value_copy_target (const CallBase &call, const DataLayout &dl)
{
	if (const auto *mem = dyn_cast<MemIntrinsic> (&call)) {
		std::optional<APInt> length = mem->getLengthInBytes ();

		if (!length)
			return std::nullopt;

		auto key = normalize_address (mem->getDest (), dl);

		if (!key)
			return std::nullopt;

		return std::make_pair (*key, length->getZExtValue ());
	}

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

	auto key = normalize_address (call.getArgOperand (0), dl);

	if (!key)
		return std::nullopt;

	return std::make_pair (*key, bytes.getZExtValue ());
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

	/// Null until gather_memory_deps () finds a load to forward.
	///
	/// Building one is cheap: MemorySSAAnalysis places a def or a use per
	/// memory-touching instruction and a phi per merge, all off dominance,
	/// with no alias query. What is expensive is asking it to skip a write
	/// it has not yet proven safe to skip - MemorySSA calls that
	/// optimizing a use, and apply_block () below never asks for it.
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

	/// Every simple load with a resolvable address, and the key it asked
	/// about - built once in gather_memory_deps () so apply_block () below
	/// can settle a load as it walks past it.
	DenseMap<LoadInst *, AddrKey> load_keys;

	/// What a store, a bulk copy, or a write this walk could not place
	/// settles one key to, over every path the walk in gather_memory_deps ()
	/// below has combined into it so far.
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

		/// Whether a write with no known value has reached here. Left
		/// standing alongside \c sources rather than clearing them: a load
		/// already folds an opaque flag and a known value together, the
		/// same as it does for a merge of disagreeing arms.
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

	/// One block's answer for every key at once: an explicit entry where a
	/// write settled or ruled one out, and \c all_opaque for a write this
	/// walk could not pin to one key, which reaches every key this map has
	/// no entry for rather than one picked out by address.
	struct MemoryState {
		DenseMap<AddrKey, KeyState> touched;
		bool all_opaque = false;

		/// Applies a write this walk could not pin to one key: every key
		/// with no entry yet reads opaque from \c all_opaque from here on,
		/// and every key already in \c touched needs that flag set on it
		/// directly, since \c all_opaque only reaches get ()'s synthesized
		/// answer for a key that stays absent.
		void mark_all_opaque ()
		{
			all_opaque = true;

			for (auto &kv : touched)
				kv.second.opaque = true;
		}

		/// \p key's answer as of here, real where \c touched has it,
		/// otherwise synthesized from \c all_opaque.
		KeyState get (const AddrKey &key) const
		{
			auto found = touched.find (key);

			if (found != touched.end ())
				return found->second;

			return KeyState { {}, 0, /*maybe_unwritten=*/!all_opaque, all_opaque };
		}

		/// Folds \p other into this the way KeyState::merge () folds one key,
		/// over the union of keys either side has touched: a key only one
		/// side has still merges against the other's \c get (), which is
		/// what a predecessor that never mentions a key answers for it.
		void merge_in (const MemoryState &other)
		{
			for (const auto &kv : other.touched) {
				KeyState folded = get (kv.first);
				folded.merge (kv.second);
				touched[kv.first] = std::move (folded);
			}

			for (auto &kv : touched) {
				if (other.touched.contains (kv.first))
					continue;

				kv.second.merge (other.get (kv.first));
			}

			all_opaque = all_opaque || other.all_opaque;
		}
	};

	/// Every key some load in the function normalizes to, so a store or a
	/// call site is only ever charged against the keys it could actually
	/// answer rather than the address space at large.
	DenseSet<AddrKey> tracked_keys;

	/// \c tracked_keys grouped by base, so a copy call's own destination
	/// finds only the keys sharing it instead of every tracked key.
	DenseMap<Value *, SmallVector<AddrKey, 2>> keys_by_base;

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

		for (Instruction &at : instructions (f)) {
			auto *load = dyn_cast<LoadInst> (&at);

			// An atomic or volatile load can change outside this function.
			if (load == nullptr || !load->isSimple ())
				continue;

			auto key = normalize_address (load->getPointerOperand (), dl);

			if (!key)
				continue;

			load_keys.try_emplace (load, *key);
			tracked_keys.insert (*key);
		}

		// A key nothing loads is nothing any consumer of this analysis
		// reads back, so settling it would cost this walk a solve for an
		// answer nobody asks.
		if (tracked_keys.empty ())
			return;

		for (const AddrKey &key : tracked_keys)
			keys_by_base[key.first].push_back (key);

		mssa = &fam.getResult<MemorySSAAnalysis> (f).getMSSA ();

		// One forward pass in reverse postorder, one lattice covering every
		// tracked key at once rather than one pass per key. A back edge
		// finds its target still absent from block_out and is dropped
		// rather than iterated to a fixed point, so a store a loop carries
		// into its own next iteration is not forwarded to a load earlier in
		// the same loop.
		DenseMap<BasicBlock *, MemoryState> block_out;

		for (BasicBlock *block : ReversePostOrderTraversal<Function *> (&f)) {
			MemoryState state;
			bool first = true;

			for (BasicBlock *pred : predecessors (block)) {
				auto found = block_out.find (pred);

				if (found == block_out.end ())
					continue;

				if (first) {
					state = found->second;
					first = false;
				} else {
					state.merge_in (found->second);
				}
			}

			apply_block (*block, state);
			block_out.try_emplace (block, std::move (state));
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

	/// Applies one block's own writes to \p state, in place, settling what
	/// each of the block's own loads reads along the way.
	///
	/// Walks MemorySSA's own access list for \p block rather than its raw
	/// instructions - already narrowed, by MemorySSAAnalysis's own
	/// construction, to the instructions that touch memory - with no scan
	/// of this walk's own needed to find them.
	void apply_block (BasicBlock &block, MemoryState &state)
	{
		const MemorySSA::AccessList *accesses = mssa->getBlockAccesses (&block);

		if (accesses == nullptr)
			return;

		for (const MemoryAccess &access : *accesses) {
			if (isa<MemoryPhi> (access))
				continue;

			Instruction *inst = cast<MemoryUseOrDef> (access).getMemoryInst ();

			if (auto *load = dyn_cast<LoadInst> (inst)) {
				auto found = load_keys.find (load);

				if (found != load_keys.end ())
					settle_load (*load, found->second, state.get (found->second));

				continue;
			}

			if (auto *store = dyn_cast<StoreInst> (inst)) {
				apply_store (*store, state);
				continue;
			}

			if (auto *call = dyn_cast<CallBase> (inst)) {
				apply_call (*call, state);
				continue;
			}

			// A fence, an atomic RMW or cmpxchg, or anything else this walk
			// does not otherwise recognize as a write: it names no address
			// this walk could use to clear it, the same as a query
			// answering MayAlias against every key at once.
			state.mark_all_opaque ();
		}
	}

	/// \p store's half of apply_block (): replaces \p state's entry for the
	/// key it names, or - a non-simple store, or an address that does not
	/// normalize - answers every key opaque, since either could still be
	/// any of them.
	void apply_store (StoreInst &store, MemoryState &state)
	{
		if (!store.isSimple ()) {
			state.mark_all_opaque ();
			return;
		}

		auto key = normalize_address (store.getPointerOperand (), dl);

		if (!key) {
			state.mark_all_opaque ();
			return;
		}

		if (!tracked_keys.contains (*key))
			return;

		state.touched[*key] = KeyState { ValueSources (store.getValueOperand ()),
		                                 dl.getTypeStoreSize (store.getValueOperand ()->getType ()),
		                                 /*maybe_unwritten=*/false, /*opaque=*/false };
	}

	/// \p call's half of apply_block (): a copy whose target settles to a
	/// constant range answers opaque for only the keys sharing its base and
	/// falling inside it, read through keys_by_base rather than every
	/// tracked key; anything may_clobber_tracked_memory () admits without a
	/// settled range answers every key opaque, the same as an unplaceable
	/// store.
	void apply_call (CallBase &call, MemoryState &state)
	{
		if (!may_clobber_tracked_memory (call))
			return;

		std::optional<std::pair<AddrKey, uint64_t>> copy = value_copy_target (call, dl);

		if (!copy) {
			state.mark_all_opaque ();
			return;
		}

		auto found = keys_by_base.find (copy->first.first);

		if (found == keys_by_base.end ())
			return;

		int64_t copy_lo = copy->first.second, copy_hi = copy_lo + (int64_t) copy->second;

		for (const AddrKey &key : found->second) {
			if (key.second < copy_lo || key.second >= copy_hi)
				continue;

			KeyState clobbered = state.get (key);
			clobbered.opaque = true;
			state.touched[key] = std::move (clobbered);
		}
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
