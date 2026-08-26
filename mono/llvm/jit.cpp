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
#include "pipelines.hpp"

#include "il-line-table.hpp"
#include "seq-point-marker.hpp"
#include "sidetables.hpp"
#include "passes/array-address.hpp"
#include "passes/array-shape.hpp"
#include "passes/class-init.hpp"
#include "passes/lower-builtins.hpp"
#include "passes/profile-counters.hpp"
#include "passes/restore-tail-position.hpp"
#include "passes/top-down-inline.hpp"
#include "timing.hpp"

#include <llvm/CodeGen/TargetLowering.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/StackMapParser.h>
#include <llvm/ADT/Any.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/ProfileData/InstrProf.h>
#include <llvm/ProfileData/InstrProfWriter.h>
#include <llvm/Support/CommandLine.h>
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
	off,
	/// The translator's output, the module after each pass is_mono_pass ()
	/// names, and the module codegen is handed.
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

/// The passes VerifyLevel::mono checks after, by the name the pass
/// instrumentation reports them under. A new pass added elsewhere in this
/// backend is not checked here until its name is added too.
bool
is_mono_pass (StringRef pass)
{
	return pass == ArrayAddressPass::name () || pass == ArrayShapePass::name ()
	       || pass == ClassInitPass::name ()
	       || pass == LowerBuiltinsPass::name ()
	       || pass == RestoreTailPositionPass::name ()
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

/// Whether a tier-1 body gathers counts for tier 2 to read.
///
/// Off, a body still counts its entries and still asks for tier 2, and the
/// tier-2 compile then reads a profile with no records in it and lays the
/// method out on static frequencies. That prices the counters on their own,
/// which MONO_LLVM_JIT_TIER2 cannot: it takes the counters and the tier away
/// together.
static bool
tier1_profiling_enabled ()
{
	static const bool on = [] {
		const char *value = ::getenv ("MONO_LLVM_JIT_TIER1_PGO");

		if (value == nullptr)
			return true;

		StringRef set (value);

		return !set.empty () && set != "0" && !set.equals_insensitive ("false");
	}();

	return on;
}

uint64_t
profile_entry_count ()
{
	static const uint64_t entry = [] () -> uint64_t {
		const char *value = ::getenv ("MONO_LLVM_JIT_PROFILE_ENTRY");

		if (value == nullptr)
			return 0;

		uint64_t set = 0;

		return StringRef (value).getAsInteger (10, set) ? 0 : set;
	}();

	return entry;
}

/*
 * The host target configuration every compile uses, detected once.
 *
 * Code model Small with Reloc::PIC_ rather than the JIT default, Large.
 * JITLink stubs a call it cannot reach, but a method reaching its own data has
 * no such fallback. Where the code lands is what keeps every reference inside
 * PCRel32 range - see jitlink-memory.cpp.
 *
 * CodeGenOptLevel::None is the tier-1 choice on purpose. It selects FastISel,
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

bool
ir_printing_enabled ()
{
	/*
	 * The instrumentations are registered whatever this answers, so an option
	 * missing from this list still prints. What it costs is the worker count
	 * that reads this: that option's output then comes out of as many threads
	 * as are compiling.
	 *
	 * Each name below is an option LLVM declares, in PrintPasses.cpp,
	 * StandardInstrumentations.cpp or DiagnosticHandler.cpp.
	 *
	 * Read from the queue rather than from the parsed options, because the
	 * option queue fills before the first MonoJit hands it to LLVM's parser.
	 */
	static const char *const printers[] = {
		"print-before",
		"print-after",
		"print-before-all",
		"print-after-all",
		"print-before-changed",
		"print-changed",
		"print-pass-numbers",
		"print-before-pass-number",
		"print-after-pass-number",
		"print-on-crash",
		"print-on-crash-path",
		"ir-dump-directory",
		"pass-remarks",
		"pass-remarks-missed",
		"pass-remarks-analysis",
	};

	std::lock_guard<std::mutex> lock (g_options_mutex);

	for (const std::string &opt : g_options) {
		StringRef name = StringRef (opt).ltrim ('-').split ('=').first;

		for (const char *printer : printers)
			if (name == printer)
				return true;
	}

	return false;
}

namespace {

thread_local Module *g_verify_module = nullptr;
thread_local VerifyLevel g_verify_level = VerifyLevel::off;

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

/// Both tiers' IR pipelines and everything they are built out of, kept per
/// thread.
///
/// None of it depends on the module a pipeline runs over, and standing one up
/// costs a couple of percent of a small method's compile. A compile thread
/// builds both once and reuses them. What one compile hands a pipeline goes in
/// the two slots below, and comes back out when the compile is done.
struct ThreadPipelines {
	/// The counts a tier-2 run reads, which the compile pushes and pops. See
	/// pushProfile ().
	IntrusiveRefCntPtr<OneFileFS> profile_fs = makeProfileFileSystem ();

	/// The engine the tier-2 inliner asks about the compile now running, read
	/// through InlineCandidatesAnalysis. Null outside a tier-2 run, which the
	/// inliner takes as leaving every site alone.
	InlineCandidates *inliner = nullptr;

	/*
	 * A context of this thread's own, which holds no IR.
	 * StandardInstrumentations takes one and keeps the reference, to read the
	 * gate `-opt-bisect-limit` sets. A compile's own context goes with the
	 * compile and the instrumentations outlive it, so they cannot have that
	 * one. The gate is process-wide, so a context with nothing in it answers
	 * for the gate as well as any other.
	 */
	LLVMContext instrument_context;

	/// One tier's pipeline and the analyses it runs against.
	struct Tier {
		// The callbacks and the instrumentations behind them are declared
		// first, so that they are destroyed after the managers and the
		// pipeline that read them.
		PassInstrumentationCallbacks pic;
		/// What makes LLVM's own `-print-after=<pass>` and the flags beside
		/// it print. Each of them is off until its option is given.
		StandardInstrumentations instrumentations;

		explicit Tier (LLVMContext &context)
		    // Verification is this backend's own, through
		    // MONO_LLVM_JIT_VERIFY, so the instrumentations are asked for
		    // none of theirs.
		    : instrumentations (context, /*DebugLogging=*/false,
		                        /*VerifyEach=*/false)
		{
		}

		LoopAnalysisManager lam;
		FunctionAnalysisManager fam;
		CGSCCAnalysisManager cgam;
		ModuleAnalysisManager mam;
		ModulePassManager mpm;

		/// Drop every cached analysis.
		///
		/// Must run while the module the results were computed over still
		/// stands. A cached MemorySSA holds references into that IR, and its
		/// destructor walks them.
		void forget_analyses ()
		{
			mam.clear ();
			cgam.clear ();
			fam.clear ();
			lam.clear ();
		}
	};

	/// The pipeline for a tier, built when the thread first compiles for it.
	///
	/// Lazily, because a tier costs a TargetMachine of its own to stand up and
	/// most threads never compile for both. A thread doing tier-1 work with
	/// tier 2 switched off would otherwise pay for a pipeline nothing runs.
	Tier &tier1 ();
	Tier &tier2 ();

private:
	/// Hangs the verifier and LLVM's own instrumentations off a tier's
	/// callbacks, which is what a pipeline runs them from.
	///
	/// Run it once for each tier. The two keep a set of callbacks each,
	/// because a callback registered twice on one set fires twice.
	void instrument (Tier &tier);

	std::optional<Tier> tier1_;
	std::optional<Tier> tier2_;
};

void
ThreadPipelines::instrument (Tier &tier)
{
	tier.pic.registerAfterPassCallback ([] (StringRef pass, Any ir, const PreservedAnalyses &) {
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

	// The analysis manager is what `-verify-analysis-invalidation` keeps its
	// record on, so the tier hands over its own.
	tier.instrumentations.registerCallbacks (tier.pic, &tier.mam);
}

/*
 * A builder is a builder and nothing else: what it returns owns what it needs,
 * and registering an analysis runs the lambda that builds it on the spot. So
 * neither builder below has to be kept. The target machine, the callbacks and
 * the file system do, and all three outlive this object.
 */
ThreadPipelines::Tier &
ThreadPipelines::tier1 ()
{
	if (tier1_)
		return *tier1_;

	Tier &tier = tier1_.emplace (instrument_context);
	MonoPipelineTuningOptions options = MonoPipelineTuningOptions::forTier1 ();

	/*
	 * With tier 2 off, a body neither counts its entries nor gathers a profile:
	 * nothing would ask for the next tier, and nothing would read the counts if
	 * it did. The mark the translator put on a body for its counter is then one
	 * no pass looks at.
	 */
	options.EnablePromotion = tier2_enabled ();
	options.EnablePGO = tier1_profiling_enabled ();

	MonoPassBuilder pb (&host_target_machine (), profile_fs.get (), &tier.pic, options);

	pb.registerModuleAnalyses (tier.mam);
	pb.registerCGSCCAnalyses (tier.cgam);
	pb.registerFunctionAnalyses (tier.fam);
	pb.registerLoopAnalyses (tier.lam);
	pb.crossRegisterProxies (tier.lam, tier.fam, tier.cgam, tier.mam);
	instrument (tier);

	tier.mpm = pb.buildTier1Pipeline ();
	return tier;
}

ThreadPipelines::Tier &
ThreadPipelines::tier2 ()
{
	if (tier2_)
		return *tier2_;

	Tier &tier = tier2_.emplace (instrument_context);
	MonoPassBuilder pb (&tier2_target_machine (), profile_fs.get (), &tier.pic,
	                    MonoPipelineTuningOptions::forTier2 ());

	pb.registerModuleAnalyses (tier.mam);
	pb.registerCGSCCAnalyses (tier.cgam);
	pb.registerFunctionAnalyses (tier.fam);
	pb.registerLoopAnalyses (tier.lam);
	pb.crossRegisterProxies (tier.lam, tier.fam, tier.cgam, tier.mam);
	instrument (tier);

	// Over the slot rather than over an engine, because the pipeline is built
	// once and the engine belongs to one compile.
	tier.mam.registerPass ([this] { return InlineCandidatesAnalysis (inliner); });

	tier.mpm = pb.buildTier2Pipeline ();
	return tier;
}

/// Hands a compile's inlining engine to the tier-2 pipeline, and takes it back.
///
/// The pipeline outlives the compile, so a pointer left behind is one the next
/// tier-2 run would ask questions of.
struct InlinerScope {
	ThreadPipelines &pipelines;

	InlinerScope (ThreadPipelines &pipelines, InlineCandidates *inliner)
	    : pipelines (pipelines)
	{
		pipelines.inliner = inliner;
	}

	~InlinerScope () { pipelines.inliner = nullptr; }
};

ThreadPipelines &
thread_pipelines ()
{
	// The machines first, both of them, even though a thread that compiles for
	// one tier only ever runs one pipeline. All of these are thread_local and
	// are therefore destroyed in reverse order of construction, so a machine
	// built on demand later than this would go while a pipeline still points at
	// it. Standing one up costs about what one small method's compile does,
	// once per thread.
	host_target_machine ();
	tier2_target_machine ();

	// On the heap, because the object runs to several kilobytes. A thread_local
	// that large puts the runtime's TLS block over the surplus glibc keeps for
	// dlopen'd modules, and the mono_tls_* variables are initial-exec, so the
	// whole module has to fit in that surplus. An embedder that dlopens the
	// runtime gets "cannot allocate memory in static TLS block" and no runtime
	// at all.
	static thread_local std::unique_ptr<ThreadPipelines> pipelines;

	if (!pipelines)
		pipelines = std::make_unique<ThreadPipelines> ();

	return *pipelines;
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
MonoJit::run_tier2_pipeline (Module &m, ArrayRef<uint8_t> profile,
                             InlineCandidates *inliner)
{
	timing::Scope timed (timing::Phase::pipeline);
	VerifyLevel verify = verify_level ();

	if (verify != VerifyLevel::off)
		verify_or_die (m, "as translated");

	// Codegen reads this to pick the optimizing target machine.
	m.addModuleFlag (Module::Error, "mono.tier2", 1);

	std::optional<timing::Scope> timed_setup (std::in_place, timing::Phase::pbsetup);
	ThreadPipelines &pipelines = thread_pipelines ();

	// The after-pass verifier is registered once with the pipeline, so which
	// module it looks at is handed over here rather than captured.
	g_verify_module = verify != VerifyLevel::off ? &m : nullptr;
	g_verify_level = verify;

	// What this compile hands the pipeline. The counts are not copied, so the
	// guard has to stand until the run is over.
	OneFileFS::CurrentFileGuard counts = pushProfile (*pipelines.profile_fs, profile);
	InlinerScope engine (pipelines, inliner);

	timed_setup.reset ();
	{
		timing::Scope timed_run (timing::Phase::prun);

		ThreadPipelines::Tier &tier = pipelines.tier2 ();

		tier.mpm.run (m, tier.mam);
		tier.forget_analyses ();
	}
	g_verify_module = nullptr;

	if (verify != VerifyLevel::off)
		verify_or_die (m, "after the tier-2 pipeline");
}

void
MonoJit::run_tier1_pipeline (Module &m)
{
	timing::Scope timed (timing::Phase::pipeline);
	VerifyLevel verify = verify_level ();

	if (verify != VerifyLevel::off)
		verify_or_die (m, "as translated");

	std::optional<timing::Scope> timed_setup (std::in_place, timing::Phase::pbsetup);
	ThreadPipelines &pipelines = thread_pipelines ();

	// The after-pass verifier is registered once with the pipeline, so which
	// module it looks at is handed over here rather than captured.
	g_verify_module = verify != VerifyLevel::off ? &m : nullptr;
	g_verify_level = verify;
	timed_setup.reset ();
	{
		timing::Scope timed_run (timing::Phase::prun);

		ThreadPipelines::Tier &tier = pipelines.tier1 ();

		tier.mpm.run (m, tier.mam);
		tier.forget_analyses ();
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
	 * Intel syntax for what MONO_JIT_DUMP prints at tier1-asm or tier2-asm,
	 * which is the syntax the Intel manuals and a debugger's disassembly here
	 * use. The option reaches the MCAsmInfo, which the instruction printer,
	 * the `.intel_syntax noprefix` directive and register printing all read,
	 * so the whole dump agrees.
	 *
	 * It has to run before the first TargetMachine, because MCAsmInfo reads the
	 * option in its constructor and every TargetMachine builds one. The object a
	 * compile publishes is not affected: an assembler dialect is a property of
	 * printed text.
	 */
	default_option_text ("x86-asm-syntax", "intel");

	// Value profiling needs compiler-rt, which we do not link.
	default_option ("disable-vp", true);

	// Allow lowering memcpy to `rep movsb` when the target supports it.
	default_option ("x86-use-fsrm-for-memcpy", true);

	/*
	 * Put a call's outgoing stack arguments in the frame the prologue
	 * allocates, instead of pushing them at the call.
	 *
	 * X86CallFrameOptimization turns those stores into pushes, so rsp moves
	 * across a call with arguments the registers did not take. LLVM records
	 * the movement as DW_CFA_GNU_args_size, which is how much a landing pad
	 * has to add back. transcode_unwind () (jinfo.cpp) drops that record,
	 * and mono's unwinder has no rule for it. A resume therefore enters the
	 * pad at the rsp of the call site. The body then runs an epilogue that
	 * is short by the pushed bytes, and its ret takes control to whatever
	 * sits below the return slot.
	 *
	 * Only tier 2 reaches this, because tier-1 codegen runs at
	 * CodeGenOptLevel::None and the pass does not run there.
	 *
	 * LLVM's own pass declines per function on Darwin, whose compact unwind
	 * encoding cannot carry the record either. It declines for a function
	 * with landing pads, or one with a frameless unwind table. That is the
	 * narrowing to take if the pushes are ever worth having back.
	 *
	 * mono/tests/eh-stack-args.cs is the exerciser.
	 */
	default_option ("no-x86-call-frame-opt", true);

	/*
	 * Fold a null check into the memory operation behind it. The translator
	 * marks every check with !make.implicit, and LLVM's ImplicitNullChecks
	 * pass rewrites a marked test and branch into a faulting access, so the
	 * check costs nothing until it fires.
	 *
	 * Only tier 2 folds a check. Tier-1 codegen runs at
	 * CodeGenOptLevel::None, where a block takes one of two selection paths
	 * and each one stops the fold on its own.
	 *
	 * FastISel selects the test as `CMP reg, 0`, and
	 * X86InstrInfo::analyzeBranchPredicate () reads `TEST reg, reg` only, so
	 * the pass leaves the block before it reads an instruction in it. A block
	 * that SelectionDAG selects gets `TEST` instead. RegAllocFast then reloads
	 * the tested pointer into a register of its own, which is not the register
	 * the test read, so isSuitableMemoryOp () refuses the dereference.
	 *
	 * LLVM expects the runtime to read __llvm_faultmaps for the handler of a
	 * faulting access. Mono instead turns a SIGSEGV inside a compiled body
	 * into a NullReferenceException from the faulting instruction, which
	 * lands in the same clause. That holds while the fault address stays in
	 * the page above null, which is what mono_is_addr_implicit_null_check ()
	 * (mono/mini/mini-runtime.c) accepts. The pass also folds an access below
	 * the checked pointer, and the translator emits none: it dereferences a
	 * checked pointer at a non-negative offset only.
	 */
	default_option ("enable-implicit-null-checks", true);

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
	/*
	 * A fresh module carries no layout, and every pass that asks about a size or
	 * an alignment then gets LLVM's own defaults instead of the target's. Those
	 * name no stack alignment at all, which is what lets a pass align a frame
	 * object past what the frame gives it: the frame is not realigned, the
	 * object lands where the alignment does not hold, and the aligned move the
	 * pass chose faults.
	 */
	if (m.getDataLayout ().isDefault ())
		m.setDataLayout (host_target_machine ().createDataLayout ());

	if (tier == JitTier::tier2) {
		run_tier2_pipeline (m, profile, inliner);
		return {};
	}

	// Emptied first: the passes append, and this thread's last compile left its
	// own sites behind.
	profile_sites ().clear ();
	run_tier1_pipeline (m);

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
	// with the target, and a fresh module has no layout at all. This covers the
	// modules that reach codegen without a pipeline - a thunk, the thrower, the
	// dispatcher - since MonoJit::optimize () sets it for the rest.
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
	auto object_code =
		std::make_shared<std::vector<std::pair<const uint8_t *, size_t>>> ();

	for (const auto &[name, extent] : extents->functions)
		if (extent.first != nullptr && extent.second != 0)
			object_code->push_back (extent);
	for (const auto &stub : extents->linker_stubs)
		if (stub.first != nullptr && stub.second != 0)
			object_code->push_back (stub);
	std::sort (object_code->begin (), object_code->end ());

	for (StringRef entry : entries) {
		CompiledMethod compiled;

		compiled.object_code = object_code;

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
