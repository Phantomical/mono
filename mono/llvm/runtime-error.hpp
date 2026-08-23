#ifndef MONO_LLVM_RUNTIME_ERROR_HPP
#define MONO_LLVM_RUNTIME_ERROR_HPP

#include "config.h"

#include <glib.h>

#include "mono/utils/mono-error.h"
#include "mono/utils/mono-error-internals.h"

// This breaks some LLVM headers
#undef PIC

#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <system_error>

namespace mono {

/// A failure the runtime reported through a MonoError, carried as an llvm::Error.
///
/// Constructing one adopts the failure. The MonoError it came from is left
/// clean, and the strings it owns are released when the llvm::Error is
/// finally consumed. Runtime code further up can take the failure back with
/// move_to () and raise it as whatever exception it describes.
class RuntimeError : public llvm::ErrorInfo<RuntimeError> {
public:
	static char ID;

	explicit RuntimeError (MonoError *error);
	~RuntimeError ();

	RuntimeError (const RuntimeError &) = delete;
	RuntimeError &operator= (const RuntimeError &) = delete;

	/// Hand the failure back to runtime code, leaving this one clean.
	void move_to (MonoError *dest);

	/// The MONO_ERROR_* code this failure was raised with.
	uint16_t code () const;

	void log (llvm::raw_ostream &os) const override;
	std::error_code convertToErrorCode () const override;

private:
	/* Formatting the message memoizes it in the error, so reading one is a write. */
	mutable MonoError error;
};

/// Returns error as an llvm::Error, adopting it. A MonoError holding no
/// failure becomes success, so a call that reports only through one needs no
/// test of its own:
///
///     if (llvm::Error error = runtime_error (metadata_error))
///             return error;
///
/// llvm::Expected asserts on a success Error, so returning one of those means
/// going through whatever the call itself said to establish the failure first:
///
///     ERROR_DECL (metadata_error);
///     MonoMethodSignature *sig = mono_method_signature_checked (method, metadata_error);
///     if (sig == nullptr)
///             return runtime_error (metadata_error);
llvm::Error runtime_error (MonoError *error);

} // namespace mono

#endif
