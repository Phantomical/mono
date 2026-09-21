/**
 * \file
 * \brief The declaration Enum.HasFlag () is written as, and the call it lowers
 * to.
 */

#ifndef MONO_LLVM_PASSES_ENUM_FLAG_HPP
#define MONO_LLVM_PASSES_ENUM_FLAG_HPP

#include <llvm/ADT/StringRef.h>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/*
 * i8 @mono.enum.hasflag (ptr this, ptr flag, ptr fallback)
 *
 * this and flag are the two boxed operands HasFlag () was called on. fallback
 * is the method itself, declared the way an ordinary call site declares it. A
 * site eliminate_enum_has_flag () (passes/builtins.hpp) leaves standing lowers
 * into a plain call of fallback instead of a probe.
 */

/// The declaration in \p m, created on first use.
constexpr llvm::StringRef enum_hasflag_name = "mono.enum.hasflag";
llvm::Function *enum_hasflag_decl (llvm::Module &m);

/// Rewrites every remaining call to the declaration into a call of its own
/// fallback operand, erases the declaration, and says whether it changed
/// anything.
bool lower_enum_has_flag (llvm::Module &m);

} // namespace mono

#endif
