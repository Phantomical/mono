/**
 * \file
 * \brief Building the LLJIT stack, compiling modules into it, and reading back
 * where the code landed.
 */

#include "jit.hpp"

#include "arch/arch.hpp"
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

namespace {

/// Codegen sets this when it hands its object over, and the link clears it when
/// it picks the object up. In between, the ELF bytes turn into a LinkGraph, and
/// neither side has a hook inside that. It stays zero unless fine timing is on.
thread_local uint64_t g_object_handed = 0;

/// Whether MONO_LLVM_JIT_HOIST, a comma-separated list, names the given
/// measurement arm.
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

/// Print what the verifier rejected - the module, the point in the pipeline,
/// and the diagnostics - then abort.
///
/// The verifier's own diagnostics name only the instruction, which does not say
/// which method or which pass.
[[noreturn]] void
report_broken_ir (const Module &m, StringRef when, StringRef diagnostics)
{
	errs () << "mono: broken IR " << when << ", in " << m.getModuleIdentifier () << "\n"
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
	return pass == ArrayAddressPass::name () || pass == ClassInitPass::name ()
	       || pass == LowerBuiltinsPass::name () || pass == RestoreTailPositionPass::name ()
	       || pass == arch::LegacyAbiPass::name ();
}

} // namespace

/*
 * Turns the linked `.mono_lines` into per-function rows. Each row gives the IL
 * offset in effect at an offset into a function. Sequence-point markers ride the
 * same channel and go to seq_points. The functions map names each function by
 * where it was linked, which is how a block header identifies itself.
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

			// Line 0 means the translator never attributed the instruction.
			if (line == 0)
				continue;

			IlLineRow row;
			row.native_offset = read_le<uint32_t> (r);
			row.il_offset = line - IL_OFFSET_LINE_BIAS;

			// A marker names where the debugger's trampolines return
			// into, so it must not enter the offset map. The offset in
			// effect there is the row before it.
			if (seq_point_is_marker (row.il_offset)) {
				row.flags = seq_point_marker_flags (row.il_offset);
				row.il_offset = seq_point_marker_offset (row.il_offset);
				if (points == nullptr)
					points = &seq_points[owner->second];
				points->push_back (row);
				continue;
			}

			if (!rows->empty () && rows->back ().native_offset == row.native_offset)
				rows->back () = row;
			else
				rows->push_back (row);
		}

		if (rows->empty ())
			out.erase (owner->second);
	}

	// The printer walks the blocks, so the rows are not ascending by address.
	// The runtime binary-searches these, so sort them - stably, to keep the
	// last row on an offset last.
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
 * The read is off the object rather than the linked graph because nothing here
 * needs relocating. A slot is a register number and a displacement, both settled
 * at codegen. The section holds a record per stackmap in the module, so the id
 * picks the marker out. A module carries at most one, since only a method's own
 * body has one.
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

		ArrayRef<uint8_t> bytes ((const uint8_t *) contents->data (), contents->size ());
		StackMapParser<llvm::endianness::little> parser (bytes);

		for (const auto &record : parser.records ()) {
			if (record.getID () != vars_stackmap_id)
				continue;

			for (const auto &location : record.locations ()) {
				// An alloca operand lowers to Direct - register plus
				// displacement is the slot's address. Anything else
				// means the operand was not the slot we named. A
				// partial list misattributes every variable after
				// it, so drop the method's variables entirely.
				if (location.getKind ()
				    != StackMapParser<
					    llvm::endianness::little>::LocationKind::Direct) {
					out.clear ();
					return;
				}

				out.push_back ({(int32_t) location.getDwarfRegNum (),
				                (int32_t) location.getOffset ()});
			}
			return;
		}
		return;
	}
}

/*
 * Whether the section holds code JITLink made up rather than code the compiler
 * emitted. Today that is the jump stubs it plants in "$__STUBS" when a call
 * cannot reach its target directly.
 *
 * The test is on the shape rather than the name. A synthesized section carries
 * no named symbol, because nothing in the object refers to a stub by name. If a
 * later LLVM renames the section, a name test leaves its code unregistered. A
 * stack walk stops dead in code the runtime cannot name.
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
 * Reads where the pieces the runtime needs landed in each linked object. That
 * is each defined function's extent, the four mono side-table sections, and the
 * sections the linker synthesized for itself. The entries are keyed by the name
 * of the dylib the object was linked into.
 *
 * The side-table sections carry no symbols and nothing refers to them, so the
 * pre-prune pass marks their blocks live. The read runs post-fixup, when the
 * addresses are final and readable in this process.
 */
class MonoJit::ObjectCapturePlugin : public ObjectLinkingLayer::Plugin {
public:
	struct Extents {
		std::vector<std::pair<std::string, std::pair<const uint8_t *, size_t>>> functions;
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
		/// The object as a debugger sees it, section addresses filled in.
		/// Empty unless gdbjit::enabled ().
		std::vector<char> debug_object;
	};

	/// The hook that sees the object bytes, which is where `.llvm_stackmaps`
	/// can be read without keeping anything else alive.
	///
	/// Upstream marks this deprecated and promises a proper mechanism for
	/// capturing object buffers. There is not one yet.
	void notifyMaterializing (MaterializationResponsibility &mr, jitlink::LinkGraph &,
	                          jitlink::JITLinkContext &, MemoryBufferRef input_object) override
	{
		timing::span_end (timing::Phase::lgraph, g_object_handed);
		g_object_handed = 0;

		// A debugger is handed the object rather than anything synthesized
		// from it. So the bytes have to survive until the link has decided
		// where everything goes and the addresses can be stamped in.
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

	void modifyPassConfig (MaterializationResponsibility &mr, jitlink::LinkGraph &g,
	                       jitlink::PassConfiguration &config) override
	{
		std::string dylib = mr.getTargetJITDylib ().getName ();

		if (timing::fine ()) {
			auto started = std::make_shared<uint64_t> (0);

			config.PrePrunePasses.push_back ([started] (jitlink::LinkGraph &) -> Error {
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
					graph.addAnonymousSymbol (*block, 0, block->getSize (),
					                          false,
					                          /*IsLive=*/true);
			}
			return Error::success ();
		});

		config.PostFixupPasses.push_back ([this,
		                                   dylib] (jitlink::LinkGraph &graph) -> Error {
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
					line_table = range.getStart ().toPtr<const uint8_t *> ();
					line_table_size = range.getSize ();
				} else if (is_linker_stub_section (section)) {
					extents.linker_stubs.emplace_back (
						range.getStart ().toPtr<const uint8_t *> (),
						range.getSize ());
				}
			}

			std::map<const uint8_t *, std::string> by_address;

			for (jitlink::Symbol *sym : graph.defined_symbols ()) {
				if (!sym->hasName () || !sym->isCallable ())
					continue;

				const uint8_t *code = sym->getAddress ().toPtr<const uint8_t *> ();

				extents.functions.emplace_back (
					std::string (*sym->getName ()),
					std::make_pair (code, (size_t) sym->getSize ()));
				by_address[code] = std::string (*sym->getName ());
			}

			if (line_table != nullptr)
				parse_line_table (line_table, line_table_size, by_address,
				                  extents.il_lines, extents.seq_points);

			if (gdbjit::enabled ())
				extents.debug_object = stamp_debug_object (dylib, graph);

			std::lock_guard<std::mutex> lock (mutex_);
			captured_[dylib] = std::move (extents);
			return Error::success ();
		});
	}

	/// The extents captured for a dylib's one object, surrendered to the caller.
	std::optional<Extents> take (StringRef dylib)
	{
		std::lock_guard<std::mutex> lock (mutex_);
		auto it = captured_.find (std::string (dylib));

		if (it == captured_.end ())
			return std::nullopt;

		Extents extents = std::move (it->second);
		captured_.erase (it);

		// notifyMaterializing () captured these before the link, so merge them
		// in here rather than writing them into the same slot.
		if (auto slots = var_slots_.find (std::string (dylib));
		    slots != var_slots_.end ()) {
			extents.var_slots = std::move (slots->second);
			var_slots_.erase (slots);
		}

		return extents;
	}

	Error notifyFailed (MaterializationResponsibility &) override { return Error::success (); }
	Error notifyRemovingResources (JITDylib &, ResourceKey) override
	{
		return Error::success ();
	}
	void notifyTransferringResources (JITDylib &, ResourceKey, ResourceKey) override {}

private:
	/// The object this dylib's compile produced, with the graph's final section
	/// addresses written into it - what a debugger is handed. Empty if the
	/// object bytes were never captured.
	std::vector<char> stamp_debug_object (const std::string &dylib, jitlink::LinkGraph &graph)
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

	std::vector<const char *> argv{"mono"};
	for (const std::string &opt : g_options)
		argv.push_back (opt.c_str ());

	if (!cl::ParseCommandLineOptions ((int) argv.size (), argv.data (), "", &errs ()))
		return createStringError (inconvertibleErrorCode (),
		                          "llvm rejected an option given with --llvm-opt");
	return Error::success ();
}

/*
 * The host target configuration every compile uses, detected once.
 *
 * Code model Small with Reloc::PIC_ rather than the JIT default, Large.
 * JITLink stubs a call it cannot reach, but a method reaching its own data has
 * no such fallback. The slab allocator is what keeps every reference inside
 * PCRel32 range - see codemem.hpp.
 *
 * CodeGenOptLevel::None is the tier-0 choice on purpose. It selects FastISel,
 * which is the cheap instruction selection this tier wants. The easy wins come
 * from the O1 IR pipeline, not from the optimizing selector. FastISel falls
 * back to SelectionDAG per block for constructs it does not cover, which costs
 * compile time and never correctness.
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

		// If codegen reaches an LLVM `unreachable`, a `ud2` beats falling
		// through into whatever bytes come next.
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
 * One per compile thread, because building one is far from free. The X86
 * subtarget resolves a ~200-entry feature string against the implication graph,
 * then builds every lowering and legalizer table behind it. For a method the
 * size the translator emits that costs more than compiling it.
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

/// The tier-0 IR pipeline and everything it is built out of, kept per thread.
///
/// None of it depends on the module it runs over, and standing it up costs a
/// couple of percent of a small method's compile. A compile thread builds it
/// once and reuses it. The caller must empty the analysis managers after each
/// run, because their results are keyed by IR the module takes with it.
struct Tier0Pipeline {
	PassInstrumentationCallbacks pic;
	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cgam;
	ModuleAnalysisManager mam;
	std::unique_ptr<PassBuilder> pb;
	ModulePassManager mpm;

	Tier0Pipeline ();

	/// Drop every cached analysis.
	///
	/// Must run while the module the results were computed over still stands.
	/// A cached MemorySSA holds references into that IR, and its destructor
	/// walks them.
	void forget_analyses ();
};

Tier0Pipeline::Tier0Pipeline ()
{
	pic.registerAfterPassCallback ([] (StringRef pass, Any, const PreservedAnalyses &) {
		if (g_verify_module == nullptr)
			return;
		if (g_verify_level == VerifyLevel::each || is_mono_pass (pass))
			verify_or_die (*g_verify_module, ("after pass \"" + pass + "\"").str ());
	});

	// A TargetMachine, so the cost-model-driven parts of the pipeline have a
	// real TargetTransformInfo to ask.
	pb = std::make_unique<PassBuilder> (&host_target_machine (), PipelineTuningOptions (),
	                                    std::nullopt, &pic);
	pb->registerModuleAnalyses (mam);
	pb->registerCGSCCAnalyses (cgam);
	pb->registerFunctionAnalyses (fam);
	pb->registerLoopAnalyses (lam);
	pb->crossRegisterProxies (lam, fam, cgam, mam);

	// Before the pipeline, so the optimizer sees the element arithmetic and
	// never sees a builtin.
	mpm.addPass (ArrayAddressPass ());
	mpm.addPass (LowerBuiltinsPass ());

	// Here, so that a check for a class an earlier check already covers costs
	// the rest of the pipeline nothing. It runs again after the pipeline,
	// because unrolling and jump threading copy whatever survived. A loop the
	// unroller straightens out ends up with one check per copied body, and the
	// second run drops all but the first.
	mpm.addPass (createModuleToFunctionPassAdaptor (ClassInitPass ()));

	// The function simplification pipeline rather than the whole O1 module
	// pipeline. A module here is a single method, so the module and CGSCC
	// layers have nothing to work on. There is no internal function to
	// specialize, and every call the translator emits leaves the module by
	// symbol. Running them anyway costs a large fraction of tier-0 compile
	// time.
	FunctionPassManager fpm = pb->buildFunctionSimplificationPipeline (
		OptimizationLevel::O1, ThinOrFullLTOPhase::None);

	fpm.addPass (ClassInitPass ());

	// At O1 the stock function simplification pipeline does not run this pass,
	// and it is load-bearing. It marks the entry thunk's call to the method
	// body as a tail call, which lets the thunk leave no frame behind. Without
	// it, every method entered through its thunk shows up twice in a stack
	// trace.
	fpm.addPass (TailCallElimPass ());

	// Last, because what it repairs is the pipeline's own doing.
	fpm.addPass (RestoreTailPositionPass ());
	mpm.addPass (createModuleToFunctionPassAdaptor (std::move (fpm)));

	// After the pipeline, so the lowering works over natural-typed calls and
	// only what survives reaches the legacy boundary convention.
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

	std::optional<timing::Scope> timed_setup (std::in_place, timing::Phase::pbsetup);
	Tier0Pipeline &pipeline = tier0_pipeline ();

	// The after-pass verifier is registered once with the pipeline, so which
	// module it looks at is handed over here rather than captured.
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
MonoJit::create (const std::shared_ptr<CodeSlabs> &slabs)
{
	ensure_native_target ();

	if (Error err = apply_options ())
		return std::move (err);

	LLJITBuilder builder;
	builder.setJITTargetMachineBuilder (host_target_machine_builder ());

	/*
	 * JITLink, not the RTDyldObjectLinkingLayer LLJIT still defaults to on
	 * ELF. LLJIT's generic platform setup attaches its eh-frame registration
	 * plugin to this layer on its own.
	 *
	 * The memory manager is built inside builder.create (), before there is a
	 * MonoJit to hold it, so the lambda captures the slabs instead.
	 */
	builder.setObjectLinkingLayerCreator (
		[slabs] (ExecutionSession &es) -> Expected<std::unique_ptr<ObjectLayer>> {
			return std::make_unique<ObjectLinkingLayer> (
				es, std::make_unique<SlabMemoryManager> (slabs));
		});

	builder.setCompileFunctionCreator (
		[] (JITTargetMachineBuilder jtmb)
			-> Expected<std::unique_ptr<IRCompileLayer::IRCompiler>> {
			return std::make_unique<MethodObjectCompiler> (std::move (jtmb));
		});

	auto jit = builder.create ();
	if (!jit)
		return jit.takeError ();

	std::unique_ptr<MonoJit> self (new MonoJit (std::move (*jit)));

	// Taking delivery of the module here is what puts its destruction, and its
	// LLVMContext with it, inside a phase. It is also the moment the object
	// exists and the link has not started.
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

	// The transform layer sits under LLJIT's init-helper layer, so the pipeline
	// runs after any platform rewriting and immediately before codegen.
	self->jit_->getIRTransformLayer ().setTransform (
		[] (ThreadSafeModule tsm,
	            MaterializationResponsibility &) -> Expected<ThreadSafeModule> {
			tsm.withModuleDo ([] (Module &m) { run_tier0_pipeline (m); });
			return std::move (tsm);
		});

	ExecutionSession &es = self->jit_->getExecutionSession ();
	self->helpers_ = &es.createBareJITDylib ("mono.helpers");
	self->stubs_ = &es.createBareJITDylib ("mono.stubs");

	return std::move (self);
}

MonoJit::MonoJit (std::unique_ptr<LLJIT> jit) : jit_ (std::move (jit)) {}

MonoJit::~MonoJit ()
{
	// The code these objects describe goes away with the LLJIT, so the debugger
	// has to be told first.
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
		// A name is a promise about what it stands for. Two addresses under one
		// name means a caller built a name that is not unique. Every later
		// module links against the first definition, so say so here rather than
		// emit code that reads the wrong object.
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
MonoJit::define_stubs (ArrayRef<std::pair<StringRef, void *>> stubs)
{
	if (stubs.empty ())
		return Error::success ();

	ExecutionSession &es = jit_->getExecutionSession ();
	SymbolMap symbols;

	for (const auto &[name, code] : stubs)
		symbols[es.intern (name)] = {
			ExecutorAddr::fromPtr (code),
			JITSymbolFlags::Exported | JITSymbolFlags::Callable,
		};

	return stubs_->define (absoluteSymbols (std::move (symbols)));
}

Expected<CompiledMethod>
MonoJit::compile (ThreadSafeModule tsm, StringRef entry)
{
	// An assertions-on LLVM refuses to codegen a module whose layout disagrees
	// with the target. A fresh module has no layout at all.
	tsm.withModuleDo ([&] (Module &m) {
		if (m.getDataLayout ().isDefault ())
			m.setDataLayout (jit_->getDataLayout ());
	});

	// A dylib per module, linked against mono.helpers and mono.stubs. It is
	// bare because these modules carry no initializers for the platform to
	// manage.
	std::string jd_name = ("jd." + Twine (module_counter_.fetch_add (1)) + "." + entry).str ();
	// MONO_LLVM_JIT_HOIST=sharedjd puts every module in one dylib instead, to
	// price making a fresh one. It is a measurement arm, not a candidate
	// implementation. The capture below is keyed by the dylib's name, so two
	// concurrent compiles take each other's object. No method can be freed on
	// its own.
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

	if (auto lines = extents->il_lines.find (entry.str ()); lines != extents->il_lines.end ()) {
		compiled.il_lines = std::move (lines->second);
		extents->il_lines.erase (lines);
	}

	for (auto &lines : extents->il_lines)
		compiled.other_il_lines.emplace_back (lines.first, std::move (lines.second));

	if (auto points = extents->seq_points.find (entry.str ());
	    points != extents->seq_points.end ())
		compiled.seq_points = std::move (points->second);

	compiled.var_slots = std::move (extents->var_slots);

	if (compiled.code == nullptr)
		return createStringError (inconvertibleErrorCode (),
		                          "the linked object for %s does not define it",
		                          entry.str ().c_str ());

	if (!extents->debug_object.empty ()) {
		gdbjit::Registration *reg = gdbjit::publish (std::move (extents->debug_object));

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

	// Before the removal: these objects describe code the dylibs are about to
	// release.
	retract_debug_objects (dylibs);

	std::vector<JITDylibSP> owned (dylibs.begin (), dylibs.end ());

	return jit_->getExecutionSession ().removeJITDylibs (std::move (owned));
}

Error
MonoJit::undefine_stubs (ArrayRef<std::string> names)
{
	if (names.empty ())
		return Error::success ();

	ExecutionSession &es = jit_->getExecutionSession ();
	SymbolNameSet symbols;

	for (const std::string &name : names)
		symbols.insert (es.intern (name));

	// A stub no module ever named has never been searched for, and remove ()
	// takes one of those as readily as one that has settled. Most of these go
	// in one step. It is all-or-nothing, so a failure leaves the dylib as it
	// was, and the retry below sees the same set.
	if (Error err = stubs_->remove (symbols)) {
		if (!err.isA<SymbolsCouldNotBeRemoved> ())
			return err;
		consumeError (std::move (err));

		// Something is linking against one of these right now. ORC refuses to
		// remove a symbol mid-materialization, so wait for the link that has
		// it. Waiting is a lookup, and a lookup drains materialization on this
		// thread, so it must happen with no lock of ours held.
		Expected<SymbolMap> settled = es.lookup (
			makeJITDylibSearchOrder (stubs_, JITDylibLookupFlags::MatchAllSymbols),
			SymbolLookupSet (symbols), LookupKind::Static, SymbolState::Ready);

		if (!settled)
			return settled.takeError ();

		if (Error again = stubs_->remove (symbols))
			return again;
	}

	// A name stays in the session's pool until somebody asks for the dead
	// entries back. The sweep walks the whole pool, so it runs once a batch's
	// worth of names have gone dead rather than once per method. The cost of
	// that is a backlog of at most that many. Dropping the set first is what
	// puts this batch's own names in the sweep rather than the next one's.
	size_t dropped = symbols.size ();

	symbols.clear ();

	if (dropped != 0 && dropped_names_.fetch_add (dropped) + dropped >= dead_name_sweep) {
		dropped_names_.store (0);
		es.getSymbolStringPool ()->clearDeadEntries ();
	}

	return Error::success ();
}

} // namespace mono
