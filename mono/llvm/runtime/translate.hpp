/**
 * \file
 * \brief Turning a method's IL into linked code.
 */

#ifndef MONO_LLVM_RUNTIME_TRANSLATE_HPP
#define MONO_LLVM_RUNTIME_TRANSLATE_HPP

#include "jit.hpp"
#include "method-symbols.hpp"

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
	MonoJitInfo *jinfo = nullptr;

	/// Where one of the method's doors leads, or null when it has no such door.
	void *at (Entry which) const
	{
		switch (which) {
		case Entry::body:
			return body;
		case Entry::interop:
			return entry;
		}
		return body;
	}
};

/// Run as a given domain for as long as this is alive.
///
/// Translation resolves everything per-domain against the domain the code will
/// run as, so the thread has to be that domain while it happens - and the
/// restore has to survive every way out of the scope, which is why it is a
/// guard rather than a call on each return path.
class DomainScope {
public:
	explicit DomainScope (MonoDomain *domain);
	~DomainScope ();

	DomainScope (const DomainScope &) = delete;
	DomainScope &operator= (const DomainScope &) = delete;

private:
	MonoDomain *entered_;
	MonoDomain *wanted_;
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

/// Compile a method, whatever it takes: the marshal wrapper for an array
/// accessor, mini's own code for a method not implemented in IL, and otherwise a
/// translation of its IL.
///
/// This is where the profiler's compilation of a method begins, so every path
/// out of it raises exactly one end - a consumer pairing the two would otherwise
/// carry an open span for the rest of the process.
llvm::Expected<Compiled> translate_and_compile (const TranslationTarget &target,
                                                MonoMethod *method,
                                                MonoJitInfo **published);

/// Translate a method's IL, compile what comes out, and register the jit info
/// for the body and for every other function the module defines.
///
/// published receives the body's record, and is left null when a metadata
/// failure was turned into a stand-in body instead of a real one.
llvm::Expected<Compiled> translate_body (const TranslationTarget &target,
                                         MonoMethod *method, MonoJitInfo **published);

} // namespace mono

#endif
