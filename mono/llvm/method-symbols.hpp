/**
 * \file
 * \brief How a declaration that names another method's code says which method,
 * and how the engine gives it the symbol it publishes that method under.
 *
 * The translator cannot know what a method's stub is called - that is the
 * engine's to decide, and the engine is the only thing that can publish one. So
 * the translator marks the declaration with the MonoMethod and which of its
 * entries it wants, and the engine renames it. Neither side has to agree on a
 * mangling because only one side has one.
 */

#ifndef MONO_LLVM_METHOD_SYMBOLS_HPP
#define MONO_LLVM_METHOD_SYMBOLS_HPP

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include <string>

namespace llvm {
class GlobalValue;
class Module;
} // namespace llvm

typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Which of a method's entry points a reference wants.
///
/// A method is published as one stub per entry, all of them redirected together
/// when its code changes, so that whichever door a caller came in through it
/// lands on the current best body.
enum class Entry {
	/// The method itself: what generated code calls, what the runtime is
	/// handed, and what an escaped function pointer holds.
	body,
	/// The C-convention entry, which only a wrapper generated for native code
	/// to enter has.
	interop,
	/// The entry a call off a value type's vtable or IMT arrives at, which
	/// unboxes the receiver before continuing into the body.
	unbox,
};

/// Mark VALUE as standing for ENTRY of METHOD.
void mark_method_entry (llvm::GlobalValue &value, MonoMethod *method, Entry entry);

/// Rename every marked declaration in M to what NAME_OF calls that entry,
/// leaving the module referring only to symbols the engine publishes.
///
/// NAME_OF is also where the engine publishes the method, since a name it has
/// not published is one nothing will link against.
///
/// Declarations only. A definition carrying the marker is something built to a
/// declaration's shape and given its attributes along the way - it names itself,
/// and renaming it would take away the name it is about to be compiled under.
llvm::Error
bind_method_symbols (llvm::Module &m,
                     llvm::function_ref<llvm::Expected<std::string> (MonoMethod *, Entry)> name_of);

} // namespace mono

#endif
