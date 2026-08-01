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

#include <cstdint>
#include <memory>

namespace mono {

/*
 * A stub is the 6 bytes of `jmpq *slot(%rip)` padded with int3 out to 16.
 *
 * Stock JITLink stubs are those 6 bytes at alignment 1, so they pack tightly
 * and a detour would run off the end of one and into its neighbour. The widest
 * patch Harmony and MonoMod write is 14 bytes - `jmp *0(%rip)` plus the 8-byte
 * destination behind it - so a stub has to own at least that many for a detour
 * to be containable. 16 is that, rounded up to the alignment, and the two
 * bytes left over trap anything that jumps into the tail.
 */
constexpr uint64_t stub_block_size = 16;
constexpr uint64_t stub_alignment = 16;

/// Build the redirectable-symbol manager the JIT publishes stubs through.
/// Fails on architectures we do not emit stubs for.
llvm::Expected<std::unique_ptr<llvm::orc::RedirectableSymbolManager>>
make_redirectable_symbol_manager (llvm::orc::ExecutionSession &es);

} // namespace mono

#endif
