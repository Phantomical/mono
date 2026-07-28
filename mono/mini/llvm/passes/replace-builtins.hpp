/**
 * \file
 * replace-builtins.hpp - replace calls to corlib methods the backend has a
 * better answer for than their managed bodies.
 *
 * One so far: `System.Buffer:Memcpy (byte*, byte*, int)` becomes llvm.memcpy.
 *
 * Memcpy is a managed method with a real body: it branches on len > 32, hands
 * the big case to the InternalMemcpy icall (which is a plain memcpy ()), and
 * otherwise dispatches on the low bits of the two pointers into one of three
 * hand-unrolled copy loops. Nothing in the front end recognizes it, so every
 * call stays a call - and the copy is usually a handful of bytes. Its sibling
 * `Buffer:Memmove` needs nothing here: intrinsics.c already turns that one into
 * OP_MEMMOVE, so a call to it never reaches LLVM in the first place.
 *
 * The reason this is a pass rather than another entry in intrinsics.c, next to
 * that Memmove case: the facts that make the rewrite pay off do not exist yet at
 * IL->MonoInst time. `Marshal:ReadInt32` calls `Memcpy (&s, addr, 4)`, and only
 * once the tier-1 inliner has folded ReadInt32 into its caller is the length
 * visibly the constant 4. At that point llvm.memcpy collapses into a single
 * 4-byte load, and the alloca behind `&s` - pinned to the stack for as long as
 * its address escapes into an opaque call - becomes SROA-able.
 *
 * Two things the transform relies on:
 *
 * Overlap. llvm.memcpy requires its two ranges to be equal or disjoint, which
 * Memcpy's callers already have to satisfy: `Buffer:Memmove` checks for overlap
 * itself and only calls Memcpy when there is none, and the len > 32 path is a
 * bare memcpy () today.
 *
 * Negative lengths. `Memcpy` takes a signed int, and a negative one copies
 * nothing today - every loop is a `while (size >= n)`. Sign-extending that into
 * the intrinsic's i64 would turn it into an enormous copy, so a signed length is
 * clamped at zero instead. The clamp is a select rather than a branch, which
 * keeps the pass CFG-preserving, and it folds away entirely whenever the length
 * is a constant - the case that matters.
 *
 * Null pointers are deliberately not checked, because the intrinsic reproduces
 * what the managed body does today on either side of its len > 32 split: a small
 * constant length expands inline, so a null faults at a managed pc and comes
 * back as a NullReferenceException, and a large or unknown one becomes a libc
 * call, which is what InternalMemcpy already was.
 *
 * This depends on the tier-1 inliner leaving Memcpy alone, which corlib arranges
 * by marking it NoInlining. Folding its body in would leave no call here to
 * rewrite, so the rewrite would simply stop happening. The refusal also happens
 * before materialization, which is what keeps the callee named by its trampoline
 * symbol - the only name a call site can be matched back to a method by. A
 * materialized body is named by mono_method_full_name () instead, and only
 * reverts to the symbol once inlining is over and this pass no longer runs.
 */

#ifndef MONO_MINI_LLVM_PASSES_REPLACE_BUILTINS_HPP
#define MONO_MINI_LLVM_PASSES_REPLACE_BUILTINS_HPP

/*
 * PassManager.h uses PIC as an identifier, and libtool passes -DPIC (as does
 * mono-tls.h when we are built as PIE), so the macro has to go before any LLVM
 * header - same dance as the other passes here.
 */
#ifdef PIC
#undef PIC
#endif

#include <llvm/IR/PassManager.h>

namespace llvm {
class Function;
class PassBuilder;
} // namespace llvm

namespace mono {

class ReplaceMonoBuiltins : public llvm::PassInfoMixin<ReplaceMonoBuiltins> {
public:
	/* Hang the pass off the extension points it wants in PB's pipeline. */
	static void register_pass (llvm::PassBuilder &pb);

	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif /* MONO_MINI_LLVM_PASSES_REPLACE_BUILTINS_HPP */
