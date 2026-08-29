/**
 * \file
 * \brief What the values in one function settle to.
 */

#ifndef MONO_LLVM_ANALYSIS_CONSTANT_VALUES_HPP
#define MONO_LLVM_ANALYSIS_CONSTANT_VALUES_HPP

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/GlobalValue.h>
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
struct ValueSources {
	llvm::SmallSet<llvm::Value *, 4> sources;

	/// Does sources contain all possible values?
	bool complete = false;

	ValueSources () = default;

	/// Reached by \p v and by nothing else.
	explicit ValueSources (llvm::Value *v) : complete (true) { sources.insert (v); }

	static ValueSources empty ()
	{
		ValueSources answer;
		answer.complete = true;
		return answer;
	}

	static ValueSources anything () { return {}; }

	bool is_empty () const { return sources.empty () && complete; }
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

	/// Get all known constants that \p v could take.
	const ValueSources &sources (llvm::Value *v) const;

	/// Answered off MemorySSA, so a caller that keeps this has to drop it when
	/// that goes.
	bool invalidate (llvm::Function &f, const llvm::PreservedAnalyses &pa,
	                 llvm::FunctionAnalysisManager::Invalidator &inv);

private:
	friend class ConstantValuesSolver;

	llvm::DenseMap<llvm::Value *, ValueSources> settled;
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
