/**
 * \file
 * \brief The ORCv2 JIT the LLVM-only backend compiles through.
 *
 * This is the execution-engine half of the new backend: it owns the LLJIT
 * stack (JITLink object layer, the tier pipeline, symbol resolution) and
 * turns method_to_llvm () modules into executable code. It deliberately knows
 * nothing about mono - no metadata, no MonoMethod, no runtime headers - so it
 * can be driven directly by unit tests; the runtime integration layers on top.
 */

#ifndef MONO_LLVM_JIT_HPP
#define MONO_LLVM_JIT_HPP

#include <llvm/ADT/FunctionExtras.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/RedirectionManager.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace llvm {
class TargetMachine;
namespace orc {
class JITCompileCallbackManager;
}
} // namespace llvm

namespace mono {

/// The host TargetMachine every compile runs against - code model Small+PIC and
/// FastISel code generation.
///
/// One instance per calling thread, reused for every module that thread
/// compiles: building one costs more than compiling a typical method, and a
/// TargetMachine cannot be shared across threads.
llvm::TargetMachine &host_target_machine ();

/// Where a compiled method's pieces landed: the code itself, and the side
/// tables the compiler wrote next to it for the runtime to read back.
struct CompiledMethod {
	void *entry = nullptr;

	const uint8_t *code = nullptr;
	size_t code_size = 0;

	/// The `.mono_lsda` clause table; null when the method has no clauses.
	const uint8_t *clause_table = nullptr;
	size_t clause_table_size = 0;
	/// The `.mono_guards` finally-guard table; null when the method has no
	/// finally body left to guard.
	const uint8_t *guard_table = nullptr;
	size_t guard_table_size = 0;

	/// The `.mono_unwind` frame description; never null for a method.
	const uint8_t *unwind_table = nullptr;
	size_t unwind_table_size = 0;

	/// Every function the linked object defines, name to [code, size): the
	/// entry, and any filter bodies compiled alongside it.
	std::vector<std::pair<std::string, std::pair<const uint8_t *, size_t>>> functions;

	/// The dylib this compile's object was linked into - what remove_dylibs ()
	/// takes to release all of the above again.
	llvm::orc::JITDylib *dylib = nullptr;
};

class MonoJit {
public:
	/// Produces the entry point of a method's code, called the first time
	/// something calls that method. Returning an error is fatal to the call.
	using LazyCompileFunction = llvm::unique_function<llvm::Expected<void *> ()>;

	/// Queue OPT for LLVM's own command-line option registry - the same
	/// options `opt` and `llc` take, e.g. "-print-after-all" or
	/// "-x86-asm-syntax=intel". A leading dash is optional.
	///
	/// The options are applied when create () builds the JIT, so everything
	/// queued has to be in before then; create () fails if LLVM rejects one.
	static void add_option (llvm::StringRef opt);

	/// Build the JIT for the host: JITLink object linking, code model
	/// Small+PIC, FastISel code generation, and the tier-0 IR pipeline
	/// applied to every added module.
	static llvm::Expected<std::unique_ptr<MonoJit>> create ();

	MonoJit (const MonoJit &) = delete;
	MonoJit &operator= (const MonoJit &) = delete;
	~MonoJit ();

	/// Make a runtime entry point (icall target, helper, libc routine) visible
	/// to JIT'd code under NAME. Idempotent: registering a name again is a
	/// no-op and the first address wins, since many call sites resolve the
	/// same helper.
	llvm::Error register_symbol (llvm::StringRef name, void *addr);

	/// Publish NAME as a redirectable stub initially jumping to TARGET.
	///
	/// This is the address handed out for a method: callers bind to it once
	/// and every tier is reached through it. Compiled code that calls NAME
	/// resolves to the stub, so a redirect_stub () is seen by callers that were
	/// compiled long before it.
	///
	/// Only defines the symbol - nothing is materialized, so this is safe to
	/// call with locks held. The address comes from stub_address (), whose
	/// lookup can run other threads' pending compiles inline and so must not.
	llvm::Error create_stub (llvm::StringRef name, void *target);

	/// Publish NAME as a stub that compiles itself the first time it is called.
	///
	/// COMPILE runs on the calling thread, in the middle of that first call,
	/// and returns the entry point to continue into; the stub is redirected
	/// there, so every later call goes straight to the code. Callers can be
	/// compiled against NAME before it has any code at all, which is what lets
	/// a method be published without compiling it.
	///
	/// Threads racing on that first call compile once and all land on the same
	/// code. Define-only, like create_stub ().
	///
	/// That first call does not always continue into the method: an async
	/// abort that arrived while the thread was compiling is thrown from the
	/// caller's frame instead, which is lazy-entry.hpp's business.
	llvm::Error create_lazy_stub (llvm::StringRef name,
	                              LazyCompileFunction compile);

	/// Point NAME's stub at TARGET, which every subsequent call through the
	/// stub reaches. Callers are untouched - nothing is patched but the stub's
	/// own slot.
	llvm::Error redirect_stub (llvm::StringRef name, void *target);

	/// The address of the stub published for NAME.
	llvm::Expected<void *> stub_address (llvm::StringRef name);

	/// Compile TSM and return where ENTRY and its side tables landed.
	///
	/// The module gets the tier-0 treatment (run_tier0_pipeline + FastISel)
	/// and lands in a JITDylib of its own that resolves external symbols
	/// through register_symbol () and the published stubs, and nothing else -
	/// a lookup never falls back to the process, so an unregistered helper
	/// fails the compile loudly.
	llvm::Expected<CompiledMethod> compile (llvm::orc::ThreadSafeModule tsm,
	                                        llvm::StringRef entry);

	/// Release DYLIBS: their code, their side tables, and the memory both were
	/// linked into, which later compiles may then reuse.
	///
	/// The caller proves the code dead - nothing executing in it, nothing about
	/// to call into it - and must have undefined any stub still pointing at it.
	llvm::Error remove_dylibs (const std::vector<llvm::orc::JITDylib *> &dylibs);

	/// Undefine NAMES, which must all be stubs this JIT published, so that the
	/// names are free to be published again.
	///
	/// The stub bodies themselves are not reclaimed - the redirection manager
	/// hands them out of blocks it owns for the life of the JIT - so what this
	/// buys is the name, not the memory.
	llvm::Error undefine_stubs (const std::vector<std::string> &names);

	/// The tier-0 IR pipeline, run over M in place: the stock O1 function
	/// simplification pipeline, whose load-bearing effect is mem2reg over the
	/// allocas the translator routes every argument, local and spill slot
	/// through.
	///
	/// Static and public so tests can assert what it does to translator
	/// output; compile () applies it to every module through the LLJIT
	/// transform layer.
	static void run_tier0_pipeline (llvm::Module &m);

	/// The DataLayout modules compiled here must carry. compile () stamps it
	/// on modules that do not have one yet.
	const llvm::DataLayout &data_layout () const;

	/// The target codegen is emitting for.
	const llvm::Triple &triple () const;

private:
	explicit MonoJit (std::unique_ptr<llvm::orc::LLJIT> jit);

	std::unique_ptr<llvm::orc::LLJIT> jit_;

	/// Dedicated dylib holding the explicitly-registered runtime helpers;
	/// every compiled module's dylib links against this and mono.stubs.
	llvm::orc::JITDylib *helpers_ = nullptr;

	/// Dylib holding the published method stubs, and the manager that emits
	/// and rewrites them.
	llvm::orc::JITDylib *stubs_ = nullptr;
	std::unique_ptr<llvm::orc::RedirectableSymbolManager> redirectable_;

	/// Hands out the re-entry trampolines lazy stubs point at until they are
	/// compiled, and owns the resolver they call through.
	std::unique_ptr<llvm::orc::JITCompileCallbackManager> callbacks_;

	/// What each name handed to register_symbol () stands for, so a repeat
	/// registration is recognized instead of tripping ORC's duplicate-definition
	/// error - and so a name given two different addresses is caught.
	std::mutex named_symbols_mutex_;
	std::unordered_map<std::string, void *> named_symbols_;

	/// Names the per-module dylibs; atomic because compiles may be concurrent.
	std::atomic<uint64_t> module_counter_ {0};

	class ObjectCapturePlugin;
	/// Captures each linked object's code extent and side tables, keyed by the
	/// per-compile dylib; compile () collects its own entry after the lookup.
	std::shared_ptr<ObjectCapturePlugin> capture_;
};

} // namespace mono

#endif
