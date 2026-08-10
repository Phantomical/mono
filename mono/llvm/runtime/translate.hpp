/**
 * \file
 * \brief Turning a method's IL into linked code.
 */

#ifndef MONO_LLVM_RUNTIME_TRANSLATE_HPP
#define MONO_LLVM_RUNTIME_TRANSLATE_HPP

#include "jit.hpp"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoJitInfo MonoJitInfo;
typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Where one method's code ended up: the body every caller reaches, the C entry
/// for a method native code enters, and - for an instance method of a value type
/// - the unboxing entry a call off that value type's vtable arrives at. The
/// jit-info record is the body's, and is null for a stand-in that only raises.
struct Compiled {
	void *entry = nullptr;
	void *body = nullptr;
	void *unbox = nullptr;
	MonoJitInfo *jinfo = nullptr;
};

/// What an engine has to supply for a method to be translated into it.
///
/// The callbacks are the parts that differ between engines: where a callee's
/// stub comes from, what is remembered so it can be freed again, and what a
/// failure means.
struct TranslationTarget {
	MonoJit *jit;

	/// The domain the code will run as - the owning linker's, never the
	/// thread's current one.
	MonoDomain *domain;

	/// Publish a callee and define its symbol in this linker.
	llvm::function_ref<llvm::Error (MonoMethod *)> publish_callee;

	/// The address a published stub was carved at.
	llvm::function_ref<llvm::Expected<void *> (llvm::StringRef)> stub_address;

	/// Note that a piece of code, and the record registered for it, belong to
	/// this method. Every compile passes through here, or freeing the method
	/// leaves that compile's code and record behind for good.
	llvm::function_ref<void (const CompiledMethod &, MonoJitInfo *)> remember;

	/// Decide what a failed translation means. A metadata failure is something
	/// the program is owed as an exception rather than a method that would not
	/// compile, and becomes a stand-in body that raises it; anything else is
	/// handed back unchanged.
	llvm::function_ref<llvm::Expected<Compiled> (llvm::Error)> recover;
};

/// Translate a method's IL, compile what comes out, and register the jit info
/// for the body and for every other function the module defines.
///
/// published receives the body's record, and is left null when a metadata
/// failure was turned into a stand-in body instead of a real one.
llvm::Expected<Compiled> translate_body (const TranslationTarget &target,
                                         MonoMethod *method, MonoJitInfo **published);

} // namespace mono

#endif
