/**
 * \file
 * \brief The symbols a method's code is published under, and which of its
 * entries a method has at all.
 *
 * A method owns two disjoint families of name. Its *stubs* are what the rest of
 * the process binds to - the address the runtime is handed, a vtable slot, a
 * generated caller's call target - and they are what the engine publishes. Its
 * *definitions* are the thunks compiled beside its body, inside the body's own
 * module, and they exist only because a definition and the stub in front of it
 * cannot share a name while they share a module.
 *
 * The two are separate functions here rather than one with a flag, because
 * confusing them is silent: a definition given a stub's name makes the module
 * define a symbol the stubs already own, and the post-link scan that goes
 * looking for the thunk by name then does not find it.
 */

#ifndef MONO_LLVM_RUNTIME_NAMING_HPP
#define MONO_LLVM_RUNTIME_NAMING_HPP

#include "method-symbols.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include <string>

typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodSignature MonoMethodSignature;

namespace llvm {
class Module;
}

namespace mono {

/// The chunk of a symbol that makes it one method's rather than another's.
///
/// The printed name is for reading; the pointer is the identity. No name scheme
/// is unique on its own - conversion operators overload on their return type,
/// which no printed signature carries, and the runtime mints wrappers whose
/// names are only as distinct as what they were generated from.
std::string identity_of (MonoMethod *method);

/// The symbol the stub for one of a method's entries is published under.
std::string stub_symbol (MonoMethod *method, Entry entry);

/// What stub_symbol () hangs off a method's base symbol for a given entry, so
/// that a caller holding the base already does not have to print the method
/// again to reach the rest of its names.
llvm::StringRef stub_suffix (Entry entry);

/// The symbol the thunk for an entry is defined under inside the body's module.
///
/// Only the interop and unbox entries have one. The body is named by the
/// translator, which builds it, and that name deliberately differs from the
/// body stub's: one prints the signature and the other does not, so a
/// self-reference inside the module resolves to the stub rather than to the
/// definition beside it, and survives a later recompile.
std::string definition_symbol (MonoMethod *method, Entry entry);

/// Give every declaration in a module that names another method's code the
/// symbol this engine publishes that entry under.
///
/// The translator marks each such declaration with the MonoMethod and leaves
/// the naming alone, so this is the only place the two sides meet.
llvm::Error bind_symbols (llvm::Module &m);

/// A symbol as a profile or a debugger should print it: the method's readable
/// name, keeping whatever suffix said which piece of the method this is.
///
/// The identity chunk is noise to a reader, and it moves between runs - which
/// is what stops two profiles of the same program from being compared, or
/// aggregated.
std::string display_name (MonoMethod *method, llvm::StringRef symbol);

/// Whether a call can ever reach a method with a boxed receiver, so that it
/// wants an unboxing entry beside its ordinary one.
///
/// Every such call comes off a value type's vtable or its IMT, which is exactly
/// where the runtime asks for the unboxing address.
bool wants_unbox_entry (MonoMethod *method, MonoMethodSignature *sig);

/// Whether a method is entered from native code, and so needs a C-convention
/// entry in front of its body and a stub of its own to publish it through.
///
/// That is exactly the wrappers generated for the other side of the boundary to
/// call - runtime-invoke, native-to-managed, the vtfixup and thunk-invoke
/// entries - each of which sets the pinvoke flag on its own signature. A
/// [DllImport] method sets it too, but it is not a wrapper: what stands behind
/// it is the marshaling wrapper, which is entered like any other method.
bool publishes_interop_entry (MonoMethod *method);

/// Whether this engine gives a method an unboxing entry of its own, and so a
/// stub to publish it through. A method not implemented in IL is entered
/// through code this engine did not generate; the runtime wraps those itself.
bool publishes_unbox_entry (MonoMethod *method);

} // namespace mono

#endif
