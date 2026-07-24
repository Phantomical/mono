/**
 * \file
 * inliner-support.hpp - the narrow boundary between the pure-LLVM inliner pass
 * (inliner.cpp) and the mono-aware materialization glue (implemented in
 * translator.cpp, which has all the mono headers in scope).
 *
 * Everything mono here is passed as an opaque `void *` (a MonoCompile * or a
 * MonoMethod *) so inliner.cpp needs no mono headers - it just threads these
 * tokens back through these calls. MonoCompile is an anonymous-struct typedef
 * and cannot be forward-declared, which is the other reason the boundary is
 * void *-typed rather than pointer-to-incomplete-type.
 */

#ifndef MONO_MINI_LLVM_INLINER_SUPPORT_HPP
#define MONO_MINI_LLVM_INLINER_SUPPORT_HPP

namespace llvm {
class Function;
class Module;
}

namespace mono {

/*
 * The tier-1 root registry, keyed by the root's LLVM Function. The translator
 * registers a root (with its opaque MonoCompile *) right before it optimizes
 * the module, and unregisters it once optimization returns; the inliner pass
 * runs inside that window and looks the cfg up to drive materialization.
 */
void register_tier1_root (llvm::Function *root, void *root_cfg);
void unregister_tier1_root (llvm::Function *root);
void *tier1_root_cfg (llvm::Function *root);

/*
 * Whether ROOT_CFG (an opaque MonoCompile *) permits inlining at all - the
 * caller-level eligibility gates: -O=inline on, not a NOOPTIMIZATION/debug
 * method (disable_inline), and not shared generic code (gshared/gsharedvt).
 * Checked once per root.
 */
bool tier1_root_allows_inlining (void *root_cfg);

/*
 * Map a direct-call target symbol back to the managed MonoMethod it names
 * (returned opaque), or NULL if SYM is not a managed-method symbol (an icall,
 * an intrinsic, the root itself, ...).
 */
void *managed_method_from_symbol (const char *sym);

/*
 * Obtain TARGET's body on demand: run its front-end and translate it into
 * module INTO as an internal Function, using ROOT_CFG for domain/opt. TARGET and
 * ROOT_CFG are opaque MonoMethod * / MonoCompile *. Returns the materialized
 * Function, or NULL if the callee cannot be materialized - a decline the caller
 * treats as "leave the trampoline call in place". This is conservative: it
 * refuses wrappers, synchronized methods, and any callee that still needs a
 * generic context - open/shared generic types and methods (the rgctx gate,
 * #26), checked up front before the front-end runs - in addition to whatever
 * the front-end itself declines.
 */
llvm::Function *materialize_callee (void *target, void *root_cfg, llvm::Module *into);

/*
 * Whether TARGET (an opaque MonoMethod *) reads or writes a static field of a
 * class that still needs its cctor to run - i.e. an accessor of class-init-
 * guarded static state. Inlining such a callee drops the class-init barrier that
 * its own managed call would have carried, so the inliner must refuse it. This
 * is decided from the callee's metadata, NOT from a class-init call in the
 * materialized IR (the barrier is often elided there), and is conservative:
 * true means "do not inline". Returns true if the body cannot be inspected.
 */
bool callee_reads_cctor_guarded_static (void *target);

} // namespace mono

#endif /* MONO_MINI_LLVM_INLINER_SUPPORT_HPP */
