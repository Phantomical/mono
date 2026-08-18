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
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace llvm {
class Function;
class TargetMachine;
} // namespace llvm

namespace mono {

class CodeArena;

namespace gdbjit {
struct Registration;
}

/// The host TargetMachine every compile runs against.
///
/// One instance per calling thread, reused for every module that thread
/// compiles. A TargetMachine cannot be shared across threads, and building one
/// costs more than compiling a typical method.
llvm::TargetMachine &host_target_machine ();

/// The widest access, in bits, this target performs atomically with a single
/// instruction when compiling a function.
///
/// An atomic load or store wider than this is legal IR, but it lowers to a call
/// into the atomic runtime library. Nothing here defines that library, so a
/// wider access has to be built some other way.
unsigned host_max_atomic_bits (const llvm::Function &f);

/// Whether the IR verifier runs over what this backend produces.
///
/// MONO_LLVM_JIT_VERIFY sets the level.
bool ir_verification_enabled ();

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

	/// Every function the linked object defines, name to [code, size): the
	/// entry, and any filter bodies compiled alongside it.
	std::vector<std::pair<std::string, std::pair<const uint8_t *, size_t>>> functions;

	/// The executable sections the linker synthesized for this object, as
	/// [code, size). They are nameless, and they sit on the path of the calls
	/// that needed them. So the runtime has to resolve an address in one.
	std::vector<std::pair<const uint8_t *, size_t>> linker_stubs;

	/// The entry function's native_offset -> il_offset rows, ascending by
	/// native offset. Empty when the module carried no line table.
	std::vector<IlLineRow> il_lines;

	/// The same rows for every other function the object defines, by name.
	/// Each filter body is a frame of its own. So each needs a map of its own
	/// to say where in the method's IL it is.
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
	/// Queue an option for LLVM's own command-line registry, the same options
	/// `opt` and `llc` take. A leading dash is optional.
	///
	/// create () applies the queued options, so queue them before it runs. It
	/// fails if LLVM rejects one.
	static void add_option (llvm::StringRef opt);

	/// Build the JIT for the host, carving its code out of the given arena.
	///
	/// Every module added to it goes through the tier-0 IR pipeline.
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
	/// the dylib resolves external symbols through register_symbol () and
	/// nothing else. A lookup never falls back to the process, so an
	/// unregistered helper fails the compile loudly.
	llvm::Expected<CompiledMethod>
	compile (llvm::orc::ThreadSafeModule tsm, llvm::StringRef entry,
	        llvm::ArrayRef<std::pair<llvm::StringRef, void *>> module_symbols = {});

	/// Release the dylibs: their code, their side tables, and the memory both
	/// were linked into. Later compiles can reuse that memory.
	///
	/// The caller proves the code dead - nothing executing in it, nothing about
	/// to call into it. Any stub still pointing at it must already be undefined.
	llvm::Error remove_dylibs (const std::vector<llvm::orc::JITDylib *> &dylibs);

	/// Run the tier-0 IR pipeline over a module in place.
	///
	/// Static and public so tests can assert what it does to translator output.
	static void run_tier0_pipeline (llvm::Module &m);

	/// The DataLayout modules compiled here must carry. compile () stamps it
	/// on modules that do not have one yet.
	const llvm::DataLayout &data_layout () const;

	/// The target codegen is emitting for.
	const llvm::Triple &triple () const;

private:
	MonoJit (std::unique_ptr<llvm::orc::LLJIT> jit);

	std::unique_ptr<llvm::orc::LLJIT> jit_;

	/// Dedicated dylib holding the explicitly-registered runtime helpers. Every
	/// compiled module's dylib links against this.
	llvm::orc::JITDylib *helpers_ = nullptr;

	/// What each name handed to register_symbol () stands for.
	std::mutex named_symbols_mutex_;
	std::unordered_map<std::string, void *> named_symbols_;

	/// Names the per-module dylibs.
	std::atomic<uint64_t> module_counter_{0};

	class ObjectCapturePlugin;
	/// Captures each linked object's code extent and side tables, keyed by the
	/// dylib's name. compile () collects its own entry after the lookup.
	std::shared_ptr<ObjectCapturePlugin> capture_;

	/// The objects a debugger has been told about, by the dylib holding the
	/// code each one describes. Empty unless gdbjit::enabled ().
	std::mutex gdb_objects_mutex_;
	std::unordered_map<llvm::orc::JITDylib *, std::vector<gdbjit::Registration *>> gdb_objects_;

	/// Take back every object a debugger was told about for the given dylibs.
	void retract_debug_objects (const std::vector<llvm::orc::JITDylib *> &dylibs);

	/// Take back every object a debugger was told about, whichever dylib it
	/// came from.
	void retract_all_debug_objects ();
};

} // namespace mono

#endif
