/**
 * \file
 * \brief Exact devirtualization for mono's LLVM JIT tier-1 pipeline.
 *
 * Turns an indirect dispatch into a direct call whenever the receiver's class
 * can be *proven* exactly - never guessed. There is no guard and no fallback
 * path: every rewrite is a theorem, so the failure mode is a site left alone,
 * not a site left wrong. Nothing here assumes anything about the class
 * hierarchy either, so no later assembly load can invalidate a rewrite.
 *
 * The translator tags each dispatch with the method the front end resolved it
 * on ("mono.virtcall", see translator-call.cpp); this pass supplies the other
 * half - the receiver's exact class - and asks the runtime what the two
 * together resolve to.
 *
 * It runs inside the inliner's round loop rather than once, because the two
 * feed each other: inlining a callee exposes the allocations inside it, which
 * lets more receivers be proven, which produces more direct calls for the next
 * round to inline. Running late in a round is also what makes receivers that
 * arrive through a field store and reload work - by then the round's
 * SROA/GVN have already forwarded them - so this pass needs no memory analysis
 * of its own.
 */

#ifndef MONO_MINI_LLVM_DEVIRT_HPP
#define MONO_MINI_LLVM_DEVIRT_HPP

#include "devirt-support.hpp"

namespace llvm {
class Module;
}

namespace mono {

/*
 * Rewrite every tagged dispatch in MODULE whose receiver class is provable into
 * a direct call to the method it resolves to. Returns how many sites were
 * rewritten.
 *
 * ROOT is the compile this module belongs to; it supplies the domain the
 * targets' trampolines are created in and the module the declarations go into.
 */
unsigned devirtualize (llvm::Module &module, const Tier1Root &root);

} // namespace mono

#endif /* MONO_MINI_LLVM_DEVIRT_HPP */
