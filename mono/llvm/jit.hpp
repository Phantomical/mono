/**
 * \file
 * \brief The ORCv2 JIT the LLVM-only backend compiles through.
 *
 * MonoJit owns the LLJIT stack and turns translated modules into executable
 * code. It knows nothing about mono metadata, so unit tests can drive it
 * directly.
 */

#ifndef MONO_LLVM_JIT_HPP
#define MONO_LLVM_JIT_HPP

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace llvm {
class Function;
class Module;
class TargetMachine;
} // namespace llvm

namespace mono {

class CodeArena;
class InlineCandidates;

namespace gdbjit {
struct Registration;
}

/// The host TargetMachine every compile runs against.
///
/// One instance per calling thread, reused for every module that thread
/// compiles. A TargetMachine cannot be shared across threads, and building one
/// costs more than compiling a typical method.
llvm::TargetMachine &host_target_machine ();

/// The same configuration with an optimizing codegen level, which is what tier
/// 2 emits through.
///
/// One instance per calling thread, on the same terms as host_target_machine ().
llvm::TargetMachine &tier2_target_machine ();

/// The widest access, in bits, this target performs atomically with a single
/// instruction when compiling a function.
///
/// An atomic load or store wider than this is legal IR, but it lowers to a call
/// into the atomic runtime library. We register no symbol for that library, so
/// a wider access has to be built some other way.
unsigned host_max_atomic_bits (const llvm::Function &f);

bool ir_verification_enabled ();

/// Whether `--llvm-opt` asked LLVM to print what its passes did.
///
/// It reads the options queued by add_option (), so it answers as soon as the
/// command line has been read and does not wait for a MonoJit to hand them to
/// LLVM's parser.
bool ir_printing_enabled ();

/// One row of a compiled function's line table: an offset from the start of the
/// function, and the IL offset in effect at it. The translator records these as
/// debug locations (il-line-table.hpp), the compiler writes them into
/// `.mono_lines` (sidetables.hpp) and the engine reads them back from there.
struct IlLineRow {
	uint32_t native_offset;
	uint32_t il_offset;
	/// The MONO_SEQ_POINT_FLAG_* bits on a sequence point row, zero on any
	/// other.
	uint8_t flags = 0;
};

/// One body an inliner folded into a compiled function, and where the code it
/// stands for sits.
///
/// native_offset is the line-table row this belongs to, so the two tables are
/// read together: the row says where the compiled method thinks it is, and these
/// say which bodies the same code really came from. depth 0 is the innermost.
/// The compiler writes them into `.mono_inlines` (sidetables.hpp).
struct IlInlineRow {
	uint32_t native_offset;
	uint32_t il_offset;
	uint32_t depth;
	/// What the compile called the folded body. Opaque to the JIT: the engine
	/// puts it in through il-line-table.hpp and reads it back here.
	uint64_t callee;
};

/// Where one instrumented function counts, and what the profile reader needs to
/// recognise the counts as its own.
///
/// The array is live code memory. Reading it hands back whatever the running
/// code has counted so far, and it stays readable until the domain goes.
/// optimize () fills in everything but the address, which only the link knows.
struct ProfileCounters {
	/// The function these count, by the name it carries in the IR.
	std::string function;
	/// The name the profile reader keys the function on.
	std::string name;
	/// The hash of the CFG the counter indices were assigned over.
	uint64_t hash = 0;
	const uint64_t *counters = nullptr;
	uint32_t count = 0;
};

/// Which of the two IR pipelines a module is compiled through.
///
/// This is the JIT's own choice of pipeline, and it is not the runtime's ranking
/// of tiers - MonoJit knows nothing about the interpreter or a detour.
enum class JitTier {
	/// The O1 function pipeline with FastISel behind it, instrumented so that
	/// a later compile has counts to read.
	tier1,
	/// The O3 function pipeline with an optimizing selector, reading the
	/// counts a tier-1 body gathered.
	tier2,
};

/// Writes an indexed profile holding what the given counters have counted so
/// far.
///
/// Reads live code memory, so the result is a snapshot: counters a running
/// thread is still bumping are read at whatever value they hold. That only
/// skews the weights, since the reader never checks the counts against each
/// other.
std::vector<uint8_t> build_profile (llvm::ArrayRef<ProfileCounters> counters);

/// The entry count every body's profile is normalized to, or zero to leave the
/// counts as they were counted. MONO_LLVM_JIT_PROFILE_ENTRY sets it.
///
/// A body reaches tier 2 either on many calls or on a heavy loop, and the raw
/// counts say which. LLVM reads a block cold against the rest of the profile, so
/// a loop-driven body's entry reads cold beside its own loop, and the calls it
/// makes there get a budget almost nothing clears. One entry count for every
/// body separates a call site the profile calls rare from one that only looks
/// rare beside a loop.
uint64_t profile_entry_count ();

/// One frame slot, as a register number and a displacement whose sum is the
/// slot's address - the shape MonoDebugVarInfo names a variable's home in.
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

	/// The `.mono_lsda` clause table. Null when the method has no clauses.
	const uint8_t *clause_table = nullptr;
	size_t clause_table_size = 0;
	/// The `.mono_guards` finally-guard table. Null when the method has no
	/// finally body left to guard.
	const uint8_t *guard_table = nullptr;
	size_t guard_table_size = 0;

	/// The `.mono_unwind` frame description.
	const uint8_t *unwind_table = nullptr;
	size_t unwind_table_size = 0;

	/// Every function of this method the linked object defines, name to
	/// [code, size): the entry, and any filter bodies compiled alongside it.
	std::vector<std::pair<std::string, std::pair<const uint8_t *, size_t>>> functions;

	/// The executable sections the linker synthesized for this object, as
	/// [code, size). They are nameless, and they sit on the path of the calls
	/// that needed them. So the runtime has to resolve an address in one.
	/// Where several methods share an object, the first of them carries all
	/// of these and the rest carry none.
	std::vector<std::pair<const uint8_t *, size_t>> linker_stubs;

	/// Every function and stub the object holds, ascending by address, shared
	/// by the methods linked into it. A gap between two of these is padding,
	/// which is what tells this method's own code from a batch neighbour's.
	///
	/// Every method a compile publishes carries it. A record built by hand for
	/// a body that shares another method's object does not.
	std::shared_ptr<const std::vector<std::pair<const uint8_t *, size_t>>> object_code;

	/// The entry function's native_offset -> il_offset rows, ascending by
	/// native offset. Empty when the module carried no line table.
	std::vector<IlLineRow> il_lines;

	/// The same rows for every other function of this method, by name. Each
	/// filter body is a frame of its own. So each needs a map of its own to
	/// say where in the method's IL it is.
	std::vector<std::pair<std::string, std::vector<IlLineRow>>> other_il_lines;

	/// The bodies folded into the entry function, ascending by native offset
	/// and then by depth. Empty when the inliners left it alone.
	std::vector<IlInlineRow> inline_frames;

	std::vector<std::pair<std::string, std::vector<IlInlineRow>>> other_inline_frames;

	/// The entry function's sequence points, ascending by native offset: where
	/// each soft-debugger check landed, and the IL offset it stands for in the
	/// encoding seq-point-marker.hpp describes. Empty unless the method was
	/// translated with sequence points in it.
	std::vector<IlLineRow> seq_points;

	/// The entry function's argument and local slots, arguments first, in the
	/// order the signature and the method header give them. Empty unless the
	/// method was translated with its variables pinned to the frame.
	std::vector<VarSlot> var_slots;

	/// Where a shared body's receiver sits in its frame, so a stack walk can
	/// recover the instantiation the frame is running as. The register is -1
	/// for a body that is not a shared one.
	VarSlot rgctx_slot { -1, 0 };

	/// Where the entry function's profile counters landed. Absent when the
	/// module was compiled with the instrumentation off.
	std::optional<ProfileCounters> profile;

	std::vector<ProfileCounters> other_profiles;

	/// The dylib this compile's object was linked into - what remove_dylibs ()
	/// takes to release all of the above again.
	llvm::orc::JITDylib *dylib = nullptr;
};

class MonoJit {
public:
	/// Queue an option for LLVM's own command-line registry, the same options
	/// `opt` and `llc` take. A leading dash is optional.
	///
	/// create () applies the queued options, so queue them before it runs. It
	/// fails if LLVM rejects one.
	static void add_option (llvm::StringRef opt);

	static llvm::Expected<std::unique_ptr<MonoJit>>
	create (CodeArena *arena);

	MonoJit (const MonoJit &) = delete;
	MonoJit &operator= (const MonoJit &) = delete;
	~MonoJit ();

	/// Make a runtime entry point visible to JIT'd code under a name.
	///
	/// Registering the same name and address again is a no-op, since many call
	/// sites resolve the same helper. A second address under one name is an
	/// error.
	llvm::Error register_symbol (llvm::StringRef name, void *addr);

	/// Compile a module and return where its entry point and side tables landed.
	///
	/// The module lands in a JITDylib of its own. module_symbols is defined
	/// there directly, ahead of the module itself - the caller's own resolved
	/// callee addresses, private to this compile and never shared. Beyond that,
	/// the dylib resolves external symbols through register_symbol (). A lookup
	/// never falls back to the process, so an unregistered helper fails the
	/// compile loudly.
	///
	/// The module must already have been through optimize (), which is where
	/// the tier is decided. Hand back what optimize () answered as layout, and
	/// the result carries it with the address the link gave the counters.
	llvm::Expected<CompiledMethod>
	compile (llvm::orc::ThreadSafeModule tsm, llvm::StringRef entry,
	        llvm::ArrayRef<std::pair<llvm::StringRef, void *>> module_symbols = {},
	        llvm::ArrayRef<ProfileCounters> layout = {});

	/// Compile one module holding several methods, on the terms compile ()
	/// states, and return one result per name in entries and in that order.
	///
	/// The methods share an object, so they share its side tables: the clause,
	/// guard and frame tables are the whole section in every result, and a
	/// reader picks its own function's block out of it. Everything else is
	/// split by name, which is why a method's side bodies have to carry the
	/// method's own name and a `$` suffix.
	llvm::Expected<std::vector<CompiledMethod>>
	compile_batch (llvm::orc::ThreadSafeModule tsm, llvm::ArrayRef<llvm::StringRef> entries,
	              llvm::ArrayRef<std::pair<llvm::StringRef, void *>> module_symbols = {},
	              llvm::ArrayRef<ProfileCounters> layout = {});

	/// Release the dylibs: their code, their side tables, and the ORC
	/// bookkeeping for them.
	///
	/// The reserved bytes stay reserved - CodeArena frees only the whole
	/// arena, so this does not give a later compile memory to reuse.
	///
	/// The caller proves the code dead - nothing executing in it, nothing about
	/// to call into it. Any stub still pointing at it must already be undefined.
	llvm::Error remove_dylibs (const std::vector<llvm::orc::JITDylib *> &dylibs);

	/// Run a tier's IR pipeline over a module in place, and return where it put
	/// the profile counters.
	///
	/// Every module goes through this before compile (): it is what lowers the
	/// translator's symbolic calls and puts the body into this backend's
	/// calling convention. The profile is a tier-2 input and build_profile ()
	/// writes it. An empty one still compiles at tier 2, only with nothing to
	/// lay the code out by.
	///
	/// Returns nothing when the module was not instrumented: every tier-2
	/// module, and a tier-1 module compiled with MONO_LLVM_JIT_TIER1_PGO off.
	///
	/// inliner, when given, is what tier 2 asks for the callee bodies it folds
	/// in. Without one the module is compiled with every call it arrived with
	/// still standing.
	static std::vector<ProfileCounters> optimize (llvm::Module &m, JitTier tier,
	                                              llvm::ArrayRef<uint8_t> profile = {},
	                                              InlineCandidates *inliner = nullptr);

	/// Run the tier-1 IR pipeline over a module in place.
	///
	/// Static and public so tests can assert what it does to translator output.
	static void run_tier1_pipeline (llvm::Module &m);

	/// Run the tier-2 IR pipeline over a module in place, against a profile
	/// build_profile () wrote.
	static void run_tier2_pipeline (llvm::Module &m, llvm::ArrayRef<uint8_t> profile,
	                                InlineCandidates *inliner = nullptr);

	/// The DataLayout modules compiled here must carry. compile () stamps it
	/// on modules that do not have one yet.
	const llvm::DataLayout &data_layout () const;

	const llvm::Triple &triple () const;

private:
	MonoJit (std::unique_ptr<llvm::orc::LLJIT> jit);

	std::unique_ptr<llvm::orc::LLJIT> jit_;

	llvm::orc::JITDylib *helpers_ = nullptr;

	std::mutex named_symbols_mutex_;
	std::unordered_map<std::string, void *> named_symbols_;

	std::atomic<uint64_t> module_counter_{0};

	class ObjectCapturePlugin;
	/// Captures each linked object's code extent and side tables, keyed by the
	/// dylib's name. compile () collects its own entry after the lookup.
	std::shared_ptr<ObjectCapturePlugin> capture_;

	/// The objects a debugger has been told about, by the dylib holding the
	/// code each one describes. Empty unless gdbjit::enabled ().
	std::mutex gdb_objects_mutex_;
	std::unordered_map<llvm::orc::JITDylib *, std::vector<gdbjit::Registration *>> gdb_objects_;

	void retract_debug_objects (const std::vector<llvm::orc::JITDylib *> &dylibs);

	void retract_all_debug_objects ();
};

} // namespace mono

#endif
