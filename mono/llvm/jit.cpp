/**
 * \file
 * \brief Building the LLJIT stack, compiling modules into it, and reading back
 * where the code landed.
 */

#include "jit.hpp"

#include "arch/arch.hpp"
#include "compiler.hpp"
#include "jitlink-memory.hpp"
#include "gdb-jit.hpp"

#include "il-line-table.hpp"
#include "seq-point-marker.hpp"
#include "sidetables.hpp"
#include "passes/array-address.hpp"
#include "passes/class-init.hpp"
#include "passes/inline-copies.hpp"
#include "passes/lower-builtins.hpp"
#include "passes/profile-counters.hpp"
#include "passes/restore-tail-position.hpp"
#include "passes/tier-counter.hpp"
#include "passes/top-down-inline.hpp"
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
#include <llvm/ADT/Any.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/ProfileData/InstrProf.h>
#include <llvm/ProfileData/InstrProfWriter.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/Instrumentation/InstrProfiling.h>
#include <llvm/Transforms/Instrumentation/PGOInstrumentation.h>
#include <llvm/Transforms/Scalar/TailRecursionElimination.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
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

/// Checks one function, which is as far as a function pass can have broken
/// anything.
void
verify_or_die (const Function &f, StringRef when)
{
	std::string diagnostics;
	raw_string_ostream os (diagnostics);

	if (verifyFunction (f, &os))
		report_broken_ir (*f.getParent (), (when + " of " + f.getName ()).str (),
		                  diagnostics);
}

/// The passes this backend writes, by the name the pass instrumentation reports
/// them under. A break introduced by one of these is a bug here.
bool
is_mono_pass (StringRef pass)
{
	return pass == ArrayAddressPass::name () || pass == ClassInitPass::name ()
	       || pass == LowerBuiltinsPass::name () || pass == RestoreTailPositionPass::name ()
	       || pass == arch::MonoAbiPass::name ();
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
 * Turns the linked `.mono_inlines` into per-function rows: which bodies were
 * folded into the code each line-table row covers. The functions map names each
 * function by where it was linked, the same way the line table identifies a
 * block.
 */
static void
parse_inline_table (const uint8_t *table, size_t size,
                    const std::map<const uint8_t *, std::string> &functions,
                    std::map<std::string, std::vector<IlInlineRow>> &out)
{
	const uint8_t *p = table;
	const uint8_t *end = table + size;

	while (p + inlines_header_size <= end) {
		if (read_le<uint32_t> (p) != inlines_section_magic
		    || read_le<uint16_t> (p + 4) != inlines_section_version) {
			errs () << "mono: .mono_inlines is not in a format this runtime "
				   "knows\n";
			return;
		}

		uint32_t count = read_le<uint32_t> (p + 8);
		const uint8_t *code = (const uint8_t *) read_le<uint64_t> (p + 12);
		const uint8_t *records = p + inlines_header_size;

		p = records + (size_t) count * inlines_record_size;
		if (p > end)
			return;

		auto owner = functions.find (code);
		if (owner == functions.end ())
			continue;

		std::vector<IlInlineRow> *rows = &out[owner->second];

		for (uint32_t i = 0; i < count; ++i) {
			const uint8_t *r = records + (size_t) i * inlines_record_size;
			IlInlineRow row;

			row.native_offset = read_le<uint32_t> (r);
			row.il_offset = read_le<uint32_t> (r + 4);
			row.depth = read_le<uint32_t> (r + 8);
			row.callee = read_le<uint64_t> (r + 12);

			/*
			 * Two rows of the line table can land on one offset, and the
			 * reader there keeps the last. Both tables are written from the
			 * same rows, so this one has to collapse the same way: a chain
			 * whose row was dropped would outlive the offset it describes.
			 * A second chain on an offset opens with depth 0, which is what
			 * says the one already here belongs to a row that lost.
			 */
			if (row.depth == 0)
				while (!rows->empty ()
				       && rows->back ().native_offset == row.native_offset)
					rows->pop_back ();

			rows->push_back (row);
		}

		if (rows->empty ())
			out.erase (owner->second);
	}

	// The runtime binary-searches these by offset, and reads a chain off as the
	// run that follows. So depth has to ascend inside one offset.
	auto by_address = [] (const IlInlineRow &a, const IlInlineRow &b) {
		if (a.native_offset != b.native_offset)
			return a.native_offset < b.native_offset;
		return a.depth < b.depth;
	};

	for (auto &kv : out)
		std::stable_sort (kv.second.begin (), kv.second.end (), by_address);
}

/*
 * The `.llvm_stackmaps` v3 header is 16 bytes, and the function list follows it
 * as 24-byte records: the function's address, its frame size, and how many of
 * the section's stackmap records belong to it. StackMapParser does not give
 * these two offsets out, and the address field is where the relocation that
 * names the function sits.
 */
constexpr uint64_t stackmap_function_list_offset = 16;
constexpr uint64_t stackmap_function_size = 24;

/// What each relocation on a section points at, by the offset it applies at.
static std::map<uint64_t, std::string>
relocation_targets (const object::ObjectFile &obj, const object::SectionRef &section)
{
	std::map<uint64_t, std::string> targets;

	// An ELF object keeps a section's relocations in a section of their own, so
	// asking the section itself for them answers nothing. Find the one that
	// applies to it instead.
	for (const object::SectionRef &relocs : obj.sections ()) {
		Expected<object::section_iterator> applies_to = relocs.getRelocatedSection ();

		if (!applies_to) {
			consumeError (applies_to.takeError ());
			continue;
		}
		if (*applies_to == obj.section_end () || **applies_to != section)
			continue;

		for (const object::RelocationRef &reloc : relocs.relocations ()) {
			object::symbol_iterator symbol = reloc.getSymbol ();

			if (symbol == obj.symbol_end ())
				continue;

			Expected<StringRef> name = symbol->getName ();

			if (!name) {
				consumeError (name.takeError ());
				continue;
			}
			targets[reloc.getOffset ()] = name->str ();
		}
	}

	return targets;
}

/*
 * Where the translator's markers say each function's frame slots ended up: its
 * arguments and locals in the order it named them - arguments then locals - and
 * a shared body's receiver, which goes to rgctx.
 *
 * The read is off the object rather than the linked graph because no slot needs
 * relocating. A slot is a register number and a displacement, both settled at
 * codegen. The id picks the marker out of the finally markers that share the
 * section, and the function list says whose marker it is: the records are
 * grouped by function, and each group names its function through the relocation
 * on its address field.
 */
static void
parse_debug_var_slots (object::ObjectFile &obj,
                       std::map<std::string, std::vector<VarSlot>> &out,
                       std::map<std::string, VarSlot> &rgctx)
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

		// The parser asserts on anything it cannot read, and an assertions-on
		// LLVM is the configuration this backend is built against.
		if (Error bad = StackMapParser<llvm::endianness::little>::validateHeader (bytes)) {
			consumeError (std::move (bad));
			return;
		}

		std::map<uint64_t, std::string> functions = relocation_targets (obj, section);

		StackMapParser<llvm::endianness::little> parser (bytes);
		unsigned record = 0;

		for (unsigned f = 0, n = parser.getNumFunctions (); f < n; ++f) {
			unsigned count = (unsigned) parser.getFunction (f).getRecordCount ();
			unsigned first = record;

			record += count;

			auto owner = functions.find (stackmap_function_list_offset
			                             + f * stackmap_function_size);

			if (owner == functions.end ())
				continue;

			for (unsigned i = first; i < first + count; ++i) {
				auto marker = parser.getRecord (i);
				bool wanted_vars = marker.getID () == vars_stackmap_id;

				if (!wanted_vars && marker.getID () != rgctx_stackmap_id)
					continue;

				std::vector<VarSlot> slots;
				bool complete = true;

				for (const auto &location : marker.locations ()) {
					// An alloca operand lowers to Direct - register
					// plus displacement is the slot's address.
					// Anything else means the operand was not the
					// slot we named. A partial list misattributes
					// every variable after it, so drop this
					// function's variables entirely.
					if (location.getKind ()
					    != StackMapParser<llvm::endianness::little>::
						    LocationKind::Direct) {
						complete = false;
						break;
					}

					slots.push_back (
						{ (int32_t) location.getDwarfRegNum (),
					          (int32_t) location.getOffset () });
				}

				if (!complete || slots.empty ())
					continue;

				if (wanted_vars)
					out[owner->second] = std::move (slots);
				else
					rgctx[owner->second] = slots.front ();
			}
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
		/// The bodies folded into each defined function, by name.
		std::map<std::string, std::vector<IlInlineRow>> inline_frames;
		/// Where a function's arguments and locals live in its frame, by
		/// name. Only a method body pins them, so a filter or a thunk has
		/// no entry here.
		std::map<std::string, std::vector<VarSlot>> var_slots;
		/// Where a shared body's receiver lives in its frame, by name. A
		/// body that is not a shared one has no entry here.
		std::map<std::string, VarSlot> rgctx_slots;
		/// The `__llvm_prf_cnts` section: every instrumented function's
		/// counters, and how many fit. Null when the module was not
		/// instrumented. Only the bound is read from here - which function
		/// owns which part of it is what `__llvm_prf_data` says.
		const uint64_t *counters = nullptr;
		size_t counter_slots = 0;
		/// The `__llvm_prf_data` records, one per instrumented function.
		const uint8_t *profile_data = nullptr;
		size_t profile_data_size = 0;
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

		std::map<std::string, std::vector<VarSlot>> var_slots;
		std::map<std::string, VarSlot> rgctx_slots;
		timing::Scope timed (timing::Phase::vslots);

		Expected<std::unique_ptr<object::ObjectFile>> obj =
			object::ObjectFile::createObjectFile (input_object);

		if (!obj) {
			consumeError (obj.takeError ());
			return;
		}

		parse_debug_var_slots (**obj, var_slots, rgctx_slots);
		if (var_slots.empty () && rgctx_slots.empty ())
			return;

		std::lock_guard<std::mutex> lock (mutex_);
		var_slots_[mr.getTargetJITDylib ().getName ()] = { std::move (var_slots),
			                                           std::move (rgctx_slots) };
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
				StringRef name = section.getName ();

				// The profile records go with our own side tables: the code
				// refers to its counters, but to nothing that says whose
				// counters they are, so the pruner drops the records.
				if (name != ".mono_lsda" && name != ".mono_guards"
				    && name != ".mono_unwind" && name != ".mono_lines"
				    && name != ".mono_inlines" && name != "__llvm_prf_data")
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
			const uint8_t *inline_table = nullptr;
			size_t inline_table_size = 0;

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
				} else if (section.getName () == ".mono_inlines") {
					inline_table =
						range.getStart ().toPtr<const uint8_t *> ();
					inline_table_size = range.getSize ();
				} else if (section.getName () == "__llvm_prf_cnts") {
					extents.counters =
						range.getStart ().toPtr<const uint64_t *> ();
					extents.counter_slots =
						range.getSize () / sizeof (uint64_t);
				} else if (section.getName () == "__llvm_prf_data") {
					extents.profile_data =
						range.getStart ().toPtr<const uint8_t *> ();
					extents.profile_data_size = range.getSize ();
				} else if (is_linker_stub_section (section)) {
					extents.linker_stubs.emplace_back (
						range.getStart ().toPtr<const uint8_t *> (),
						range.getSize ());
				}
			}

			std::map<const uint8_t *, std::string> by_address;

			for (jitlink::Symbol *sym : graph.defined_symbols ()) {
				if (!sym->hasName ())
					continue;

				if (!sym->isCallable ())
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

			if (inline_table != nullptr)
				parse_inline_table (inline_table, inline_table_size, by_address,
				                    extents.inline_frames);

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
			extents.var_slots = std::move (slots->second.first);
			extents.rgctx_slots = std::move (slots->second.second);
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
	/// The frame slots read off each object before it was linked, by dylib:
	/// each function's arguments and locals, and each shared body's receiver.
	std::map<std::string, std::pair<std::map<std::string, std::vector<VarSlot>>,
	                                std::map<std::string, VarSlot>>>
		var_slots_;
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

// Read here rather than through runtime/options.hpp, which would put mono's
// headers in this file. It has to answer as that copy does: a tier-1 body the
// translator marked for a counter, in a pipeline that instruments nothing, keeps
// a mark no pass ever reads.
static bool
tier2_enabled ()
{
	static const bool on = [] {
		const char *value = ::getenv ("MONO_LLVM_JIT_TIER2");

		if (value == nullptr)
			return true;

		StringRef set (value);

		return !set.empty () && set != "0" && !set.equals_insensitive ("false");
	}();

	return on;
}

/*
 * The host target configuration every compile uses, detected once.
 *
 * Code model Small with Reloc::PIC_ rather than the JIT default, Large.
 * JITLink stubs a call it cannot reach, but a method reaching its own data has
 * no such fallback. Where the code lands is what keeps every reference inside
 * PCRel32 range - see jitlink-memory.cpp.
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

/// The name the in-memory profile is mounted under. PGOInstrumentationUse takes
/// a file name and a file system, so the profile has to be a file to it.
constexpr const char *profile_file = "/mono.profdata";

/// Gives one of LLVM's own command-line options the default this backend wants.
///
/// A setting `--llvm-opt` carried is left alone, so each of these stays
/// something a sweep can move. apply_options () parses that command line while
/// the first MonoJit is built, which is before any pipeline is built, so the
/// count below already covers it.
///
/// Does nothing when this build of LLVM has no option of that name. The type
/// has to be the one the option was declared with, which is not checked.
template <typename T>
void
default_option (StringRef name, T value)
{
	cl::Option *opt = cl::getRegisteredOptions ().lookup (name);

	if (opt != nullptr && opt->getNumOccurrences () == 0)
		static_cast<cl::opt<T> *> (opt)->setValue (value);
}

/// Gives one of LLVM's own command-line options the default this backend wants,
/// where the type it was declared with is private to the target that declares
/// it. The value is the text a command line carries, and the option's own parser
/// reads it, so a value that parser does not know gets a message on stderr and
/// the option keeps its default.
///
/// It leaves a setting `--llvm-opt` carried alone, the way default_option ()
/// does, and does nothing when this build of LLVM has no option of that name.
void
default_option_text (StringRef name, StringRef value)
{
	cl::Option *opt = cl::getRegisteredOptions ().lookup (name);

	if (opt != nullptr && opt->getNumOccurrences () == 0)
		(void) opt->addOccurrence (0, name, value);
}

/// Puts counters in each body that can promote, and the entry counter that
/// decides when it does.
void
add_instrumentation (ModulePassManager &mpm)
{
	InstrProfOptions counters;

	// Value profiling needs compiler-rt, which we do not link.
	default_option ("disable-vp", true);

	// Promotion keeps a counter inside a loop in a register, and adds it back
	// to the array at each exit from the loop. A hot loop then pays one atomic
	// add for the whole loop, not one for each turn of it.
	//
	// Atomic has to stay off for any of that to happen. With it set, the
	// lowering writes every increment as an atomicrmw. It records a promotion
	// candidate only for an increment it wrote as a load, an add and a store,
	// so promotion becomes dead code. ProfileAtomicPass below makes each
	// counter promotion did not take atomic again, so this setting is not what
	// keeps them safe.
	counters.DoCounterPromotion = true;
	counters.Atomic = false;

	// Makes the add at a loop exit an atomicrmw. Without it, the exit reads and
	// writes the counter in two steps. The promoter then offers that pair to
	// the loop outside, which hoists the write out of the whole nest. Tier 2
	// reads these counters while the code still runs, so a count written at
	// each turn of the outer loop is worth more than one held to the end.
	default_option ("atomic-counter-update-promoted", true);

	// LLVM otherwise refuses a loop that any exit leaves through a return. That
	// refusal keeps a profile read in the middle of a long loop from
	// under-reporting it. Almost every loop a C# method ends with has that
	// shape, so the refusal costs most of what promotion is worth here. A read
	// that comes early loses only the turns the threads now in the loop took.
	// Every entry that already left the loop is in the count, and entry count
	// is what takes a body to tier 2.
	default_option ("skip-ret-exit-block", false);

	// LLVM gives a loop with more exiting blocks than this nothing at all, at
	// three by default. A `for` with three early returns already has four, and
	// then the whole loop keeps its per-turn atomics. Eight admits the loops a
	// method is written with by hand.
	//
	// What a higher setting admits is a write-back on each further exit, and
	// those are exits that mostly do not run: one exit is taken per pass through
	// the loop whatever the count. So the code at the exits is what holds this
	// down, not the time in it, and a measurement of that code is what a move to
	// sixteen wants.
	default_option<unsigned> ("speculative-counter-promotion-max-exiting", 8);

	mpm.addPass (ProfileSelectPass ());
	mpm.addPass (PGOInstrumentationGen (PGOInstrumentationType::FDO));
	mpm.addPass (ProfileGatherPass ());

	// Promotion wants each loop to have a preheader and exits that only it
	// branches to, and the translator gives it neither. LLVM's own pipeline
	// canonicalizes at this same point, behind the hash. Tier 2 reads the
	// profile back against the CFG the hash was taken over, so it still
	// matches.
	mpm.addPass (createModuleToFunctionPassAdaptor (LoopSimplifyPass ()));

	mpm.addPass (InstrProfilingLoweringPass (counters));
	mpm.addPass (ProfileAtomicPass ());
	mpm.addPass (ProfileLocalizePass ());

	// Behind the instrumentation, never in front - see passes/tier-counter.hpp.
	mpm.addPass (TierCounterPass ());
}

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
	pic.registerAfterPassCallback ([] (StringRef pass, Any ir, const PreservedAnalyses &) {
		if (g_verify_module == nullptr)
			return;
		if (g_verify_level != VerifyLevel::each && !is_mono_pass (pass))
			return;

		std::string when = ("after pass \"" + pass + "\"").str ();

		// A function pass fires once for each function, so checking the whole
		// module every time costs a square in what the module holds. A batched
		// compile holds one function per method in it.
		if (const Function *const *f = any_cast<const Function *> (&ir))
			verify_or_die (**f, when);
		else
			verify_or_die (*g_verify_module, when);
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

	if (tier2_enabled ())
		add_instrumentation (mpm);

	// Here, so that a check for a class an earlier check already covers costs
	// the rest of the pipeline nothing. It runs again after the pipeline,
	// because unrolling and jump threading copy whatever survived. A loop the
	// unroller straightens out ends up with one check per copied body, and the
	// second run drops all but the first.
	mpm.addPass (createModuleToFunctionPassAdaptor (ClassInitPass ()));

	// The front end translated a few callees in beside the body and marked each
	// one always-inline. This folds them in, and simplification below then sees
	// one body: the arguments are constants where the caller had them, and the
	// class-init check on a folded entry is dominated by the caller's.
	//
	// Behind the instrumentation, so the counters and the CFG hash beside them
	// describe the body with its calls still standing. Tier 2 reads the profile
	// back at the same point in its own pipeline. A hash taken over the folded
	// shape matches nothing there, and the tier-2 body is then laid out with no
	// weights at all.
	mpm.addPass (AlwaysInlinerPass ());

	// A copy the fold above did not take goes back to being a call through the
	// callee's thunk. Such a copy is entered by a direct call and has no jit
	// info of its own, so a stack walk over its frame finds nothing.
	mpm.addPass (StripInlineCopiesPass ());

	// The function simplification pipeline rather than the whole O1 module
	// pipeline. The only interprocedural work a module here has is the fold
	// above: what is left is the methods it came in with, and every call still
	// standing leaves the module by symbol. Running the module and CGSCC layers
	// anyway costs a large fraction of tier-1 compile time.
	FunctionPassManager fpm = pb->buildFunctionSimplificationPipeline (
		OptimizationLevel::O1, ThinOrFullLTOPhase::None);

	fpm.addPass (ClassInitPass ());

	// At O1 the stock function simplification pipeline does not run this pass.
	// It marks the entry thunk's call to the method body as a tail call, which
	// lets the thunk leave no frame behind. Without it, every method entered
	// through its thunk shows up twice in a stack trace.
	fpm.addPass (TailCallElimPass ());

	// Last, because what it repairs is the pipeline's own doing.
	fpm.addPass (RestoreTailPositionPass ());
	mpm.addPass (createModuleToFunctionPassAdaptor (std::move (fpm)));

	// After the pipeline, so the lowering works over natural-typed calls and
	// only what survives reaches the C convention.
	mpm.addPass (arch::MonoAbiPass ());
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
	// The machine first. Both are thread_local and are therefore destroyed in
	// reverse order of registration, and the pipeline's PassBuilder points at
	// the machine.
	host_target_machine ();

	// On the heap, because the object is ~1.9K. A thread_local that large puts
	// the runtime's TLS block over the surplus glibc keeps for dlopen'd
	// modules, and the mono_tls_* variables are initial-exec, so the whole
	// module has to fit in that surplus. An embedder that dlopens the runtime
	// gets "cannot allocate memory in static TLS block" and no runtime at all.
	static thread_local std::unique_ptr<Tier0Pipeline> pipeline;

	if (!pipeline)
		pipeline = std::make_unique<Tier0Pipeline> ();

	return *pipeline;
}

/// Mounts a profile as a file and hands back the pass that reads it.
///
/// The pass holds the file system, so it stays alive as long as the pass does.
PGOInstrumentationUse
profile_use_pass (ArrayRef<uint8_t> profile)
{
	IntrusiveRefCntPtr<vfs::InMemoryFileSystem> fs (new vfs::InMemoryFileSystem ());

	fs->addFile (profile_file, 0,
	             MemoryBuffer::getMemBufferCopy (
			     StringRef ((const char *) profile.data (), profile.size ()),
			     profile_file));

	return PGOInstrumentationUse (profile_file, "", /*IsCS=*/false, fs);
}

} // namespace

/*
 * The optimizing half of the host configuration, which is the only thing tier 2
 * changes about codegen. It is a second TargetMachine per compile thread rather
 * than a setting on the first, because the level is fixed when the machine is
 * built and the subtarget tables behind it cost more than a small method's
 * whole compile.
 */
TargetMachine &
tier2_target_machine ()
{
	static thread_local std::unique_ptr<TargetMachine> tm = [] {
		JITTargetMachineBuilder builder = host_target_machine_builder ();

		builder.setCodeGenOptLevel (CodeGenOptLevel::Aggressive);
		return cantFail (builder.createTargetMachine ());
	}();

	return *tm;
}

/*
 * Says where each instrumented function put its counters, from what the link
 * resolved in `__llvm_prf_data`.
 *
 * Records are matched to sites by the name the reader keys on, so neither side
 * depends on the order the other lists them in. A site is dropped rather than
 * guessed at when its record is missing, disagrees about how many counters the
 * function has, or points outside the counter section: a wrong array reads as
 * real weights at the next tier, while no array only costs that tier its input.
 */
std::vector<ProfileCounters>
locate_counters (ArrayRef<ProfileCounters> layout, const uint64_t *counters,
                 size_t counter_slots, const uint8_t *data, size_t data_size)
{
	std::vector<ProfileCounters> found;

	if (layout.empty () || counters == nullptr)
		return found;

	std::vector<ProfileArray> arrays = read_profile_arrays (data, data_size);
	const uint64_t *end = counters + counter_slots;

	for (const ProfileCounters &site : layout) {
		uint64_t key = profile_name_key (site.name);

		for (const ProfileArray &array : arrays) {
			if (array.name_key != key || array.hash != site.hash
			    || array.count != site.count)
				continue;
			if (array.counters < counters || array.counters + site.count > end)
				continue;

			found.push_back (site);
			found.back ().counters = array.counters;
			break;
		}
	}

	return found;
}

std::vector<uint8_t>
build_profile (ArrayRef<ProfileCounters> counters)
{
	InstrProfWriter writer;

	// What the reader checks before it looks at a single record. A profile that
	// does not say IR-level is one it refuses whole.
	if (Error err = writer.mergeProfileKind (InstrProfKind::IRInstrumentation)) {
		consumeError (std::move (err));
		return {};
	}

	for (const ProfileCounters &fn : counters) {
		std::vector<uint64_t> counts (fn.counters, fn.counters + fn.count);

		writer.addRecord (NamedInstrProfRecord (fn.name, fn.hash, std::move (counts)),
		                  [] (Error err) { consumeError (std::move (err)); });
	}

	std::unique_ptr<MemoryBuffer> written = writer.writeBuffer ();

	if (!written)
		return {};

	return std::vector<uint8_t> (written->getBufferStart (), written->getBufferEnd ());
}

void
apply_profile (Module &m, ArrayRef<uint8_t> profile)
{
	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cgam;
	ModuleAnalysisManager mam;
	PassBuilder pb;

	pb.registerModuleAnalyses (mam);
	pb.registerCGSCCAnalyses (cgam);
	pb.registerFunctionAnalyses (fam);
	pb.registerLoopAnalyses (lam);
	pb.crossRegisterProxies (lam, fam, cgam, mam);

	ModulePassManager mpm;

	mpm.addPass (ProfileSelectPass ());
	mpm.addPass (profile_use_pass (profile));
	mpm.run (m, mam);

	mam.clear ();
	cgam.clear ();
	fam.clear ();
	lam.clear ();
}

void
MonoJit::run_tier2_pipeline (Module &m, ArrayRef<uint8_t> profile,
                             InlineCandidates *inliner)
{
	timing::Scope timed (timing::Phase::pipeline);
	VerifyLevel verify = verify_level ();

	if (verify != VerifyLevel::off)
		verify_or_die (m, "as translated");

	// Codegen reads this to pick the optimizing target machine.
	m.addModuleFlag (Module::Error, "mono.tier2", 1);

	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cgam;
	ModuleAnalysisManager mam;
	PassBuilder pb (&tier2_target_machine ());

	pb.registerModuleAnalyses (mam);
	pb.registerCGSCCAnalyses (cgam);
	pb.registerFunctionAnalyses (fam);
	pb.registerLoopAnalyses (lam);
	pb.crossRegisterProxies (lam, fam, cgam, mam);

	ModulePassManager mpm;

	mpm.addPass (ArrayAddressPass ());
	mpm.addPass (LowerBuiltinsPass ());

	mpm.addPass (ProfileSelectPass ());

	if (!profile.empty ()) {
		mpm.addPass (profile_use_pass (profile));

		// The summary the weights are read against. Nothing downstream builds
		// it, and without it every hot/cold question answers the same way.
		mpm.addPass (RequireAnalysisPass<ProfileSummaryAnalysis, Module> ());
	}

	mpm.addPass (createModuleToFunctionPassAdaptor (ClassInitPass ()));

	// The front end translated a few callees in beside the body and marked each
	// one always-inline. This folds them in, and simplification below then sees
	// one body: the arguments are constants where the caller had them, and the
	// class-init check on a folded entry is dominated by the caller's.
	mpm.addPass (AlwaysInlinerPass ());

	mpm.addPass (createModuleToFunctionPassAdaptor (
		pb.buildFunctionSimplificationPipeline (OptimizationLevel::O3,
	                                                ThinOrFullLTOPhase::None)));

	/*
	 * Behind simplification, and that is what a cost model needs: freshly
	 * translated managed IR is a null check on every dereference and a bounds
	 * check on every element, and a threshold read against it is spent before
	 * any of the real work is costed.
	 */
	/*
	 * What a candidate is put through before the cost model reads it, and what
	 * a root the loop folded into is put through after. Both are built here so
	 * the pass runs the pipeline this tier settled on rather than one of its
	 * own, and both have to outlive mpm.run () below.
	 *
	 * The candidate pipeline is a module one because it runs over a module of
	 * the inliner's own, holding the candidate and the trivial callees it
	 * brought with it.
	 */
	ModulePassManager materialize;

	materialize.addPass (ArrayAddressPass ());
	materialize.addPass (LowerBuiltinsPass ());
	materialize.addPass (AlwaysInlinerPass ());
	materialize.addPass (createModuleToFunctionPassAdaptor (
		pb.buildFunctionSimplificationPipeline (OptimizationLevel::O3,
	                                                ThinOrFullLTOPhase::None)));

	FunctionPassManager simplify = pb.buildFunctionSimplificationPipeline (
		OptimizationLevel::O3, ThinOrFullLTOPhase::None);

	if (inliner != nullptr)
		mpm.addPass (TopDownInlinerPass (*inliner, tier2_target_machine (),
		                                 materialize, simplify));

	// A copy neither inliner folded in goes back to being a call through the
	// callee's thunk, which is where it was before the body was asked for.
	mpm.addPass (StripInlineCopiesPass ());

	FunctionPassManager fpm;

	fpm.addPass (ClassInitPass ());
	fpm.addPass (TailCallElimPass ());
	fpm.addPass (RestoreTailPositionPass ());
	mpm.addPass (createModuleToFunctionPassAdaptor (std::move (fpm)));

	mpm.addPass (arch::MonoAbiPass ());

	mpm.run (m, mam);

	mam.clear ();
	cgam.clear ();
	fam.clear ();
	lam.clear ();

	if (verify != VerifyLevel::off)
		verify_or_die (m, "after the tier-2 pipeline");
}

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
MonoJit::create (CodeArena *arena)
{
	ensure_native_target ();

	if (Error err = apply_options ())
		return std::move (err);

	/*
	 * Intel syntax for what MONO_LLVM_JIT_ASM prints, which is the syntax the
	 * Intel manuals and a debugger's disassembly here use. The option reaches
	 * the MCAsmInfo, which the instruction printer, the `.intel_syntax noprefix`
	 * directive and register printing all read, so the whole dump agrees.
	 *
	 * It has to run before the first TargetMachine, because MCAsmInfo reads the
	 * option in its constructor and every TargetMachine builds one. The object a
	 * compile publishes is not affected: an assembler dialect is a property of
	 * printed text.
	 */
	default_option_text ("x86-asm-syntax", "intel");

	LLJITBuilder builder;
	builder.setJITTargetMachineBuilder (host_target_machine_builder ());

	/*
	 * JITLink, not the RTDyldObjectLinkingLayer LLJIT still defaults to on
	 * ELF. LLJIT's generic platform setup attaches its eh-frame registration
	 * plugin to this layer on its own.
	 *
	 * The memory manager is built inside builder.create (), before there is a
	 * MonoJit to hold it, so the lambda carries what it needs instead.
	 */
	builder.setObjectLinkingLayerCreator (
		[arena] (ExecutionSession &es) -> Expected<std::unique_ptr<ObjectLayer>> {
			return std::make_unique<ObjectLinkingLayer> (
				es, std::make_unique<CodeMemoryManager> (arena));
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

	ExecutionSession &es = self->jit_->getExecutionSession ();
	self->helpers_ = &es.createBareJITDylib ("mono.helpers");

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

std::vector<ProfileCounters>
MonoJit::optimize (Module &m, JitTier tier, ArrayRef<uint8_t> profile,
                   InlineCandidates *inliner)
{
	if (tier == JitTier::tier2) {
		run_tier2_pipeline (m, profile, inliner);
		return {};
	}

	// Emptied first: the passes append, and this thread's last compile left its
	// own sites behind.
	profile_sites ().clear ();
	run_tier0_pipeline (m);

	std::vector<ProfileCounters> layout;

	// One per body that can promote, which is one per method the module holds.
	for (const ProfileSite &site : profile_sites ())
		layout.push_back (ProfileCounters { site.function, site.name, site.hash,
		                                    nullptr, site.counters });

	return layout;
}

/*
 * Whether the object's function belongs to the method entry names. The
 * translator gives a method's side bodies the method's own name and a `$`
 * suffix, so the name is what says which method of a batch a filter body came
 * in with.
 */
static bool
belongs_to (StringRef entry, StringRef function)
{
	return function == entry
	       || (function.starts_with (entry) && function.drop_front (entry.size ())
	                                                   .starts_with ("$"));
}

Expected<CompiledMethod>
MonoJit::compile (ThreadSafeModule tsm, StringRef entry,
                  ArrayRef<std::pair<StringRef, void *>> module_symbols,
                  ArrayRef<ProfileCounters> layout)
{
	Expected<std::vector<CompiledMethod>> compiled =
		compile_batch (std::move (tsm), entry, module_symbols, layout);

	if (!compiled)
		return compiled.takeError ();
	return std::move (compiled->front ());
}

Expected<std::vector<CompiledMethod>>
MonoJit::compile_batch (ThreadSafeModule tsm, ArrayRef<StringRef> entries,
                        ArrayRef<std::pair<StringRef, void *>> module_symbols,
                        ArrayRef<ProfileCounters> layout)
{
	if (entries.empty ())
		return createStringError (inconvertibleErrorCode (),
		                          "a compile was asked for with no entry points");

	// An assertions-on LLVM refuses to codegen a module whose layout disagrees
	// with the target. A fresh module has no layout at all.
	tsm.withModuleDo ([&] (Module &m) {
		if (m.getDataLayout ().isDefault ())
			m.setDataLayout (jit_->getDataLayout ());
	});

	// A dylib per module, linked against mono.helpers. It is bare because
	// these modules carry no initializers for the platform to manage.
	std::string jd_name =
		("jd." + Twine (module_counter_.fetch_add (1)) + "." + entries.front ()).str ();

	JITDylib &jd = [&] () -> JITDylib & {
		timing::Scope timed (timing::Phase::dylib);

		JITDylib &made = jit_->getExecutionSession ().createBareJITDylib (jd_name);

		made.addToLinkOrder (*helpers_);
		return made;
	}();

	// The caller's own resolved callee addresses, defined directly into this
	// module's dylib rather than a table shared across compiles - nothing but
	// this one link ever asks for these names again.
	if (!module_symbols.empty ()) {
		ExecutionSession &es = jit_->getExecutionSession ();
		SymbolMap symbols;

		for (const auto &[name, addr] : module_symbols)
			symbols[es.intern (name)] = {
				ExecutorAddr::fromPtr (addr),
				JITSymbolFlags::Exported | JITSymbolFlags::Callable,
			};

		if (Error err = jd.define (absoluteSymbols (std::move (symbols))))
			return std::move (err);
	}

	{
		timing::Scope timed (timing::Phase::addir);

		if (Error err = jit_->addIRModule (jd, std::move (tsm)))
			return std::move (err);
	}

	// One lookup for the whole batch: each one takes the session lock, and the
	// first is what materializes the module anyway.
	SymbolLookupSet wanted;

	for (StringRef entry : entries)
		wanted.add (jit_->mangleAndIntern (entry));

	Expected<SymbolMap> found = jit_->getExecutionSession ().lookup (
		makeJITDylibSearchOrder (&jd), std::move (wanted));

	if (!found)
		return found.takeError ();

	std::optional<ObjectCapturePlugin::Extents> extents = capture_->take (jd_name);
	if (!extents)
		return createStringError (inconvertibleErrorCode (),
		                          "no object was captured while compiling %s",
		                          entries.front ().str ().c_str ());

	std::vector<ProfileCounters> profiles =
		locate_counters (layout, extents->counters, extents->counter_slots,
		                 extents->profile_data, extents->profile_data_size);
	std::vector<CompiledMethod> results;

	for (StringRef entry : entries) {
		CompiledMethod compiled;

		compiled.entry = (*found)[jit_->mangleAndIntern (entry)].getAddress ().toPtr<void *> ();
		compiled.dylib = &jd;
		compiled.clause_table = extents->clause_table;
		compiled.clause_table_size = extents->clause_table_size;
		compiled.guard_table = extents->guard_table;
		compiled.guard_table_size = extents->guard_table_size;
		compiled.unwind_table = extents->unwind_table;
		compiled.unwind_table_size = extents->unwind_table_size;

		for (const auto &[name, extent] : extents->functions) {
			if (!belongs_to (entry, name))
				continue;
			if (name == entry) {
				compiled.code = extent.first;
				compiled.code_size = extent.second;
			}
			compiled.functions.emplace_back (name, extent);
		}

		if (compiled.code == nullptr)
			return createStringError (inconvertibleErrorCode (),
			                          "the linked object for %s does not define it",
			                          entry.str ().c_str ());

		for (auto &[name, rows] : extents->il_lines) {
			if (name == entry)
				compiled.il_lines = std::move (rows);
			else if (belongs_to (entry, name))
				compiled.other_il_lines.emplace_back (name, std::move (rows));
		}

		for (auto &[name, rows] : extents->inline_frames) {
			if (name == entry)
				compiled.inline_frames = std::move (rows);
			else if (belongs_to (entry, name))
				compiled.other_inline_frames.emplace_back (name,
				                                           std::move (rows));
		}

		if (auto points = extents->seq_points.find (entry.str ());
		    points != extents->seq_points.end ())
			compiled.seq_points = std::move (points->second);

		if (auto slots = extents->var_slots.find (entry.str ());
		    slots != extents->var_slots.end ())
			compiled.var_slots = std::move (slots->second);

		if (auto rgctx = extents->rgctx_slots.find (entry.str ());
		    rgctx != extents->rgctx_slots.end ())
			compiled.rgctx_slot = rgctx->second;

		for (ProfileCounters &counters : profiles) {
			if (counters.function == entry)
				compiled.profile = std::move (counters);
			else if (belongs_to (entry, counters.function))
				compiled.other_profiles.push_back (std::move (counters));
		}

		results.push_back (std::move (compiled));
	}

	// The stubs belong to the object rather than to any one method in it, and
	// a perf map that names a range twice cannot symbolize it. So the first
	// method carries them.
	results.front ().linker_stubs = std::move (extents->linker_stubs);

	if (!extents->debug_object.empty ()) {
		gdbjit::Registration *reg = gdbjit::publish (std::move (extents->debug_object));

		if (reg != nullptr) {
			std::lock_guard<std::mutex> lock (gdb_objects_mutex_);

			gdb_objects_[&jd].push_back (reg);
		}
	}

	return results;
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

} // namespace mono
