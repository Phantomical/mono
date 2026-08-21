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

/// One tier-2 compile's shared inlining state.
///
/// Both inliners translate into the same module, so both read and add to the
/// same list of what it defines. A method translated in twice would leave the
/// module holding two bodies for it. And a candidate that calls back into a
/// method above it would copy that method into its own callee.
///
/// The budget counts bodies rather than instructions, and the two inliners spend
/// one counter between them. A budget of its own for each would make a compile's
/// translation count the product rather than the sum.
struct InlineScope {
	/// The method the module is being built for. A folded body's code belongs
	/// to this method's frame, and a detour on the folded method has to be able
	/// to find it again.
	MonoMethod *root = nullptr;
	llvm::SmallVector<MonoMethod *, 8> defined;
	uint32_t budget = 0;
};

/// Whether a callee can be folded into its caller without changing what the
/// program does.
///
/// Correctness only. What a fold is worth is a separate question, and one this
/// does not answer.
bool may_fold (MonoDomain *domain, MonoMethod *callee);

/// Reads a four-byte little-endian IL operand: a metadata token, or a
/// displacement.
uint32_t il_read_u32 (const unsigned char *at);

/// Returns the method a call site's token names, or null when the metadata
/// does not resolve it.
MonoMethod *il_call_target (MonoMethod *method, uint32_t token);

/// Whether the body can lose its own frame without changing what a method it
/// calls reports.
///
/// A helper that reads the frame it was called from carries NoInlining, as
/// GetCurrentMethod and the rest do. Fold the body in and such a helper sees the
/// caller's frame instead. A call through a pointer names no method, so a body
/// holding one is refused outright.
bool loses_its_frame_safely (MonoMethod *method, MonoMethodHeader *header);

/// Translates callee into the module its caller is being built in, marks the
/// result as an inline copy and spends one of the scope's budget.
///
/// decl is the declaration the caller calls, and the body takes its place, so
/// no call site needs rewriting. Returns null when the translation failed. The
/// caller's site then calls a declaration of the published entry as it did
/// before, which is what StripInlineCopiesPass makes of a copy either way.
///
/// cfg must hold callee's header, and it has to outlive the call. externals
/// collects what the new body names, so resolve them after this rather than
/// before it.
llvm::Function *materialize_inline_copy (llvm::Module &module, MonoDomain *domain,
                                         MonoMethod *callee, MonoCompile *cfg,
                                         llvm::Function &decl,
                                         std::vector<ExternalSymbol> &externals,
                                         ModuleTypes &types, InlineScope &scope);

} // namespace mono

#endif
