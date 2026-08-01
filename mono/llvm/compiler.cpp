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
 * The gather pass is shared with the tiered backend
 * (mono/mini/llvm/passes/eh-gather.cpp), as is the `.mono_lsda` format its
 * reader parses. The `.mono_unwind` section is this backend's own: the CFI
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

#include "sidetables.hpp"

#include "../mini/llvm/engine.hpp"
#include "../mini/llvm/passes/eh-gather.hpp"

#include <llvm/BinaryFormat/ELF.h>
#include <llvm/CodeGen/AsmPrinter.h>
#include <llvm/CodeGen/MachineFunctionPass.h>
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
#include <llvm/MC/MCObjectWriter.h>
#include <llvm/MC/MCSectionELF.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Target/TargetMachine.h>

using namespace llvm;

namespace mono {
namespace {

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

	SideTableEmitPass (MCStreamer *streamer, const MonoEHSideChannel *sc)
		: MachineFunctionPass (ID), streamer_ (streamer), sc_ (sc)
	{
	}

	StringRef getPassName () const override { return "Mono side-table emission"; }

	bool runOnMachineFunction (MachineFunction &) override { return false; }

	void getAnalysisUsage (AnalysisUsage &au) const override
	{
		au.setPreservesAll ();
		MachineFunctionPass::getAnalysisUsage (au);
	}

	bool doFinalization (Module &) override
	{
		emit_clause_table ();
		emit_unwind_table ();
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
	 * `.mono_unwind`: the function's CFI program, initial frame state first.
	 * The offsets are label differences against the frame's begin label, which
	 * sits at the function's entry, so the writer folds them to constants.
	 */
	void emit_unwind_table ()
	{
		MCStreamer &streamer = *streamer_;
		MCContext &ctx = streamer.getContext ();
		ArrayRef<MCDwarfFrameInfo> frames = streamer.getDwarfFrameInfos ();

		if (frames.empty ())
			return;
		if (frames.size () > 1)
			report_fatal_error ("mono: multiple frames in one JIT module - "
			                    ".mono_unwind attribution is ambiguous");

		const MCDwarfFrameInfo &frame = frames.front ();
		const std::vector<MCCFIInstruction> &initial =
			ctx.getAsmInfo ()->getInitialFrameState ();

		streamer.switchSection (ctx.getELFSection (
			".mono_unwind", ELF::SHT_PROGBITS, ELF::SHF_ALLOC));

		streamer.emitIntValue (unwind_section_magic, 4);
		streamer.emitIntValue (unwind_section_version, 2);
		streamer.emitIntValue (0, 2);
		streamer.emitIntValue (initial.size () + frame.Instructions.size (), 4);

		auto emit_record = [&] (const UnwindRecord &r, bool at_entry) {
			if (at_entry || r.at == nullptr) {
				streamer.emitIntValue (0, 4);
			} else {
				streamer.emitValue (
					MCBinaryExpr::createSub (
						MCSymbolRefExpr::create (r.at, ctx),
						MCSymbolRefExpr::create (frame.Begin, ctx),
						ctx),
					4);
			}
			streamer.emitIntValue (r.op, 1);
			streamer.emitIntValue (static_cast<uint32_t> (r.reg), 4);
			streamer.emitIntValue (static_cast<uint64_t> (r.value), 8);
		};

		for (const MCCFIInstruction &i : initial)
			emit_record (transcribe_cfi (i), /*at_entry=*/true);
		for (const MCCFIInstruction &i : frame.Instructions)
			emit_record (transcribe_cfi (i), /*at_entry=*/false);
	}

	MCStreamer *streamer_;
	const MonoEHSideChannel *sc_;
};

char SideTableEmitPass::ID;

/*
 * A faithful restatement of TargetMachine::addPassesToEmitMC followed by
 * PassManager::run - the recipe SimpleCompiler drives - kept open so the gather
 * runs after addMachinePasses () and the side-table writer against the object
 * streamer. Any drift from the stock method between LLVM versions is a silent
 * codegen difference, so change this only against the current implementation.
 */
Error
emit_object (TargetMachine &tm, Module &m, raw_pwrite_stream &out,
             MonoEHSideChannel &side_channel)
{
	legacy::PassManager pm;

	auto *mmiwp = new MachineModuleInfoWrapperPass (&tm);
	TargetPassConfig *tpc = tm.createPassConfig (pm);

	tpc->setDisableVerify (true);
	pm.add (tpc);
	pm.add (mmiwp);
	if (tpc->addISelPasses ())
		return make_error<StringError> (
			"target does not support instruction selection",
			inconvertibleErrorCode ());
	tpc->addMachinePasses ();

	/*
	 * After the machine passes and before the AsmPrinter, so it sees the final
	 * landing-pad set. Read-only: a module with no landing pads emits an
	 * object byte-identical to SimpleCompiler's.
	 */
	pm.add (new MonoEHGatherPass (&side_channel));

	tpc->setInitialized ();

	/*
	 * The AsmPrinter must emit into the MCContext the MMI created - the
	 * external-context contract addPassesToEmitMC relies on, and what lets the
	 * side-table writer share the same context and symbols.
	 */
	MCContext *ctx = &mmiwp->getMMI ().getContext ();

	const MCSubtargetInfo &sti = *tm.getMCSubtargetInfo ();
	const MCRegisterInfo &mri = *tm.getMCRegisterInfo ();
	std::unique_ptr<MCCodeEmitter> mce (
		tm.getTarget ().createMCCodeEmitter (*tm.getMCInstrInfo (), *ctx));
	std::unique_ptr<MCAsmBackend> mab (
		tm.getTarget ().createMCAsmBackend (sti, mri, tm.Options.MCOptions));
	if (!mce || !mab)
		return make_error<StringError> ("target does not support MC emission",
		                                inconvertibleErrorCode ());

	/*
	 * A plain MCELFStreamer, reproducing exactly what createMCObjectStreamer
	 * does for ELF.
	 */
	std::unique_ptr<MCObjectWriter> ow = mab->createObjectWriter (out);
	auto elf_streamer = std::make_unique<MCELFStreamer> (
		*ctx, std::move (mab), std::move (ow), std::move (mce));
	if (tm.Options.MCOptions.MCRelaxAll)
		elf_streamer->getAssembler ().setRelaxAll (true);
	MCStreamer *streamer_ptr = elf_streamer.get ();
	std::unique_ptr<MCStreamer> streamer (std::move (elf_streamer));

	AsmPrinter *printer = tm.getTarget ().createAsmPrinter (tm, std::move (streamer));
	if (!printer)
		return make_error<StringError> ("target does not support an AsmPrinter",
		                                inconvertibleErrorCode ());
	pm.add (printer);

	/*
	 * After the printer on purpose: doFinalization runs in reverse pass order,
	 * so the side tables are written while the streamer is still open, before
	 * the AsmPrinter's own finalization ends the object.
	 */
	pm.add (new SideTableEmitPass (streamer_ptr, &side_channel));

	pm.run (m);
	return Error::success ();
}

} // namespace

MethodObjectCompiler::MethodObjectCompiler (orc::JITTargetMachineBuilder jtmb)
	: IRCompiler (orc::irManglingOptionsFromTargetOptions (jtmb.getOptions ())),
	  jtmb_ (std::move (jtmb))
{
}

Expected<std::unique_ptr<MemoryBuffer>>
MethodObjectCompiler::operator() (Module &m)
{
	Expected<std::unique_ptr<TargetMachine>> tm = jtmb_.createTargetMachine ();
	if (!tm)
		return tm.takeError ();

	MonoEHSideChannel side_channel;
	SmallVector<char, 0> buffer;
	{
		raw_svector_ostream stream (buffer);
		if (Error err = emit_object (**tm, m, stream, side_channel))
			return std::move (err);
	}

	return std::make_unique<SmallVectorMemoryBuffer> (
		std::move (buffer), m.getModuleIdentifier () + "-jitted-objectbuffer",
		/*RequiresNullTerminator=*/false);
}

} // namespace mono
