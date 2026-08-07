/**
 * \file
 * \brief MonoJit - the ORCv2/LLJIT execution engine for the LLVM-only backend.
 */

#include "jit.hpp"

#include "arch/arch.hpp"
#include "callbacks.hpp"
#include "compiler.hpp"
#include "codemem.hpp"
#include "gdb-jit.hpp"

#include "il-line-table.hpp"
#include "seq-point-marker.hpp"
#include "sidetables.hpp"
#include "passes/array-address.hpp"
#include "passes/class-init.hpp"
#include "passes/lower-builtins.hpp"
#include "passes/restore-tail-position.hpp"
#include "stubs.hpp"
#include "timing.hpp"

#include <llvm/CodeGen/TargetLowering.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/StackMapParser.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/Scalar/TailRecursionElimination.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::orc;

namespace mono {

/*
 * Where a stub lands when the compile behind it failed. The trampoline has
 * already put the call's arguments back and jumped here, so this is running as
 * the method the caller asked for: there is no value it could return and no
 * caller that would know what to do with one.
 */
[[noreturn]] static void
lazy_compile_failed ()
{
	/*
	 * Printed and left by hand rather than through report_fatal_error, which
	 * ends in exit() when it is told not to produce a crash diagnostic. exit()
	 * runs the static destructors of every C++ library loaded into the
	 * process, LLVM's own among them, while the threads that are still
	 * compiling are using what those destructors free.
	 */
	static const char msg[] =
		"LLVM ERROR: a method failed to compile on first call\n";
	[[maybe_unused]] ssize_t written = write (2, msg, sizeof (msg) - 1);
	fflush (nullptr);
	_exit (1);
}

namespace {

/*
 * Set when codegen hands its object over, cleared when the link picks it up:
 * what is between the two is turning the ELF bytes into a LinkGraph, which
 * neither side has a hook inside of.
 */
thread_local uint64_t g_object_handed = 0;

/*
 * MONO_LLVM_JIT_HOIST names the experiments below, comma separated. None of
 * them is on by default and none is meant to be: they exist to put a number on
 * what taking one piece of per-compile work away is worth, and one of them
 * (`sharedjd`) is not even safe under concurrent compiles.
 */
bool
hoisting (StringRef what)
{
	static const char *setting = std::getenv ("MONO_LLVM_JIT_HOIST");

	if (setting == nullptr)
		return false;

	StringRef all (setting);

	while (!all.empty ()) {
		auto [head, rest] = all.split (',');

		if (head == what)
			return true;
		all = rest;
	}
	return false;
}

/// How much of what this backend produces the IR verifier gets to see.
enum class VerifyLevel {
	/// Nothing is checked.
	off,
	/// The translator's output, every pass written here, and the module
	/// codegen is handed.
	mono,
	/// The above plus every stock pass in the optimization pipeline.
	each,
};

/*
 * MONO_LLVM_JIT_VERIFY picks the level, defaulting to `mono` against an LLVM
 * built with assertions - the configuration that is being checked rather than
 * shipped. Malformed IR is worth that: it does not crash codegen, it
 * miscompiles, because the register allocator reads whatever the broken value
 * happened to leave behind.
 */
VerifyLevel
verify_level ()
{
	static VerifyLevel level = [] {
#ifdef MONO_LLVM_ASSERTIONS
		constexpr VerifyLevel unset = VerifyLevel::mono;
#else
		constexpr VerifyLevel unset = VerifyLevel::off;
#endif
		const char *v = std::getenv ("MONO_LLVM_JIT_VERIFY");

		if (v == nullptr)
			return unset;

		StringRef setting (v);
		if (setting == "0" || setting == "off")
			return VerifyLevel::off;
		if (setting == "each" || setting == "all")
			return VerifyLevel::each;
		return VerifyLevel::mono;
	}();

	return level;
}

/*
 * The verifier's diagnostics name the offending instruction and nothing else,
 * which in a JIT that compiles thousands of methods leaves out both the method
 * and what had just run over it. Print those, and the module the failure is
 * about, so this is diagnosable from the log rather than by bisecting.
 */
[[noreturn]] void
report_broken_ir (const Module &m, StringRef when, StringRef diagnostics)
{
	errs () << "mono: broken IR " << when << ", in " << m.getModuleIdentifier ()
	        << "\n"
	        << diagnostics << m << "\n";
	report_fatal_error ("mono: IR verification failed", /*GenCrashDiag=*/false);
}

void
verify_or_die (const Module &m, StringRef when)
{
	std::string diagnostics;
	raw_string_ostream os (diagnostics);

	if (verifyModule (m, &os))
		report_broken_ir (m, when, diagnostics);
}

/// The passes this backend writes, by the name the pass instrumentation reports
/// them under. A break introduced by one of these is a bug here.
bool
is_mono_pass (StringRef pass)
{
	return pass == ArrayAddressPass::name () ||
	       pass == ClassInitPass::name () ||
	       pass == LowerBuiltinsPass::name () ||
	       pass == RestoreTailPositionPass::name () ||
	       pass == arch::LegacyAbiPass::name ();
}

/// Gives mono.stubs a definition for a published stub the first time a module
/// names it.
///
/// Most published methods are never named by anything: their address goes into
/// a vtable slot or comes back to the runtime, and a call reaches them through
/// that rather than through a symbol. Those never cost a symbol at all - and a
/// module that does name some gets them all in one definition, since a link
/// asks for everything it is missing at once.
class StubGenerator : public DefinitionGenerator {
public:
	StubGenerator (StubTable &table, std::mutex &defs)
		: table_ (&table), defs_ (&defs)
	{
	}

	Error tryToGenerate (LookupState &, LookupKind, JITDylib &jd,
	                     JITDylibLookupFlags,
	                     const SymbolLookupSet &lookup) override
	{
		/*
		 * Claiming and defining have to look like one step to undefine_stubs ():
		 * a name claimed here is already marked as one the linker knows, so an
		 * undefine landing in between would go looking for a symbol that does
		 * not exist yet and then leave this definition standing over a table
		 * entry that is gone.
		 */
		std::lock_guard<std::mutex> lock (*defs_);
		SymbolMap symbols;

		for (const auto &[name, flags] : lookup) {
			void *code = table_->claim_for_linker (*name);

			if (code != nullptr)
				symbols[name] = {
					ExecutorAddr::fromPtr (code),
					JITSymbolFlags::Exported | JITSymbolFlags::Callable,
				};
		}

		if (symbols.empty ())
			return Error::success ();

		return jd.define (absoluteSymbols (std::move (symbols)));
	}

private:
	StubTable *table_;
	std::mutex *defs_;
};

} // namespace

template <typename T>
static T
read_le (const uint8_t *p)
{
	T value;

	std::memcpy (&value, p, sizeof (T));
	return value;
}

/*
 * Turn the linked `.mono_lines` into per-function rows: the IL offset in effect
 * at each offset into each function, and the sequence-point markers that ride
 * the same channel. FUNCTIONS names each function by where it was linked, which
 * is what a block's header identifies itself with.
 *
 * Several rows landing on one offset is what a run of IL instructions collapses
 * to once the optimizer is done with it. They arrive in code order, so the last
 * one is the offset in effect there; keeping that one is what makes the map
 * single-valued, and it agrees with the "most recent point execution passed"
 * lookup that reads the map back.
 */
static void
parse_line_table (const uint8_t *table, size_t size,
                  const std::map<const uint8_t *, std::string> &functions,
                  std::map<std::string, std::vector<IlLineRow>> &out,
                  std::map<std::string, std::vector<IlLineRow>> &seq_points)
{
	const uint8_t *p = table;
	const uint8_t *end = table + size;

	while (p + lines_header_size <= end) {
		if (read_le<uint32_t> (p) != lines_section_magic
		    || read_le<uint16_t> (p + 4) != lines_section_version) {
			errs () << "mono: .mono_lines is not in a format this runtime "
			           "knows\n";
			return;
		}

		uint32_t count = read_le<uint32_t> (p + 8);
		const uint8_t *code = (const uint8_t *) read_le<uint64_t> (p + 12);
		const uint8_t *records = p + lines_header_size;

		p = records + (size_t) count * lines_record_size;
		if (p > end)
			return;

		auto owner = functions.find (code);
		if (owner == functions.end ())
			continue;

		std::vector<IlLineRow> *rows = &out[owner->second];
		std::vector<IlLineRow> *points = nullptr;

		for (uint32_t i = 0; i < count; ++i) {
			const uint8_t *r = records + (size_t) i * lines_record_size;
			uint32_t line = read_le<uint32_t> (r + 4);

			/*
			 * Line 0 is "no source location" - what an instruction the
			 * translator never attributed produces. The bias keeps a real
			 * IL offset of 0 from looking like one.
			 */
			if (line == 0)
				continue;

			IlLineRow row;
			row.native_offset = read_le<uint32_t> (r);
			row.il_offset = line - IL_OFFSET_LINE_BIAS;

			/*
			 * A sequence point marker says where the soft debugger's
			 * trampolines return into, not what IL offset is in effect
			 * there - the offset in effect is whatever the row before it
			 * said, which is what keeping it out of the map below keeps.
			 */
			if (seq_point_is_marker (row.il_offset)) {
				row.flags = seq_point_marker_flags (row.il_offset);
				row.il_offset = seq_point_marker_offset (row.il_offset);
				if (points == nullptr)
					points = &seq_points[owner->second];
				points->push_back (row);
				continue;
			}

			if (!rows->empty ()
			    && rows->back ().native_offset == row.native_offset)
				rows->back () = row;
			else
				rows->push_back (row);
		}

		if (rows->empty ())
			out.erase (owner->second);
	}

	/*
	 * Rows are emitted in the order the printer walked the blocks, which is not
	 * ascending by address. The runtime binary-searches these, so sort -
	 * stably, so the last-row-wins choice above survives.
	 */
	auto by_address = [] (const IlLineRow &a, const IlLineRow &b) {
		return a.native_offset < b.native_offset;
	};

	for (auto &kv : out)
		std::stable_sort (kv.second.begin (), kv.second.end (), by_address);
	for (auto &kv : seq_points)
		std::stable_sort (kv.second.begin (), kv.second.end (), by_address);
}

/*
 * Where the translator's debug-variable marker says this method's arguments and
 * locals ended up, in the order it named them - arguments then locals.
 *
 * Read out of the object rather than off the linked graph because none of it
 * needs relocating: a slot is a register number and a displacement, both settled
 * at codegen. The section holds a record per stackmap in the module, so the
 * marker is found by its id; a module has at most one, since only a method's own
 * body carries it.
 */
static void
parse_debug_var_slots (object::ObjectFile &obj, std::vector<VarSlot> &out)
{
	for (const object::SectionRef &section : obj.sections ()) {
		Expected<StringRef> name = section.getName ();

		if (!name) {
			consumeError (name.takeError ());
			continue;
		}
		if (*name != ".llvm_stackmaps")
			continue;

		Expected<StringRef> contents = section.getContents ();

		if (!contents) {
			consumeError (contents.takeError ());
			return;
		}

		ArrayRef<uint8_t> bytes ((const uint8_t *) contents->data (),
		                         contents->size ());
		StackMapParser<llvm::endianness::little> parser (bytes);

		for (const auto &record : parser.records ()) {
			if (record.getID () != vars_stackmap_id)
				continue;

			for (const auto &location : record.locations ()) {
				/*
				 * An alloca operand lowers to Direct - register plus
				 * displacement is the slot's address. Anything else
				 * means the operand was not the slot we named, and a
				 * partial list would misattribute every variable after
				 * it, so give up on the method's variables entirely.
				 */
				if (location.getKind ()
				    != StackMapParser<llvm::endianness::little>::
				               LocationKind::Direct) {
					out.clear ();
					return;
				}

				out.push_back ({ (int32_t) location.getDwarfRegNum (),
				                 (int32_t) location.getOffset () });
			}
			return;
		}
		return;
	}
}

/*
 * Whether SECTION holds code JITLink made up rather than code the compiler
 * emitted - the jump stubs it plants when a call cannot reach its target
 * directly, which today live in a section it calls "$__STUBS".
 *
 * Keyed on the shape rather than the name: a synthesized section carries no
 * named symbol, because nothing in the object refers to a stub by name. A
 * later LLVM renaming the section would otherwise silently leave its code
 * unregistered, and code the runtime cannot name is code a stack walk stops
 * dead in.
 */
static bool
is_linker_stub_section (jitlink::Section &section)
{
	if ((section.getMemProt () & orc::MemProt::Exec) == orc::MemProt::None)
		return false;
	if (section.blocks ().empty ())
		return false;

	for (jitlink::Symbol *sym : section.symbols ())
		if (sym->hasName ())
			return false;
	return true;
}

/*
 * Reads, for every linked object, where the pieces the runtime needs landed:
 * each defined function's extent and the two mono side-table sections. Keyed by
 * the per-compile dylib, whose name is unique, so a method compiled twice never
 * collides with itself.
 *
 * The side-table sections carry no symbols and nothing references them, so
 * JITLink's pruning would drop them; the pre-prune pass marks their blocks
 * live. The read itself runs post-fixup, when addresses are final - and with
 * the in-process memory manager those addresses are readable memory in this
 * process from finalization on.
 */
class MonoJit::ObjectCapturePlugin : public ObjectLinkingLayer::Plugin {
public:
	struct Extents {
		std::vector<std::pair<std::string, std::pair<const uint8_t *, size_t>>>
			functions;
		const uint8_t *clause_table = nullptr;
		size_t clause_table_size = 0;
		const uint8_t *guard_table = nullptr;
		size_t guard_table_size = 0;
		const uint8_t *unwind_table = nullptr;
		size_t unwind_table_size = 0;
		/// Each executable section the linker synthesized for itself.
		std::vector<std::pair<const uint8_t *, size_t>> linker_stubs;
		/// Each defined function's line table, by name.
		std::map<std::string, std::vector<IlLineRow>> il_lines;
		/// Each defined function's sequence point markers, by name.
		std::map<std::string, std::vector<IlLineRow>> seq_points;
		/// Where the body's arguments and locals live in its frame.
		std::vector<VarSlot> var_slots;
		/// The object as a debugger should see it, section addresses filled
		/// in. Empty unless gdbjit::enabled ().
		std::vector<char> debug_object;
	};

	/*
	 * The one hook that sees the object bytes, which is the only place
	 * `.llvm_stackmaps` exists in a form nothing else has to be kept alive
	 * for. Upstream marks this deprecated and promises "a proper mechanism for
	 * capturing object buffers"; there is not one yet, so this is the
	 * mechanism.
	 */
	void notifyMaterializing (MaterializationResponsibility &mr,
	                          jitlink::LinkGraph &, jitlink::JITLinkContext &,
	                          MemoryBufferRef input_object) override
	{
		timing::span_end (timing::Phase::lgraph, g_object_handed);
		g_object_handed = 0;

		/*
		 * A debugger is handed the object rather than anything synthesized
		 * from it, so the bytes have to survive until the link has decided
		 * where everything goes and the addresses can be stamped in.
		 */
		if (gdbjit::enabled ()) {
			std::vector<char> bytes (input_object.getBufferStart (),
			                         input_object.getBufferEnd ());
			std::lock_guard<std::mutex> lock (mutex_);

			objects_[mr.getTargetJITDylib ().getName ()] = std::move (bytes);
		}

		std::vector<VarSlot> var_slots;
		timing::Scope timed (timing::Phase::vslots);

		Expected<std::unique_ptr<object::ObjectFile>> obj =
			object::ObjectFile::createObjectFile (input_object);

		if (!obj) {
			consumeError (obj.takeError ());
			return;
		}

		parse_debug_var_slots (**obj, var_slots);
		if (var_slots.empty ())
			return;

		std::lock_guard<std::mutex> lock (mutex_);
		var_slots_[mr.getTargetJITDylib ().getName ()] = std::move (var_slots);
	}

	void modifyPassConfig (MaterializationResponsibility &mr,
	                       jitlink::LinkGraph &g,
	                       jitlink::PassConfiguration &config) override
	{
		std::string dylib = mr.getTargetJITDylib ().getName ();

		if (timing::fine ()) {
			auto started = std::make_shared<uint64_t> (0);

			config.PrePrunePasses.push_back (
				[started] (jitlink::LinkGraph &) -> Error {
					*started = timing::span_begin (timing::Phase::jlink);
					return Error::success ();
				});
			config.PostFixupPasses.push_back (
				[started] (jitlink::LinkGraph &) -> Error {
					timing::span_end (timing::Phase::jlink, *started);
					return Error::success ();
				});
		}

		config.PrePrunePasses.push_back ([] (jitlink::LinkGraph &graph) -> Error {
			for (jitlink::Section &section : graph.sections ()) {
				if (section.getName () != ".mono_lsda"
				    && section.getName () != ".mono_guards"
				    && section.getName () != ".mono_unwind"
				    && section.getName () != ".mono_lines")
					continue;
				for (jitlink::Block *block : section.blocks ())
					graph.addAnonymousSymbol (*block, 0,
					                          block->getSize (), false,
					                          /*IsLive=*/true);
			}
			return Error::success ();
		});

		config.PostFixupPasses.push_back (
			[this, dylib] (jitlink::LinkGraph &graph) -> Error {
				Extents extents;
				const uint8_t *line_table = nullptr;
				size_t line_table_size = 0;

				for (jitlink::Section &section : graph.sections ()) {
					jitlink::SectionRange range (section);

					if (section.getName () == ".mono_lsda") {
						extents.clause_table =
							range.getStart ().toPtr<const uint8_t *> ();
						extents.clause_table_size = range.getSize ();
					} else if (section.getName () == ".mono_guards") {
						extents.guard_table =
							range.getStart ().toPtr<const uint8_t *> ();
						extents.guard_table_size = range.getSize ();
					} else if (section.getName () == ".mono_unwind") {
						extents.unwind_table =
							range.getStart ().toPtr<const uint8_t *> ();
						extents.unwind_table_size = range.getSize ();
					} else if (section.getName () == ".mono_lines") {
						line_table =
							range.getStart ().toPtr<const uint8_t *> ();
						line_table_size = range.getSize ();
					} else if (is_linker_stub_section (section)) {
						extents.linker_stubs.emplace_back (
							range.getStart ()
								.toPtr<const uint8_t *> (),
							range.getSize ());
					}
				}

				std::map<const uint8_t *, std::string> by_address;

				for (jitlink::Symbol *sym : graph.defined_symbols ()) {
					if (!sym->hasName () || !sym->isCallable ())
						continue;

					const uint8_t *code =
						sym->getAddress ().toPtr<const uint8_t *> ();

					extents.functions.emplace_back (
						std::string (*sym->getName ()),
						std::make_pair (code, (size_t) sym->getSize ()));
					by_address[code] = std::string (*sym->getName ());
				}

				if (line_table != nullptr)
					parse_line_table (line_table, line_table_size,
					                  by_address, extents.il_lines,
					                  extents.seq_points);

				if (gdbjit::enabled ())
					extents.debug_object = stamp_debug_object (dylib, graph);

				std::lock_guard<std::mutex> lock (mutex_);
				captured_[dylib] = std::move (extents);
				return Error::success ();
			});
	}

	/// The extents captured for DYLIB's one object, surrendered to the caller.
	std::optional<Extents> take (StringRef dylib)
	{
		std::lock_guard<std::mutex> lock (mutex_);
		auto it = captured_.find (std::string (dylib));

		if (it == captured_.end ())
			return std::nullopt;

		Extents extents = std::move (it->second);
		captured_.erase (it);

		/*
		 * Captured by the other hook, before linking, so it is merged here
		 * rather than written into the same slot.
		 */
		if (auto slots = var_slots_.find (std::string (dylib));
		    slots != var_slots_.end ()) {
			extents.var_slots = std::move (slots->second);
			var_slots_.erase (slots);
		}

		return extents;
	}

	Error notifyFailed (MaterializationResponsibility &) override
	{
		return Error::success ();
	}
	Error notifyRemovingResources (JITDylib &, ResourceKey) override
	{
		return Error::success ();
	}
	void notifyTransferringResources (JITDylib &, ResourceKey, ResourceKey) override
	{
	}

private:
	/// The object DYLIB's compile produced, with GRAPH's final section
	/// addresses written into it - what a debugger is handed. Empty when the
	/// bytes were never taken, which is every compile if gdb registration was
	/// turned on after this JIT started.
	std::vector<char> stamp_debug_object (const std::string &dylib,
	                                      jitlink::LinkGraph &graph)
	{
		std::vector<char> bytes;
		{
			std::lock_guard<std::mutex> lock (mutex_);
			auto it = objects_.find (dylib);

			if (it == objects_.end ())
				return {};
			bytes = std::move (it->second);
			objects_.erase (it);
		}

		return gdbjit::debug_object (
			std::move (bytes), [&graph] (StringRef name) -> uint64_t {
				jitlink::Section *section = graph.findSectionByName (name);

				if (section == nullptr || section->blocks ().empty ())
					return 0;
				return jitlink::SectionRange (*section).getStart ().getValue ();
			});
	}

	std::mutex mutex_;
	std::map<std::string, Extents> captured_;
	std::map<std::string, std::vector<VarSlot>> var_slots_;
	/// Each in-flight compile's object bytes, taken before the link and given
	/// back once it has settled. Only populated when gdbjit::enabled ().
	std::map<std::string, std::vector<char>> objects_;
};

static void
ensure_native_target ()
{
	static std::once_flag once;
	std::call_once (once, [] {
		InitializeNativeTarget ();
		InitializeNativeTargetAsmPrinter ();
		InitializeNativeTargetAsmParser ();
	});
}

static std::mutex g_options_mutex;
static std::vector<std::string> g_options;

void
MonoJit::add_option (StringRef opt)
{
	std::lock_guard<std::mutex> lock (g_options_mutex);

	g_options.push_back (opt.starts_with ("-") ? opt.str () : "-" + opt.str ());
}

/*
 * cl::ParseCommandLineOptions () is all-at-once - each call re-parses argv from
 * scratch - so the queued options are handed over in one batch, and only once.
 * Passing an error stream is what keeps a bad option from calling exit () out
 * from under the runtime.
 */
static Error
apply_options ()
{
	static bool applied = false;

	std::lock_guard<std::mutex> lock (g_options_mutex);
	if (applied || g_options.empty ())
		return Error::success ();
	applied = true;

	std::vector<const char *> argv {"mono"};
	for (const std::string &opt : g_options)
		argv.push_back (opt.c_str ());

	if (!cl::ParseCommandLineOptions ((int) argv.size (), argv.data (), "",
	                                  &errs ()))
		return createStringError (inconvertibleErrorCode (),
		                          "llvm rejected an option given with --llvm-opt");
	return Error::success ();
}

/*
 * The host target configuration every compile uses, detected once.
 *
 * Code model Small with Reloc::PIC_ rather than the JIT default (Large):
 * JITLink reroutes any reference it cannot prove in-range through an in-graph
 * GOT slot or PLT stub whose outgoing edge is a full 64-bit pointer, so
 * Small+PIC is correct wherever sections land and considerably denser.
 *
 * CodeGenOptLevel::None is the tier-0 choice on purpose: it selects FastISel,
 * which is the cheap-and-cheerful instruction selection this tier wants - the
 * easy wins come from the O1 IR pipeline (run_tier0_pipeline), not from the
 * optimizing selector. FastISel falls back to SelectionDAG per block for
 * constructs it does not cover (musttail among them), which costs compile
 * time, never correctness.
 */
static JITTargetMachineBuilder
host_target_machine_builder ()
{
	static const JITTargetMachineBuilder jtmb = [] {
		ensure_native_target ();

		auto b = cantFail (JITTargetMachineBuilder::detectHost ());
		b.setCodeGenOptLevel (CodeGenOptLevel::None);
		b.setCPU (std::string (sys::getHostCPUName ()));
		b.setCodeModel (CodeModel::Small);
		b.setRelocationModel (Reloc::PIC_);

		/*
		 * If codegen ever reaches an LLVM `unreachable` (a translator bug, or
		 * UB the IL could not rule out), a `ud2` beats falling through into
		 * whatever bytes come next.
		 */
		b.getOptions ().TrapUnreachable = true;

		StringMap<bool> features = sys::getHostCPUFeatures ();
		std::vector<std::string> feature_vec;
		for (auto &kv : features)
			if (kv.second)
				feature_vec.push_back ((Twine ("+") + kv.first ()).str ());
		b.addFeatures (feature_vec);
		return b;
	}();
	return jtmb;
}

/*
 * One per compile thread, because building one is far from free - the X86
 * subtarget alone resolves a ~200-entry feature string against the implication
 * graph and then builds every lowering and legalizer table behind it, which for
 * methods the size the translator emits costs more than compiling them. A
 * TargetMachine is not safe to share between threads (this is why ORC's stock
 * ConcurrentIRCompiler builds one per module), but reusing one for module after
 * module on a single thread is exactly what SimpleCompiler does.
 */
TargetMachine &
host_target_machine ()
{
	static thread_local std::unique_ptr<TargetMachine> tm =
		cantFail (host_target_machine_builder ().createTargetMachine ());
	return *tm;
}

unsigned
host_max_atomic_bits (const Function &f)
{
	const TargetSubtargetInfo *sti = host_target_machine ().getSubtargetImpl (f);
	const TargetLowering *tli = sti != nullptr ? sti->getTargetLowering () : nullptr;

	return tli != nullptr ? tli->getMaxAtomicSizeInBitsSupported () : 0;
}

bool
ir_verification_enabled ()
{
	return verify_level () != VerifyLevel::off;
}

namespace {

thread_local Module *g_verify_module = nullptr;
thread_local VerifyLevel g_verify_level = VerifyLevel::off;

/*
 * The tier-0 IR pipeline and everything it is built out of, kept per thread.
 *
 * None of it depends on the module it is about to run over, and standing it up
 * costs a couple of percent of a small method's compile, so a compile thread
 * builds it once and reuses it. What that does make the caller responsible for
 * is emptying the analysis managers after each run, since their results are
 * keyed by IR the module is about to take with it.
 */
struct Tier0Pipeline {
	PassInstrumentationCallbacks pic;
	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cgam;
	ModuleAnalysisManager mam;
	std::unique_ptr<PassBuilder> pb;
	ModulePassManager mpm;

	Tier0Pipeline ();

	/*
	 * Drop every cached analysis. Has to happen while the module the results
	 * were computed over is still standing: a cached MemorySSA holds
	 * references into that IR and its destructor walks them.
	 */
	void forget_analyses ();
};

Tier0Pipeline::Tier0Pipeline ()
{
	pic.registerAfterPassCallback (
		[] (StringRef pass, Any, const PreservedAnalyses &) {
			if (g_verify_module == nullptr)
				return;
			if (g_verify_level == VerifyLevel::each || is_mono_pass (pass))
				verify_or_die (*g_verify_module,
				               ("after pass \"" + pass + "\"").str ());
		});

	/*
	 * A TargetMachine so TargetTransformInfo is real; without one the
	 * cost-model-driven parts of the pipeline silently no-op.
	 */
	pb = std::make_unique<PassBuilder> (&host_target_machine (),
	                                    PipelineTuningOptions (), std::nullopt,
	                                    &pic);
	pb->registerModuleAnalyses (mam);
	pb->registerCGSCCAnalyses (cgam);
	pb->registerFunctionAnalyses (fam);
	pb->registerLoopAnalyses (lam);
	pb->crossRegisterProxies (lam, fam, cgam, mam);

	/*
	 * Before the pipeline, so the optimizer sees the element arithmetic;
	 * after it, so it works over natural-typed calls and only what survives
	 * is lowered to the legacy boundary convention.
	 */
	mpm.addPass (ArrayAddressPass ());
	mpm.addPass (LowerBuiltinsPass ());

	/*
	 * Once here, so a check the translator emitted for a class an earlier one
	 * already covers costs the rest of the pipeline nothing, and again after,
	 * because unrolling and jump threading copy whatever survived: a loop the
	 * unroller straightens out ends up with one check per copied body, all but
	 * the first of which the second run drops.
	 */
	mpm.addPass (createModuleToFunctionPassAdaptor (ClassInitPass ()));

	/*
	 * The function simplification pipeline rather than the whole O1 module
	 * pipeline: a module here is a single method, so the module and CGSCC
	 * layers have nothing to work on - no internal function to specialize, and
	 * no callee body to inline, since every call the translator emits leaves
	 * the module by symbol. Running them anyway costs a large fraction of
	 * tier-0 compile time.
	 */
	FunctionPassManager fpm = pb->buildFunctionSimplificationPipeline (
		OptimizationLevel::O1, ThinOrFullLTOPhase::None);

	fpm.addPass (ClassInitPass ());

	/*
	 * At O1 this is the one pass that only the module pipeline would have run,
	 * and it is load-bearing: it marks the entry thunk's call to the method
	 * body as a tail call, which is what lets the thunk leave no frame behind.
	 * Without it every method entered through its thunk - anything the runtime
	 * calls, so every reflection invoke - shows up twice in a stack trace.
	 */
	fpm.addPass (TailCallElimPass ());

	/*
	 * Last, because what it repairs is the pipeline's own doing: SimplifyCFG
	 * merges a function's returning blocks into one, which turns the ret a tail
	 * call needs to sit in front of into a branch and leaves the marker meaning
	 * nothing. It steps around musttail, whose adjacency is a verifier rule, and
	 * around nothing else.
	 */
	fpm.addPass (RestoreTailPositionPass ());
	mpm.addPass (createModuleToFunctionPassAdaptor (std::move (fpm)));
	mpm.addPass (arch::LegacyAbiPass ());
}

void
Tier0Pipeline::forget_analyses ()
{
	mam.clear ();
	cgam.clear ();
	fam.clear ();
	lam.clear ();
}

Tier0Pipeline &
tier0_pipeline ()
{
	static thread_local Tier0Pipeline pipeline;

	return pipeline;
}

} // namespace

void
MonoJit::run_tier0_pipeline (Module &m)
{
	timing::Scope timed (timing::Phase::pipeline);
	VerifyLevel verify = verify_level ();

	if (verify != VerifyLevel::off)
		verify_or_die (m, "as translated");

	std::optional<timing::Scope> timed_setup (std::in_place,
	                                          timing::Phase::pbsetup);
	Tier0Pipeline &pipeline = tier0_pipeline ();

	/*
	 * The after-pass verifier is registered once with the pipeline, so which
	 * module it looks at is handed over here rather than captured.
	 */
	g_verify_module = verify != VerifyLevel::off ? &m : nullptr;
	g_verify_level = verify;
	timed_setup.reset ();
	{
		timing::Scope timed_run (timing::Phase::prun);

		pipeline.mpm.run (m, pipeline.mam);
		pipeline.forget_analyses ();
	}
	g_verify_module = nullptr;
}

Expected<std::unique_ptr<MonoJit>>
MonoJit::create ()
{
	ensure_native_target ();

	if (Error err = apply_options ())
		return std::move (err);

	Expected<std::shared_ptr<CodeSlabs>> slabs = CodeSlabs::create ();
	if (!slabs)
		return slabs.takeError ();

	LLJITBuilder builder;
	builder.setJITTargetMachineBuilder (host_target_machine_builder ());

	/*
	 * JITLink, not the RTDyldObjectLinkingLayer LLJIT still defaults to on
	 * ELF. LLJIT's generic platform setup attaches its eh-frame registration
	 * plugin to this layer on its own.
	 *
	 * The slabs are made here rather than by the MonoJit constructor because
	 * this lambda runs inside builder.create (), before there is a MonoJit to
	 * own them.
	 */
	builder.setObjectLinkingLayerCreator (
		[&slabs] (ExecutionSession &es) -> Expected<std::unique_ptr<ObjectLayer>> {
			return std::make_unique<ObjectLinkingLayer> (
				es, std::make_unique<SlabMemoryManager> (*slabs));
		});

	/*
	 * The compiler that carries mono's clause gather and side-table emission
	 * along with stock codegen; SimpleCompiler emits neither.
	 */
	builder.setCompileFunctionCreator (
		[] (JITTargetMachineBuilder jtmb)
			-> Expected<std::unique_ptr<IRCompileLayer::IRCompiler>> {
			return std::make_unique<MethodObjectCompiler> (std::move (jtmb));
		});

	auto jit = builder.create ();
	if (!jit)
		return jit.takeError ();

	std::unique_ptr<MonoJit> self (
		new MonoJit (std::move (*jit), std::move (*slabs)));

	/*
	 * Without a hook here the module and its LLVMContext are dropped inside
	 * ORC's own emit, where nothing accounts for them; taking delivery of it is
	 * also the moment the object exists and the link has not started.
	 */
	if (timing::fine ())
		self->jit_->getIRCompileLayer ().setNotifyCompiled (
			[] (MaterializationResponsibility &, ThreadSafeModule tsm) {
				{
					timing::Scope timed (timing::Phase::tsmfree);

					tsm = ThreadSafeModule ();
				}
				g_object_handed = timing::span_begin (timing::Phase::lgraph);
			});

	self->capture_ = std::make_shared<ObjectCapturePlugin> ();
	static_cast<ObjectLinkingLayer &> (self->jit_->getObjLinkingLayer ())
		.addPlugin (self->capture_);

	/*
	 * Every added module goes through the tier-0 pipeline on its way to the
	 * compiler. The transform layer sits under LLJIT's init-helper layer, so
	 * this runs after any platform rewriting and immediately before codegen.
	 */
	self->jit_->getIRTransformLayer ().setTransform (
		[] (ThreadSafeModule tsm, MaterializationResponsibility &)
			-> Expected<ThreadSafeModule> {
			tsm.withModuleDo ([] (Module &m) { run_tier0_pipeline (m); });
			return std::move (tsm);
		});

	ExecutionSession &es = self->jit_->getExecutionSession ();
	self->helpers_ = &es.createBareJITDylib ("mono.helpers");
	self->stubs_ = &es.createBareJITDylib ("mono.stubs");

	auto table = StubTable::create (es.getTargetTriple (), *self->slabs_);
	if (!table)
		return table.takeError ();
	self->stub_table_ = std::move (*table);
	self->stubs_->addGenerator (std::make_unique<StubGenerator> (
		*self->stub_table_, self->stub_defs_mutex_));

	auto callbacks = LazyCallbacks::create ((void *) &lazy_compile_failed);
	if (!callbacks)
		return callbacks.takeError ();
	self->callbacks_ = std::move (*callbacks);

	return std::move (self);
}

MonoJit::MonoJit (std::unique_ptr<LLJIT> jit, std::shared_ptr<CodeSlabs> slabs)
	: slabs_ (std::move (slabs)), jit_ (std::move (jit))
{
}

MonoJit::~MonoJit ()
{
	/* The domain's code goes with the linker below, so its description has to
	 * go too. */
	retract_all_debug_objects ();
}

const DataLayout &
MonoJit::data_layout () const
{
	return jit_->getDataLayout ();
}

const Triple &
MonoJit::triple () const
{
	return jit_->getExecutionSession ().getTargetTriple ();
}

Error
MonoJit::register_symbol (StringRef name, void *addr)
{
	std::lock_guard<std::mutex> lock (named_symbols_mutex_);

	auto it = named_symbols_.find (name.str ());
	if (it != named_symbols_.end ()) {
		/*
		 * A name is a promise about what it stands for. Two addresses under one
		 * name means a caller built a name that is not unique, and the first
		 * definition is the one every later module would silently link against -
		 * so say so here rather than emit code that reads the wrong object.
		 */
		if (it->second != addr)
			return createStringError (
				inconvertibleErrorCode (),
				"symbol %s already stands for a different address",
				name.str ().c_str ());
		return Error::success ();
	}

	SymbolMap symbols;
	symbols[jit_->getExecutionSession ().intern (name)] = {
		ExecutorAddr::fromPtr (addr),
		JITSymbolFlags::Exported | JITSymbolFlags::Callable,
	};
	if (Error err = helpers_->define (absoluteSymbols (std::move (symbols))))
		return err;

	named_symbols_.emplace (name.str (), addr);
	return Error::success ();
}

Error
MonoJit::create_stub (StringRef name, void *target)
{
	Expected<void *> stub = stub_table_->reserve (name, target);

	if (!stub)
		return stub.takeError ();
	return Error::success ();
}

Expected<void *>
MonoJit::create_keyed_stub (StringRef name, void *target, void *key)
{
	return stub_table_->reserve_keyed (name, target, key);
}

Error
MonoJit::create_lazy_stub (StringRef name, LazyCompileFunction compile)
{
	ExecutionSession &es = jit_->getExecutionSession ();
	std::string method = name.str ();

	Expected<void *> trampoline = callbacks_->reserve (
		name, [this, &es, method, compile = std::move (compile)] () mutable {
			Expected<void *> code = compile ();
			if (!code) {
				es.reportError (code.takeError ());
				return (void *) &lazy_compile_failed;
			}

			if (Error err = redirect_stub (method, *code)) {
				es.reportError (std::move (err));
				return (void *) &lazy_compile_failed;
			}

			return *code;
		});
	if (!trampoline)
		return trampoline.takeError ();

	if (Error err = create_stub (name, *trampoline)) {
		callbacks_->release (name);
		return err;
	}

	return Error::success ();
}

Error
MonoJit::redirect_stub (StringRef name, void *target)
{
	return stub_table_->redirect (name, target);
}

Expected<void *>
MonoJit::stub_address (StringRef name)
{
	void *code = stub_table_->find (name);

	if (code == nullptr)
		return make_error<StringError> ("no stub was published for " + name,
		                                inconvertibleErrorCode ());
	return code;
}

Expected<CompiledMethod>
MonoJit::compile (ThreadSafeModule tsm, StringRef entry)
{
	/*
	 * An assertions-on LLVM refuses to codegen a module whose layout
	 * disagrees with the target, and a fresh module has none at all.
	 */
	tsm.withModuleDo ([&] (Module &m) {
		if (m.getDataLayout ().isDefault ())
			m.setDataLayout (jit_->getDataLayout ());
	});

	/*
	 * A dylib per module, resolving only through mono.helpers and mono.stubs:
	 * JIT'd code can reach exactly what was registered and the methods that
	 * have been published, nothing else. Calls to another method bind to its
	 * stub by name, which is what keeps them correct across promotions. Bare,
	 * because these modules carry no initializers for the platform to manage.
	 */
	std::string jd_name =
		("jd." + Twine (module_counter_.fetch_add (1)) + "." + entry).str ();
	/*
	 * MONO_LLVM_JIT_HOIST=sharedjd puts every module in one dylib instead, to
	 * price making a fresh one. It is a measurement arm and not a candidate
	 * implementation: the capture below is keyed by the dylib's name, so two
	 * compiles running at once would take each other's object, and no method
	 * can be freed on its own any more.
	 */
	if (hoisting ("sharedjd"))
		jd_name = "jd.shared";

	JITDylib &jd = [&] () -> JITDylib & {
		timing::Scope timed (timing::Phase::dylib);

		if (shared_jd_ != nullptr)
			return *shared_jd_;

		JITDylib &made = jit_->getExecutionSession ().createBareJITDylib (jd_name);

		made.addToLinkOrder (*helpers_);
		made.addToLinkOrder (*stubs_);
		if (hoisting ("sharedjd"))
			shared_jd_ = &made;
		return made;
	}();

	{
		timing::Scope timed (timing::Phase::addir);

		if (Error err = jit_->addIRModule (jd, std::move (tsm)))
			return std::move (err);
	}

	Expected<ExecutorAddr> sym = jit_->lookup (jd, entry);
	if (!sym)
		return sym.takeError ();

	std::optional<ObjectCapturePlugin::Extents> extents = capture_->take (jd_name);
	if (!extents)
		return createStringError (inconvertibleErrorCode (),
		                          "no object was captured while compiling %s",
		                          entry.str ().c_str ());

	CompiledMethod compiled;
	compiled.entry = sym->toPtr<void *> ();
	compiled.dylib = &jd;
	compiled.clause_table = extents->clause_table;
	compiled.clause_table_size = extents->clause_table_size;
	compiled.guard_table = extents->guard_table;
	compiled.guard_table_size = extents->guard_table_size;
	compiled.unwind_table = extents->unwind_table;
	compiled.unwind_table_size = extents->unwind_table_size;
	compiled.linker_stubs = std::move (extents->linker_stubs);

	for (auto &[name, extent] : extents->functions) {
		if (name == entry) {
			compiled.code = extent.first;
			compiled.code_size = extent.second;
		}
	}
	compiled.functions = std::move (extents->functions);

	if (auto lines = extents->il_lines.find (entry.str ());
	    lines != extents->il_lines.end ()) {
		compiled.il_lines = std::move (lines->second);
		extents->il_lines.erase (lines);
	}

	for (auto &lines : extents->il_lines)
		compiled.other_il_lines.emplace_back (lines.first,
		                                      std::move (lines.second));

	if (auto points = extents->seq_points.find (entry.str ());
	    points != extents->seq_points.end ())
		compiled.seq_points = std::move (points->second);

	compiled.var_slots = std::move (extents->var_slots);

	if (compiled.code == nullptr)
		return createStringError (inconvertibleErrorCode (),
		                          "the linked object for %s does not define it",
		                          entry.str ().c_str ());

	if (!extents->debug_object.empty ()) {
		gdbjit::Registration *reg =
			gdbjit::publish (std::move (extents->debug_object));

		if (reg != nullptr) {
			std::lock_guard<std::mutex> lock (gdb_objects_mutex_);

			gdb_objects_[&jd].push_back (reg);
		}
	}

	return compiled;
}

void
MonoJit::retract_debug_objects (const std::vector<JITDylib *> &dylibs)
{
	std::vector<gdbjit::Registration *> going;
	{
		std::lock_guard<std::mutex> lock (gdb_objects_mutex_);

		if (gdb_objects_.empty ())
			return;

		for (JITDylib *jd : dylibs) {
			auto it = gdb_objects_.find (jd);

			if (it == gdb_objects_.end ())
				continue;
			going.insert (going.end (), it->second.begin (), it->second.end ());
			gdb_objects_.erase (it);
		}
	}

	for (gdbjit::Registration *reg : going)
		gdbjit::retract (reg);
}

void
MonoJit::retract_all_debug_objects ()
{
	decltype (gdb_objects_) going;
	{
		std::lock_guard<std::mutex> lock (gdb_objects_mutex_);

		going.swap (gdb_objects_);
	}

	for (auto &[jd, registrations] : going)
		for (gdbjit::Registration *reg : registrations)
			gdbjit::retract (reg);
}

Error
MonoJit::remove_dylibs (const std::vector<JITDylib *> &dylibs)
{
	if (dylibs.empty ())
		return Error::success ();

	/*
	 * Ahead of the removal: a debugger asked about one of these addresses
	 * after the code is gone would read an object describing memory that has
	 * been handed to the next method along.
	 */
	retract_debug_objects (dylibs);

	std::vector<JITDylibSP> owned (dylibs.begin (), dylibs.end ());

	return jit_->getExecutionSession ().removeJITDylibs (std::move (owned));
}

Error
MonoJit::undefine_stubs (const std::vector<std::string> &names)
{
	if (names.empty ())
		return Error::success ();

	ExecutionSession &es = jit_->getExecutionSession ();
	StubTable::Removed removed;

	/*
	 * Out of the table first: nothing can reach these stubs by name once they
	 * are gone from it, and no later link can ask for a definition of one. The
	 * lock is what makes "what the linker knows" a settled question - without
	 * it a name can be claimed by a link that has not defined it yet.
	 */
	{
		std::lock_guard<std::mutex> lock (stub_defs_mutex_);
		Expected<StubTable::Removed> taken = stub_table_->remove (names);

		if (!taken)
			return taken.takeError ();
		removed = std::move (*taken);
	}

	SymbolNameSet symbols;

	for (const std::string &name : removed.defined)
		symbols.insert (es.intern (name));

	/*
	 * Undefine before reclaiming: a stub has to be unreachable by address as
	 * well as by name before its block can be handed to the next method along.
	 */
	if (!symbols.empty ()) {
		/*
		 * ORC refuses to remove a symbol that is materializing, and one a link
		 * has just been handed is exactly that until that link's query
		 * finishes. Waiting for it is a lookup, so it happens with no lock
		 * held: a lookup drains materialization inline and what it drains may
		 * be a link that wants the generator.
		 */
		Expected<SymbolMap> settled = es.lookup (
			makeJITDylibSearchOrder (stubs_, JITDylibLookupFlags::MatchAllSymbols),
			SymbolLookupSet (symbols), LookupKind::Static, SymbolState::Ready);

		if (!settled)
			return settled.takeError ();

		if (Error err = stubs_->remove (symbols))
			return err;
	}

	stub_table_->reclaim (std::move (removed));
	for (const std::string &name : names)
		callbacks_->release (name);

	/*
	 * A name stays in the session's pool until somebody asks for the dead
	 * entries back. The sweep walks the whole pool, so it runs once a batch's
	 * worth of names have gone dead rather than once per method; the cost of
	 * that is a backlog of at most that many. Dropping the set first is what
	 * puts this batch's own names in the sweep rather than the next one's.
	 */
	size_t dropped = symbols.size ();

	symbols.clear ();

	if (dropped != 0
	    && dropped_names_.fetch_add (dropped) + dropped >= dead_name_sweep) {
		dropped_names_.store (0);
		es.getSymbolStringPool ()->clearDeadEntries ();
	}

	return Error::success ();
}

} // namespace mono
