/**
 * \file
 * \brief Lowering the calls the translator leaves standing for the runtime.
 *
 * A few methods cannot be called the way their metadata reads. The translator
 * emits those sites as a call to a `mono.builtin.*` declaration that says what
 * the site means rather than how it is reached. No opcode that can reach such
 * a method needs a special case for it.
 *
 * The one builtin so far is the string constructor. It has no instance to
 * fill in, since a string's length is not known until the arguments have
 * been read. So the runtime compiles it as a method that builds the string
 * and returns it, taking a this it never reads. The translator asks only for
 * the object. The null this belongs to the lowering.
 */

#ifndef MONO_LLVM_PASSES_LOWER_BUILTINS_HPP
#define MONO_LLVM_PASSES_LOWER_BUILTINS_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace mono {

/// The name prefix of every builtin declaration, the attribute naming which
/// builtin it is, and the attribute naming the function it stands for. Only
/// the translator writes these three.
constexpr llvm::StringRef builtin_prefix = "mono.builtin.";
constexpr llvm::StringRef builtin_attribute = "mono-builtin";
constexpr llvm::StringRef builtin_target_attribute = "mono-builtin-target";

constexpr llvm::StringRef builtin_string_constructor = "string_constructor";

/// Rewrites every call to a `mono.builtin.*` declaration into the call it
/// stands for, erases the declarations, and says whether it changed anything.
bool lower_runtime_builtins (llvm::Module &m);

} // namespace mono

#endif
