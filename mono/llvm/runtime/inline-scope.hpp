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
#include <llvm/IR/ValueHandle.h>

#include <cstdint>
#include <vector>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/// Which inliner is asking, and so which count a copy is charged to.
enum class Inliner {
	/// The shape-test pre-pass, which runs at both compiled tiers.
	trivial,

	/// The tier-2 cost model.
	costed,
};

/// What the two inliners share while they build one method's body.
///
/// A batch reuses one of these and resets root, inlined and budget for each
/// member, because defined describes the module and the rest describe the
/// method.
struct InlineScope {
	/// The method the body is being built for. An inlined body's code belongs
	/// to this method's frame, and a detour on the inlined method has to be
	/// able to find it again.
	MonoMethod *root = nullptr;

	/// The methods the module holds the published body of: the root, or every
	/// member of a batch. A call to one of them has to leave through that
	/// method's entry, so the translator declares it under a name of its own.
	///
	/// A copy is not one of these. A copy belongs to the root, and only the
	/// sites under that root are moved onto it.
	llvm::SmallVector<MonoMethod *, 8> defined;

	/// One method this root has taken in, and the body it was taken into.
	struct Inlined {
		MonoMethod *method;

		/// The copy that stands for the method, null when none does. Root is
		/// null, because it has a body rather than a copy.
		///
		/// A handle rather than a raw pointer. The pipeline erases a copy once
		/// it has inlined every call to it, and a raw pointer then reads freed
		/// memory rather than failing to compile.
		llvm::WeakVH copy;
	};

	/// The methods already inlined into root, root itself included. A second
	/// copy of one is dead weight, and without root a candidate that calls
	/// back into it copies root into its own callee.
	///
	/// A later caller under this root is moved onto the copy that stands rather
	/// than left on the published entry, which is what makes the first copy
	/// enough. inlined_copy_in () is what finds it.
	///
	/// This decides the set root ends up with, so it names root's own chain and
	/// nothing else. A batch member then inlines what it inlines compiled
	/// alone, and the two tiers hash the same CFG.
	llvm::SmallVector<Inlined, 8> inlined;

	/// How many more methods each inliner may translate into this body. A
	/// compile translates at most the two added together.
	///
	/// A method is charged once, to the inliner that took it in first.
	struct Budget {
		/// What the shape-test pre-pass spends, at both compiled tiers.
		uint32_t trivial = 0;

		/// What the tier-2 cost model spends. A candidate arrives with its own
		/// trivial callees inlined, and those come out of trivial above.
		uint32_t costed = 0;

		/// IL bytes the tier-2 cost model may still translate into this root,
		/// alongside costed above. Charged in materialize_inline_copy () the
		/// same way costed is: once per candidate translated for the costed
		/// inliner, whether the inline is accepted or not. Not charged against
		/// trivial, which already keeps its own candidates small through
		/// trivial_inline_il_limit ().
		uint32_t costed_bytes = 0;

		uint32_t &of (Inliner who) { return who == Inliner::trivial ? trivial : costed; }
	};

	Budget budget;
};

/// Whether sequence points are on. Both inliners refuse every inline while
/// they are.
///
/// Traces that once, so a run whose host asked for sequence points says why it
/// inlined nothing.
bool inlining_off_for_seq_points ();

/// Whether a callee can be inlined into its caller without changing what the
/// program does.
///
/// Correctness only. What an inline is worth is a separate question, and one
/// this does not answer.
bool is_inlinable (MonoDomain *domain, MonoMethod *callee);

/// Whether the backend writes the method's body itself, rather than
/// translating its IL.
///
/// Both inliners bound what they translate by the callee's IL size, and such a
/// body is a handful of instructions whatever that size is. Vector4f's operator
/// + is 58 bytes of ldfld and newobj over the pre-pass's 32-byte limit, and one
/// fadd once translated. VectorOperations::CompareOrdered is 154 bytes, over
/// the cost model's own 64-byte limit at a cold call site, and one vector
/// compare once translated.
bool is_builtin (MonoMethod *method);

/// Whether a body of this shape can be translated in as an inline copy: no
/// clauses at all, and at most il_limit bytes of IL.
///
/// The clause half is correctness. Inlining a clause-bearing body would need
/// its clauses merged into the caller's table, which neither inliner does. The
/// size half is cost, and each inliner passes its own limit, so that half of the
/// answer is about the caller that asked rather than about the callee alone.
bool is_small_and_clause_free (MonoMethodHeader *header, uint32_t il_limit);

/// Whether a body of this shape is small enough for the cost model to
/// translate and weigh it, clauses included. clause_survives_inline ()
/// (passes/top-down-inline.cpp) is what keeps a clause-bearing inline safe.
bool is_small_enough (MonoMethodHeader *header, uint32_t il_limit);

/// Whether any of header's clauses is a filter.
bool has_filter_clause (MonoMethodHeader *header);

/// Whether this root has taken callee in already. It answers yes for root
/// itself, which the list holds so that a candidate calling back into root does
/// not copy root into its own callee.
bool already_inlined (const InlineScope &scope, MonoMethod *callee);

/// Returns the copy of callee standing in module, or null when this root holds
/// none there.
///
/// Ask this for a callee already_inlined () named, to move a site onto the body
/// that stands rather than leave it on the published entry. A method in
/// scope.inlined does not always have such a body:
///
/// - root has a body rather than a copy, and nothing may call it.
/// - The pipeline erases a copy once it has inlined every call to it.
/// - A copy built for a cost-model candidate stands in that candidate's own
///   module until the link puts it beside root.
///
/// The last two are ordinary rather than rare, so a caller that finds no copy
/// builds one of its own instead of leaving the site on the published entry.
llvm::Function *inlined_copy_in (const InlineScope &scope, MonoMethod *callee,
                                 const llvm::Module &module);

/// Whether from reaches to by calls that stay inside this root's copies.
///
/// Ask before moving a site in to onto from. A copy that already reaches the
/// caller must keep its call: the two would otherwise inline into each other,
/// and the pipeline inlines a cycle of always-inline bodies for as long as it
/// can allocate.
bool copy_reaches (const llvm::Function &from, const llvm::Function &to);

uint32_t il_read_u32 (const unsigned char *at);

/// Returns the method a call site's token names, or null when the metadata
/// does not resolve it.
MonoMethod *il_call_target (MonoMethod *method, uint32_t token);

/// Translates callee into the module its caller is being built in, marks the
/// result as an inline copy and spends one of who's count.
///
/// The body is built under a name of its own rather than over the declaration
/// the caller calls, so the module can hold the method's own body beside it and
/// a caller that inlined nothing keeps its call. The caller moves the sites
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
                                         MonoMethod *callee, TranslateInput *cfg,
                                         std::vector<ExternalSymbol> &externals,
                                         ModuleTypes &types, InlineScope &scope,
                                         Inliner who);

} // namespace mono

#endif
