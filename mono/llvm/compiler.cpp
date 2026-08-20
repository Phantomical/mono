/**
 * \file
 * \brief The IR-to-object compiler: stock codegen plus mono's side tables.
 *
 * ORC's SimpleCompiler is TargetMachine::addPassesToEmitMC plus PassManager::run
 * and nothing else. What mono needs from codegen that the stock recipe cannot
 * give it is a seat between the machine passes and the AsmPrinter: the clause
 * gather has to see the final landing-pad set, and the side tables have to be
 * written into the object while the streamer is still open, with the code
 * offsets as label differences the writer folds at layout. So the recipe is
 * restated here, faithfully, with those two additions.
 *
 * The gather pass is shared with the tiered backend (passes/eh-gather.cpp),
 * as is the `.mono_lsda` format its reader parses. The `.mono_unwind` section is this backend's own: the CFI
 * program LLVM tracked for the function, recorded at the MC layer where it is
 * still target-neutral semantics rather than DWARF bytes.
 */

/*
 * LLVM uses `PIC` as an identifier (PassInstrumentationCallbacks); mono's build
 * defines it as a macro.
 */
#ifdef PIC
#undef PIC
#endif

#include "compiler.hpp"

#include "jit.hpp"
#include "sidetables.hpp"
#include "timing.hpp"

#include "eh-side-channel.hpp"
#include "passes/eh-gather.hpp"
#include "passes/finally-range.hpp"

#include <llvm/BinaryFormat/ELF.h>
#include <llvm/CodeGen/AsmPrinter.h>
#include <llvm/CodeGen/AsmPrinterHandler.h>
#include <llvm/CodeGen/MachineFunctionPass.h>
#include <llvm/CodeGen/MachineInstr.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/Pass.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/TargetPassConfig.h>
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

/// Marks where the pass manager starts on a function, ahead of the IR passes
/// codegen runs and of ISel; the mark after ISel closes it.
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

/// Marks the point ISel is finished with a function and the machine passes are
/// about to start on it; the mark ahead of the printer closes it.
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

/// Marks the start of the AsmPrinter over one function; the side-table pass,
/// which the legacy pass manager runs immediately after the printer, closes it.
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
 * Collects the rows `.mono_lines` is written from, as the printer walks each
 * function's instructions.
 *
 * The translator gives every instruction a debug location whose line is the IL
 * offset in effect at it (il-line-table.hpp); all a row needs on top of that is
 * a label to take a difference against, so one is planted wherever the line
 * changes. That is the same thing a `.loc` directive would do, which is why the
 * module's compile unit can say NoDebug: nothing here wants a line table, and
 * without one no `.debug_*` section is produced at all.
 */
class IlLineHandler : public AsmPrinterHandler {
public:
	struct Row {
		const MCSymbol *at;
		uint32_t line;
	};

	struct Function {
		std::string name;
		std::vector<Row> rows;
	};

	explicit IlLineHandler (MCStreamer *streamer) : streamer_ (streamer) {}

	const std::vector<Function> &functions () const { return functions_; }

	void beginFunction (const MachineFunction *mf) override
	{
		const DISubprogram *sp = mf->getFunction ().getSubprogram ();

		functions_.push_back ({ mf->getName ().str (), {} });
		line_ = 0;

		/*
		 * The prologue carries no location of its own, and a frame stopped
		 * in it still has to name an IL offset, so the function opens at the
		 * subprogram's own line - the method's first IL byte.
		 */
		if (sp != nullptr)
			record (sp->getScopeLine ());
	}

	void beginInstruction (const MachineInstr *mi) override
	{
		if (mi->isMetaInstruction ())
			return;

		const DebugLoc &loc = mi->getDebugLoc ();

		if (loc)
			record (loc.getLine ());
	}

	void endFunction (const MachineFunction *) override {}
	void endModule () override {}

private:
	/*
	 * An instruction with no location leaves the line in effect alone rather
	 * than clearing it: the translator attributes what it emits, so a gap is
	 * codegen's own bookkeeping and belongs to whatever surrounds it.
	 */
	void record (unsigned line)
	{
		if (line == 0 || line == line_)
			return;

		MCSymbol *at = streamer_->getContext ().createTempSymbol ();

		streamer_->emitLabel (at);
		functions_.back ().rows.push_back ({ at, (uint32_t) line });
		line_ = line;
	}

	MCStreamer *streamer_;
	std::vector<Function> functions_;
	unsigned line_ = 0;
};

/// One `.mono_unwind` record: the wire form of one MCCFIInstruction.
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

/*
 * Writes both side tables at doFinalization, which the legacy pass manager runs
 * in reverse pass order: added after the AsmPrinter, this runs once every
 * function has been emitted but before the printer's own finalization closes
 * the streamer and writes the object.
 */
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

	/*
	 * This runs immediately after the AsmPrinter's own, so the interval since
	 * the marker pass ahead of the printer is the printer over one function.
	 */
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
	/*
	 * `.mono_lsda`, in the tiered backend's v2 format (mono_lsda.hpp), whose
	 * reader parses it back at load. The section carries no function identity -
	 * records are attributed to the one method the module holds - so a second
	 * clause-bearing function would silently misattribute geometry; abort
	 * instead.
	 */
	void emit_clause_table ()
	{
		MCStreamer &streamer = *streamer_;
		MCContext &ctx = streamer.getContext ();
		unsigned records = 0;

		for (const MonoEHFunctionClauses &fn : sc_->functions) {
			/*
			 * Declined by the gather. The reader treats an absent section as
			 * a refusal, never a publishable empty table.
			 */
			if (fn.declined)
				continue;

			/*
			 * A filter body compiled alongside its method carries no clauses
			 * of its own; the section describes the method alone.
			 */
			if (fn.function.find ("$filter") != std::string::npos)
				continue;

			if (++records > 1)
				report_fatal_error ("mono: multiple EH functions in one "
				                    "JIT module - .mono_lsda attribution "
				                    "is ambiguous");

			MCSymbol *begin = ctx.getOrCreateSymbol (fn.function);

			streamer.switchSection (ctx.getELFSection (
				".mono_lsda", ELF::SHT_PROGBITS, ELF::SHF_ALLOC));

			auto from_begin = [&] (const MCSymbol *sym) {
				return MCBinaryExpr::createSub (
					MCSymbolRefExpr::create (sym, ctx),
					MCSymbolRefExpr::create (begin, ctx), ctx);
			};

			streamer.emitIntValue (0x4d4c5344u, 4); /* 'MLSD' */
			streamer.emitIntValue (2, 2);
			streamer.emitIntValue (fn.clauses.size (), 2);

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

			for (const MonoEHFinallyBody *body : bodies) {
				streamer.emitIntValue ((uint32_t) body->clause_index, 4);
				streamer.emitValue (from_begin (body->body_begin), 4);
				streamer.emitValue (from_begin (body->body_end), 4);
				streamer.emitIntValue ((uint32_t) body->exvar_offset, 4);
				streamer.emitIntValue ((uint32_t) body->exvar_dwarf_reg, 4);
			}
		}
	}

	/*
	 * `.mono_unwind`: one block per function, each its CFI program with the
	 * initial frame state first. The rule offsets are label differences
	 * against the frame's begin label, which sits at the function's entry, so
	 * the writer folds them to constants; the begin label itself is emitted
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
		 * Only an object streamer plants the frame and rule labels; textual
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

	/*
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

	MCStreamer *streamer_;
	const MonoEHSideChannel *sc_;
	const IlLineHandler *lines_;
};

char SideTableEmitPass::ID;

/// What codegen writes to OUT: the ELF object the linker loads, or the assembly
/// text a human reads.
enum class OutputKind { object, assembly };

/*
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
 * A faithful restatement of TargetMachine::addPassesToEmitMC followed by
 * PassManager::run - the recipe SimpleCompiler drives - kept open so the gather
 * runs after addMachinePasses () and the side-table writer against the object
 * streamer. Any drift from the stock method between LLVM versions is a silent
 * codegen difference, so change this only against the current implementation.
 */
Error
emit_object (TargetMachine &tm, Module &m, raw_pwrite_stream &out,
             MonoEHSideChannel &side_channel, OutputKind kind)
{
	std::optional<legacy::PassManager> pm (std::in_place);
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
	 * After the machine passes and before the AsmPrinter, so it sees the final
	 * landing-pad set. Read-only: a module with no landing pads emits an
	 * object byte-identical to SimpleCompiler's.
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

	timed_part.reset ();
	timed_setup.reset ();
	{
		timing::Scope timed_run (timing::Phase::cgrun);

		g_object_started = 0;
		pm->run (m);
		timing::span_end (timing::Phase::objout, g_object_started);
	}
	{
		timing::Scope timed_free (timing::Phase::pmfree);

		pm.reset ();
	}
	return Error::success ();
}

/// The directory MONO_LLVM_JIT_ASM_DIR names, or an empty string if it names
/// none. A dump goes to one file for each method there instead of to stderr.
StringRef
asm_dump_directory ()
{
	static const char *dir = std::getenv ("MONO_LLVM_JIT_ASM_DIR");

	return dir != nullptr ? StringRef (dir) : StringRef ();
}

/// Whether to dump this module, whose identifier is the full name of the one
/// method it holds. MONO_LLVM_JIT_ASM keeps the methods whose name contains it.
/// A directory with no filter beside it takes every method that reaches codegen.
bool
dumping_asm (StringRef module_name)
{
	static const char *filter = std::getenv ("MONO_LLVM_JIT_ASM");

	if (filter == nullptr)
		return !asm_dump_directory ().empty ();

	return module_name.contains (filter);
}

/*
 * A method name holds characters a path cannot carry - `/` in a generic
 * argument, and the spaces of a signature - so each run of them becomes one
 * underscore. Long names are cut to leave room for the extension and for the
 * uniquing suffix, since a file name has 255 bytes on the file systems this
 * runs on.
 */
std::string
asm_file_stem (StringRef module_name)
{
	constexpr size_t max_stem = 200;
	std::string stem;
	bool last_was_separator = false;

	for (char c : module_name.take_front (max_stem)) {
		if (isalnum (static_cast<unsigned char> (c)) || c == '.' || c == '-') {
			stem.push_back (c);
			last_was_separator = false;
		} else if (!last_was_separator) {
			stem.push_back ('_');
			last_was_separator = true;
		}
	}

	return stem.empty () ? std::string ("method") : stem;
}

/*
 * Open the file a method's assembly goes to, under the directory
 * MONO_LLVM_JIT_ASM_DIR names.
 *
 * Two methods can want one name: a generic instantiation differs from another
 * only in characters the stem drops, and MONO_LLVM_JIT_RECOMPILE gives one
 * method several compiles. So this counts up a suffix until a name is free.
 * CD_CreateNew is what makes that safe while several compile threads run - the
 * loser of a race gets EEXIST and takes the next number, rather than the two
 * writing over one file.
 */
Expected<std::unique_ptr<raw_fd_ostream>>
open_asm_file (StringRef module_name)
{
	StringRef dir = asm_dump_directory ();
	std::string stem = asm_file_stem (module_name);

	if (std::error_code ec = sys::fs::create_directories (dir))
		return createStringError (ec, "cannot create %s", dir.str ().c_str ());

	for (unsigned attempt = 0; attempt < 10000; attempt++) {
		SmallString<256> path (dir);
		std::string name = attempt == 0
		                 ? stem + ".s"
		                 : stem + "." + std::to_string (attempt) + ".s";
		int fd = -1;

		sys::path::append (path, name);

		std::error_code ec = sys::fs::openFileForWrite (
			path, fd, sys::fs::CD_CreateNew, sys::fs::OF_Text);
		if (!ec)
			return std::make_unique<raw_fd_ostream> (fd, /*shouldClose=*/true);
		if (ec != std::errc::file_exists)
			return createStringError (ec, "cannot write %s", path.c_str ());
	}

	return createStringError (std::make_error_code (std::errc::file_exists),
	                          "%s already holds 10000 files named for %s",
	                          dir.str ().c_str (), stem.c_str ());
}

/*
 * Write the assembly M compiles to - side-table sections included, which is the
 * half no offline llc run can reproduce. It goes to a file of its own under
 * MONO_LLVM_JIT_ASM_DIR, or to stderr when that variable names no directory.
 *
 * Codegen consumes the module it is given, and the MCContext, the MMI and the
 * streamer are entangled with the single run they were built for, so this
 * compiles a clone and leaves the caller's module for the object run. Being a
 * separate run also means nothing here can change the code that gets published:
 * a bug in the printout stays a bug in the printout. The side channel it fills
 * is discarded for the same reason.
 */
Error
dump_assembly (TargetMachine &tm, const Module &m)
{
	std::unique_ptr<Module> copy = CloneModule (m);
	MonoEHSideChannel side_channel;
	std::unique_ptr<raw_fd_ostream> file;
	raw_pwrite_stream *out = &errs ();

	if (!asm_dump_directory ().empty ()) {
		Expected<std::unique_ptr<raw_fd_ostream>> opened
			= open_asm_file (m.getModuleIdentifier ());
		if (!opened)
			return opened.takeError ();
		file = std::move (*opened);
		out = file.get ();
	}

	*out << "*** assembly for " << m.getModuleIdentifier () << " ***\n";
	return emit_object (tm, *copy, *out, side_channel, OutputKind::assembly);
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
	TargetMachine &tm = m.getModuleFlag ("mono.tier2") != nullptr
	                            ? tier2_target_machine ()
	                            : host_target_machine ();

	if (dumping_asm (m.getName ()))
		if (Error err = dump_assembly (tm, m))
			return std::move (err);

	{
		raw_svector_ostream stream (buffer);
		if (Error err = emit_object (tm, m, stream, side_channel, OutputKind::object))
			return std::move (err);
	}

	return std::make_unique<SmallVectorMemoryBuffer> (
		std::move (buffer), m.getModuleIdentifier () + "-jitted-objectbuffer",
		/*RequiresNullTerminator=*/false);
}

} // namespace mono
