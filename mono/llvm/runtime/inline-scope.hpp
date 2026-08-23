/**
 * \file
 * \brief What the two tier-2 inliners share: what a compile's module already
 * defines, how much translation they may still add to it, and the gates that
 * are about correctness rather than cost.
 */

#ifndef MONO_LLVM_RUNTIME_INLINE_SCOPE_HPP
#define MONO_LLVM_RUNTIME_INLINE_SCOPE_HPP

#include "method-to-llvm.hpp"

#include <llvm/ADT/SmallVector.h>

#include <cstdint>
#include <vector>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/// What the two inliners share while they build one method's body.
///
/// A batch reuses one of these and resets root, folded and budget for each
/// member, because defined describes the module and the rest describe the
/// method.
///
/// The budget counts bodies rather than instructions, and the two inliners spend
/// one counter between them. A budget of its own for each would make a compile's
/// translation count the product rather than the sum.
struct InlineScope {
	/// The method the body is being built for. A folded body's code belongs to
	/// this method's frame, and a detour on the folded method has to be able to
	/// find it again.
	MonoMethod *root = nullptr;

	/// The methods the module holds the published body of: the root, or every
	/// member of a batch. A call to one of them has to leave through that
	/// method's entry, so the translator declares it under a name of its own.
	///
	/// A copy is not one of these. Only the caller that asked for a copy has its
	/// sites moved onto it.
	llvm::SmallVector<MonoMethod *, 8> defined;

	/// The methods already folded into root, root itself included. A second copy
	/// of one is dead weight, and without root a candidate that calls back into
	/// it copies root into its own callee.
	///
	/// This decides the set root ends up with, so it names root's own chain and
	/// nothing else. A batch member then folds what it folds compiled alone, and
	/// the two tiers hash the same CFG.
	llvm::SmallVector<MonoMethod *, 8> folded;

	uint32_t budget = 0;
};

/// Whether a callee can be folded into its caller without changing what the
/// program does.
///
/// Correctness only. What a fold is worth is a separate question, and one this
/// does not answer.
bool may_fold (MonoDomain *domain, MonoMethod *callee);

/// Whether a body of this shape can be translated in as an inline copy: no
/// clauses at all, and at most il_limit bytes of IL.
///
/// The clause half is correctness. Folding a clause-bearing body in would need
/// its clauses merged into the caller's table, which neither inliner does. The
/// size half is cost, and each inliner passes its own limit, so that half of the
/// answer is about the caller that asked rather than about the callee alone.
bool is_small_and_clause_free (MonoMethodHeader *header, uint32_t il_limit);

uint32_t il_read_u32 (const unsigned char *at);

/// Returns the method a call site's token names, or null when the metadata
/// does not resolve it.
MonoMethod *il_call_target (MonoMethod *method, uint32_t token);

/// Whether an internal call reports something it reads off the frame that
/// called it.
///
/// The caller has already found that target has no IL. Such a body keeps no
/// frame of its own, so a fold moves the frame it reads. This names the corlib
/// calls that walk the managed stack. Each one reaches mono_stack_walk_no_il (),
/// mono_method_get_last_managed () or mono_runtime_get_caller_from_stack_mark ().
///
/// A method outside corlib answers yes. An embedder registers the internal calls
/// it wants, and this backend has read no set but corlib's.
///
/// Add an entry for a new internal call that reads its caller. Without one the
/// fold changes what that call reports, and no failure names this gate.
bool reads_the_callers_frame (MonoMethod *target);

/// Whether the body can lose its own frame without changing what a method it
/// calls reports.
///
/// Fold the body in and a helper it calls sees the caller's frame instead. Two
/// marks say a helper reads that frame: NoInlining, which is what a managed one
/// carries, and an internal call reads_the_callers_frame () names. A call
/// through a pointer names no method, so a body holding one is refused outright.
bool loses_its_frame_safely (MonoMethod *method, MonoMethodHeader *header);

/// Translates callee into the module its caller is being built in, marks the
/// result as an inline copy and spends one of the scope's budget.
///
/// The body is built under a name of its own rather than over the declaration
/// the caller calls, so the module can hold the method's own body beside it and
/// a caller that folded nothing keeps its call. The caller moves the sites
/// meant to reach it onto the returned function.
///
/// Returns null when the translation failed, and the sites then call the
/// published entry as they did before, which is what StripInlineCopiesPass makes
/// of a copy either way.
///
/// cfg must hold callee's header, and it has to outlive the call. externals
/// collects what the new body names, so resolve them after this rather than
/// before it.
llvm::Function *materialize_inline_copy (llvm::Module &module, MonoDomain *domain,
                                         MonoMethod *callee, MonoCompile *cfg,
                                         std::vector<ExternalSymbol> &externals,
                                         ModuleTypes &types, InlineScope &scope);

} // namespace mono

#endif
