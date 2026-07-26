/**
 * \file
 * inliner-support.hpp - the narrow boundary between the pure-LLVM inliner pass
 * (inliner.cpp) and the mono-aware materialization glue (implemented in
 * translator.cpp, which has all the mono headers in scope).
 *
 * mini.h comes in for MonoCompile and MonoMethod - the root cfg and the callee
 * the pass hands straight back through these calls.
 */

#ifndef MONO_MINI_LLVM_INLINER_SUPPORT_HPP
#define MONO_MINI_LLVM_INLINER_SUPPORT_HPP

#include <mono/mini/mini.h>

/*
 * mono-tls.h (via mini.h) defines PIC when we are built as PIE, and LLVM's
 * PassBuilder.h uses PIC as an identifier - drop it so a TU that includes this
 * header before its LLVM headers still compiles.
 */
#ifdef PIC
#undef PIC
#endif

namespace llvm {
class Function;
class Module;
}

namespace mono {

/*
 * The tier-1 root registry, keyed by the root's LLVM Function. The translator
 * registers a root (with its MonoCompile) right before it optimizes the module,
 * and unregisters it once optimization returns; the inliner pass runs inside
 * that window and looks the cfg up to drive materialization.
 */
void register_tier1_root (llvm::Function *root, MonoCompile *root_cfg);
void unregister_tier1_root (llvm::Function *root);
MonoCompile *tier1_root_cfg (llvm::Function *root);

/*
 * Whether ROOT_CFG permits inlining at all - the caller-level eligibility
 * gates: -O=inline on, not a debug method (disable_inline), and not shared
 * generic code (gshared/gsharedvt). A NOOPTIMIZATION method never becomes a
 * root in the first place, since it declines to the classic JIT before LLVM
 * ever sees it. Checked once per root.
 */
bool tier1_root_allows_inlining (MonoCompile *root_cfg);

/*
 * Which of the gates above turned ROOT_CFG down, as a MONO_INLINER_TRACE tag.
 * Only meaningful when tier1_root_allows_inlining () returned false.
 */
const char *tier1_root_refusal_reason (MonoCompile *root_cfg);

/*
 * Map a direct-call target symbol back to the managed MonoMethod it names, or
 * NULL if SYM is not a managed-method symbol (an icall, an intrinsic, the root
 * itself, ...).
 */
MonoMethod *managed_method_from_symbol (const char *sym);

/*
 * Obtain TARGET's body on demand: run its front-end and translate it into
 * module INTO as an internal Function, using ROOT_CFG for domain/opt. Returns
 * the materialized Function, or NULL if the callee cannot be materialized - a
 * decline the caller treats as "leave the trampoline call in place". This is
 * conservative: it refuses wrappers, synchronized methods, and any callee that
 * still needs a generic context - open/shared generic types and methods (the
 * rgctx gate, #26), checked up front before the front-end runs - in addition to
 * whatever the front-end itself declines.
 */
llvm::Function *materialize_callee (MonoMethod *target, MonoCompile *root_cfg,
                                    llvm::Module *into);

/*
 * Whether TARGET reads or writes a static field of a class that still needs its
 * cctor to run - i.e. an accessor of class-init-guarded static state. Inlining
 * such a callee drops the class-init barrier that its own managed call would
 * have carried, so the inliner must refuse it. This is decided from the callee's
 * metadata, NOT from a class-init call in the materialized IR (the barrier is
 * often elided there), and is conservative: true means "do not inline". Returns
 * true if the body cannot be inspected.
 */
bool callee_reads_cctor_guarded_static (MonoMethod *target);

} // namespace mono

#endif /* MONO_MINI_LLVM_INLINER_SUPPORT_HPP */
