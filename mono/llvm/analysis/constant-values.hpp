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

/// All original source values that a later instruction could result in.
///
/// A value reached by a path this walk cannot name stands for that path itself,
/// so its own presence in its set is what says the set is not every value it
/// holds. That is what a caller folding a rule over the set already handles: the
/// rule reads the value, answers nothing for it, and the merge gives up. There
/// is no second field to consult and forget.
struct ValueSources {
	llvm::SmallPtrSet<llvm::Value *, 1> sources;

	ValueSources () = default;

	/// Reached by \p v and by nothing else.
	explicit ValueSources (llvm::Value *v) { sources.insert (v); }

	/// Whether nothing has settled here yet, which is what a value the walk
	/// has not reached holds. Distinct from a value that stands for itself.
	bool is_empty () const { return sources.empty (); }

	/// Insert a new value into the set.
	///
	/// \returns true if the value was not already contained in the set
	bool insert (llvm::Value *value) { return sources.insert (value).second; }
	bool insert (const ValueSources &other)
	{
		if (this == &other || other.is_empty ())
			return false;

		std::size_t isz = sources.size ();

		sources.insert_range (other.sources);
		return isz != sources.size ();
	}

	/// The same, with \p transform applied to each source \p other names.
	///
	/// A transform answering null has no value to put here for that source, so
	/// \p stands_for goes in instead and the set stops naming every value.
	template<typename F>
	bool insert (const ValueSources &other, llvm::Value *stands_for, F transform)
	{
		if (other.is_empty ())
			return false;

		bool modified = false;

		for (llvm::Value *source : other.sources) {
			llvm::Value *held = transform (source);

			if (held == nullptr)
				held = stands_for;

			modified |= sources.insert (held).second;
		}

		return modified;
	}
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
	///
	/// A set holding \p inst itself has a path this walk could not name, and
	/// \p inst is what stands for it. So a caller reading a rule off each
	/// source needs no separate test: the rule answers nothing for that one.
	const ValueSources &sources (llvm::Value *inst) const;

	/// Answered off MemorySSA, so a caller that keeps this has to drop it when
	/// that goes.
	bool invalidate (llvm::Function &f, const llvm::PreservedAnalyses &pa,
	                 llvm::FunctionAnalysisManager::Invalidator &inv);

private:
	friend class ConstantValuesSolver;

	llvm::DenseMap<llvm::Value *, ValueSources> lookup;
};

/// An analysis that finds all potential sources for each instruction.
class MonoConstantValues : public llvm::AnalysisInfoMixin<MonoConstantValues> {
	friend llvm::AnalysisInfoMixin<MonoConstantValues>;
	static llvm::AnalysisKey Key;

public:
	using Result = ConstantValues;

	Result run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
