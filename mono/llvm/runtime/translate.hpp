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

class MonoDomainMethod;

/// Where one method's code ended up. The jit-info record is null for a stand-in
/// that only raises.
struct Compiled {
	void *body = nullptr;
	MonoJitInfo *jinfo = nullptr;
};

/// Note that a piece of code, and the record registered for it, belong to a
/// method - see TranslationTarget::remember.
using RememberFn = llvm::function_ref<void (const CompiledMethod &, MonoJitInfo *)>;

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

	/// Publish a callee and answer the record its stub was carved on.
	llvm::function_ref<llvm::Expected<MonoDomainMethod *> (MonoMethod *)> publish_callee;

	/// Note that a piece of code, and the record registered for it, belong to
	/// this method. Every compile passes through here, or freeing the method
	/// leaves that compile's code and record behind for good.
	RememberFn remember;

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

/// Compile the C-convention entry native code enters \p method through.
///
/// The entry is a module of its own rather than a rider on the body's, so that
/// its address is the method's for good: it calls \p body, the method's stub, so
/// every later compile is reached through the redirect the stub already gets.
llvm::Expected<void *> compile_interop_entry (MonoJit &jit, MonoDomain *domain,
                                              MonoMethod *method, void *body,
                                              RememberFn remember);

} // namespace mono

#endif
