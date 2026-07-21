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
 * The stock DWARF .eh_frame section of a compiled module, as loaded.
 *
 * THE EH PORT PLUGS IN HERE: it reads this out of the CompileResult to learn
 * where the FDEs/CIEs for the just-compiled method landed, and feeds them to
 * mono's unwinder. (Distinct from the mono-format "mono_eh_frame" global that
 * CompileResult::mono_eh_frame resolves - that one only ever existed under the
 * forked LLVM.)
 */
struct EhFrameInfo {
	uint8_t *addr = nullptr;
	uint64_t size = 0;
};

/*
 * Everything one compile() produces.
 *
 * These are returned by value rather than left in per-thread state on purpose.
 * The object-file facts (code_size, eh_frame) are discovered by the object
 * layer's NotifyLoaded hook, which runs on whichever thread materializes the
 * module - and that is NOT the calling thread once the JIT is given compile
 * threads, which tiering will want. So they are collected under a lock keyed by
 * the module's own JITDylib and handed back here, which stays correct whoever
 * does the work.
 */
struct CompileResult {
	/* Executable address of the entry function. */
	uint64_t entry = 0;
	/*
	 * Machine-code size of the entry function, from the emitted object's ELF
	 * symbol table (st_size, which LLVM's AsmPrinter fills via `.size fn, .-fn`).
	 * mono needs it for cfg->code_len: it sizes the method's MonoJitInfo, and a
	 * zero-length jit-info makes mini_jit_info_table_find() unable to find the
	 * method at all.
	 */
	uint64_t code_size = 0;
	/* Stock DWARF .eh_frame of this module (for the EH port). */
	EhFrameInfo eh_frame;
	/* Address of the mono-format "mono_eh_frame" global, or 0 if absent. */
	uint64_t mono_eh_frame = 0;
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
	 *   eh_symbol    - if non-empty, looked up and reported in the result's
	 *                  mono_eh_frame field (the mono-format global, per the donor).
	 */
	CompileResult compile (llvm::Function *entry,
	                       llvm::ArrayRef<llvm::GlobalVariable *> callee_vars,
	                       uint64_t *callee_addrs,
	                       llvm::StringRef eh_symbol);

	~MonoLLVMJIT ();

private:
	MonoLLVMJIT ();
	MonoLLVMJIT (const MonoLLVMJIT &) = delete;

	/*
	 * Register the libc routines LLVM's codegen lowers IR intrinsics into
	 * (memcpy/memmove/memset from llvm.mem*, fmod from frem). Required because
	 * this engine has no process-symbol generator to fall back on.
	 */
	void register_c_runtime_symbols ();

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
