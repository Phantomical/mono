/**
 * \file
 * engine.hpp - C++-only interface for the ORCv2 in-process JIT engine.
 *
 * This header is consumed ONLY by engine.cpp and by mono/unit-tests/
 * test-llvm-engine.cpp. It exposes LLVM C++ types, so it must never be included
 * by mono's C sources - those go through the extern "C" boundary in backend.h,
 * which is deliberately kept small and must NOT grow to serve tests.
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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>

namespace llvm {
class Function;
class GlobalVariable;
class LLVMContext;
class MCSymbol;
class MemoryBuffer;
class Module;
namespace object {
class ObjectFile;
} // namespace object
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
	/*
	 * The `.llvm_stackmaps` section of this module ({nullptr,0} unless the
	 * translator planted a llvm.experimental.stackmap, which it does only for
	 * gshared methods). Task #15 parses it to recover cfg->llvm_this_reg/offset -
	 * the home slot of this/mrgctx a stack walk needs to rebuild the frame's
	 * generic context.
	 */
	EhFrameInfo stackmaps;
	/*
	 * The `.gcc_except_table` (Itanium LSDA) section of this module ({nullptr,0}
	 * unless the translator gave the function a personalityFn and emitted an
	 * invoke/landingpad, i.e. an EH method). The EH port (M2) feeds it to
	 * mono::decode_gcc_except_table to recover the method's exception clauses.
	 */
	EhFrameInfo gcc_except_table;
	/* Address of the mono-format "mono_eh_frame" global, or 0 if absent. */
	uint64_t mono_eh_frame = 0;
};

/* ---- EH gather side channel (C2) -----------------------------------------
 *
 * MonoEHGatherPass (a MachineFunctionPass in engine.cpp, scheduled right after
 * addMachinePasses() in MonoIRCompiler's object-emission pipeline) reads the
 * target-neutral MF.getLandingPads() and recovers, per landing pad, mono's IL
 * clause_index from the type_info_N global's i32 initializer - all in-process,
 * before any relocation. It EMITS NOTHING: it only populates this side channel,
 * a stack-local of one MonoIRCompiler::operator() call threaded into the pass
 * (plan 12 1.4). C3 turns the gathered tuples into the .mono_lsda section
 * (plan 12 2); C2 is inert (a non-EH module has no landing pads -> an empty side
 * channel -> byte-identical output).
 *
 * The MCSymbol* fields are the very symbols the AsmPrinter emits into .text;
 * they are kept as symbols here (C3 turns them into func_begin-relative label
 * differences). They are not resolvable in this offline stage and tests must not
 * dereference them - the clause_index and the tuple counts are what C2 asserts.
 */

/*
 * One (invoke range, catch clause) tuple, as the gather pass sees it. A landing
 * pad carries ONE (begin,end) pair per invoke that unwinds to it, so a try with
 * N protected calls produces N clauses that share a handler and clause_index but
 * cover disjoint invoke ranges. .mono_lsda is thus one entry per invoke range.
 */
struct MonoEHClause {
	/* This invoke's try range: a paired LandingPadInfo BeginLabels[i]/EndLabels[i]. */
	const llvm::MCSymbol *try_begin = nullptr;
	const llvm::MCSymbol *try_end = nullptr;
	/* The handler entry: LandingPadInfo LandingPadLabel. */
	const llvm::MCSymbol *handler = nullptr;
	/* The IL clause index, smuggled through the type_info_N i32 initializer. */
	int clause_index = -1;
	/*
	 * False if the clause_index could not be safely recovered (the type_info was
	 * not a GlobalVariable carrying a ConstantInt initializer): downstream must
	 * decline (CAP-EH-0), never guess.
	 */
	bool clause_resolved = false;
};

/* Everything the gather pass found for one EH-bearing MachineFunction. */
struct MonoEHFunctionClauses {
	/*
	 * The function's name (MF.getName()). C3 keys the emitted section to the
	 * function symbol - "one record per method in the method's own object"
	 * (plan 12 2). The JIT is one function per module, but this is structured so
	 * C3 can emit for the right function.
	 */
	std::string function;
	std::vector<MonoEHClause> clauses;
	/*
	 * A negative TypeId (an exception-specification filter) was seen: out of the
	 * catch-only milestone. Recorded so a later slice can decline; never a crash.
	 */
	bool has_filter = false;
	/*
	 * Something unexpected was seen (a missing begin/end/lpad label, an
	 * unresolvable type_info, or a filter/cleanup TypeId): downstream must
	 * decline this method to the classic JIT (CAP-EH-0).
	 */
	bool declined = false;
};

/*
 * The per-compile side channel: one entry per EH-bearing function. A non-EH
 * module (no landing pads) leaves this empty, so the gather pass is inert.
 */
struct MonoEHSideChannel {
	std::vector<MonoEHFunctionClauses> functions;
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
	 * The singleton if it has already been built, else nullptr. Reclamation
	 * paths use this so that a domain unload in a process that never JITted
	 * anything does not construct an LLJIT just to find it has nothing to free.
	 */
	static MonoLLVMJIT *get_singleton_if_created ();

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
	 *   owner        - opaque lifetime key for the code this compile produces;
	 *                  mono passes the MonoCompile's MonoDomain *. Everything
	 *                  compiled under one key is torn down together by
	 *                  release_owner (). nullptr means "never reclaim", which is
	 *                  what the unit tests want and what a caller with no domain
	 *                  to name gets.
	 */
	CompileResult compile (llvm::Function *entry,
	                       llvm::ArrayRef<llvm::GlobalVariable *> callee_vars,
	                       uint64_t *callee_addrs,
	                       llvm::StringRef eh_symbol,
	                       void *owner = nullptr);

	/*
	 * Tear down every JITDylib compiled under OWNER: unregister its symbols,
	 * deregister its .eh_frame with the host unwinder and unmap its code and
	 * data. Returns the number of dylibs removed.
	 *
	 * THE CALLER MUST HAVE ESTABLISHED THAT THE CODE IS DEAD. This unmaps
	 * executable pages; a surviving pointer into them is a use-after-free. See
	 * the lifetime argument on mono_llvm_jit_release_domain () below.
	 *
	 * A key that was never compiled for (including nullptr) is a no-op.
	 */
	uint64_t release_owner (void *owner);

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

	/*
	 * owner key -> the dylibs compiled under it, for release_owner ().
	 *
	 * Keyed by an opaque pointer rather than typed as MonoDomain * so that this
	 * header stays free of mono types (see the file comment). The map entry is
	 * erased by release_owner (), which mono calls from the domain's own free
	 * path, so a MonoDomain * key never outlives the domain and cannot be
	 * aliased by a later domain reusing the address.
	 */
	std::mutex owners_mutex_;
	std::map<void *, std::vector<llvm::orc::JITDylib *>> owners_;
};

/* ---- relocation audit ----------------------------------------------------
 *
 * The engine allocates JIT sections with a stock llvm::SectionMemoryManager,
 * which offers no low-address guarantee: in practice every section lands above
 * 4 GB, and code, rodata and eh_frame come from SEPARATE mmap'd blocks whose
 * distance from one another is likewise unbounded. That is only sound if the
 * emitted object references everything with 64-bit-wide relocations.
 *
 * It is not merely a style question, because RuntimeDyld does not fail cleanly
 * on a 32-bit overflow - RuntimeDyldELF::resolveX86_64Relocation guards the
 * narrow cases with a bare assert(), and the shipped LLVM 18.1.3 is built with
 * assertions OFF. An out-of-range value is therefore truncated silently and
 * the JIT jumps or loads through a bogus address.
 *
 * So instead of asking the target machine what code model it thinks it has
 * (which only reads the model back, never whether the emitted code respects
 * it), the engine audits the
 * relocations LLVM actually emitted. That is the property that matters, and it
 * is observable no matter how the code model got chosen. Which relocations
 * count as unsafe - an exhaustive classification over the x86-64 psABI set - is
 * documented on x86_64_reloc_truncates_address() in engine.cpp.
 *
 * These three entry points are part of the intra-directory interface only
 * because mono/unit-tests/test-llvm-engine.cpp drives them; nothing outside
 * mono/mini/llvm/ and that test may use them.
 */

/* Tally over a set of objects. */
struct RelocAudit {
	uint64_t total = 0;
	/* Relocations that narrow a 64-bit address into a <= 32-bit field. */
	uint64_t truncating = 0;
	/* "<section>/<reloc type>" of the first offender, for diagnostics. */
	std::string first_offender;
};

/*
 * Classify every relocation in `obj`.
 *
 * Returns an all-zero audit for anything that is not x86-64 ELF - not because
 * other targets are safe, but because the unsafe relocation set is per-ABI and
 * only x86-64 has been analysed. An all-zero audit is a "did not look", and
 * callers must treat it as such rather than as a pass.
 */
RelocAudit audit_relocations (const llvm::object::ObjectFile &obj);

/*
 * The running total across every object this process has JITted, accumulated
 * from the object layer's NotifyLoaded hook.
 */
RelocAudit jit_reloc_audit ();

/*
 * The target machine configuration the engine itself JITs with (host CPU and
 * features, O3, and LLVM's default code model - Large on x86-64 for a JIT).
 * Exposed so the test can emit a probe object through an otherwise-identical
 * target machine with only the code model varied.
 */
llvm::orc::JITTargetMachineBuilder host_target_machine_builder ();

/* ---- C1 compiler-equivalence hooks ---------------------------------------
 *
 * The engine compiles IR through MonoIRCompiler (engine.cpp), a drop-in for
 * LLJIT's default object-emission compiler that hand-inlines
 * addPassesToEmitMC so the EH port can later splice a MachineFunctionPass and a
 * custom MCStreamer into the pipeline. These two hooks compile `m` to an object
 * both ways - through MonoIRCompiler and through LLVM's stock
 * TMOwningSimpleCompiler - from the same host JITTargetMachineBuilder, so
 * test-llvm-engine.cpp can assert the two objects are byte-identical (proof that
 * MonoIRCompiler is observably inert). They exist ONLY for that test; nothing in
 * the engine's runtime path calls them.
 */
llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
compile_object_with_mono_compiler (llvm::Module &m);

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
compile_object_with_simple_compiler (llvm::Module &m);

/* ---- C2 EH-gather hook ---------------------------------------------------
 *
 * Compile `m` through MonoIRCompiler's exact object-emission pipeline - the one
 * with the MonoEHGatherPass scheduled after addMachinePasses() - and return the
 * populated side channel; the object bytes are discarded. This is the same pass
 * the runtime path runs, so what the test sees is what the runtime gathers.
 * Exists ONLY for test-llvm-engine.cpp's C2 assertions; nothing in the engine's
 * runtime path calls it.
 */
llvm::Expected<MonoEHSideChannel> gather_eh_sidechannel (llvm::Module &m);

} // namespace mono

#endif /* __MONO_MINI_LLVM_ENGINE_HPP__ */
