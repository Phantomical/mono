/**
 * \file
 * \brief What the values in one function settle to.
 */

#ifndef MONO_LLVM_ANALYSIS_CONSTANT_VALUES_HPP
#define MONO_LLVM_ANALYSIS_CONSTANT_VALUES_HPP

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>

namespace llvm {
class Constant;
class Function;
class Value;
} // namespace llvm

namespace mono {

class ConstantValuesSolver;

/// The values that can reach one value.
///
/// A set holds the value itself where the walk did not settle all of its paths.
///
/// Past `max_sources` the walk gives up and empties the set. Every rule reading
/// one needs its sources to agree, and a value this many reach rarely has them
/// agree. Dropping only the ones past the limit would be wrong: a rule folding
/// over what was left would find the agreement the dropped source denies.
///
/// Giving up latches, so the state moves one way and the walk settles.
struct ValueSources {
	llvm::SmallPtrSet<llvm::Value *, 4> sources;

	/// Sources past which the walk gives up.
	static constexpr unsigned max_sources = 4;

	ValueSources () = default;

	explicit ValueSources (llvm::Value *v) { sources.insert (v); }

	bool is_empty () const { return sources.empty (); }

	/// Whether the walk gave up on the owner rather than name what reaches it.
	///
	/// The set is empty either way, so a rule folding over it needs no such
	/// question. This is what a test reads to tell the two apart.
	bool is_widened () const { return widened; }

	/// Insert a new value into the set.
	///
	/// \returns true if the value was not already contained in the set
	bool insert (llvm::Value *value)
	{
		if (widened || !sources.insert (value).second)
			return false;

		widen_past_limit ();
		return true;
	}

	bool insert (const ValueSources &other)
	{
		if (widened || this == &other)
			return false;

		if (other.widened)
			return widen ();

		if (other.is_empty ())
			return false;

		std::size_t isz = sources.size ();

		sources.insert_range (other.sources);

		if (isz == sources.size ())
			return false;

		widen_past_limit ();
		return true;
	}

	/// The same, with \p transform applied to each of \p other's sources.
	///
	/// \p owner is the value this set belongs to. A transform that returns null
	/// puts it in place of that source.
	template<typename F>
	bool insert (const ValueSources &other, llvm::Value *owner, F transform)
	{
		if (widened)
			return false;

		if (other.widened)
			return widen ();

		if (other.is_empty ())
			return false;

		llvm::SmallVector<llvm::Value *, 4> reaching (other.sources.begin (),
		                                              other.sources.end ());
		bool modified = false;

		for (llvm::Value *source : reaching) {
			llvm::Value *held = transform (source);

			if (held == nullptr)
				held = owner;

			modified |= sources.insert (held).second;
		}

		if (modified)
			widen_past_limit ();

		return modified;
	}

private:
	bool widen ()
	{
		if (widened)
			return false;

		widened = true;
		sources.clear ();
		return true;
	}

	void widen_past_limit ()
	{
		if (sources.size () > max_sources)
			widen ();
	}

	/// Set once the walk gave up, so a later insert does not reopen the set.
	bool widened = false;
};

/// Contains known constants for all values in the current function.
class ConstantValues {
public:
	/// If \p v has a known constant value then return that, null otherwise.
	llvm::Constant *value (llvm::Value *v) const;

	/// If \p v has a known global value then return that.
	llvm::GlobalValue *global (llvm::Value *v) const
	{
		return llvm::dyn_cast_or_null<llvm::GlobalValue> (value (v));
	}

	/// Every value \p inst can be reached by.
	const ValueSources &sources (llvm::Value *inst) const;

	bool invalidate (llvm::Function &f, const llvm::PreservedAnalyses &pa,
	                 llvm::FunctionAnalysisManager::Invalidator &inv);

private:
	friend class ConstantValuesSolver;

	llvm::DenseMap<llvm::Value *, ValueSources> lookup;

	/// Which analysis below built this, so invalidate () asks about that one.
	llvm::AnalysisKey *built_by = nullptr;
};

/// An analysis that finds all potential sources for each instruction.
///
/// A load stands as its own source. What a store left in the field it reads is
/// `MonoMemoryValues`, below.
class MonoConstantValues : public llvm::AnalysisInfoMixin<MonoConstantValues> {
	friend llvm::AnalysisInfoMixin<MonoConstantValues>;
	static llvm::AnalysisKey Key;

public:
	using Result = ConstantValues;

	Result run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

/// The same walk, with the stores that reach each load folded in.
///
/// A store forwards to a load only where both name the same (base, constant
/// offset) pair, which trades the precision an alias query would answer for
/// not asking one. `top-down-inline.cpp`, `fold-delegate.cpp` and
/// `devirtualize.cpp` ask for this, and each runs at tier 2 alone.
/// Everything else takes `MonoConstantValues`.
class MonoMemoryValues : public llvm::AnalysisInfoMixin<MonoMemoryValues> {
	friend llvm::AnalysisInfoMixin<MonoMemoryValues>;
	static llvm::AnalysisKey Key;

public:
	using Result = ConstantValues;

	Result run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
