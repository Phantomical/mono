/**
 * \file
 * \brief The symbols a method's code is published under, and which entries a
 * method has at all.
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

/// The symbol a method's stub is published under.
std::string stub_symbol (MonoMethod *method);

/// The symbol the C-convention entry is compiled under.
///
/// A suffix on the stub's symbol, because the stub already answers to the
/// method's own. Still a name of the method's rather than an anonymous one, so
/// that a dump, a profile and a stack trace all say which method it belongs to.
std::string interop_symbol (MonoMethod *method);

/// Give every declaration in a module that names another method's code the
/// symbol this engine publishes that method under.
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
/// entry in front of its body.
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
