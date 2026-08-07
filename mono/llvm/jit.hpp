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
class Function;
class TargetMachine;
} // namespace llvm

namespace mono {

class CodeSlabs;
class LazyCallbacks;
class StubTable;

/// The host TargetMachine every compile runs against - code model Small+PIC and
/// FastISel code generation.
///
/// One instance per calling thread, reused for every module that thread
/// compiles: building one costs more than compiling a typical method, and a
/// TargetMachine cannot be shared across threads.
llvm::TargetMachine &host_target_machine ();

/// The widest access, in bits, this target performs atomically with a single
/// instruction when compiling F.
///
/// An atomic load or store wider than this is legal IR but lowers to a call
/// into the atomic runtime library, which nothing here provides a definition
/// for, so an access past this width has to be built some other way.
unsigned host_max_atomic_bits (const llvm::Function &f);

/// Whether the IR verifier runs over what this backend produces. Set by
/// MONO_LLVM_JIT_VERIFY: `0`/`off` to turn it off, `each` to check after every
/// pass in the pipeline rather than only the ones written here.
bool ir_verification_enabled ();

/// One row of a compiled function's line table: an offset from the start of the
/// function, and the IL offset in effect at it. The translator records these as
/// debug locations (il-line-table.hpp), the compiler writes them into
/// `.mono_lines` (sidetables.hpp) and the engine reads them back from there.
struct IlLineRow {
	uint32_t native_offset;
	uint32_t il_offset;
	/// MonoSeqPointFlags, on a sequence point row and zero on any other.
	uint8_t flags = 0;
};

/// One frame slot, as the address of a register plus a displacement - the shape
/// a MonoDebugVarInfo names a variable's home in.
struct VarSlot {
	int32_t dwarf_reg;
	int32_t offset;
};

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

	/// The jump stubs the linker synthesized for this object, as [code, size).
	/// Executable, nameless, and on the path of the calls that needed them, so
	/// the runtime has to be able to resolve an address in one.
	std::vector<std::pair<const uint8_t *, size_t>> linker_stubs;

	/// The entry function's native_offset -> il_offset rows, ascending by
	/// native offset. Empty when the module carried no line table.
	std::vector<IlLineRow> il_lines;

	/// The same rows for every other function the object defines, by name -
	/// the filter bodies, each of which is a frame of its own and needs a map
	/// of its own to say where in the method's IL it is.
	std::vector<std::pair<std::string, std::vector<IlLineRow>>> other_il_lines;

	/// The entry function's sequence points, ascending by native offset: where
	/// each soft-debugger check landed, and the IL offset it stands for in the
	/// encoding seq-point-marker.hpp describes. Empty unless the method was
	/// translated with sequence points in it.
	std::vector<IlLineRow> seq_points;

	/// The entry function's argument and local slots, arguments first, in the
	/// order the signature and the method header give them. Empty unless the
	/// method was translated with its variables pinned to the frame.
	std::vector<VarSlot> var_slots;

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
	/// The linker is not told about NAME here. It hears about a stub the first
	/// time a module names one, which is what keeps publishing a method that
	/// nobody links against down to a block and a table entry.
	llvm::Error create_stub (llvm::StringRef name, void *target);

	/// Publish NAME as a stub that hands KEY to TARGET in the register a
	/// callee's key travels in, and return the address it was carved at.
	///
	/// This is how a body shared by many methods - one written against a
	/// prototype rather than against a method - is told which of them a call
	/// came in for. Like create_stub (), the linker is not told about NAME;
	/// unlike it, a name that already has a stub gets that one back, since two
	/// threads reaching one method together both ask for it.
	llvm::Expected<void *> create_keyed_stub (llvm::StringRef name, void *target,
	                                          void *key);

	/// Publish NAME as a stub that compiles itself the first time it is called.
	///
	/// COMPILE runs on the calling thread, in the middle of that first call,
	/// and returns the entry point to continue into; the stub is redirected
	/// there, so every later call goes straight to the code. Callers can be
	/// compiled against NAME before it has any code at all, which is what lets
	/// a method be published without compiling it.
	///
	/// Threads racing on that first call compile once and all land on the same
	/// code.
	///
	/// That first call does not always continue into the method: an async
	/// abort that arrived while the thread was compiling is thrown from the
	/// caller's frame instead, which is the arch resolver's business.
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

	/// Undefine NAMES, which must all be stubs this JIT published, so that both
	/// the names and the stub blocks behind them are free to be handed out
	/// again.
	///
	/// The caller proves nothing can reach the stubs: a later method published
	/// here may be given the very same block.
	///
	/// A compile running concurrently may still be linking against one of
	/// NAMES; that link either gets its definition or fails to find the name,
	/// and either way this returns without an error.
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
	MonoJit (std::unique_ptr<llvm::orc::LLJIT> jit,
	         std::shared_ptr<CodeSlabs> slabs);

	/// The code memory this domain's objects are linked into. Declared before
	/// jit_ so it outlives the LLJIT, and with it the ObjectLinkingLayer whose
	/// SlabMemoryManager hands memory out of it.
	std::shared_ptr<CodeSlabs> slabs_;

	/// Every stub this JIT has published. Declared before jit_ so it outlives
	/// the dylib generator that reads it.
	std::unique_ptr<StubTable> stub_table_;

	/// Held while a stub is being handed to mono.stubs and while one is being
	/// taken back out of the table, so a name is never sitting claimed-but-not-
	/// yet-defined when undefine_stubs () decides what the linker knows about.
	std::mutex stub_defs_mutex_;

	std::unique_ptr<llvm::orc::LLJIT> jit_;

	/// Dedicated dylib holding the explicitly-registered runtime helpers;
	/// every compiled module's dylib links against this and mono.stubs.
	llvm::orc::JITDylib *helpers_ = nullptr;

	/// The dylib a module's reference to a stub is resolved through. A stub
	/// reaches it only once some module has named one; stub_table_ holds them
	/// all either way.
	llvm::orc::JITDylib *stubs_ = nullptr;

	/// Hands out the re-entry trampolines lazy stubs point at until they are
	/// compiled, and owns the resolver they call through.
	std::unique_ptr<LazyCallbacks> callbacks_;

	/// What each name handed to register_symbol () stands for, so a repeat
	/// registration is recognized instead of tripping ORC's duplicate-definition
	/// error - and so a name given two different addresses is caught.
	std::mutex named_symbols_mutex_;
	std::unordered_map<std::string, void *> named_symbols_;

	/// Names the per-module dylibs; atomic because compiles may be concurrent.
	std::atomic<uint64_t> module_counter_ {0};

	/// The one dylib every module goes into under MONO_LLVM_JIT_HOIST=sharedjd,
	/// and null otherwise.
	llvm::orc::JITDylib *shared_jd_ = nullptr;

	/// How many undefined names it takes to be worth sweeping the session's
	/// symbol-string pool for the entries they left dead.
	static constexpr uint64_t dead_name_sweep = 1024;

	/// Names undefined since the last sweep.
	std::atomic<uint64_t> dropped_names_ {0};

	class ObjectCapturePlugin;
	/// Captures each linked object's code extent and side tables, keyed by the
	/// per-compile dylib; compile () collects its own entry after the lookup.
	std::shared_ptr<ObjectCapturePlugin> capture_;
};

} // namespace mono

#endif
