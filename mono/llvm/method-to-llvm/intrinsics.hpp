/**
 * \file
 * \brief The registry of methods this backend compiles into something other
 * than a call.
 */

#ifndef MONO_LLVM_METHOD_TO_LLVM_INTRINSICS_HPP
#define MONO_LLVM_METHOD_TO_LLVM_INTRINSICS_HPP

#include "mono/metadata/metadata.h"
#include "mono/metadata/object-forward.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Error.h>

#include <optional>
#include <string_view>

namespace mono {

class MethodLLVMEmitter;

/// A call site the registry is asked about.
struct BuiltinCall {
	/// The callee, after a constrained. prefix has resolved it to an override.
	MonoMethod *callee;
	/// The signature the call site was written against, which for a vararg site
	/// is the site's own.
	MonoMethodSignature *sig;
	/// The method being translated.
	MonoMethod *caller;
	/// The class a constrained. prefix named, or null.
	MonoClass *constrained;
	/// Whether the receiver has already been boxed for this call.
	bool box_receiver;
};

/// What a registry entry answers: nothing where it leaves the call standing,
/// and otherwise success or the failure the emission ran into.
using BuiltinResult = std::optional<llvm::Error>;

/// Writes what the call compiles to, or answers nothing where no entry claims
/// it and the caller has to emit the call.
BuiltinResult emit_builtin_call (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
                                 const BuiltinCall &call);

/// Writes the whole of method's body, or answers nothing where the backend
/// does not write it.
///
/// Call it with the builder at the end of the entry block. An entry that
/// answers success has written the body's terminator too.
BuiltinResult emit_builtin_body (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
                                 MonoMethod *method);

/// Whether method has to be compiled from the body the backend writes, because
/// its own IL computes something else.
bool builtin_body_replaces_il (MonoMethod *method);

/// The param_count of an entry that matches whatever arity it is asked about.
constexpr int any_params = -1;

/// The name an entry's class is matched by.
struct ClassKey {
	/// The assembly's name, or null for corlib.
	const char *assembly;
	const char *name_space;
	const char *name;
};

/// One method the backend writes the whole body of.
struct BuiltinBody {
	ClassKey klass;
	/// The method's name, or empty to take every method the class declares.
	std::string_view name;
	/// The arity this row is written for, or any_params.
	int param_count;
	/// Whether the method's own IL computes what this body computes. False
	/// keeps the method out of every engine that runs the IL.
	bool il_agrees;
	/// Whether this row answers at all, or null for one that always does.
	/// Asked at each lookup, so it can read an --llvm-opt.
	bool (*enabled) ();
	BuiltinResult (*emit) (MethodLLVMEmitter &, llvm::IRBuilder<> &, MonoMethod *);
};

/// Returns the rows for the SIMD types' operations.
llvm::ArrayRef<BuiltinBody> simd_bodies ();

/// One System.Math or System.MathF method the backend answers itself.
struct MathBuiltin {
	std::string_view name;
	int param_count;
};

/// Every name math_intrinsic_for () can answer, so the registry selects those
/// methods without a second copy of the table it reads.
llvm::ArrayRef<MathBuiltin> math_builtins ();

} // namespace mono

#endif
