/**
 * \file
 * \brief Naming a callee that a site can enter directly.
 *
 * A pass answering a site that used to dispatch needs two things of the method
 * it answers with: whether a caller may name that method at all, and a
 * declaration to call it through. Neither question depends on what settled the
 * site, so they live here rather than beside any one of the passes that ask.
 */

#ifndef MONO_LLVM_PASSES_DIRECT_CALL_HPP
#define MONO_LLVM_PASSES_DIRECT_CALL_HPP

namespace llvm {
class Function;
class FunctionType;
class Module;
} // namespace llvm

typedef struct _MonoMethod MonoMethod;

namespace mono {

struct CompileState;

/// \p target as a caller can name it, or null where it cannot.
///
/// Null covers an abstract method, an unbound generic one, one implemented
/// outside IL, and one whose entry needs a context. A synchronized method comes
/// back as its wrapper, which is what a direct call has to name so that the
/// body still runs under its lock.
MonoMethod *nameable (MonoMethod *target);

/// The entry a call of \p shape enters \p target through, declared in \p m.
///
/// Null where the engine will not publish an entry, and null as well where the
/// module already holds a declaration for the method under another prototype: a
/// site calling one prototype through another is not the call the IL settled.
llvm::Function *entry_for (llvm::Module &m, MonoMethod *target,
                           llvm::FunctionType *shape, const CompileState &compile);

} // namespace mono

#endif
