/**
 * \file
 * \brief The IR-to-object compiler: stock codegen plus mono's side tables.
 *
 * ORC's SimpleCompiler is TargetMachine::addPassesToEmitMC plus PassManager::run
 * and nothing else. What mono needs from codegen that the stock recipe cannot
 * give it is a seat between the machine passes and the AsmPrinter. The clause
 * gather has to see the final landing-pad set, and the side tables have to be
 * written into the object while the streamer is still open, with the code
 * offsets as label differences the writer folds at layout. So the recipe is
 * restated here, faithfully, with those two additions.
 *
 * The gather pass (passes/eh-gather.cpp) fills in the `.mono_lsda` format;
 * mono_lsda.cpp is its reader. `.mono_unwind` is written straight from here
 * rather than by a separate pass. sidetables.hpp has the format.
 */

/*
 * LLVM uses `PIC` as an identifier (PassInstrumentationCallbacks); mono's build
 * defines it as a macro.
 */
#ifdef PIC
#undef PIC
#endif

#include "compiler.hpp"

#include "method-symbols.hpp"

#include "dump.hpp"
#include "il-line-table.hpp"
#include "jit.hpp"
#include "sidetables.hpp"
#include "timing.hpp"

#include "debugging/perf/jitdump.hpp"
#include "eh-side-channel.hpp"
#include "passes/eh-gather.hpp"
#include "passes/faulting-location.hpp"
#include "passes/finally-range.hpp"

#include <llvm/Analysis/RuntimeLibcallInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/CodeGen/AsmPrinter.h>
#include <llvm/CodeGen/AsmPrinterHandler.h>
#include <llvm/CodeGen/MachineFunctionPass.h>
#include <llvm/CodeGen/MachineInstr.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/Pass.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/MCAsmBackend.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCAssembler.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDwarf.h>
#include <llvm/MC/MCELFStreamer.h>
#include <llvm/MC/MCExpr.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCObjectWriter.h>
#include <llvm/MC/MCSectionELF.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;

namespace mono {
namespace {

/*
 * Where the two halves of codegen's run hand a start time over to the pass that
 * closes it out. Nothing outside the accounting reads these, and a codegen run
 * never leaves the thread it started on.
 */
thread_local uint64_t g_printer_started = 0;
thread_local uint64_t g_object_started = 0;
thread_local uint64_t g_isel_done = 0;
thread_local uint64_t g_function_started = 0;

class FunctionMarkPass : public FunctionPass {
public:
	static char ID;

	FunctionMarkPass () : FunctionPass (ID) {}

	StringRef getPassName () const override { return "Mono codegen timing mark"; }

	bool runOnFunction (Function &) override
	{
		g_function_started = timing::span_begin (timing::Phase::isel);
		return false;
	}

	void getAnalysisUsage (AnalysisUsage &au) const override
	{
		au.setPreservesAll ();
	}
};

char FunctionMarkPass::ID;

class MachinePassMarkPass : public MachineFunctionPass {
public:
	static char ID;

	MachinePassMarkPass () : MachineFunctionPass (ID) {}

	StringRef getPassName () const override { return "Mono ISel timing mark"; }

	bool runOnMachineFunction (MachineFunction &) override
	{
		timing::span_end (timing::Phase::isel, g_function_started);
		g_function_started = 0;
		g_isel_done = timing::span_begin (timing::Phase::mpass);
		return false;
	}

	void getAnalysisUsage (AnalysisUsage &au) const override
	{
		au.setPreservesAll ();
		MachineFunctionPass::getAnalysisUsage (au);
	}
};

char MachinePassMarkPass::ID;

class PrinterMarkPass : public MachineFunctionPass {
public:
	static char ID;

	PrinterMarkPass () : MachineFunctionPass (ID) {}

	StringRef getPassName () const override { return "Mono printer timing mark"; }

	bool runOnMachineFunction (MachineFunction &) override
	{
		timing::span_end (timing::Phase::mpass, g_isel_done);
		g_isel_done = 0;
		g_printer_started = timing::span_begin (timing::Phase::emit);
		return false;
	}

	void getAnalysisUsage (AnalysisUsage &au) const override
	{
		au.setPreservesAll ();
		MachineFunctionPass::getAnalysisUsage (au);
	}
};

char PrinterMarkPass::ID;

/*
 * Collects the rows `.mono_lines` and `.mono_inlines` are written from, as the
 * printer walks each function's instructions.
 *
 * The translator gives every instruction a debug location whose line is the IL
 * offset in effect at it (il-line-table.hpp); all a row needs on top of that is
 * a label to take a difference against, so one is planted wherever the line
 * changes. That is the same thing a `.loc` directive does, which is why the
 * module's compile unit can say NoDebug. This pipeline has no use for a DWARF
 * line table, and without one no `.debug_*` section is produced at all.
 *
 * An instruction the inliner brought in carries the callee's location, with the
 * call site behind it in the `inlinedAt` chain. Only the outermost of those is
 * an offset into this function, so that one is the row's line. The rest become
 * the row's inlined chain, each naming the body its code came from.
 */
class IlLineHandler : public AsmPrinterHandler {
public:
	/// One body the code at a row came from, and where in that body it was.
	struct Inlined {
		uint64_t callee;
		uint32_t line;
	};

	struct Row {
		const MCSymbol *at;
		uint32_t line;
		/// The bodies folded in here, innermost first. Empty for code the
		/// compiled method wrote itself.
		SmallVector<Inlined, 2> inlined;
	};

	struct Function {
		std::string name;
		std::vector<Row> rows;
	};

	explicit IlLineHandler (MCStreamer *streamer) : streamer_ (streamer) {}

	const std::vector<Function> &functions () const { return functions_; }

	// A reused printer keeps its user handlers across runs, so the rows of the
	// last module are still here when the next one opens.
	void beginModule (Module *m) override
	{
		functions_.clear ();
		line_ = 0;
		inlined_.clear ();
		ids_ = il_debug_subprogram_ids (*m);
	}

	void beginFunction (const MachineFunction *mf) override
	{
		const DISubprogram *sp = mf->getFunction ().getSubprogram ();

		functions_.push_back ({ mf->getName ().str (), {} });
		line_ = 0;
		inlined_.clear ();

		/*
		 * The prologue carries no location of its own, and a frame stopped
		 * in it still has to name an IL offset, so the function opens at the
		 * subprogram's own line - the method's first IL byte.
		 */
		if (sp != nullptr)
			record (sp->getScopeLine (), {});
	}

	void beginInstruction (const MachineInstr *mi) override
	{
		if (mi->isMetaInstruction ())
			return;

		const DILocation *loc = mi->getDebugLoc ();

		if (loc == nullptr)
			return;

		SmallVector<Inlined, 2> inlined;

		for (; loc->getInlinedAt () != nullptr; loc = loc->getInlinedAt ()) {
			auto id = ids_.find (loc->getScope ()->getSubprogram ());

			// A scope this compile did not create names no method. The
			// chain is read as one run from the innermost body out, so a
			// hole in it makes every frame past the hole the caller of the
			// wrong body. Take the whole row's chain off instead.
			if (id == ids_.end ()) {
				inlined.clear ();
				break;
			}

			inlined.push_back ({ id->second, loc->getLine () });
		}

		record (loc->getLine (), inlined);
	}

	void endFunction (const MachineFunction *) override {}
	void endModule () override {}

private:
	/**
	 * An instruction with no location leaves the line in effect alone rather
	 * than clearing it: the translator attributes what it emits, so a gap is
	 * codegen's own bookkeeping and belongs to whatever surrounds it.
	 *
	 * A row opens wherever the chain changes as well as wherever the line does.
	 * The engine keys a chain on the exact offset of the row that governs it
	 * (mono_jit_info_llvm_inline_frames ()). So code that comes back out of a
	 * folded body, at the call site it was folded through, needs a row of its
	 * own to say the chain has ended. The line there is the one already in
	 * effect, and on its own it would open no row.
	 */
	void record (unsigned line, ArrayRef<Inlined> inlined)
	{
		bool same_chain =
			inlined.size () == inlined_.size ()
			&& std::equal (inlined.begin (), inlined.end (), inlined_.begin (),
			               [] (const Inlined &a, const Inlined &b) {
				               return a.callee == b.callee && a.line == b.line;
			               });

		if (line == 0 || (line == line_ && same_chain))
			return;

		MCSymbol *at = streamer_->getContext ().createTempSymbol ();

		streamer_->emitLabel (at);
		functions_.back ().rows.push_back (
			{ at, (uint32_t) line, SmallVector<Inlined, 2> (inlined) });
		line_ = line;
		inlined_.assign (inlined.begin (), inlined.end ());
	}

	MCStreamer *streamer_;
	std::vector<Function> functions_;
	DenseMap<const DISubprogram *, uint64_t> ids_;
	unsigned line_ = 0;
	SmallVector<Inlined, 2> inlined_;
};

struct UnwindRecord {
	const MCSymbol *at;
	uint8_t op;
	int32_t reg;
	int64_t value;
};

UnwindRecord
transcribe_cfi (const MCCFIInstruction &i)
{
	UnwindRecord r = { i.getLabel (), MONO_UNWIND_OP_UNSUPPORTED, 0, 0 };

	switch (i.getOperation ()) {
	case MCCFIInstruction::OpDefCfa:
		r.op = MONO_UNWIND_OP_DEF_CFA;
		r.reg = static_cast<int32_t> (i.getRegister ());
		r.value = i.getOffset ();
		break;
	case MCCFIInstruction::OpDefCfaOffset:
		r.op = MONO_UNWIND_OP_DEF_CFA_OFFSET;
		r.value = i.getOffset ();
		break;
	case MCCFIInstruction::OpAdjustCfaOffset:
		r.op = MONO_UNWIND_OP_ADJUST_CFA_OFFSET;
		r.value = i.getOffset ();
		break;
	case MCCFIInstruction::OpGnuArgsSize:
		r.op = MONO_UNWIND_OP_ARGS_SIZE;
		r.value = i.getOffset ();
		break;
	case MCCFIInstruction::OpDefCfaRegister:
		r.op = MONO_UNWIND_OP_DEF_CFA_REGISTER;
		r.reg = static_cast<int32_t> (i.getRegister ());
		break;
	case MCCFIInstruction::OpOffset:
		r.op = MONO_UNWIND_OP_OFFSET;
		r.reg = static_cast<int32_t> (i.getRegister ());
		r.value = i.getOffset ();
		break;
	case MCCFIInstruction::OpRememberState:
		r.op = MONO_UNWIND_OP_REMEMBER_STATE;
		break;
	case MCCFIInstruction::OpRestoreState:
		r.op = MONO_UNWIND_OP_RESTORE_STATE;
		break;
	case MCCFIInstruction::OpRestore:
		r.op = MONO_UNWIND_OP_RESTORE;
		r.reg = static_cast<int32_t> (i.getRegister ());
		break;
	case MCCFIInstruction::OpSameValue:
		r.op = MONO_UNWIND_OP_SAME_VALUE;
		r.reg = static_cast<int32_t> (i.getRegister ());
		break;
	default:
		/*
		 * Recorded, not dropped: the reader has to know the description is
		 * incomplete so it can decline the method rather than mis-unwind it.
		 * The value carries LLVM's operation for the error message.
		 */
		r.value = i.getOperation ();
		break;
	}

	return r;
}

/**
 * Keeps the room a perf jit dump record needs free after each function.
 *
 * perf lays an image over the code a record names, longer than the code by the
 * frame description, and `.eh_frame_hdr` sits at the end of it. Where the next
 * function begins inside that reach, perf cuts the map back and takes the header
 * out of reach, so a profile unwinds no further than the frame it sampled.
 * `code_slack ()` is the room the description needs. The code allocator already
 * leaves it past an object, and this leaves it between the functions inside one,
 * which is what a batch of methods in one object needs.
 *
 * The bytes go in after the printer plants the function's end label and writes
 * its ELF size, so they land outside the symbol and no record names them.
 *
 * Nothing is emitted while no dump is open, because `code_slack ()` is then
 * zero.
 */
class CodeSlackHandler : public AsmPrinterHandler {
public:
	explicit CodeSlackHandler (MCStreamer *streamer) : streamer_ (streamer) {}

	void endModule () override {}
	void beginFunction (const MachineFunction *) override {}

	void endFunction (const MachineFunction *mf) override
	{
		size_t slack = perf::code_slack ();

		if (slack == 0 || mf->getSection () == nullptr)
			return;

		/* Nops rather than zeros, so that a disassembly of the gap reads as
		 * padding and a stray jump into it runs to the next function. */
		streamer_->switchSection (mf->getSection ());
		streamer_->emitNops ((int64_t) slack, 0, SMLoc (), mf->getSubtarget ());
	}

private:
	MCStreamer *streamer_;
};

class SideTableEmitPass : public MachineFunctionPass {
public:
	static char ID;

	SideTableEmitPass (MCStreamer *streamer, const MonoEHSideChannel *sc,
	                   const IlLineHandler *lines)
		: MachineFunctionPass (ID), streamer_ (streamer), sc_ (sc),
		  lines_ (lines)
	{
	}

	StringRef getPassName () const override { return "Mono side-table emission"; }

	bool runOnMachineFunction (MachineFunction &) override
	{
		timing::span_end (timing::Phase::emit, g_printer_started);
		g_printer_started = 0;
		return false;
	}

	void getAnalysisUsage (AnalysisUsage &au) const override
	{
		au.setPreservesAll ();
		MachineFunctionPass::getAnalysisUsage (au);
	}

	bool doFinalization (Module &) override
	{
		{
			timing::Scope timed (timing::Phase::sidetbl);

			emit_clause_table ();
			emit_guard_table ();
			emit_unwind_table ();
			emit_line_table ();
			emit_inline_table ();
		}

		/*
		 * doFinalization runs in reverse pass order, so from here to the end of
		 * the run is the AsmPrinter closing the object out and every pass below
		 * it being finalized.
		 */
		g_object_started = timing::span_begin (timing::Phase::objout);
		return false;
	}

private:
	/**
	 * `.mono_lsda`, in the v3 format mono_lsda.hpp declares and mono_lsda.cpp
	 * reads back at load. One block per clause-bearing function, each naming
	 * where that function was linked, so an object holding a batch of methods
	 * keeps each method's geometry apart.
	 */
	void emit_clause_table ()
	{
		MCStreamer &streamer = *streamer_;
		MCContext &ctx = streamer.getContext ();

		for (const MonoEHFunctionClauses &fn : sc_->functions) {
			/*
			 * Declined by the gather. The reader treats an absent block as
			 * a refusal, never a publishable empty table.
			 */
			if (fn.declined)
				continue;

			/*
			 * A filter body compiled alongside its method carries no clauses
			 * of its own: a block describes a method.
			 */
			if (fn.function.find (filter_body_suffix) != std::string::npos)
				continue;

			MCSymbol *begin = ctx.getOrCreateSymbol (fn.function);

			streamer.switchSection (ctx.getELFSection (
				".mono_lsda", ELF::SHT_PROGBITS, ELF::SHF_ALLOC));

			auto from_begin = [&] (const MCSymbol *sym) {
				return MCBinaryExpr::createSub (
					MCSymbolRefExpr::create (sym, ctx),
					MCSymbolRefExpr::create (begin, ctx), ctx);
			};

			streamer.emitIntValue (0x4d4c5344u, 4); /* 'MLSD' */
			streamer.emitIntValue (3, 2);
			streamer.emitIntValue (fn.clauses.size (), 2);
			streamer.emitValue (MCSymbolRefExpr::create (begin, ctx), 8);

			for (const MonoEHClause &c : fn.clauses) {
				streamer.emitValue (from_begin (c.try_begin), 4);
				streamer.emitValue (
					MCBinaryExpr::createSub (
						MCSymbolRefExpr::create (c.try_end, ctx),
						MCSymbolRefExpr::create (c.try_begin, ctx),
						ctx),
					4);
				streamer.emitValue (from_begin (c.handler), 4);
				streamer.emitIntValue ((uint32_t) c.clause_index, 4);
				streamer.emitIntValue ((uint32_t) c.kind, 4);
			}
		}
	}

	/*
	 * `.mono_guards`: where each finally handler body ended up and where its
	 * thread-abort guard byte lives. Absent when the method has no finally, or
	 * when every body was optimized away - there is then nothing a thread can be
	 * stopped inside, so an absent section is a fact and not a refusal.
	 *
	 * A body whose slot could not be pinned to one place is dropped rather than
	 * published: an abort then lands inside that finally, where a wrong slot
	 * would have the runtime flag a byte belonging to something else.
	 */
	void emit_guard_table ()
	{
		MCStreamer &streamer = *streamer_;
		MCContext &ctx = streamer.getContext ();

		for (const MonoEHFinallyFunction &fn : sc_->finally_functions) {
			std::vector<const MonoEHFinallyBody *> bodies;

			for (const MonoEHFinallyBody &body : fn.bodies)
				if (body.exvar_dwarf_reg >= 0)
					bodies.push_back (&body);

			if (bodies.empty ())
				continue;

			MCSymbol *begin = ctx.getOrCreateSymbol (fn.function);

			streamer.switchSection (ctx.getELFSection (
				".mono_guards", ELF::SHT_PROGBITS, ELF::SHF_ALLOC));

			auto from_begin = [&] (const MCSymbol *sym) {
				return MCBinaryExpr::createSub (
					MCSymbolRefExpr::create (sym, ctx),
					MCSymbolRefExpr::create (begin, ctx), ctx);
			};

			streamer.emitIntValue (guards_section_magic, 4);
			streamer.emitIntValue (guards_section_version, 2);
			streamer.emitIntValue (bodies.size (), 2);
			streamer.emitValue (MCSymbolRefExpr::create (begin, ctx), 8);

			for (const MonoEHFinallyBody *body : bodies) {
				streamer.emitIntValue ((uint32_t) body->clause_index, 4);
				streamer.emitValue (from_begin (body->body_begin), 4);
				streamer.emitValue (from_begin (body->body_end), 4);
				streamer.emitIntValue ((uint32_t) body->exvar_offset, 4);
				streamer.emitIntValue ((uint32_t) body->exvar_dwarf_reg, 4);
			}
		}
	}

	/**
	 * `.mono_unwind`: one block per function, each its CFI program with the
	 * initial frame state first. The rule offsets are label differences
	 * against the frame's begin label, which sits at the function's entry, so
	 * the writer folds them to constants. The begin label itself is emitted
	 * whole, as the address the reader matches a block to a function by.
	 */
	void emit_unwind_table ()
	{
		MCStreamer &streamer = *streamer_;
		MCContext &ctx = streamer.getContext ();
		ArrayRef<MCDwarfFrameInfo> frames = streamer.getDwarfFrameInfos ();

		if (frames.empty ())
			return;

		/*
		 * Only an object streamer plants the frame and rule labels. Textual
		 * assembly stands the `.cfi_*` directives in for them and fills the
		 * label fields with a dummy pointer. There is nothing to take a
		 * difference against, and the directives say the same thing.
		 */
		if (frames.front ().Begin == nullptr)
			return;

		const std::vector<MCCFIInstruction> &initial =
			ctx.getAsmInfo ()->getInitialFrameState ();

		streamer.switchSection (ctx.getELFSection (
			".mono_unwind", ELF::SHT_PROGBITS, ELF::SHF_ALLOC));

		for (const MCDwarfFrameInfo &frame : frames) {
			auto emit_record = [&] (const UnwindRecord &r, bool at_entry) {
				if (at_entry || r.at == nullptr) {
					streamer.emitIntValue (0, 4);
				} else {
					streamer.emitValue (
						MCBinaryExpr::createSub (
							MCSymbolRefExpr::create (r.at, ctx),
							MCSymbolRefExpr::create (frame.Begin,
							                         ctx),
							ctx),
						4);
				}
				streamer.emitIntValue (r.op, 1);
				streamer.emitIntValue (static_cast<uint32_t> (r.reg), 4);
				streamer.emitIntValue (static_cast<uint64_t> (r.value), 8);
			};

			streamer.emitIntValue (unwind_section_magic, 4);
			streamer.emitIntValue (unwind_section_version, 2);
			streamer.emitIntValue (0, 2);
			streamer.emitIntValue (initial.size ()
			                               + frame.Instructions.size (),
			                       4);
			streamer.emitValue (MCSymbolRefExpr::create (frame.Begin, ctx), 8);

			for (const MCCFIInstruction &i : initial)
				emit_record (transcribe_cfi (i), /*at_entry=*/true);
			for (const MCCFIInstruction &i : frame.Instructions)
				emit_record (transcribe_cfi (i), /*at_entry=*/false);
		}
	}

	/**
	 * `.mono_lines`: one block per function, each row an IL offset and the code
	 * offset it takes effect at, as a label difference the writer folds at
	 * layout. The rows are the ones the printer's handler planted labels for
	 * while it walked the machine code.
	 */
	void emit_line_table ()
	{
		MCStreamer &streamer = *streamer_;
		MCContext &ctx = streamer.getContext ();

		for (const IlLineHandler::Function &fn : lines_->functions ()) {
			if (fn.rows.empty ())
				continue;

			MCSymbol *begin = ctx.getOrCreateSymbol (fn.name);

			streamer.switchSection (ctx.getELFSection (
				".mono_lines", ELF::SHT_PROGBITS, ELF::SHF_ALLOC));

			streamer.emitIntValue (lines_section_magic, 4);
			streamer.emitIntValue (lines_section_version, 2);
			streamer.emitIntValue (0, 2);
			streamer.emitIntValue (fn.rows.size (), 4);
			streamer.emitValue (MCSymbolRefExpr::create (begin, ctx), 8);

			for (const IlLineHandler::Row &row : fn.rows) {
				streamer.emitValue (
					MCBinaryExpr::createSub (
						MCSymbolRefExpr::create (row.at, ctx),
						MCSymbolRefExpr::create (begin, ctx), ctx),
					4);
				streamer.emitIntValue (row.line, 4);
			}
		}
	}

	/**
	 * `.mono_inlines`, the chain of bodies folded into each row of the line
	 * table. Only a function that had something folded into it gets a block, so
	 * a method the inliners left alone pays nothing.
	 */
	void emit_inline_table ()
	{
		MCStreamer &streamer = *streamer_;
		MCContext &ctx = streamer.getContext ();

		for (const IlLineHandler::Function &fn : lines_->functions ()) {
			size_t count = 0;

			for (const IlLineHandler::Row &row : fn.rows)
				count += row.inlined.size ();

			if (count == 0)
				continue;

			MCSymbol *begin = ctx.getOrCreateSymbol (fn.name);

			streamer.switchSection (ctx.getELFSection (
				".mono_inlines", ELF::SHT_PROGBITS, ELF::SHF_ALLOC));

			streamer.emitIntValue (inlines_section_magic, 4);
			streamer.emitIntValue (inlines_section_version, 2);
			streamer.emitIntValue (0, 2);
			streamer.emitIntValue (count, 4);
			streamer.emitValue (MCSymbolRefExpr::create (begin, ctx), 8);

			for (const IlLineHandler::Row &row : fn.rows) {
				uint32_t depth = 0;

				for (const IlLineHandler::Inlined &frame : row.inlined) {
					streamer.emitValue (
						MCBinaryExpr::createSub (
							MCSymbolRefExpr::create (row.at, ctx),
							MCSymbolRefExpr::create (begin, ctx),
							ctx),
						4);
					streamer.emitIntValue (frame.line, 4);
					streamer.emitIntValue (depth++, 4);
					streamer.emitIntValue (frame.callee, 8);
				}
			}
		}
	}

	MCStreamer *streamer_;
	const MonoEHSideChannel *sc_;
	const IlLineHandler *lines_;
};

char SideTableEmitPass::ID;

enum class OutputKind { object, assembly };

/**
 * The streamer for one codegen run: either the ELF one createMCObjectStreamer
 * builds, or the assembly printer addPassesToEmitFile's AssemblyFile arm does.
 */
Expected<std::unique_ptr<MCStreamer>>
make_streamer (TargetMachine &tm, MCContext &ctx, raw_pwrite_stream &out,
               OutputKind kind)
{
	const MCSubtargetInfo &sti = *tm.getMCSubtargetInfo ();
	const MCAsmInfo &mai = *tm.getMCAsmInfo ();
	const MCInstrInfo &mii = *tm.getMCInstrInfo ();
	const MCRegisterInfo &mri = *tm.getMCRegisterInfo ();
	std::unique_ptr<MCAsmBackend> mab (
		tm.getTarget ().createMCAsmBackend (sti, mri, tm.Options.MCOptions));

	if (!mab)
		return make_error<StringError> ("target does not support MC emission",
		                                inconvertibleErrorCode ());

	if (kind == OutputKind::assembly) {
		std::unique_ptr<MCInstPrinter> printer (
			tm.getTarget ().createMCInstPrinter (
				tm.getTargetTriple (),
				tm.Options.MCOptions.OutputAsmVariant.value_or (
					mai.getAssemblerDialect ()),
				mai, mii, mri));

		if (!printer)
			return make_error<StringError> (
				"target does not support an MCInstPrinter",
				inconvertibleErrorCode ());

		return std::unique_ptr<MCStreamer> (tm.getTarget ().createAsmStreamer (
			ctx, std::make_unique<formatted_raw_ostream> (out),
			std::move (printer), /*CE=*/nullptr, std::move (mab)));
	}

	std::unique_ptr<MCCodeEmitter> mce (
		tm.getTarget ().createMCCodeEmitter (mii, ctx));

	if (!mce)
		return make_error<StringError> ("target does not support MC emission",
		                                inconvertibleErrorCode ());

	std::unique_ptr<MCObjectWriter> ow = mab->createObjectWriter (out);
	auto streamer = std::make_unique<MCELFStreamer> (
		ctx, std::move (mab), std::move (ow), std::move (mce));

	if (tm.Options.MCOptions.MCRelaxAll)
		streamer->getAssembler ().setRelaxAll (true);

	return std::unique_ptr<MCStreamer> (std::move (streamer));
}

/*
 * A codegen pipeline, built and ready to run over a module.
 *
 * Members die in reverse order of declaration. The pass manager owns the MMI
 * whose MCContext the streamer writes into, and the AsmPrinter that owns the
 * streamer, so it has to go before the buffer those writes land in.
 */
struct ObjectPipeline {
	/// Where an object run puts its bytes. An assembly run writes to the
	/// caller's stream and leaves this empty.
	SmallVector<char, 0> object;
	std::optional<raw_svector_ostream> object_stream;

	MonoEHSideChannel side_channel;

	MCStreamer *streamer = nullptr;
	std::optional<legacy::PassManager> pm;
};

/*
 * A faithful restatement of TargetMachine::addPassesToEmitMC - the recipe
 * SimpleCompiler drives - kept open so the gather runs after addMachinePasses ()
 * and the side-table writer against the object streamer. Any drift from the
 * stock method between LLVM versions is a silent codegen difference, so change
 * this only against the current implementation.
 *
 * The passes hold pointers into p and write to out, so the pipeline is good for
 * as long as both live and for no other destination.
 */
Error
build_object_pipeline (TargetMachine &tm, ObjectPipeline &p, raw_pwrite_stream &out,
                       OutputKind kind)
{
	std::optional<legacy::PassManager> &pm = p.pm;
	MonoEHSideChannel &side_channel = p.side_channel;

	pm.emplace ();

	std::optional<timing::Scope> timed_setup (std::in_place,
	                                          timing::Phase::cgsetup);
	std::optional<timing::Scope> timed_part (std::in_place, timing::Phase::mmi);

	auto *mmiwp = new MachineModuleInfoWrapperPass (&tm);

	timed_part.emplace (timing::Phase::cgpass);

	TargetPassConfig *tpc = tm.createPassConfig (*pm);

	/*
	 * The two IR verifier runs this gates bracket codegen's own IR passes
	 * (CodeGenPrepare and friends), which are the last thing to touch the
	 * module before ISel reads it. The machine verifier is a separate,
	 * far more expensive switch and stays off either way.
	 */
	tpc->setDisableVerify (!ir_verification_enabled ());
	pm->add (tpc);
	pm->add (mmiwp);

	/*
	 * Both describe the target to codegen, and the legacy manager builds each
	 * of them from an empty Triple when nobody adds one. An empty triple has no
	 * runtime libcalls at all, so PreISelIntrinsicLowering finds no memcpy,
	 * memmove or memset to call and writes a byte-at-a-time loop in place of
	 * each one. That measured 2 GB/s against glibc's 55 GB/s on a 64KB copy.
	 */
	TargetLibraryInfoImpl tlii (tm.getTargetTriple (), tm.Options.VecLib);

	pm->add (new TargetLibraryInfoWrapperPass (tlii));
	pm->add (new RuntimeLibraryInfoWrapper (
		tm.getTargetTriple (), tm.Options.ExceptionModel, tm.Options.FloatABIType,
		tm.Options.EABIVersion, tm.Options.MCOptions.ABIName, tm.Options.VecLib));

	if (timing::fine ())
		pm->add (new FunctionMarkPass ());

	if (tpc->addISelPasses ())
		return make_error<StringError> (
			"target does not support instruction selection",
			inconvertibleErrorCode ());

	if (timing::fine ())
		pm->add (new MachinePassMarkPass ());

	tpc->addMachinePasses ();

	/*
	 * After the machine passes, which is where ImplicitNullChecks folds a check
	 * into the dereference and leaves it unattributed, and before the gather and
	 * the printer, both of which read the locations back.
	 */
	pm->add (new MonoFaultingLocationPass ());

	/*
	 * After the machine passes and before the AsmPrinter, so it sees the final
	 * landing-pad set. It plants a label at each end of every range it
	 * publishes, so a module with landing pads comes out changed.
	 */
	pm->add (new MonoEHGatherPass (&side_channel));

	/*
	 * Also before the printer, and after the frame is laid out: it plants the
	 * labels that name where the finally bodies ended up, and reads the guard
	 * slot's frame home out of markers PEI has already resolved.
	 */
	pm->add (new MonoFinallyRangePass (&side_channel));

	tpc->setInitialized ();

	/*
	 * The AsmPrinter must emit into the MCContext the MMI created - the
	 * external-context contract addPassesToEmitMC relies on, and what lets the
	 * side-table writer share the same context and symbols.
	 */
	MCContext *ctx = &mmiwp->getMMI ().getContext ();

	timed_part.emplace (timing::Phase::strm);

	Expected<std::unique_ptr<MCStreamer>> streamer =
		make_streamer (tm, *ctx, out, kind);
	if (!streamer)
		return streamer.takeError ();

	timed_part.emplace (timing::Phase::aprint);

	MCStreamer *streamer_ptr = streamer->get ();
	AsmPrinter *printer = tm.getTarget ().createAsmPrinter (tm, std::move (*streamer));
	if (!printer)
		return make_error<StringError> ("target does not support an AsmPrinter",
		                                inconvertibleErrorCode ());

	/*
	 * Registered before the printer is initialized, which is what keeps it
	 * ahead of the printer's own handlers and alive for the whole run.
	 */
	auto lines = std::make_unique<IlLineHandler> (streamer_ptr);
	IlLineHandler *lines_ptr = lines.get ();

	printer->addAsmPrinterHandler (std::move (lines));

	printer->addAsmPrinterHandler (std::make_unique<CodeSlackHandler> (streamer_ptr));

	/*
	 * Ahead of the printer, so the pass below it closes the interval the
	 * printer took over one function.
	 */
	if (timing::fine ())
		pm->add (new PrinterMarkPass ());

	pm->add (printer);

	/*
	 * After the printer on purpose: doFinalization runs in reverse pass order,
	 * so the side tables are written while the streamer is still open, before
	 * the AsmPrinter's own finalization ends the object.
	 */
	pm->add (new SideTableEmitPass (streamer_ptr, &side_channel, lines_ptr));

	/*
	 * Last, as the stock recipe has it. It hands each MachineFunction back to
	 * the MMI, and a pipeline that runs a second time needs that: the MMI keys
	 * them by Function, and the Functions of the compiled module are about to
	 * go.
	 */
	pm->add (createFreeMachineFunctionPass ());

	p.streamer = streamer_ptr;
	return Error::success ();
}

/// Runs the pipeline over m, which codegen consumes.
void
run_object_pipeline (ObjectPipeline &p, Module &m)
{
	timing::Scope timed_run (timing::Phase::cgrun);

	g_object_started = 0;
	p.pm->run (m);
	timing::span_end (timing::Phase::objout, g_object_started);
}

/*
 * The pipeline this thread reuses for the objects it compiles, one for each
 * target machine it is asked for. A thread that compiles once keeps its
 * pipeline, and the ~55 passes in it, for the rest of its life.
 *
 * LLVM supports the reuse and this leans on three parts of it.
 * legacy::PassManager::run () calls doInitialization () and doFinalization ()
 * on the passes for each module. MachineModuleInfo::finalize () resets the
 * MCContext. The AsmPrinter keeps the handlers we added and drops the ones it
 * made for itself.
 */
Expected<ObjectPipeline *>
thread_object_pipeline (TargetMachine &tm)
{
	/*
	 * The machine is thread_local as well and the pipeline points at it, so the
	 * pipeline has to be registered after it and therefore dies before it. The
	 * caller got tm from that call, so the order already holds.
	 */
	static thread_local std::vector<
		std::pair<TargetMachine *, std::unique_ptr<ObjectPipeline>>> built;

	for (auto &[machine, pipeline] : built)
		if (machine == &tm)
			return pipeline.get ();

	auto fresh = std::make_unique<ObjectPipeline> ();

	fresh->object_stream.emplace (fresh->object);
	if (Error err = build_object_pipeline (tm, *fresh, *fresh->object_stream,
	                                       OutputKind::object))
		return std::move (err);

	built.emplace_back (&tm, std::move (fresh));
	return built.back ().second.get ();
}

Error
emit_object_reused (TargetMachine &tm, Module &m, SmallVectorImpl<char> &object,
                    MonoEHSideChannel &side_channel)
{
	Expected<ObjectPipeline *> pipeline = thread_object_pipeline (tm);

	if (!pipeline)
		return pipeline.takeError ();

	ObjectPipeline &p = **pipeline;

	run_object_pipeline (p, m);

	/*
	 * The run ended in MachineModuleInfo::finalize (), which reset the MCContext
	 * and freed the sections and symbols the streamer still lists. The reset
	 * here puts the streamer back to its start state. It clears containers of
	 * its own and reads none of what the context freed.
	 */
	p.streamer->reset ();

	object = std::move (p.object);
	side_channel = std::move (p.side_channel);
	p.side_channel = MonoEHSideChannel ();
	return Error::success ();
}

Error
emit_object_fresh (TargetMachine &tm, Module &m, SmallVectorImpl<char> &object,
                   MonoEHSideChannel &side_channel)
{
	raw_svector_ostream out (object);
	ObjectPipeline p;

	if (Error err = build_object_pipeline (tm, p, out, OutputKind::object))
		return err;

	run_object_pipeline (p, m);
	side_channel = std::move (p.side_channel);
	{
		timing::Scope timed_free (timing::Phase::pmfree);

		p.pm.reset ();
	}
	return Error::success ();
}

Error
emit_assembly_text (TargetMachine &tm, Module &m, raw_pwrite_stream &out)
{
	ObjectPipeline p;

	if (Error err = build_object_pipeline (tm, p, out, OutputKind::assembly))
		return err;

	run_object_pipeline (p, m);
	return Error::success ();
}

/**
 * Writes the assembly one method in m compiles to - side-table sections
 * included, which is the half no offline llc run can reproduce.
 *
 * Codegen consumes the module it is given, and the MCContext, the MMI and the
 * streamer are entangled with the single run they were built for, so this
 * compiles a clone and leaves the caller's module for the object run. Being a
 * separate run also means it cannot change the code that gets published: a
 * bug in the printout stays a bug in the printout. The side channel it fills
 * is discarded for the same reason.
 *
 * A batch shares one module between its members, so the clone keeps the wanted
 * method's body and drops the others. A body dropped that way becomes a
 * declaration, which is what a call leaving the module compiles to in any case,
 * and the dump then holds one method's code and one method's side tables.
 */
Error
dump_method_assembly (TargetMachine &tm, const Module &m, DumpPoint point,
                      StringRef entry, StringRef name)
{
	std::unique_ptr<Module> copy = CloneModule (m);

	for (Function &f : *copy)
		if (!f.isDeclaration () && f.getName () != entry)
			f.deleteBody ();

	return with_dump_stream (point, name, [&] (raw_pwrite_stream &out) -> Error {
		out << "*** assembly for " << name << " ***\n";

		return emit_assembly_text (tm, *copy, out);
	});
}

/// How a compile gets its codegen pipeline. MONO_LLVM_JIT_CODEGEN picks.
enum class PipelineUse { reuse, fresh, compare };

/**
 * `fresh` builds a pipeline for each method, which is what an A/B against the
 * reuse measures. `compare` runs both over one method and stops the process
 * when the two objects differ.
 */
PipelineUse
pipeline_use ()
{
	static const PipelineUse use = [] {
		const char *setting = std::getenv ("MONO_LLVM_JIT_CODEGEN");

		if (setting == nullptr)
			return PipelineUse::reuse;
		if (StringRef (setting) == "fresh")
			return PipelineUse::fresh;
		if (StringRef (setting) == "compare")
			return PipelineUse::compare;
		return PipelineUse::reuse;
	}();

	return use;
}

Error
emit_method_object (TargetMachine &tm, Module &m, SmallVectorImpl<char> &object,
                    MonoEHSideChannel &side_channel)
{
	switch (pipeline_use ()) {
	case PipelineUse::fresh:
		return emit_object_fresh (tm, m, object, side_channel);

	case PipelineUse::compare: {
		// Codegen consumes the module it is given, so the control run takes a
		// copy of it.
		std::unique_ptr<Module> copy = CloneModule (m);
		SmallVector<char, 0> control;
		MonoEHSideChannel control_channel;

		if (Error err = emit_object_fresh (tm, *copy, control, control_channel))
			return err;
		if (Error err = emit_object_reused (tm, m, object, side_channel))
			return err;
		if (StringRef (control.data (), control.size ())
		    != StringRef (object.data (), object.size ()))
			report_fatal_error (
				Twine ("mono: the reused codegen pipeline wrote a "
			               "different object for ")
					+ m.getModuleIdentifier (),
				/*GenCrashDiag=*/false);
		return Error::success ();
	}

	case PipelineUse::reuse:
		break;
	}

	return emit_object_reused (tm, m, object, side_channel);
}

} // namespace

MethodObjectCompiler::MethodObjectCompiler (orc::JITTargetMachineBuilder jtmb)
	: IRCompiler (orc::irManglingOptionsFromTargetOptions (jtmb.getOptions ()))
{
}

Expected<std::unique_ptr<MemoryBuffer>>
MethodObjectCompiler::operator() (Module &m)
{
	timing::Scope timed (timing::Phase::codegen);
	MonoEHSideChannel side_channel;
	SmallVector<char, 0> buffer;

	// The tier-2 pipeline stamps this, and the level it wants is fixed when the
	// machine is built rather than settable here.
	const bool tier2 = m.getModuleFlag ("mono.tier2") != nullptr;
	TargetMachine &tm
		= tier2 ? tier2_target_machine () : host_target_machine ();
	const DumpPoint point = tier2 ? DumpPoint::tier2_asm : DumpPoint::tier1_asm;

	if (dump_point_enabled (point))
		for (const Function &f : m) {
			if (!is_published_body (f))
				continue;

			std::string name = dump_name_of (f);

			if (dumping (point, name.c_str ()))
				if (Error err = dump_method_assembly (tm, m, point,
				                                      f.getName (), name))
					return std::move (err);
		}

	if (Error err = emit_method_object (tm, m, buffer, side_channel))
		return std::move (err);

	return std::make_unique<SmallVectorMemoryBuffer> (
		std::move (buffer), m.getModuleIdentifier () + "-jitted-objectbuffer",
		/*RequiresNullTerminator=*/false);
}

} // namespace mono
