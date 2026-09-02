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

namespace perf {
class BatchSink;
} // namespace perf

/// Where one method's code ended up. The jit-info record is null for a stand-in
/// that only raises.
struct Compiled {
	void *body = nullptr;
	MonoJitInfo *jinfo = nullptr;
};

/// Records that a piece of code, and the record registered for it, belong to
/// a method. Every compile passes through here, or freeing the method leaves
/// that compile's code and record behind for good.
using RememberFn = llvm::function_ref<void (const CompiledMethod &, MonoJitInfo *)>;

/// Run as a given domain for as long as this is alive.
///
/// The restore has to survive every way out of the scope, which is why it is
/// a guard rather than a call on each return path.
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

struct TranslationTarget {
	MonoJit *jit;

	/// The domain the code will run as - the owning linker's, never the
	/// thread's current one.
	MonoDomain *domain;

	/// Publish a callee and return the record its stub was carved on.
	llvm::function_ref<llvm::Expected<MonoDomainMethod *> (MonoMethod *)> publish_callee;

	/// Runs after a compile publishes; see RememberFn.
	RememberFn remember;

	/// Decide what a failed translation means. A metadata failure is something
	/// the program is owed as an exception rather than a method that fails to
	/// compile, and becomes a stand-in body that raises it. Anything else is
	/// handed back unchanged.
	llvm::function_ref<llvm::Expected<Compiled> (llvm::Error)> recover;

	JitTier tier = JitTier::tier1;

	/// The counts a tier-2 body is laid out by. Empty for a method promoted
	/// before it ran, and unread at tier 1.
	llvm::ArrayRef<uint8_t> profile;

	/// Where a batch compile collects every member's jitdump pieces, so they
	/// publish as the batch's own object rather than one record apiece. Null
	/// for a target compiled outside compile_bodies (), which dumps itself
	/// as it publishes.
	perf::BatchSink *dump_sink = nullptr;
};

/// Compile a method, whatever it takes: the marshal wrapper for an array
/// accessor, mini's own code for a method not implemented in IL, and otherwise a
/// translation of its IL.
///
/// This raises the profiler's jit_begin, and jit_failed when no body gets
/// published. It raises no end on success: a non-null *published is what
/// tells the caller to raise jit_done once the method can be looked up.
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

struct BatchResult {
	llvm::Expected<Compiled> code;
	/// The body's record, on the terms translate_body () states.
	MonoJitInfo *published = nullptr;
};

/// Translate several methods into one module and compile them together, on the
/// terms translate_and_compile () states for each of them.
///
/// The results line up with methods. Sharing a module is what makes this worth
/// asking for: the per-compile cost LLVM charges is paid once for the batch
/// rather than once for each method. The methods must share a domain, a linker
/// and a tier.
///
/// Anything the shared module cannot hold is compiled one method at a time
/// instead. A tier-2 compile among them goes on its own, since its code is
/// laid out by its own method's counts. So does the whole batch, when one
/// member fails to translate. A caller gets the same answers whether it asks
/// for the batch or for each method on its own.
std::vector<BatchResult>
translate_and_compile_batch (llvm::ArrayRef<const TranslationTarget *> targets,
                             llvm::ArrayRef<MonoMethod *> methods);

/// Compile the C-convention entry native code enters \p method through.
///
/// The entry is a module of its own rather than a rider on the body's, so its
/// address is the method's for good. It calls \p body, the method's stub, so
/// every later compile is reached through the redirect the stub already gets.
llvm::Expected<void *> compile_interop_entry (MonoJit &jit, MonoDomain *domain,
                                              MonoMethod *method, void *body,
                                              RememberFn remember);

} // namespace mono

#endif
