/**
 * \file
 * runtime-address.hpp: recovering the runtime address behind a constant operand.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#ifndef __MONO_MINI_LLVM_PASSES_RUNTIME_ADDRESS_HPP__
#define __MONO_MINI_LLVM_PASSES_RUNTIME_ADDRESS_HPP__

#include <cstdint>

namespace llvm {
class DataLayout;
class Value;
}

namespace mono {

/*
 * The address a constant operand names, if it names one at all.
 *
 * A pass reasoning about runtime data - which vtable a barrier guards, which
 * class an allocator call builds - has to cope with two spellings of the same
 * fixed address. One is a bare integer literal, or an inttoptr of one: a
 * pointer the translator had nowhere better to put. The other is a reference to
 * one of the external globals that name runtime data for the JIT linker to
 * resolve, which is what most of them are now. Constant offsets and ptrtoint/
 * inttoptr casts can sit on top of either.
 *
 * Resolving the symbol case needs the address the name was registered with, so
 * this only answers for globals the engine actually knows.
 */
bool
runtime_address (const llvm::Value *v, const llvm::DataLayout &dl, uint64_t *addr);

} // namespace mono

#endif /* __MONO_MINI_LLVM_PASSES_RUNTIME_ADDRESS_HPP__ */
