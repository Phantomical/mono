/**
 * \file
 * engine.hpp - C++-only interface for the ORCv2 in-process JIT engine.
 *
 * This header is consumed ONLY within mono/mini/llvm/ (by engine.cpp and the
 * engine unit test). It exposes LLVM C++ types, so it must never be included by
 * mono's C sources - those go through the extern "C" boundary in backend.h.
 *
 * The engine is the LLVM-18 replacement for the *execution-engine* half of the
 * legacy mono/mini/llvm-jit.cpp (MCJIT/RuntimeDyld). It is built on LLJIT/ORCv2
 * because the donor engine's ORCv1 legacy layers (LegacyRTDyldObjectLinkingLayer,
 * LegacyIRCompileLayer, VModuleKey, createLegacyLookupResolver) were removed from
 * LLVM years before 18 - adapting them was not an option, so this is a rewrite.
 */

#ifndef __MONO_MINI_LLVM_ENGINE_HPP__
#define __MONO_MINI_LLVM_ENGINE_HPP__

#include <cstdint>
#include <memory>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>

namespace llvm {
class Function;
class GlobalVariable;
class LLVMContext;
} // namespace llvm

namespace mono {

/*
 * The .eh_frame section of the most recently materialized module, captured by
 * MonoJitMemoryManager::registerEHFrames().
 *
 * STEP 3b (exception handling) PLUGS IN HERE: after each compile() the EH port
 * reads last_eh_frame() to learn where RuntimeDyld placed the FDEs/CIEs for the
 * just-compiled method, and feeds that to mono's unwinder. (This is the stock
 * DWARF .eh_frame section - distinct from the mono-format "mono_eh_frame" global
 * that compile()'s eh_out parameter resolves.)
 */
struct EhFrameInfo {
	uint8_t *addr = nullptr;
	uint64_t size = 0;
};

/*
 * The in-process JIT. A process-wide singleton, matching the legacy engine's
 * single global `jit`. Synchronous (0 compile threads) for the first milestone.
 */
class MonoLLVMJIT {
public:
	/* Create-on-first-use singleton. */
	static MonoLLVMJIT *get_singleton ();

	/*
	 * The LLVMContext all JIT modules must be built in. mono's translator
	 * (step 3b) should create its jit module here rather than in the global
	 * context, so a future background-compile thread stays race-free.
	 */
	llvm::LLVMContext &context ();

	/*
	 * Register a runtime helper (icall target, libc shim, ...) by name.
	 * Uses ORCv2 absoluteSymbols - the explicit-registration path the README
	 * mandates in place of the spike's -rdynamic/process-symbol search.
	 */
	void register_symbol (llvm::StringRef name, void *addr);

	/* Run an O2 function-simplification pipeline over `func` in place. */
	void optimize (llvm::Function *func);

	/*
	 * Compile the module that owns `entry` and return the executable address
	 * of `entry`.
	 *
	 * NON-DESTRUCTIVE: the JIT compiles a private CLONE of the module; the
	 * caller's module is left intact and the caller retains ownership. mono's
	 * translator keeps using its module after compile() returns (e.g.
	 * mono_llvm_remove_gc_safepoint_poll), so compile() must not consume or
	 * free it.
	 *
	 *   callee_vars  - GlobalVariables whose materialized addresses the caller
	 *                  needs; their addresses are written to callee_addrs[i].
	 *                  (Resolved by name in the clone, so pass the originals.)
	 *   eh_symbol    - if non-empty, its address is written to *eh_out (this is
	 *                  the mono-format "mono_eh_frame" global, per the donor).
	 */
	uint64_t compile (llvm::Function *entry,
	                  llvm::ArrayRef<llvm::GlobalVariable *> callee_vars,
	                  uint64_t *callee_addrs,
	                  llvm::StringRef eh_symbol,
	                  uint64_t *eh_out);

	/*
	 * .eh_frame captured during the most recent compile() on this thread.
	 * STEP 3b: this C++ accessor has no extern "C" counterpart yet - 3b's EH
	 * port will add a boundary accessor (in backend.h) so the C translator can
	 * read the captured section and feed mono's unwinder.
	 */
	const EhFrameInfo &last_eh_frame () const;

	~MonoLLVMJIT ();

private:
	MonoLLVMJIT ();
	MonoLLVMJIT (const MonoLLVMJIT &) = delete;
	MonoLLVMJIT &operator= (const MonoLLVMJIT &) = delete;

	std::unique_ptr<llvm::orc::LLJIT> jit_;
	llvm::orc::ThreadSafeContext tsctx_;
	/*
	 * Dedicated dylib holding the explicitly-registered runtime helpers. Each
	 * compiled module's dylib links ONLY to this (never to a process-symbol
	 * generator), so JIT'd code resolves externals exclusively through
	 * register_symbol() - the explicit path the README mandates.
	 */
	llvm::orc::JITDylib *helpers_jd_ = nullptr;
	uint64_t module_counter_ = 0;
};

} // namespace mono

#endif /* __MONO_MINI_LLVM_ENGINE_HPP__ */
