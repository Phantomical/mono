/**
 * \file
 * \brief Redirectable call stubs - the only address the JIT ever publishes.
 *
 * Every method is published as a stub that jumps through a writable slot, so a
 * later tier can be swapped in by writing the slot: callers keep their direct
 * call to the stub and pick up the new code on their next call. That is the
 * mechanism promotion is built on, and it is also what makes runtime detours
 * (Harmony/MonoMod) work, which is where the unusual stub geometry comes from.
 */

#ifndef MONO_LLVM_STUBS_HPP
#define MONO_LLVM_STUBS_HPP

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/RedirectionManager.h>
#include <llvm/Support/Error.h>

#include <memory>

namespace mono {

/// Build the redirectable-symbol manager the JIT publishes stubs through.
/// Fails on architectures we do not emit stubs for.
llvm::Expected<std::unique_ptr<llvm::orc::RedirectableSymbolManager>>
make_redirectable_symbol_manager (llvm::orc::ExecutionSession &es);

} // namespace mono

#endif
