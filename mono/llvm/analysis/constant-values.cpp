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
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/ModRef.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <utility>

using namespace llvm;

namespace mono {
namespace {

/// The most values one set names before it reports only that there are more.
///
/// Every rule folded over a set wants all of it to agree, so a set this wide
/// already answers nothing. The cap is also what bounds the solve: a value
/// climbs at most this far, so its readers are re-read a fixed number of times
/// rather than once per value in the function.
constexpr unsigned source_cap = 8;

/// Folds \p from into \p into, and says whether \p into grew.
///
/// A set only grows, which is what makes the walk terminate.
bool
join (ValueSources &into, const ValueSources &from)
{
	if (from.is_empty ())
		return false;

	bool grew = false;

	for (Value *v : from.sources) {
		if (into.sources.contains (v))
			continue;

		if (into.sources.size () >= source_cap) {
			grew |= std::exchange (into.complete, false);
			break;
		}

		into.sources.insert (v);
		grew = true;
	}

	if (!from.complete && into.complete) {
		into.complete = false;
		grew = true;
	}

	return grew;
}

/// Whether \p alloc hands back memory that reads as zero.
///
/// Read off the declaration rather than assumed: not every allocation form
/// carries the kind, and one that does not can hand back anything.
bool
zero_fills (const CallBase &alloc)
{
	const Function *callee = alloc.getCalledFunction ();

	// getAllocKind () reads through an attribute this one may not carry, and
	// the installed LLVM has the assertion that would have caught it off.
	if (!callee->hasFnAttribute (Attribute::AllocKind))
		return false;

	AllocFnKind kind = callee->getFnAttribute (Attribute::AllocKind).getAllocKind ();

	return (kind & AllocFnKind::Zeroed) != AllocFnKind::Unknown;
}

/// The range a value copy writes, or nothing where this compile cannot read
/// how wide it is.
///
/// Alias analysis reads the length of a memory intrinsic off the call, which
/// is what bounds a memcpy. A value copy is an ordinary call, so its width is
/// read here or the write is taken as running to the end of the object.
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

/// Returns \p held as a constant of type \p want, or null where it cannot be
/// one.
///
/// The walk reaches a value through a cast, and through a store whose type is
/// not the load's, so what it names can be the right value under the wrong
/// type. A caller writes the answer back where it asked, so it needs the type
/// it asked with.
Constant *
as_type (Constant *held, Type *want)
{
	if (held->getType () == want)
		return held;

	if (held->getType ()->isPointerTy () && want->isIntegerTy ())
		return ConstantExpr::getPtrToInt (held, want);

	if (held->getType ()->isIntegerTy () && want->isPointerTy ())
		return ConstantExpr::getIntToPtr (held, want);

	return nullptr;
}

/// Returns the object \p address names and the offset it sits at inside it. A
/// null base is an offset this compile cannot read.
std::pair<Value *, int64_t>
base_and_offset (Value *address, const DataLayout &dl)
{
	APInt offset (dl.getIndexTypeSizeInBits (address->getType ()), 0);
	Value *base = address->stripAndAccumulateConstantOffsets (dl, offset,
	                                                          /*AllowNonInbounds=*/true);

	if (!offset.isSignedIntN (64))
		return {nullptr, 0};

	return {base, offset.getSExtValue ()};
}

} // namespace

/// Drives the walk over one function and hands back what it settled.
class ConstantValuesSolver : public InstVisitor<ConstantValuesSolver, ValueSources> {
	Function &f;
	const DataLayout &dl;
	MemorySSA &mssa;
	ConstantValues &into;

	/// The values this walk settles, least dependent first.
	///
	/// Reverse post-order puts a value behind everything that reaches it, so an
	/// acyclic dependence settles in one visit and only a value carried round a
	/// loop is read again. Draining in any other order costs a pass that reads
	/// operands nothing has settled yet.
	SmallVector<Instruction *, 64> order;

	/// Where a value sits in `order`. A value with no entry is one no other
	/// value here is reached through, and pushing it would settle nothing.
	DenseMap<Value *, unsigned> ranked;

	BitVector queued;
	std::priority_queue<unsigned, SmallVector<unsigned, 64>, std::greater<unsigned>> work;

	/// Maps a value to the loads built from it. A load is visited again when
	/// the base it reads through, or a store into it, grows.
	DenseMap<Value *, SmallPtrSet<Value *, 2>> dependents;

	/// The writes that reach one load.
	struct MemoryReach {
		/// The value operand of each store this walk can read the load out of.
		SmallVector<Value *, 4> stored;

		/// Whether a path reaches memory this function never wrote.
		bool unwritten = false;

		/// Whether a path reaches a write this analysis cannot read.
		bool opaque = false;
	};

	/// The walk each load this solve has run one for came back with, so a load
	/// re-read as its stores settle costs a lookup rather than another walk.
	DenseMap<LoadInst *, MemoryReach> reaches;

public:
	ConstantValuesSolver (Function &f, MemorySSA &mssa, ConstantValues &into)
		: f (f), dl (f.getParent ()->getDataLayout ()), mssa (mssa), into (into)
	{
	}

	void solve ()
	{
		SmallPtrSet<BasicBlock *, 16> reached;

		for (BasicBlock *block : ReversePostOrderTraversal<Function *> (&f)) {
			reached.insert (block);
			rank (*block);
		}

		// A block no path from the entry reaches sits in no post-order, and
		// its values still have to answer: a phi in a reachable block can
		// name one as the value an unreachable edge brings.
		for (BasicBlock &block : f)
			if (!reached.contains (&block))
				rank (block);

		queued.resize (order.size (), true);

		for (unsigned at = 0; at < order.size (); at++)
			work.push (at);

		while (!work.empty ()) {
			unsigned at = work.top ();

			work.pop ();
			queued.reset (at);
			settle (*order[at]);
		}
	}

	ValueSources visitPHINode (PHINode &phi)
	{
		ValueSources sources = ValueSources::empty ();

		for (Value *incoming : phi.incoming_values ())
			join (sources, sources_of (incoming));

		return sources;
	}

	ValueSources visitSelectInst (SelectInst &pick)
	{
		ValueSources sources = ValueSources::empty ();

		join (sources, sources_of (pick.getTrueValue ()));
		join (sources, sources_of (pick.getFalseValue ()));

		return sources;
	}

	/// These three keep the address and change the type, so what reaches the
	/// cast is what reaches its operand. Every other cast computes a value of
	/// its own and is left to the fold below.
	ValueSources visitPtrToIntInst (PtrToIntInst &at) { return sources_of (at.getOperand (0)); }

	ValueSources visitIntToPtrInst (IntToPtrInst &at) { return sources_of (at.getOperand (0)); }

	ValueSources visitBitCastInst (BitCastInst &at) { return sources_of (at.getOperand (0)); }

	ValueSources visitFreezeInst (FreezeInst &frozen)
	{
		const ValueSources of = sources_of (frozen.getOperand (0));

		for (Value *v : of.sources) {
			if (isa<UndefValue> (v))
				return ValueSources::anything ();
		}

		return of;
	}

	/// What \p load reads where nothing in this function wrote the location.
	///
	/// A zero for each allocation the pointer can name that says it zeroes.
	/// Memory that promises nothing is uninitialised, and no value names that,
	/// so only the zero sets \p forwarded.
	ValueSources unwritten_contents (LoadInst &load, bool &forwarded)
	{
		Value *base = base_and_offset (load.getPointerOperand (), dl).first;

		if (base == nullptr)
			return ValueSources::anything ();

		depend (base, &load);

		const ValueSources from = sources_of (base);

		// Nothing has settled for the pointer yet, so wait rather than
		// answering off a base this walk has not read.
		if (from.is_empty ())
			return ValueSources::empty ();

		ValueSources sources = ValueSources::empty ();

		if (!from.complete)
			join (sources, ValueSources::anything ());

		for (Value *src : from.sources) {
			// Loading through a null pointer is UB, so no path that does it
			// reaches an answer.
			if (isa<ConstantPointerNull> (src))
				continue;

			CallBase *alloc = allocation_behind (src);

			if (alloc == nullptr || !zero_fills (*alloc)) {
				join (sources, ValueSources::anything ());
				continue;
			}

			forwarded = true;
			join (sources, ValueSources (Constant::getNullValue (load.getType ())));
		}

		return sources;
	}

	/// The writes that reach \p load.
	///
	/// Reads no settled value, which is what lets the answer be kept. Asking one
	/// here would leave every load that walked earlier holding a stale answer.
	MemoryReach walk_memory (LoadInst &load)
	{
		MemoryLocation where = MemoryLocation::get (&load);
		BatchAAResults aa (mssa.getAA ());
		MemorySSAWalker *walker = mssa.getWalker ();

		SmallPtrSet<MemoryAccess *, 8> seen;
		SmallVector<MemoryAccess *, 8> work{walker->getClobberingMemoryAccess (&load, aa)};
		MemoryReach reach;

		while (!work.empty ()) {
			MemoryAccess *at = work.pop_back_val ();

			if (!seen.insert (at).second)
				continue;

			if (mssa.isLiveOnEntryDef (at)) {
				reach.unwritten = true;
				continue;
			}

			if (auto *merge = dyn_cast<MemoryPhi> (at)) {
				for (Use &incoming : merge->incoming_values ()) {
					auto *from = cast<MemoryAccess> (incoming.get ());

					work.push_back (walker->getClobberingMemoryAccess (from, where, aa));
				}
				continue;
			}

			auto step_past = [&] {
				work.push_back (walker->getClobberingMemoryAccess (
					cast<MemoryDef> (at)->getDefiningAccess (), where, aa));
			};

			Instruction *wrote = cast<MemoryUseOrDef> (at)->getMemoryInst ();

			if (auto *copy = dyn_cast<CallBase> (wrote)) {
				std::optional<MemoryLocation> writes = value_copy_target (*copy);

				if (writes.has_value () && aa.alias (*writes, where) == AliasResult::NoAlias) {
					step_past ();
					continue;
				}
			}

			auto *store = dyn_cast<StoreInst> (wrote);

			// Not a store at all, so we don't know what it did. Assume it could
			// have done anything at all and step past it.
			if (store == nullptr) {
				reach.opaque = true;
				step_past ();
				continue;
			}

			AliasResult alias = aa.alias (MemoryLocation::get (store), where);
			if (alias == AliasResult::NoAlias) {
				step_past ();
				continue;
			}

			// This walk reads a whole store or nothing, so a store wider
			// than the load names no value either.
			if (store->isSimple ()
			    && dl.getTypeStoreSize (store->getValueOperand ()->getType ())
			           == dl.getTypeStoreSize (load.getType ()))
				reach.stored.push_back (store->getValueOperand ());
			else
				reach.opaque = true;

			// MustAlias means we stop walking backwards here.
			if (alias == AliasResult::MustAlias)
				continue;

			step_past ();
		}

		return reach;
	}

	ValueSources visitLoadInst (LoadInst &load)
	{
		const ValueSources self = ValueSources (&load);

		// Atomic or volatile loads are considered to potentially be modified
		// from outside the function. As such, we don't propagate known values
		// through them.
		if (!load.isSimple ())
			return self;

		Value *base = base_and_offset (load.getPointerOperand (), dl).first;

		if (base != nullptr) {
			depend (base, &load);

			// We don't know what the pointer is yet, so don't read any
			// values through it.
			if (sources_of (base).is_empty ())
				return ValueSources::empty ();
		}

		auto placed = reaches.try_emplace (&load);

		if (placed.second) {
			placed.first->second = walk_memory (load);

			for (Value *stored : placed.first->second.stored)
				depend (stored, &load);
		}

		const MemoryReach &reach = placed.first->second;
		ValueSources sources = ValueSources::empty ();
		bool forwarded = !reach.stored.empty ();

		if (reach.opaque)
			join (sources, ValueSources::anything ());

		for (Value *stored : reach.stored)
			join (sources, sources_of (stored));

		if (reach.unwritten)
			join (sources, unwritten_contents (load, forwarded));

		// If we can't find anything then we get to be the source value.
		if (!forwarded)
			return self;

		return sources;
	}

	ValueSources visitInstruction (Instruction &at)
	{
		// A call produces a value of its own, and so does anything else with
		// a side effect. Only an operation over its operands folds away.
		if (at.mayHaveSideEffects () || at.mayReadFromMemory () || at.isTerminator ()
		    || at.getType ()->isVoidTy ())
			return ValueSources (&at);

		SmallVector<Constant *, 4> operands;

		for (const Use &use : at.operands ()) {
			Constant *held = into.value (use.get ());

			if (held == nullptr)
				return ValueSources (&at);

			operands.push_back (held);
		}

		Constant *folded = ConstantFoldInstOperands (&at, operands, dl);

		if (folded == nullptr)
			return ValueSources (&at);

		return ValueSources (folded);
	}

private:
	ValueSources sources_of (Value *v) const
	{
		// A constant and an argument hold themselves wherever they are read,
		// so neither needs an entry of its own.
		if (isa<Constant> (v) || isa<Argument> (v))
			return ValueSources (v);

		auto found = into.settled.find (v);
		if (found == into.settled.end ())
			return ValueSources::anything ();

		return found->second;
	}

	/// Gives each of \p block's values the rank it settles at.
	void rank (BasicBlock &block)
	{
		for (Instruction &at : block) {
			// A constant and an argument hold themselves. They are entered
			// here rather than answered on lookup, because a caller reads
			// the entry by reference.
			for (const Use &use : at.operands ()) {
				Value *held = use.get ();

				if (isa<Constant> (held) || isa<Argument> (held))
					into.settled.try_emplace (held, ValueSources (held));
			}

			// No value is an operand of anything, so settling one answers
			// nothing and costs an entry in each map here.
			if (at.getType ()->isVoidTy ())
				continue;

			ranked.try_emplace (&at, order.size ());
			order.push_back (&at);
			into.settled.try_emplace (&at, ValueSources::empty ());
		}
	}

	void push (Value *v)
	{
		auto found = ranked.find (v);

		if (found == ranked.end () || queued.test (found->second))
			return;

		queued.set (found->second);
		work.push (found->second);
	}

	void push_users (Value *v)
	{
		for (User *user : v->users ())
			push (user);

		auto found = dependents.find (v);
		if (found == dependents.end ())
			return;

		for (Value *dependent : found->second)
			push (dependent);
	}

	void depend (Value *on, Value *dependent) { dependents[on].insert (dependent); }

	void settle (Instruction &at)
	{
		ValueSources next = visit (at);
		auto placed = into.settled.try_emplace (&at, ValueSources::empty ());

		if (!join (placed.first->second, next))
			return;

		push_users (&at);
	}
};

const ValueSources &
ConstantValues::sources (Value *v) const
{
	static const ValueSources unknown = ValueSources::anything ();
	auto found = settled.find (v);

	if (found == settled.end ())
		return unknown;

	return found->second;
}

Constant *
ConstantValues::value (Value *v) const
{
	const ValueSources &from = sources (v);
	Constant *held = nullptr;

	// A constant holds itself, including one from a function this walk never
	// ran over.
	if (auto *direct = dyn_cast<Constant> (v))
		held = direct;
	else if (from.complete && from.sources.size () == 1)
		held = dyn_cast<Constant> (*from.sources.begin ());

	if (held == nullptr)
		return nullptr;

	return as_type (held, v->getType ());
}

bool
ConstantValues::invalidate (Function &f, const PreservedAnalyses &pa,
                            FunctionAnalysisManager::Invalidator &inv)
{
	return !pa.getChecker<MonoConstantValues> ().preserved ()
	       || inv.invalidate<MemorySSAAnalysis> (f, pa);
}

AnalysisKey MonoConstantValues::Key;

ConstantValues
MonoConstantValues::run (Function &f, FunctionAnalysisManager &fam)
{
	ConstantValues values;
	ConstantValuesSolver solver (f, fam.getResult<MemorySSAAnalysis> (f).getMSSA (), values);

	solver.solve ();

	return values;
}

} // namespace mono
