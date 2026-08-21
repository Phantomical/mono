/**
 * \file
 * \brief The symbols a method's code is published under, and which entries a
 * method has at all.
 */

#ifndef MONO_LLVM_RUNTIME_NAMING_HPP
#define MONO_LLVM_RUNTIME_NAMING_HPP

#include "mono/llvm/method-symbols.hpp"

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
/// The printed name is for reading. The pointer is the identity. No name
/// scheme is unique on its own. A conversion operator overloads on its return
/// type, which no printed signature carries. A generated wrapper's name is only
/// as distinct as what it was generated from.
std::string identity_of (MonoMethod *method);

/// The symbol a method's stub is published under.
std::string stub_symbol (MonoMethod *method);

/// The symbol the C-convention entry is compiled under.
///
/// A suffix on the stub's symbol, because the stub already answers to the
/// method's own. It keeps the method's name rather than an anonymous one, so a
/// dump, a profile and a stack trace can say which method it is.
std::string interop_symbol (MonoMethod *method);

/// Give every declaration in a module that names another method's code the
/// symbol this engine publishes that method under.
///
/// The translator marks each such declaration with the MonoMethod and leaves
/// the naming alone, so this is the only place the two sides meet.
llvm::Error bind_symbols (llvm::Module &m);

/// A symbol as a profile or a debugger prints it: the method's readable
/// name, keeping whatever suffix names which piece this is.
///
/// The identity chunk is noise to a reader. It also moves between runs, so
/// two profiles of the same program cannot otherwise be compared or merged.
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
/// call: runtime-invoke, native-to-managed, the vtfixup and thunk-invoke
/// entries. Each sets the pinvoke flag on its own signature. A [DllImport]
/// method sets it too, but it is not a wrapper. What stands behind it is the
/// marshaling wrapper, which is entered like any other method.
bool publishes_interop_entry (MonoMethod *method);

/// Whether this engine gives a method an unboxing entry of its own, and so a
/// stub to publish it through. A method not implemented in IL is entered
/// through code this engine did not generate. The runtime wraps such methods
/// itself.
bool publishes_unbox_entry (MonoMethod *method);

} // namespace mono

#endif
