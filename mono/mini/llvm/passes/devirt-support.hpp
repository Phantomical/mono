/**
 * \file
 * devirt-support.hpp - the boundary between the devirtualization pass
 * (devirt.cpp) and the mono-aware glue that resolves and names call targets
 * (implemented in translator.cpp, which has the metadata headers in scope).
 *
 * Same split as inliner-support.hpp, and it borrows Tier1Root from there: the
 * pass decides *which* sites are resolvable from the IR, and asks these two
 * calls what a site actually resolves to and how to name it.
 */

#ifndef MONO_MINI_LLVM_DEVIRT_SUPPORT_HPP
#define MONO_MINI_LLVM_DEVIRT_SUPPORT_HPP

#include "inliner-support.hpp"

namespace llvm {
class Function;
class FunctionType;
}

namespace mono {

/*
 * The method a virtual call to DECLARED reaches when its receiver is exactly an
 * instance of KLASS, or NULL if the site must be left alone.
 *
 * Refusals are conservative and set *REASON to a short tag naming which one
 * fired, for the pass's trace. A NULL return is never a failure the caller has
 * to handle - it just means the call site stays indirect.
 */
MonoMethod *resolve_exact_virtual_target (MonoMethod *declared, MonoClass *klass,
                                          const char **reason);

/*
 * A declaration of TARGET in ROOT's module, of type SIG, resolving through a
 * real symbol to TARGET's stable trampoline entry - the same edge
 * get_direct_callee () builds for a call the front end emitted directly, so a
 * rewritten site is indistinguishable from one that was never virtual (which is
 * what lets the inliner pick it up).
 *
 * Returns NULL if TARGET has no reachable trampoline.
 */
llvm::Function *direct_callee_decl (MonoMethod *target, llvm::FunctionType *sig,
                                    const Tier1Root &root);

} // namespace mono

#endif /* MONO_MINI_LLVM_DEVIRT_SUPPORT_HPP */
