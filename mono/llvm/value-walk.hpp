/**
 * \file
 * \brief Walking a value through the merges standing in front of it.
 *
 * A pass that wants to know what a value holds cannot read the value alone. The
 * optimizer puts merges between the producer and the use: a phi where two paths
 * join, and a select. GVN's partial-redundancy elimination inserts fresh phis as
 * well, over values the translator wrote as single instructions.
 *
 * The traversal through those merges is the same for every question, so it lives
 * here and each question stays with its caller. A caller brings a rule that
 * reads one value and merges two results. This header walks the merges, stops a
 * cycle and bounds the work.
 */

#ifndef MONO_LLVM_VALUE_WALK_HPP
#define MONO_LLVM_VALUE_WALK_HPP

#include "passes/strip-casts.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include <optional>

namespace mono {

/// Bounds how many merge nodes one walk visits, so a large phi web costs a
/// compile only a small, fixed amount of work.
constexpr unsigned merge_walk_budget = 24;

/// Holds what one walk shares across its recursive calls.
///
/// `walk_value ()` reads and writes this state, and hands the same one to the
/// rule's `arms ()`, so a rule's own traversal spends the same budget.
/// `visiting` names the values on the walk's own call stack, which is what stops
/// a cycle: a value found there contributes nothing and is not visited again.
/// That is sound by induction. If every arm outside the cycle names one answer,
/// the cycle itself never disagrees with it.
struct WalkState {
	llvm::SmallPtrSet<const llvm::Value *, 8> visiting;
	unsigned budget = merge_walk_budget;
};

/*
 * What `walk_value ()` asks of the rule it is given. The rule names the answer
 * as `Answer` and provides:
 *
 *   std::optional<Answer> leaf (const llvm::Value *v)
 *       Reads v on its own, with no merge under it. `std::nullopt` skips v.
 *
 *   Answer exhausted ()
 *       The answer for a merge the budget cannot pay for.
 *
 *   bool skips_null ()
 *       Whether the walk skips a null pointer constant instead of reading it.
 *
 *   llvm::SmallVector<const llvm::Value *, 4> arms (const llvm::Value *v,
 *                                                  WalkState &state)
 *       The values a merge node of the rule's own kind names, and empty where v
 *       is not one. A rule with no such node always returns empty. The walk
 *       marks v as visiting around this call, so a resolved arm that reaches v
 *       again stops there.
 *
 *   bool fold (std::optional<Answer> &acc, Answer arm)
 *       Folds one arm's answer into acc. Returns false where the merge is
 *       settled. The walk then stops reading arms and hands back whatever acc
 *       holds.
 *
 * `std::nullopt` and an answer are different results. `std::nullopt` means
 * "found nothing": the walk skipped every value it reached, by the cycle rule or
 * by the rule's own. A merge above this one can still settle on its other arms.
 * Every other result is concrete and carries out to the merge above. The
 * function a caller calls is where a `std::nullopt` turns into a result the
 * caller can read.
 */

template <typename Rule>
std::optional<typename Rule::Answer>
walk_value (const llvm::Value *v, Rule &rule, WalkState &state);

namespace detail {

/// Folds the answers \p arms give into one, through \p rule.
template <typename Rule>
std::optional<typename Rule::Answer>
merge_arms (llvm::ArrayRef<const llvm::Value *> arms, Rule &rule, WalkState &state)
{
	std::optional<typename Rule::Answer> acc;

	for (const llvm::Value *arm : arms) {
		std::optional<typename Rule::Answer> got = walk_value (arm, rule, state);

		if (!got)
			continue;

		if (!rule.fold (acc, *got))
			return acc;
	}

	return acc;
}

} // namespace detail

/// Reads \p v through \p rule: the leaf answer where \p v is not a merge, and
/// the fold of the values it merges where it is one. Spends \p state, so one
/// state covers one question.
template <typename Rule>
std::optional<typename Rule::Answer>
walk_value (const llvm::Value *v, Rule &rule, WalkState &state)
{
	v = strip_casts (v);

	if (state.visiting.count (v)
	    || (rule.skips_null () && llvm::isa<llvm::ConstantPointerNull> (v)))
		return std::nullopt;

	const auto *phi = llvm::dyn_cast<llvm::PHINode> (v);
	const auto *select = phi == nullptr ? llvm::dyn_cast<llvm::SelectInst> (v) : nullptr;
	llvm::SmallVector<const llvm::Value *, 4> other;

	if (phi == nullptr && select == nullptr) {
		// Resolving the rule's arms can lead back to v itself. Marking v
		// visiting across the call is what keeps that from recursing forever.
		state.visiting.insert (v);
		other = rule.arms (v, state);
		state.visiting.erase (v);
	}

	if (phi == nullptr && select == nullptr && other.empty ())
		return rule.leaf (v);

	// A merge this walk cannot afford to enter gets a concrete answer, not a
	// skip. Unlike the back edge of a cycle, nothing bounds what the unexplored
	// side of a merge can name.
	if (state.budget == 0)
		return rule.exhausted ();

	--state.budget;
	state.visiting.insert (v);

	std::optional<typename Rule::Answer> result;

	if (phi != nullptr) {
		llvm::SmallVector<const llvm::Value *, 4> incoming (
			phi->incoming_values ().begin (), phi->incoming_values ().end ());

		result = detail::merge_arms (incoming, rule, state);
	} else if (select != nullptr) {
		const llvm::Value *pair[] = { select->getTrueValue (),
			                      select->getFalseValue () };

		result = detail::merge_arms (pair, rule, state);
	} else {
		result = detail::merge_arms (other, rule, state);
	}

	state.visiting.erase (v);

	return result;
}

} // namespace mono

#endif
