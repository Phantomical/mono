/**
 * \file
 * \brief Folding a hot method's callees into it, hottest call site first.
 */

#ifndef MONO_LLVM_PASSES_TOP_DOWN_INLINE_HPP
#define MONO_LLVM_PASSES_TOP_DOWN_INLINE_HPP

#include <llvm/IR/PassManager.h>

#include <cstdint>
#include <utility>

namespace llvm {
class Function;
class InlineCost;
class TargetMachine;
} // namespace llvm

namespace mono {

/// The interface the engine implements so the pass can ask about a candidate.
///
/// The pass reads IR and a profile. Two questions need managed metadata to
/// answer: whether a method can be folded at all, and what its body is. Both go
/// through here, so the pass itself names no metadata.
class InlineCandidates {
public:
	virtual ~InlineCandidates ();

	/// The body to weigh at a site calling \p decl, translated into \p into and
	/// marked as a copy.
	///
	/// into can hold more than the returned function - a candidate brings its
	/// own trivial callees in beside it. The returned function need not share
	/// decl's name either: the caller reconciles any leftover declaration once
	/// the body is linked in, so the site still needs no rewriting of its own.
	///
	/// What comes back is fresh translator output, unless the engine folded the
	/// method into this root already. It then returns the body standing in
	/// decl's own module, which the caller links nothing for. Read getParent ()
	/// to tell the two apart.
	///
	/// Null means the site keeps its call. The engine refuses the method, its
	/// metadata will not load, or the compile has spent the translation it is
	/// allowed.
	virtual llvm::Function *materialize (llvm::Function &decl, llvm::Module &into) = 0;

	virtual void folded (llvm::Function &caller, llvm::Function &callee) = 0;

	/// Says that the cost model weighed \p callee at a site in \p caller and
	/// declined it.
	///
	/// The site keeps its call and StripInlineCopiesPass erases the body behind
	/// the pass, so no other output says the candidate was translated at all.
	///
	/// \p count is what the profile gives the site's block, and it is what
	/// decides the budget: LLVM reads a site cold against the rest of the
	/// caller and hands a cold one much less. \p cost holds the two numbers
	/// where the model reached one. A cost equal to the budget is a count that
	/// stopped there, because the model gives up as soon as a candidate is
	/// over.
	virtual void declined (llvm::Function &caller, llvm::Function &callee,
	                       const llvm::InlineCost &cost, uint64_t count) = 0;

	/// How many folds deep past the root a chain can go. Without a limit a call
	/// graph with a cycle in it never runs out of sites.
	virtual unsigned depth_limit () const = 0;
};

/// Says which engine the inliner is to ask about the module it is running over.
///
/// The pipeline holding the pass is built once per compile thread and run for
/// many compiles, while the candidates belong to one of them. So the binding
/// arrives as an analysis over a slot: a compile writes its own engine into the
/// slot before it runs, and the pass reads back whatever is there.
class InlineCandidatesAnalysis : public llvm::AnalysisInfoMixin<InlineCandidatesAnalysis> {
	friend llvm::AnalysisInfoMixin<InlineCandidatesAnalysis>;
	static llvm::AnalysisKey Key;

	InlineCandidates **slot_;

public:
	/// slot is read at each run, so it has to outlive the analysis manager this
	/// is registered in. Null in it is ordinary and means no engine is
	/// listening: the pass then leaves every site alone.
	explicit InlineCandidatesAnalysis (InlineCandidates *&slot) : slot_ (&slot) {}

	struct Result {
		InlineCandidates *candidates;

		/// Never kept. The engine belongs to one compile, and a result cached
		/// over the last one names an engine that has gone.
		bool invalidate (llvm::Module &, const llvm::PreservedAnalyses &,
		                 llvm::ModuleAnalysisManager::Invalidator &)
		{
			return true;
		}
	};

	Result run (llvm::Module &, llvm::ModuleAnalysisManager &) { return Result { *slot_ }; }
};

/// Folds a method's hottest call sites into it.
///
/// Sites are ranked by the caller's own block counts, so a caller the profile
/// describes spends its budget where the calls really are.
///
/// Each candidate is materialized when its site reaches the front of the queue.
/// So a site the gates or the cost model refuse costs nothing but the questions.
/// A body materialized and then declined is left where it is, and
/// StripInlineCopiesPass takes it off.
class TopDownInlinerPass : public llvm::PassInfoMixin<TopDownInlinerPass> {
public:
	/// materialize is run over the module each candidate is translated into,
	/// and settles what shape the cost model weighs. simplify is run over a
	/// root the loop folded anything into.
	///
	/// The two are the caller's rather than the pass's own so that a candidate
	/// reaches the cost model in the same shape a tier-1 body has. The profile
	/// is keyed on that shape, and a mismatch loses the weights in silence.
	///
	/// Which engine to ask comes from InlineCandidatesAnalysis instead, which
	/// the analysis manager the pass runs under has to have registered.
	TopDownInlinerPass (llvm::TargetMachine &target, llvm::ModulePassManager materialize,
	                    llvm::FunctionPassManager simplify)
	    : target_ (&target), materialize_ (std::move (materialize)),
	      simplify_ (std::move (simplify))
	{
	}

	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);

private:
	llvm::TargetMachine *target_;
	llvm::ModulePassManager materialize_;
	llvm::FunctionPassManager simplify_;
};

} // namespace mono

#endif
