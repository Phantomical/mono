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

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/RedirectionManager.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mono {

class MonoJit {
public:
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

	/// Publish NAME as a redirectable stub initially jumping to TARGET, and
	/// return the stub's address.
	///
	/// This is the address handed out for a method: callers bind to it once
	/// and every tier is reached through it. Compiled code that calls NAME
	/// resolves to the stub, so a redirect_stub () is seen by callers that were
	/// compiled long before it.
	llvm::Expected<void *> create_stub (llvm::StringRef name, void *target);

	/// Point NAME's stub at TARGET, which every subsequent call through the
	/// stub reaches. Callers are untouched - nothing is patched but the stub's
	/// own slot.
	llvm::Error redirect_stub (llvm::StringRef name, void *target);

	/// The address of the stub published for NAME.
	llvm::Expected<void *> stub_address (llvm::StringRef name);

	/// Compile TSM and return the executable address of ENTRY.
	///
	/// The module gets the tier-0 treatment (run_tier0_pipeline + FastISel)
	/// and lands in a JITDylib of its own that resolves external symbols
	/// through register_symbol () and the published stubs, and nothing else -
	/// there is no process-symbol search, so an unregistered helper fails the
	/// compile loudly.
	llvm::Expected<void *> compile (llvm::orc::ThreadSafeModule tsm,
	                                llvm::StringRef entry);

	/// The tier-0 IR pipeline, run over M in place: the stock per-module O1
	/// pipeline, whose load-bearing effect is mem2reg over the allocas the
	/// translator routes every argument, local and spill slot through.
	///
	/// Static and public so tests can assert what it does to translator
	/// output; compile () applies it to every module through the LLJIT
	/// transform layer.
	static void run_tier0_pipeline (llvm::Module &m);

	/// The DataLayout modules compiled here must carry. compile () stamps it
	/// on modules that do not have one yet.
	const llvm::DataLayout &data_layout () const;

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

	/// Names ever handed to register_symbol (), so a repeat registration is
	/// recognized instead of tripping ORC's duplicate-definition error.
	std::mutex named_symbols_mutex_;
	std::unordered_map<std::string, void *> named_symbols_;

	/// Names the per-module dylibs; atomic because compiles may be concurrent.
	std::atomic<uint64_t> module_counter_ {0};
};

} // namespace mono

#endif
