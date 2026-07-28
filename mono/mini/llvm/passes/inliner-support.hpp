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

/* The translator's per-compile state; opaque to the pass (translator-internal.hpp). */
struct MonoLLVMModule;

namespace mono {

/*
 * The compile the inliner pass is running inside, handed to it when its
 * pipeline is built.
 *
 * An LLVM pass is normally reachable only through the IR it is given, which is
 * why this used to be a process-wide registry the pass looked itself up in.
 * It does not have to be: the -O2 pipeline is rebuilt for every compile
 * (MonoLLVMJIT::optimize ()), so the pass object can simply be constructed
 * knowing which compile it belongs to. That keeps per-compile state per
 * compile, and is what lets several compiles optimize at once without sharing
 * anything.
 *
 * FUNC is the tier-1 root - the one method the translator emitted standalone
 * into this module. CFG is its MonoCompile and MODULE the translator state
 * (in particular the LLVMContext) the whole compile is being built in;
 * materializing a callee needs both.
 */
struct Tier1Root {
	llvm::Function *func;
	MonoCompile *cfg;
	MonoLLVMModule *module;
};

/*
 * Whether ROOT_CFG permits inlining at all - the caller-level eligibility gates:
 * -O=inline on, and not a method whose optimizer the user turned off
 * (disable_inline). A NOOPTIMIZATION method never becomes a root in the first
 * place, since it declines to the classic JIT before LLVM ever sees it. Checked
 * once per root.
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
 * ROOT's module as an internal Function, using ROOT's cfg for domain/opt. Returns
 * the materialized Function, or NULL if the callee cannot be materialized - a
 * decline the caller treats as "leave the trampoline call in place". This is
 * conservative: it refuses wrappers, synchronized methods, and any callee that
 * still needs a generic context - open/shared generic types and methods (the
 * rgctx gate, #26), checked up front before the front-end runs - in addition to
 * whatever the front-end itself declines.
 *
 * The one shared callee it does accept is one instantiated over exactly the type
 * parameters ROOT is itself shared over. The root's call site already computes
 * the runtime generic context such a body expects, so it is materialized as the
 * shared body it is and folded in as it stands.
 *
 * DECL is the declaration the body is replacing, and it is what decides the
 * body's shape: whatever the call sites were emitted against is what they will
 * be repointed at. In particular a declaration carrying a `nest` parameter gets
 * a body that accepts one, used or not.
 */
llvm::Function *materialize_callee (MonoMethod *target, const Tier1Root &root,
                                    const llvm::Function *decl);

} // namespace mono

#endif /* MONO_MINI_LLVM_INLINER_SUPPORT_HPP */
