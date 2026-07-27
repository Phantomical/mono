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

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>

/* MonoDomain, the lifetime key compiled code is owned by. Forward decls only -
 * this header stays clear of the rest of mono. */
#include <mono/utils/mono-forward.h>

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
namespace jitlink {
class LinkGraph;
} // namespace jitlink
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
	 * The `.mono_lsda` section of this module ({nullptr,0} unless MonoLSDAStreamer
	 * wrote one - i.e. an EH-bearing method whose catch clauses the C2 gather pass
	 * resolved). This is mono's own target-neutral clause table (magic 'MLSD',
	 * code-relative offsets; plan 12 2), captured through the same
	 * capture_named_section hook as eh_frame. C4/C6 parse it into the method's
	 * MonoJitExceptionInfo[]; C3 captures it but leaves it unused (the EH gate
	 * still declines every clause-bearing method, so this is {nullptr,0} for every
	 * method that currently reaches the runtime path).
	 */
	EhFrameInfo mono_lsda;
	/* Address of the mono-format "mono_eh_frame" global, or 0 if absent. */
	uint64_t mono_eh_frame = 0;
};

/* ---- EH gather side channel (C2) -----------------------------------------
 *
 * MonoEHGatherPass (a MachineFunctionPass in passes/eh-gather.cpp, scheduled right after
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
	/* The IL clause index, smuggled through the type_info_N initializer. */
	int clause_index = -1;
	/*
	 * The clause's IL flags (a MonoExceptionEnum: NONE=0/catch, FINALLY=2,
	 * FAULT=4), smuggled alongside clause_index through the type_info_N global's
	 * 2-word {i32 clause_index, i32 kind} initializer. MonoLSDAStreamer writes it
	 * into the v2 section's kind column so the section is self-describing. For a
	 * catch clause (all F1 admits) it is 0; the legacy 1-word i32 initializer
	 * (clause_index only) leaves it 0 too.
	 */
	int kind = 0;
	/*
	 * False if the clause_index could not be safely recovered (the type_info was
	 * not a GlobalVariable carrying a ConstantInt / 2-word struct initializer):
	 * downstream must decline (CAP-EH-0), never guess.
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
 * One PC range a FINALLY clause's handler body occupies, as MonoFinallyRangePass
 * found it. A clause can have SEVERAL: the optimizer duplicates a body along its
 * entry paths, and each surviving copy is a range of its own.
 *
 * This is what the runtime's thread-abort guard asks about a stopped frame ("is it
 * inside this finally?"), so the bounds have to be exact. They are labels the pass
 * plants at the run's two ends - a label emits no code and can sit anywhere in a
 * block, so the range names where the body actually lies.
 */
struct MonoEHFinallyBody {
	const llvm::MCSymbol *body_begin = nullptr;
	const llvm::MCSymbol *body_end = nullptr;
	/* The IL clause index, read back from the markers bracketing the run. */
	int clause_index = -1;
};

/* The finally body ranges MonoFinallyRangePass found in one MachineFunction. */
struct MonoEHFinallyFunction {
	/*
	 * MF.getName (), which MonoLSDAStreamer::finishImpl () matches against
	 * MonoEHFunctionClauses::function to write both into one record.
	 */
	std::string function;
	std::vector<MonoEHFinallyBody> bodies;
};

/*
 * The per-compile side channel: one entry per EH-bearing function. A non-EH
 * module (no landing pads) leaves this empty, so the gather pass is inert.
 *
 * finally_functions is a SEPARATE list because a different pass fills it, at a
 * different point in the pipeline; MonoLSDAStreamer::finishImpl () joins the two
 * by function name when it writes the section.
 */
struct MonoEHSideChannel {
	std::vector<MonoEHFunctionClauses> functions;
	std::vector<MonoEHFinallyFunction> finally_functions;
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
	 * Register a runtime helper (icall target, libc shim, cross-method call
	 * target, ...) by name. Uses ORCv2 absoluteSymbols - the explicit-
	 * registration path the README mandates in place of the spike's
	 * -rdynamic/process-symbol search.
	 *
	 * Idempotent: every caller here names a target that is stable for the life
	 * of the process (an icall wrapper, a pinvoke target, a method's specific
	 * trampoline, ...), and the same name is expected to be registered from
	 * many call sites across many compiles, so a repeat registration under a
	 * name already on file is a no-op. If it names a DIFFERENT address than
	 * before, the first address wins (see the definition for why that is
	 * still correct rather than a bug being papered over).
	 */
	void register_symbol (llvm::StringRef name, void *addr);

	/*
	 * Reverse lookup for register_symbol (): the name ADDR was registered
	 * under, or nullptr if it was never registered. Used by the disassembler
	 * to annotate tier-1 call targets (every one of which is a direct
	 * `call @symbol` resolved against a name registered here). Like
	 * named_symbols_, first-wins on a collision and stable for the life of
	 * the process, so the returned pointer needs no lifetime management by
	 * the caller.
	 */
	const char *resolve_symbol_name (void *addr);

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
	 *   owner        - lifetime key for the code this compile produces; mono
	 *                  passes the MonoCompile's domain. Everything compiled
	 *                  under one domain is torn down together by release_owner
	 *                  (). nullptr means "never reclaim", which is what the unit
	 *                  tests want and what a caller with no domain to name gets.
	 */
	CompileResult compile (llvm::Function *entry,
	                       llvm::ArrayRef<llvm::GlobalVariable *> callee_vars,
	                       uint64_t *callee_addrs,
	                       llvm::StringRef eh_symbol,
	                       llvm::orc::ThreadSafeContext tsctx,
	                       MonoDomain *owner = nullptr);

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
	uint64_t release_owner (MonoDomain *owner);

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
	/*
	 * Dedicated dylib holding the explicitly-registered runtime helpers. Each
	 * compiled module's dylib links ONLY to this (never to a process-symbol
	 * generator), so JIT'd code resolves externals exclusively through
	 * register_symbol() - the explicit path the README mandates.
	 */
	llvm::orc::JITDylib *helpers_jd_ = nullptr;
	/*
	 * Only names dylibs, so relaxed ordering is enough - but it has to be
	 * atomic, because concurrent compiles all reach for the next value.
	 */
	std::atomic<uint64_t> module_counter_ {0};

	/*
	 * Every name ever handed to register_symbol (), so a repeat registration
	 * can be recognized as such rather than tripping ORC's "symbol already
	 * defined" failure. Guards helpers_jd_ against being asked to define the
	 * same name twice - which is expected once cross-method calls and icall/
	 * ABS call targets start naming their absolute symbols through this path,
	 * since many call sites across many compiles end up naming the same
	 * callee.
	 */
	std::mutex named_symbols_mutex_;
	std::unordered_map<std::string, void *> named_symbols_;

	/*
	 * The reverse of named_symbols_, kept in lockstep under the same mutex,
	 * so resolve_symbol_name () doesn't have to scan named_symbols_ linearly.
	 * Same first-wins/never-erased lifetime as named_symbols_.
	 */
	std::unordered_map<void *, std::string> symbols_by_addr_;

	/*
	 * owner domain -> the dylibs compiled under it, for release_owner ().
	 *
	 * The map entry is erased by release_owner (), which mono calls from the
	 * domain's own free path, so a key never outlives its domain and cannot be
	 * aliased by a later domain reusing the address.
	 */
	std::mutex owners_mutex_;
	std::map<MonoDomain *, std::vector<llvm::orc::JITDylib *>> owners_;
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
 * These four entry points are part of the intra-directory interface only
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
 * Runs accumulate_reloc_audit_from_graph()'s classification (engine.cpp) over
 * a caller-built LinkGraph and returns the per-graph tally WITHOUT touching
 * the process-global jit_reloc_audit() accumulator. Test-only: it exists so
 * mono/unit-tests/test-llvm-engine.cpp can exercise the graph-boundary
 * predicate directly against a hand-built LinkGraph (same-graph cross-section,
 * external, absolute, ...) instead of only observing it indirectly through a
 * real compile, the way test_reloc_widths does.
 */
RelocAudit audit_relocations_graph (llvm::jitlink::LinkGraph &g);

/* ---- eh-frame registry (test-only) ---------------------------------------
 *
 * MonoEHFrameRegistrar (engine.cpp) wraps llvm::jitlink::InProcessEHFrameRegistrar
 * so host unwinder registration (__register_frame) is unchanged (doc 26 J3:
 * behavior-preserving, native crash-dump unwinders keep FDE coverage), while
 * ALSO recording every register/deregister into this process-global tally. That
 * recording is the substitute for the deregister-before-unmap assert JL1 removed
 * from ~MonoJitMemoryManager (see release_owner () in engine.cpp): there is no
 * longer a per-object destructor to assert from, so a reclamation integration
 * test reads this accessor instead (doc 26 Q5 / J4).
 */

/* Tally over every .eh_frame range this process has registered/deregistered. */
struct EhFrameRegistryStats {
	uint64_t registered = 0;
	uint64_t deregistered = 0;
	/* Ranges that are currently registered with the host unwinder and have not
	 * yet been deregistered - i.e. still "live" over mapped memory. */
	std::vector<EhFrameInfo> live;
};

/*
 * Snapshot of the eh-frame registry's tallies and live set, guarded by the same
 * mutex the registrar's register/deregister hooks take. Test-only: nothing in
 * the engine's runtime path reads this.
 */
EhFrameRegistryStats eh_frame_registry_stats ();

/*
 * The target machine configuration the engine itself JITs with (host CPU and
 * features, O3, and code model Small+PIC - see host_target_machine_builder ()).
 * Exposed so the test can emit a probe object through an otherwise-identical
 * target machine with only the code model varied.
 */
llvm::orc::JITTargetMachineBuilder host_target_machine_builder ();

/* ---- C1 compiler hook -----------------------------------------------------
 *
 * The engine compiles IR through MonoIRCompiler (engine.cpp), a drop-in for
 * LLJIT's default object-emission compiler that hand-inlines
 * addPassesToEmitMC so the EH port can splice a MachineFunctionPass and a
 * custom MCStreamer into the pipeline. Exposed so test-llvm-engine.cpp can
 * drive MonoIRCompiler directly on a hand-built module; nothing in the
 * engine's runtime path calls it.
 */
llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
compile_object_with_mono_compiler (llvm::Module &m);

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
