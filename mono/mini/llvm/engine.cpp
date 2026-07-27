/**
 * \file
 * engine.cpp - ORCv2 in-process JIT engine for unmodified system LLVM 18.
 *
 * Replaces the execution-engine half of the legacy mono/mini/llvm-jit.cpp. See
 * engine.hpp for the adapt-vs-rewrite rationale (short version: the donor's
 * ORCv1 legacy layers do not exist in LLVM 18, so this is an LLJIT/ORCv2
 * rewrite that preserves the donor's *external* contract - the mono_llvm_*
 * entry points at the bottom of this file - so step 3b's translator links
 * against it unchanged).
 *
 * Two parts:
 *   1. The pure-LLVM engine core (class mono::MonoLLVMJIT and the JITLink
 *      object-linking plugin MonoObjectLinkingPlugin).
 *   2. The extern "C" mono boundary: thin adapters that unwrap the llvm-c
 *      handles the translator passes and forward to the core.
 *
 * The engine's tests are NOT here. They used to be - three selftests behind a
 * single extern "C" entry point, compiled into libmono in every build - and are
 * now mono/unit-tests/test-llvm-engine.cpp, which drives the engine through
 * engine.hpp and reports each check independently.
 */

/*
 * libtool compiles this TU with -DPIC (position-independent code). LLVM's
 * PassBuilder.h uses `PIC` as an identifier (PassInstrumentationCallbacks), so
 * the macro would rewrite it and break the header. engine.cpp has no use for
 * mono's PIC macro, so drop it before any LLVM header is seen.
 */
#ifdef PIC
#undef PIC
#endif

#include "engine.hpp"
#include "mono_lsda_format.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/CodeGen/AsmPrinter.h>
#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineFunctionPass.h>
#include <llvm/CodeGen/MachineInstrBuilder.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/ExecutionEngine/JITLink/EHFrameSupport.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h>
#include <llvm/ExecutionEngine/JITLink/x86_64.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/MapperJITLinkMemoryManager.h>
#include <llvm/ExecutionEngine/Orc/MemoryMapper.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/MC/MCAsmBackend.h>
#include <llvm/MC/MCAssembler.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCELFStreamer.h>
#include <llvm/MC/MCExpr.h>
#include <llvm/MC/MCObjectWriter.h>
#include <llvm/MC/MCSectionELF.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/MCSymbol.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/Utils/Cloning.h>

/*
 * The custom passes. inliner.hpp reaches into mono proper (mini.h) for
 * MonoCompile/MonoMethod, and mini.h's DW_* unwind macros collide with
 * llvm/BinaryFormat/Dwarf.h, so these have to come after the LLVM headers.
 */
#include "passes/eh-gather.hpp"
#include "passes/elide-class-init.hpp"
#include "passes/finally-range.hpp"
#include "passes/inliner.hpp"
#include "passes/pass-dump.hpp"

using namespace llvm;
using namespace llvm::orc;

namespace mono {

/*
 * Facts about one emitted object that are only visible at materialization time.
 *
 * WHY THE ELF SYMBOL SIZE for code_size, and not the two obvious alternatives:
 *
 *   - NOT the .eh_frame FDE pc_range. That is a whole SECTION, so an FDE would
 *     still have to be parsed and matched to the right function; worse, LLVM
 *     emits no FDE at all for a nounwind leaf, which lands right back on zero.
 *   - NOT the RTDyld code-section allocation size. A module can hold more than
 *     one function (mono's modules carry the GC safepoint poll alongside the
 *     method), so the section over-reports the entry function's extent.
 *
 * st_size is exactly the function's byte length: for ELF targets LLVM's
 * AsmPrinter emits `.size <fn>, .-<fn>` around every function body, so the
 * symbol table is authoritative and per-function.
 */
struct ObjectInfo {
	/* Function symbol name -> machine-code size. */
	std::map<std::string, uint64_t> func_sizes;
	EhFrameInfo eh_frame;
	/*
	 * The loaded `.llvm_stackmaps` section, or {nullptr,0} if the module emitted
	 * none. Present only for gshared methods, where the translator plants a
	 * llvm.experimental.stackmap recording the home slot of this/mrgctx; task #15
	 * parses it back into cfg->llvm_this_reg/offset. Same {addr,size} shape as
	 * eh_frame, captured by the same section-name loop.
	 */
	EhFrameInfo stackmaps;
	/*
	 * The loaded `.mono_lsda` section, or {nullptr,0} if the module emitted none.
	 * This is the target-neutral, SHF_ALLOC clause table MonoLSDAStreamer writes
	 * from the C2 gather side channel (magic 'MLSD', code-relative offsets; plan
	 * 12 2). Present only for an EH-bearing method with resolved catch clauses;
	 * C4/C6 parse it into the method's MonoJitExceptionInfo[]. Same {addr,size}
	 * shape as eh_frame, captured by the same section-name loop. Because both the
	 * try/handler labels and the func_begin anchor sit in .text, the object writer
	 * folds every offset to a constant - the section carries NO relocations.
	 */
	EhFrameInfo mono_lsda;
};

/* ---- relocation audit ----------------------------------------------------
 *
 * See engine.hpp for what this guards and why the emitted relocations, rather
 * than the target machine's reported code model, are the observable that is
 * checked.
 */

/*
 * Does this x86-64 relocation drive an address - or a displacement to one -
 * through a field narrower than 64 bits, with no indirection wide enough to
 * absorb the range? In this engine every JIT section is mmap'd by a stock
 * SectionMemoryManager with no low-address guarantee, and code, rodata,
 * eh_frame and data land in separate slabs an unbounded distance apart. So any
 * 32-bit (or narrower) absolute address, and any 32-bit displacement between
 * two independently-mapped sections, can overflow its field - and
 * RuntimeDyldELF resolves those forms behind a bare assert(isInt<32>()) that is
 * compiled out in the shipped LLVM, so the overflow is written silently. One
 * concrete case: the small model encodes each FDE pc-begin as a PC32 from the
 * .eh_frame slab to the code slab (the large model emits PC64 there).
 *
 * The only non-obvious members are the GOT/PLT/TLS-GD forms below. RuntimeDyld
 * does interpose - a stub for PLT32, a GOT entry for GOTPCREL* and the GD/IE
 * TLS forms - but it allocates that GOT through allocateDataSection() into the
 * RWData slab group, a different mmap group from the CodeMem the referring code
 * lives in, and rewrites the reference into a PC32 against it. That code->GOT
 * displacement is 32-bit and cross-group, so it hits the same assert; the
 * indirection does not help. By contrast GOT32/GOTOFF64 are offsets within the
 * GOT, SIZE32/SIZE64 are symbol sizes, and DTPOFF32/TPOFF32 are TLS-block /
 * thread-pointer offsets - none is a mapped address, so none truncates.
 *
 * The switch is exhaustive over the defined R_X86_64_* set (llvm's
 * ELFRelocs/x86_64.def) so that widening coverage is always a visible edit;
 * default catches only types no psABI defines and, like a target this analysis
 * has not covered, is reported as not-truncating rather than guessed unsafe.
 *
 * Non-x86-64 targets are not classified at all; see audit_relocations().
 */
static bool
x86_64_reloc_truncates_address (uint64_t type)
{
	switch (type) {
	/* Narrow absolute or PC-relative addresses/displacements. */
	case ELF::R_X86_64_8:
	case ELF::R_X86_64_PC8:
	case ELF::R_X86_64_16:
	case ELF::R_X86_64_PC16:
	case ELF::R_X86_64_32:
	case ELF::R_X86_64_32S:
	case ELF::R_X86_64_PC32:
	/* 32-bit displacements to a stub or GOT/TLS entry in another mmap slab. */
	case ELF::R_X86_64_PLT32:
	case ELF::R_X86_64_GOTPCREL:
	case ELF::R_X86_64_GOTPCRELX:
	case ELF::R_X86_64_REX_GOTPCRELX:
	case ELF::R_X86_64_GOTPC32:
	case ELF::R_X86_64_TLSGD:
	case ELF::R_X86_64_TLSLD:
	case ELF::R_X86_64_GOTTPOFF:
	case ELF::R_X86_64_GOTPC32_TLSDESC:
		return true;

	/* Full 64-bit addresses or displacements - resolved at full width. */
	case ELF::R_X86_64_64:
	case ELF::R_X86_64_PC64:
	case ELF::R_X86_64_GOT64:
	case ELF::R_X86_64_GOTPCREL64:
	case ELF::R_X86_64_GOTPC64:
	case ELF::R_X86_64_GOTPLT64:
	case ELF::R_X86_64_GOTOFF64:
	case ELF::R_X86_64_PLTOFF64:
	/* 64-bit TLS module id / offsets, filled at full width. */
	case ELF::R_X86_64_DTPMOD64:
	case ELF::R_X86_64_DTPOFF64:
	case ELF::R_X86_64_TPOFF64:
	case ELF::R_X86_64_TLSDESC:
	/* Offsets and sizes, not addresses. */
	case ELF::R_X86_64_GOT32:
	case ELF::R_X86_64_DTPOFF32:
	case ELF::R_X86_64_TPOFF32:
	case ELF::R_X86_64_SIZE32:
	case ELF::R_X86_64_SIZE64:
	/* Full-width dynamic-linker relocs, or no field written at all. */
	case ELF::R_X86_64_RELATIVE:
	case ELF::R_X86_64_IRELATIVE:
	case ELF::R_X86_64_GLOB_DAT:
	case ELF::R_X86_64_JUMP_SLOT:
	case ELF::R_X86_64_COPY:
	case ELF::R_X86_64_TLSDESC_CALL:
	case ELF::R_X86_64_NONE:
		return false;

	/* No psABI-defined x86-64 type reaches here; treat an unknown type as
	 * unclassified (not truncating), matching audit_relocations()'s contract. */
	default:
		return false;
	}
}

/*
 * Classify every relocation in `obj`. Declared in engine.hpp.
 *
 * Returns an all-zero audit for anything that is not x86-64 ELF - not because
 * other targets are safe, but because the unsafe relocation set is per-ABI and
 * only x86-64 has been analysed. A port must extend
 * x86_64_reloc_truncates_address() (and the code-model choice below) before
 * this says anything about it. An all-zero audit is a "did not look", and
 * test-llvm-engine.cpp treats it as such rather than as a pass.
 *
 * This reads raw ELF Rela entries straight out of addPassesToEmitFile()'s
 * output, BEFORE JITLink ever sees the object - it has no visibility into the
 * GOT/PLT synthesis JITLink's table managers perform (see
 * accumulate_reloc_audit_from_graph() below), so its "truncating" label
 * describes ELF *codegen* shape only, not post-link safety.
 * accumulate_reloc_audit_from_graph() is the authority on the latter once
 * JITLink has run; the two are not required to agree, and under Small+PIC
 * they will not - a PLT32/REX_GOTPCRELX relocation this function correctly
 * calls truncating is routinely resolved safely through an in-graph stub or
 * GOT slot by the time JITLink is done with it. That is expected, not a bug:
 * this function's callers (test_reloc_widths's small/large probes) use it to
 * prove the ELF codegen shape differs between code models, independent of
 * what JITLink later does with the result.
 */
RelocAudit
audit_relocations (const object::ObjectFile &obj)
{
	RelocAudit audit;

	const auto *elf = dyn_cast<object::ELFObjectFileBase> (&obj);
	if (!elf || elf->getEMachine () != ELF::EM_X86_64)
		return audit;

	for (const object::SectionRef &sec : obj.sections ()) {
		for (const object::RelocationRef &rel : sec.relocations ()) {
			audit.total++;
			if (!x86_64_reloc_truncates_address (rel.getType ()))
				continue;
			audit.truncating++;
			if (!audit.first_offender.empty ())
				continue;

			SmallString<32> type_name;
			rel.getTypeName (type_name);
			std::string sec_name = "?";
			if (Expected<StringRef> n = sec.getName ())
				sec_name = n->str ();
			else
				consumeError (n.takeError ());
			audit.first_offender = sec_name + "/" + type_name.c_str ();
		}
	}
	return audit;
}

/*
 * Running total across every JIT-emitted object, so the unit test can assert
 * on real compiled output rather than on a reconstruction. Accumulated from
 * the NotifyLoaded hook, which is why it is a lock-guarded global and not
 * thread-local (same reasoning as g_object_info below).
 */
static std::mutex g_reloc_audit_mutex;
static RelocAudit g_jit_reloc_audit;

/*
 * The JITLink-edge analogue of x86_64_reloc_truncates_address(). The engine now
 * links through JITLink, which resolves the ELF relocations LLVM emitted into
 * its own Edge::Kind set (llvm/ExecutionEngine/JITLink/x86_64.h) - so the
 * always-on runtime audit classifies edge kinds rather than raw ELF relocation
 * types. The classification is the SAME property: does this edge drive an
 * address, or a displacement to one, through a field narrower than 64 bits?
 *
 * Under the Large code model this engine pins, cross-section references stay
 * 64-bit (Pointer64 / Delta64 / NegDelta64), so nothing here fires - the exact
 * parity the reloc-widths unit test asserts. The 32-bit and narrower forms are
 * listed truncating for the same reason their ELF counterparts were: JITLink's
 * InProcessMemoryManager offers no in-window placement guarantee under Large,
 * and a 32-bit field cannot span an arbitrary inter-segment distance.
 *
 * The caller applies this ONLY to edges that cross a placement boundary (target
 * external, absolute, or in a different section than the referring block). An
 * intra-section narrow delta is always in range - the section is one contiguous
 * block - and is never a hazard; the RTDyld-era ELF audit never saw those at all
 * because the assembler folded same-section references to constants before any
 * relocation was emitted, whereas JITLink re-materializes them as edges (notably
 * the FDE->CIE NegDelta32 pointer inside .eh_frame). Gating on cross-section
 * keeps this audit's answer identical to the ELF audit's.
 *
 * Unlike RTDyld's silent assert(isInt<32>()), JITLink RANGE-CHECKS every 32-bit
 * edge and hard-errors on overflow rather than truncating - so this audit is now
 * a belt-and-braces diagnostic on top of a linker that already cannot corrupt an
 * address silently. It is kept because it names the offending edge before the
 * link would fail, and because it is the observable the reloc-widths test reads.
 *
 * Under Small+PIC (see host_target_machine_builder()) this returns true for
 * the overwhelming majority of edges JITLink resolves - GOT loads, PLT stubs,
 * the .eh_frame PC32 FDE pc-begin field - because that is what Small+PIC
 * codegen and linking legitimately look like. That is now the expected,
 * common case, not itself a hazard signal: this function only answers "is the
 * field narrow", never "is the reference safe". See
 * accumulate_reloc_audit_from_graph() below for the boundary test that turns
 * the answer into the actual hazard signal.
 */
static bool
x86_64_edge_kind_truncates (jitlink::Edge::Kind kind)
{
	using namespace llvm::jitlink::x86_64;
	switch (kind) {
	/* Narrow absolute addresses, deltas and PC-relative displacements. */
	case Pointer32:
	case Pointer32Signed:
	case Pointer16:
	case Pointer8:
	case Delta32:
	case NegDelta32:
	case BranchPCRel32:
	case BranchPCRel32ToPtrJumpStub:
	case BranchPCRel32ToPtrJumpStubBypassable:
	case PCRel32:
	case PCRel32GOTLoadRelaxable:
	case PCRel32GOTLoadREXRelaxable:
	case PCRel32TLVPLoadREXRelaxable:
	/* GOT/TLS requests that transform INTO a 32-bit form. */
	case RequestGOTAndTransformToDelta32:
	case RequestGOTAndTransformToPCRel32GOTLoadREXRelaxable:
	case RequestGOTAndTransformToPCRel32GOTLoadRelaxable:
	case RequestTLSDescInGOTAndTransformToDelta32:
	case RequestTLVPAndTransformToPCRel32TLVPLoadREXRelaxable:
		return true;

	/* Full 64-bit addresses or displacements - resolved at full width. */
	case Pointer64:
	case Delta64:
	case NegDelta64:
	case Delta64FromGOT:
	case RequestGOTAndTransformToDelta64:
	case RequestGOTAndTransformToDelta64FromGOT:
		return false;

	/* An x86_64 edge kind this analysis has not covered - treat as
	 * unclassified (not truncating), matching audit_relocations()'s contract. */
	default:
		return false;
	}
}

/*
 * The classification shared by the always-on runtime audit
 * (accumulate_reloc_audit_from_graph() below) and the test-only
 * audit_relocations_graph() (engine.hpp): scans every relocation edge in `g`
 * and tallies into `one`. A non-x86-64 graph is left unclassified (see
 * audit_relocations()'s contract) - `one` comes back all-zero.
 *
 * A narrow field is only a truncation hazard when its target is genuinely
 * external or absolute - i.e. NOT resolved within this LinkGraph. Under
 * Small+PIC (see host_target_machine_builder()) that is the only case left
 * once JITLink's PLTTableManager/GOTTableManager have run: both are
 * default-added PostPrunePasses (ELF_x86_64.cpp's buildTables_ELF_x86_64,
 * pushed by link_ELF_x86_64 before it calls Ctx->modifyPassConfig(), i.e.
 * unconditionally ahead of anything this engine's own modifyPassConfig()
 * adds), and both reroute every edge whose target is not defined in this
 * graph onto an in-graph stub or GOT slot (llvm/include/llvm/ExecutionEngine/
 * JITLink/x86_64.h, PLTTableManager::visitEdge / GOTTableManager::visitEdge)
 * well before either PostAllocationPasses or PreFixupPasses run. The GOT
 * slot's own outgoing edge to the real (possibly far) target is
 * unconditionally Pointer64 - full width, never flagged truncating - so the
 * only things a narrow edge can still point at, post-table-managers, are
 * either that safe indirection or a target this pass never sees at all.
 *
 * THIS IS WHY THE CALLER REGISTERS THIS AS A PostAllocationPasses ENTRY, NOT
 * A PreFixupPasses ONE (see MonoObjectLinkingPlugin::modifyPassConfig()) -
 * i.e. BEFORE x86_64::optimizeGOTAndStubAccesses, not after. That pass (also
 * PreFixupPasses, added by the same default target config) RELAXES a
 * BranchPCRel32ToPtrJumpStubBypassable edge straight back to a bare
 * BranchPCRel32 pointing at the ORIGINAL (still external/absolute) target
 * whenever it computes the real, now-resolved displacement as in-range - a
 * pure optimization, verified safe at the moment it fires, per the design.
 * But once that relaxation has happened, the edge is STRUCTURALLY IDENTICAL
 * (same Kind, same target) to a hypothetical raw narrow edge that was never
 * routed through a table manager at all - this classification cannot tell
 * the two apart from Kind + target alone. Running BEFORE relaxation avoids
 * that ambiguity entirely: at PostAllocationPasses time every genuinely
 * external/absolute reference is STILL in its table-manager-converted,
 * always-safe stub/GOT form (defined target), so the boundary check never
 * has anything to second-guess. (Confirmed empirically: an earlier version of
 * this pass ran PreFixup/post-relaxation and fired a real, reproducible false
 * positive on the actual corpus - not the unit tests - on `call memmove`,
 * whenever libc happened to land within 2 GB of the JIT's own mmap region
 * under stock ASLR, which relaxation legitimately (and safely) exploits.)
 *
 * A same-graph target in a DIFFERENT SECTION from the referring block (e.g.
 * .text -> $__STUBS, $__STUBS -> $__GOT, .text -> a co-located data global -
 * exactly get_jit_callee()'s tramp-var load pattern - or .eh_frame -> .text)
 * is therefore NOT a hazard and is not counted: one LinkGraph is one compiled
 * method, and JITLink allocates every section of one graph from a single
 * allocate() call, so any same-graph reference is provably in-window
 * regardless of section. (Probed directly: a synthetic module exercising
 * exactly these same-graph cross-section shapes, run to completion including
 * a genuinely ~34 TB-away external call, gave total=18,
 * old-section-boundary-truncating=10, new-graph-boundary-truncating=0 - all
 * 10 of the old predicate's hits were same-graph cross-section edges, and all
 * 18 were, in fact, safe. See .claude/scratch/jitlink-j5/ for the probe.)
 */
static void
classify_reloc_audit_from_graph (jitlink::LinkGraph &g, RelocAudit &one)
{
	if (g.getTargetTriple ().getArch () != Triple::x86_64)
		return;

	for (auto *block : g.blocks ()) {
		for (const jitlink::Edge &edge : block->edges ()) {
			if (!edge.isRelocation ())
				continue;
			one.total++;
			if (!x86_64_edge_kind_truncates (edge.getKind ()))
				continue;
			const jitlink::Symbol &target = edge.getTarget ();
			bool cross_boundary = !target.isDefined () || target.isAbsolute ();
			if (!cross_boundary)
				continue;
			one.truncating++;
			if (one.first_offender.empty ())
				one.first_offender = block->getSection ().getName ().str ()
				                     + "/" + g.getEdgeKindName (edge.getKind ());
		}
	}
}

/*
 * The always-on runtime audit, run as a JITLink PostAllocation pass (see
 * MonoObjectLinkingPlugin::modifyPassConfig() - deliberately NOT PreFixup;
 * classify_reloc_audit_from_graph()'s doc comment above explains why) over
 * every object this engine links. Replaces the RTDyld NotifyLoaded path:
 * instead of re-reading raw ELF relocations, it scans the LinkGraph's
 * table-manager-synthesized edges (GOT/PLT already built, just not yet
 * relaxed or fixed up) and accumulates into the same process-global tally the
 * reloc-widths test reads. The classification itself lives in
 * classify_reloc_audit_from_graph() above, shared with the test-only
 * audit_relocations_graph() (engine.hpp).
 */
static void
accumulate_reloc_audit_from_graph (jitlink::LinkGraph &g)
{
	RelocAudit one;
	classify_reloc_audit_from_graph (g, one);

	std::lock_guard<std::mutex> lock (g_reloc_audit_mutex);
	bool first = g_jit_reloc_audit.truncating == 0 && one.truncating != 0;
	g_jit_reloc_audit.total += one.total;
	g_jit_reloc_audit.truncating += one.truncating;
	if (g_jit_reloc_audit.first_offender.empty ())
		g_jit_reloc_audit.first_offender = one.first_offender;

	/*
	 * Say so once. JITLink hard-errors before it would truncate, so this is a
	 * named warning ahead of a link that would otherwise fail with a less
	 * legible out-of-range diagnostic - not a post-mortem. Warn rather than
	 * abort: the engine is a compiler backend and killing the process on a
	 * diagnostic is worse than a link failure the caller can decline on.
	 */
	if (first)
		fprintf (stderr, "mono llvm engine: WARNING: JIT object contains an "
		         "address-truncating relocation (%s); JIT sections are mapped "
		         "above 4 GB, so this may silently corrupt addresses\n",
		         g_jit_reloc_audit.first_offender.c_str ());
}

/* Declared in engine.hpp. */
RelocAudit
jit_reloc_audit ()
{
	std::lock_guard<std::mutex> lock (g_reloc_audit_mutex);
	return g_jit_reloc_audit;
}

/*
 * Declared in engine.hpp. Test-only: runs the same classification
 * accumulate_reloc_audit_from_graph() uses on a caller-built LinkGraph and
 * returns the tally without touching g_jit_reloc_audit, so a unit test can
 * hand-build a LinkGraph exercising a specific cross-boundary shape (same-
 * graph cross-section, external, absolute, ...) and check the classifier's
 * answer directly instead of only observing it indirectly through a real
 * compile.
 */
RelocAudit
audit_relocations_graph (jitlink::LinkGraph &g)
{
	RelocAudit one;
	classify_reloc_audit_from_graph (g, one);
	return one;
}

/*
 * Collected object facts, keyed by the name of the JITDylib the module was
 * added to (compile() gives every module its own, uniquely named).
 *
 * DELIBERATELY NOT a thread_local. The object-linking plugin's capture passes
 * run on whichever thread materializes the module, which stops being the calling
 * thread the moment the JIT is configured with compile threads (what tiering
 * wants). A thread-local channel would then leave the caller reading an empty
 * value - a zero code_size - so the keyed map is what makes this survive that
 * change.
 */
static std::mutex g_object_info_mutex;
static std::map<std::string, ObjectInfo> g_object_info;

/*
 * Locate a live section by name in a linked graph and return where it landed,
 * {nullptr,0} if the section is absent or was dead-stripped (present container
 * but no surviving blocks - doc 26 P1: findSectionByName() keeps the empty
 * Section container after prune, so liveness is `blocks().empty()`, NOT a null
 * section). The address is the SectionRange start (final executor address, since
 * InProcessMemoryManager maps working memory in place) and the byte length is
 * SectionRange::getSize().
 */
static EhFrameInfo
capture_graph_section (jitlink::LinkGraph &g, StringRef want)
{
	EhFrameInfo r;
	jitlink::Section *sec = g.findSectionByName (want);
	if (!sec || sec->blocks ().empty ())
		return r;
	jitlink::SectionRange range (*sec);
	r.addr = range.getStart ().toPtr<uint8_t *> ();
	r.size = range.getSize ();
	return r;
}

/*
 * Keep a symbol-less, edge-less SHF_ALLOC metadata section alive across
 * jitlink::prune(). doc 26 P1/P3: the real ObjectLinkingLayer seeds liveness
 * only from the Scope::Default symbols the MaterializationResponsibility tracks,
 * so a section reachable by nothing (`.mono_lsda`: 0 symbols, 0 incoming edges)
 * or by only a local symbol (`.llvm_stackmaps`: local __LLVM_StackMaps, its
 * reloc outgoing) is dead-stripped - dropping the section mono's EH / gshared
 * this-slot recovery must read back. This runs as a PrePrune pass.
 *
 * For a section that already carries symbols (.llvm_stackmaps), setLive(true) on
 * them is the cleaner mitigation (no synthetic symbol); for a truly symbol-less
 * section (.mono_lsda) an anchor Symbol MUST be added. `seen` records that the
 * section was actually emitted this object, so the PostAllocation pass can assert
 * the keep-live held (present-at-PrePrune ==> non-empty range) - the negative
 * test that the dead-strip trap stays shut.
 */
static void
keep_section_live (jitlink::LinkGraph &g, StringRef name, StringRef anchor_name,
                   bool &seen)
{
	using namespace llvm::jitlink;
	Section *sec = g.findSectionByName (name);
	if (!sec || sec->blocks ().empty ())
		return;
	seen = true;

	/* Prefer marking existing symbols live over synthesizing an anchor. */
	bool any_symbol = false;
	for (Symbol *sym : sec->symbols ()) {
		sym->setLive (true);
		any_symbol = true;
	}
	if (any_symbol)
		return;

	/*
	 * No symbol to mark: add a live Scope::Local anchor at offset 0 of the
	 * section's first (only) block. Exact 18.1.3 signature (doc 26 P4): 8 args,
	 * none defaulted. Scope::Local skips addDefinedSymbol's uniqueness assert and
	 * is invisible to setAutoClaimResponsibilityForObjectSymbols (auto-claim only
	 * concerns Scope::Default object symbols), so the anchor cannot clash.
	 */
	Block &block = **sec->blocks ().begin ();
	g.addDefinedSymbol (block, /*Offset=*/0, anchor_name, /*Size=*/block.getSize (),
	                    Linkage::Strong, Scope::Local, /*IsCallable=*/false,
	                    /*IsLive=*/true);
}

/*
 * The ObjectLinkingLayer plugin that ports every metadata capture the RTDyld
 * NotifyLoaded hook used to do. Its passes are LAYER-GLOBAL (installed once, run
 * per link), so each captured fact is threaded back to the right in-flight
 * compile() by the SAME key compile() already drains on: the name of the
 * per-module JITDylib, read here from MaterializationResponsibility. Every pass
 * writes its slice of the ObjectInfo into g_object_info[jdname]; compile() picks
 * it up (and erases it) once its synchronous lookup has driven materialization.
 *
 * Passes, in phase order:
 *   PrePrune       - keep .mono_lsda and .llvm_stackmaps live (the dead-strip fix)
 *   PostAllocation - capture their ranges + the entry's code size (Symbol::getSize
 *                    == ELF st_size, doc 26 P2), and assert the keep-live held;
 *                    ALSO the always-on reloc-width audit over table-manager-
 *                    synthesized (not yet relaxed) edges - see
 *                    classify_reloc_audit_from_graph()'s doc comment in
 *                    engine.cpp for why this runs here and not PreFixup
 *   PostFixup      - capture the .eh_frame range (self-survives prune via its
 *                    keep-alive edge; bytes are final post-fixup, which is where
 *                    the translator later transcodes them into mono unwind ops)
 */
class MonoObjectLinkingPlugin : public ObjectLinkingLayer::Plugin {
public:
	void modifyPassConfig (orc::MaterializationResponsibility &mr, jitlink::LinkGraph &,
	                       jitlink::PassConfiguration &config) override
	{
		std::string jd_name = mr.getTargetJITDylib ().getName ();

		/*
		 * Per-link state shared between the PrePrune keep-live and the
		 * PostAllocation assertion. A shared_ptr is captured by both lambdas; the
		 * pass config is per-link, so this is per-link too (no cross-object state).
		 */
		auto seen = std::make_shared<SectionPresence> ();

		config.PrePrunePasses.push_back ([seen] (jitlink::LinkGraph &g) -> Error {
			keep_section_live (g, ".mono_lsda", "__mono_lsda_anchor", seen->lsda);
			keep_section_live (g, ".llvm_stackmaps", "__mono_stackmaps_anchor",
			                   seen->stackmaps);
			return Error::success ();
		});

		config.PostAllocationPasses.push_back (
			[jd_name, seen] (jitlink::LinkGraph &g) -> Error {
				ObjectInfo info;

				/*
				 * Per-function machine-code size. Symbol::getSize() == ELF st_size
				 * for a defined function symbol (doc 26 P2/Q1), so no retained ELF
				 * buffer is needed. Keyed by the raw symbol name; compile() looks it
				 * up through ORC, whose DataLayout global prefix is empty on ELF
				 * x86-64 - the two coincide (they would diverge on a leading-'_'
				 * target; mangle the key there).
				 */
				for (jitlink::Symbol *sym : g.defined_symbols ()) {
					if (!sym->hasName () || !sym->isCallable ())
						continue;
					uint64_t size = sym->getSize ();
					if (size)
						info.func_sizes[sym->getName ().str ()] = size;
				}

				info.mono_lsda = capture_graph_section (g, ".mono_lsda");
				info.stackmaps = capture_graph_section (g, ".llvm_stackmaps");

				/*
				 * NEGATIVE TEST (always on): a section that WAS emitted this object
				 * (seen at PrePrune) must have a non-empty range now. If it does
				 * not, the keep-live failed and the section was dead-stripped -
				 * exactly the trap that would silently lose .mono_lsda (EH declines)
				 * or .llvm_stackmaps (gshared this-slot recovery breaks). Convert
				 * that into an immediate abort rather than a silent {null,0}, the
				 * same posture as the one-method-per-module invariant.
				 */
				if (seen->lsda && info.mono_lsda.addr == nullptr)
					report_fatal_error (
						"mono: .mono_lsda dead-stripped despite keep-live - "
						"JITLink capture regressed");
				if (seen->stackmaps && info.stackmaps.addr == nullptr)
					report_fatal_error (
						"mono: .llvm_stackmaps dead-stripped despite keep-live - "
						"JITLink capture regressed");

				std::lock_guard<std::mutex> lock (g_object_info_mutex);
				ObjectInfo &slot = g_object_info[jd_name];
				slot.func_sizes = std::move (info.func_sizes);
				slot.mono_lsda = info.mono_lsda;
				slot.stackmaps = info.stackmaps;
				return Error::success ();
			});

		/*
		 * Registered on PostAllocationPasses, NOT PreFixupPasses - deliberately
		 * BEFORE x86_64::optimizeGOTAndStubAccesses (a PreFixupPasses entry the
		 * default target config always adds ahead of this plugin's own
		 * PreFixupPasses additions; see accumulate_reloc_audit_from_graph()'s doc
		 * comment for why running after it, which an earlier version of this
		 * pass did, is observably wrong). PostAllocationPasses still runs strictly
		 * after buildTables_ELF_x86_64 (a PostPrunePasses entry, i.e. before
		 * allocation), so every genuinely external/absolute reference has
		 * already been rerouted through an in-graph stub/GOT slot by the time
		 * this sees it - exactly the property the classification relies on.
		 */
		config.PostAllocationPasses.push_back ([] (jitlink::LinkGraph &g) -> Error {
			accumulate_reloc_audit_from_graph (g);
			return Error::success ();
		});

		config.PostFixupPasses.push_back ([jd_name] (jitlink::LinkGraph &g) -> Error {
			EhFrameInfo eh = capture_graph_section (g, ".eh_frame");
			std::lock_guard<std::mutex> lock (g_object_info_mutex);
			g_object_info[jd_name].eh_frame = eh;
			return Error::success ();
		});
	}

	/* Drop any partial capture for a link that failed to materialize. */
	Error notifyFailed (orc::MaterializationResponsibility &mr) override
	{
		std::lock_guard<std::mutex> lock (g_object_info_mutex);
		g_object_info.erase (mr.getTargetJITDylib ().getName ());
		return Error::success ();
	}

	Error notifyRemovingResources (orc::JITDylib &, orc::ResourceKey) override
	{
		return Error::success ();
	}

	void notifyTransferringResources (orc::JITDylib &, orc::ResourceKey,
	                                  orc::ResourceKey) override
	{
	}

private:
	/* Which keep-live sections were actually emitted by one object (see above). */
	struct SectionPresence {
		bool lsda = false;
		bool stackmaps = false;
	};
};

/* ---- mono-owned eh-frame registrar (doc 26 J3) ---------------------------
 *
 * EHFrameRegistrationPlugin (llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h) needs
 * an llvm::jitlink::EHFrameRegistrar to actually register/deregister each
 * object's .eh_frame. JL1 wired the stock InProcessEHFrameRegistrar there
 * directly; MonoEHFrameRegistrar below replaces it so this engine can observe
 * every register/deregister without changing what happens to the host unwinder.
 *
 * OPTION-I DECISION (keep host registration): MonoEHFrameRegistrar FORWARDS both
 * calls to an InProcessEHFrameRegistrar it owns, so libgcc's __register_frame
 * still runs exactly as it did before this slice - a native crash-dump unwinder
 * walking this process keeps FDE coverage for JIT frames. Dropping that
 * registration (tier-0 parity: the classic JIT never calls __register_frame at
 * all) is a noted follow-up, not done here - this slice is required to be
 * behavior-preserving for managed code.
 *
 * What is new is the RECORDING: every register/deregister is tallied into
 * g_eh_frame_registry (mutex-guarded, keyed like g_object_info above), which
 * eh_frame_registry_stats () (engine.hpp) exposes to mono/unit-tests. This
 * substitutes for the deregister-before-unmap assert JL1 dropped from
 * ~MonoJitMemoryManager: that assert could fire from a per-object destructor
 * the old RTDyld-era memory manager had one of; JITLink's InProcessMemoryManager
 * is a single object the whole ObjectLinkingLayer shares, so there is no
 * per-object destructor left to assert from (see release_owner () below). A
 * deregister for a range this registry does not have live is exactly the
 * violation that assert used to catch, so it is treated the same way - an
 * immediate report_fatal_error, not a silent drift.
 */

static std::mutex g_eh_frame_registry_mutex;
static uint64_t g_eh_frame_registered_count = 0;
static uint64_t g_eh_frame_deregistered_count = 0;
/* Currently-registered ranges, keyed by their starting address. */
static std::map<uint64_t, uint64_t> g_eh_frame_live;

static void
record_eh_frame_registered (orc::ExecutorAddrRange r)
{
	std::lock_guard<std::mutex> lock (g_eh_frame_registry_mutex);
	g_eh_frame_registered_count++;
	g_eh_frame_live[r.Start.getValue ()] = r.size ();
}

static void
record_eh_frame_deregistered (orc::ExecutorAddrRange r)
{
	std::lock_guard<std::mutex> lock (g_eh_frame_registry_mutex);
	g_eh_frame_deregistered_count++;

	uint64_t start = r.Start.getValue ();
	uint64_t size = r.size ();
	auto it = g_eh_frame_live.find (start);
	/*
	 * Recording is now unconditional on both sides (registerEHFrames records
	 * before forwarding to the host, deregisterEHFrames records before
	 * forwarding too - see MonoEHFrameRegistrar below), the same way
	 * EHFrameRegistrationPlugin itself pushes into EHFrameRanges[K]
	 * unconditionally in notifyEmitted before calling registerEHFrames. So a
	 * deregister for a range this registry does not have live is NOT the host
	 * registration call failing (that can no longer desync the two sides) - it
	 * means our own register/deregister bookkeeping has genuinely come apart,
	 * the exact ordering violation the dropped ~MonoJitMemoryManager assert
	 * used to catch. Convert that into an immediate abort, the same posture as
	 * the one-method-per-module and keep-live invariants elsewhere in this
	 * file.
	 */
	if (it == g_eh_frame_live.end ())
		report_fatal_error (
			Twine ("mono: .eh_frame deregistered for a range that was not "
				   "registered/live - eh-frame registrar accounting "
				   "regressed (addr=0x")
			+ Twine::utohexstr (start) + Twine (", size=") + Twine (size)
			+ Twine (")"));
	/*
	 * Right start address, wrong size: the accounting bug the size field
	 * exists to catch. A start-only match would let a mismatched deregister
	 * silently erase the wrong bookkeeping entry, so verify size too before
	 * erasing.
	 */
	if (it->second != size)
		report_fatal_error (
			Twine ("mono: .eh_frame deregistered with mismatched size - "
				   "eh-frame registrar accounting regressed (addr=0x")
			+ Twine::utohexstr (start) + Twine (", registered_size=")
			+ Twine (it->second) + Twine (", deregistered_size=")
			+ Twine (size) + Twine (")"));
	g_eh_frame_live.erase (it);
}

/* Declared in engine.hpp. */
EhFrameRegistryStats
eh_frame_registry_stats ()
{
	std::lock_guard<std::mutex> lock (g_eh_frame_registry_mutex);
	EhFrameRegistryStats stats;
	stats.registered = g_eh_frame_registered_count;
	stats.deregistered = g_eh_frame_deregistered_count;
	stats.live.reserve (g_eh_frame_live.size ());
	for (const auto &kv : g_eh_frame_live) {
		EhFrameInfo info;
		info.addr = reinterpret_cast<uint8_t *> (kv.first);
		info.size = kv.second;
		stats.live.push_back (info);
	}
	return stats;
}

/*
 * The EHFrameRegistrar EHFrameRegistrationPlugin drives. See the file-comment
 * block above for the option-i rationale (host registration kept) and what the
 * recording substitutes for.
 */
class MonoEHFrameRegistrar : public jitlink::EHFrameRegistrar {
public:
	Error registerEHFrames (orc::ExecutorAddrRange eh_frame_section) override
	{
		/*
		 * Record FIRST, unconditionally, before forwarding to the host - mirrors
		 * EHFrameRegistrationPlugin::notifyEmitted, which pushes into its own
		 * EHFrameRanges[K] unconditionally and only THEN calls
		 * Registrar->registerEHFrames (range). If our recording were gated on
		 * the host call succeeding, a host-registration failure (possible on a
		 * non-libgcc target; not on this build) would leave the plugin holding a
		 * range it will unconditionally replay to deregisterEHFrames on teardown
		 * while our own bookkeeping never recorded it - hitting the not-live
		 * report_fatal_error below for a plugin-side non-bug. Recording
		 * unconditionally keeps our bookkeeping symmetric with the plugin's, so
		 * that abort is reserved for a genuine accounting bug. The host call's
		 * Error is still forwarded/propagated to the caller either way.
		 */
		record_eh_frame_registered (eh_frame_section);
		return host_.registerEHFrames (eh_frame_section);
	}

	Error deregisterEHFrames (orc::ExecutorAddrRange eh_frame_section) override
	{
		/* Unconditional for the same reason as registerEHFrames above. */
		record_eh_frame_deregistered (eh_frame_section);
		return host_.deregisterEHFrames (eh_frame_section);
	}

private:
	/*
	 * Owned by value, not by pointer: InProcessEHFrameRegistrar (LLVM 18.1.3,
	 * EHFrameSupport.h) is stateless and default-constructible with no
	 * arguments - the same construction JL1 used directly.
	 */
	jitlink::InProcessEHFrameRegistrar host_;
};

/* ---- singleton bootstrap -------------------------------------------------- */

static std::once_flag g_targets_once;

/*
 * Set one of LLVM's internal boolean cl::opts. Several codegen knobs we need are
 * static file-local options with no public setter, so the registered-options map
 * is the only way in. A missing name means the LLVM we built against renamed or
 * dropped the knob, which would silently change codegen - fatal rather than
 * quietly ignored.
 */
static void
set_llvm_flag (const char *name, bool value)
{
	auto &opts = cl::getRegisteredOptions ();
	auto it = opts.find (name);
	if (it == opts.end ())
		report_fatal_error (Twine ("LLVM dropped/renamed the '") + name + "' option");
	static_cast<cl::opt<bool> *> (it->second)->setValue (value);
}

static void
ensure_native_target ()
{
	std::call_once (g_targets_once, [] {
		InitializeNativeTarget ();
		InitializeNativeTargetAsmPrinter ();
		InitializeNativeTargetAsmParser ();

		/*
		 * Enable the ImplicitNullChecks machine pass process-wide. The pass is
		 * gated by a static default-false cl::opt inside TargetPassConfig
		 * ("enable-implicit-null-checks"); it is not reachable by symbol, so we
		 * flip it through the registered-options map once, here, before the
		 * first emit_object() runs the codegen pipeline. The pass folds an
		 * explicit `icmp/br` null check into a bare faulting load only on
		 * branches the translator tagged `!make.implicit`, so it is inert for
		 * any function without such tags. Recovery of a folded null fault rides
		 * the runtime's existing null-page SIGSEGV -> NRE handler (no faultmap
		 * is consulted). Instant off-switch:
		 * MONO_DEBUG=llvm-disable-implicit-null-checks (drops the tags, so the
		 * pass folds nothing). See design doc 25.
		 */
		set_llvm_flag ("enable-implicit-null-checks", true);

		/*
		 * Turn off X86CallFrameOptimization. For a call that needs stack
		 * arguments it rewrites the stores into prologue-reserved space as
		 * pushes right before the call plus a compensating `add %rsp` after it,
		 * so the CFA offset is 0x10 higher at the call than at the rest of the
		 * body. The runtime's EH does not model that: when it jumps to a catch
		 * handler it restores the stack pointer the protected call was made
		 * with, and never undoes the pushes - the handler then runs, and
		 * returns, one argument area below the real frame. Both the classic and
		 * the tier-1 backend need the CFA offset to be the same at every
		 * instruction of a method for that to be sound, which is what this
		 * gives us. See mono/tests/bug-gh-17285.cs.
		 */
		set_llvm_flag ("no-x86-call-frame-opt", true);
	});
}

/*
 * The code model is pinned to Small, with Reloc::PIC_, rather than left at
 * LLVM's JIT default (getEffectiveX86CodeModel: JIT && Is64Bit -> Large).
 * Large keeps every reference 64-bit-wide so sections can land anywhere
 * above 4 GB with no distance limit - correct, but bigger than it needs to
 * be: a 10-byte movabs-plus-load in place of a 7-byte RIP-relative lea/mov,
 * repeated for every code->code, code->data and code->GOT reference in every
 * JITted method.
 *
 * Correctness here does not depend on where code lands: the engine backs its
 * object-linking layer with a bounded MapperJITLinkMemoryManager slab (still
 * no co-location of a method's JITDylib with any other's - see
 * setObjectLinkingLayerCreator() below), but Small+PIC would be correct over
 * any memory manager because it never emits a bare narrow relocation straight
 * to a target that might be far away. Every reference JITLink's stock x86-64
 * ELF pipeline cannot prove is in range is intercepted, before this engine's
 * own reloc-audit pass runs
 * (deliberately scheduled at PostAllocation, before relaxation - see
 * classify_reloc_audit_from_graph()), by the default-added
 * PLTTableManager/GOTTableManager and rerouted through an in-graph stub or GOT
 * slot whose own outgoing edge is a full 64-bit Pointer64 - correct at any
 * distance by construction. optimizeGOTAndStubAccesses then relaxes that
 * indirection back to a direct, narrower form only when the true target
 * turns out to already be in range - a pure size optimization layered on a
 * mechanism that does not depend on it for correctness. (Probe-confirmed on
 * a module with a target deliberately
 * placed ~34 TB away, executed correctly through the unrelaxed stub+GOT path;
 * see .claude/scratch/jitlink-j5/ and accumulate_reloc_audit_from_graph()
 * below, which is where that invariant is checked on every real compile.)
 *
 * The bounded reservation is what makes more references land in-range so
 * optimizeGOTAndStubAccesses can relax them; co-locating different methods'
 * JITDylibs so cross-method calls could relax too is a further size/perf
 * follow-up, not a correctness requirement, and is still deferred.
 */
JITTargetMachineBuilder
host_target_machine_builder ()
{
	/*
	 * Self-sufficient on purpose. createTargetMachine() on the returned builder
	 * fails with "Unable to find target for this triple" unless the native
	 * target has been registered, and that failure is indistinguishable from a
	 * real one at the call site. ensure_native_target() is a std::call_once, so
	 * paying for it here costs nothing and removes the ordering precondition
	 * entirely.
	 */
	ensure_native_target ();

	auto jtmb = cantFail (JITTargetMachineBuilder::detectHost ());
	jtmb.setCodeGenOptLevel (CodeGenOptLevel::Aggressive);
	jtmb.setCPU (std::string (sys::getHostCPUName ()));
	jtmb.setCodeModel (CodeModel::Small);
	jtmb.setRelocationModel (Reloc::PIC_);

	/*
	 * If codegen ever reaches an LLVM `unreachable` anyway (a translator bug,
	 * or UB in the IL we can't prove impossible), we want a `ud2` there
	 * instead of silently falling through into whatever bytes follow - a
	 * defined fault beats undefined behavior.
	 */
	jtmb.getOptions ().TrapUnreachable = true;

	StringMap<bool> features;
	if (sys::getHostCPUFeatures (features)) {
		std::vector<std::string> feature_vec;
		for (auto &kv : features)
			if (kv.second)
				feature_vec.push_back ((Twine ("+") + kv.first ()).str ());
		jtmb.addFeatures (feature_vec);
	}
	return jtmb;
}

/* ---- MONO_TIER1_DUMP_DIR: textual tier-1 asm dump ------------------------
 *
 * A debugging aid, not part of the compile pipeline proper: every method that
 * reaches tier 1 gets its final assembly written out as a plain-text .s file,
 * one per method. Read once, lazily, on the first compile after startup - most
 * runs never set this, so paying for a getenv/mkdir on every compile would be
 * pure waste.
 */

static std::once_flag g_dump_dir_once;
static const char *g_tier1_dump_dir;

static const char *
tier1_dump_dir ()
{
	std::call_once (g_dump_dir_once, [] {
		const char *dir = std::getenv ("MONO_TIER1_DUMP_DIR");
		if (!dir || !dir [0])
			return;
		if (std::error_code ec = sys::fs::create_directories (dir)) {
			fprintf (stderr, "mono llvm engine: MONO_TIER1_DUMP_DIR='%s': %s\n",
			         dir, ec.message ().c_str ());
			return;
		}
		g_tier1_dump_dir = dir;
	});
	return g_tier1_dump_dir;
}

/*
 * The translator names the LLVM entry function after mono_method_full_name()
 * (translator.cpp), which carries characters no filesystem name should - '/',
 * ':', '<', '>', spaces, the commas of a generic argument list. Replace
 * anything outside [A-Za-z0-9._-] with '_'. A deeply nested generic can still
 * run past a filesystem's name-length limit even after that, so an overlong
 * name is truncated and given a hash suffix - truncation alone would collide
 * two long names that only differ after the cut point.
 */
static std::string
sanitize_dump_filename (StringRef name)
{
	std::string out;
	out.reserve (name.size ());
	for (char c : name)
		out.push_back (isalnum ((unsigned char) c) || c == '.' || c == '_' || c == '-'
		               ? c : '_');

	constexpr size_t kMaxLen = 160;
	if (out.size () <= kMaxLen)
		return out;

	char hash[17];
	snprintf (hash, sizeof (hash), "%016llx",
	          (unsigned long long) hash_value (name));
	out.resize (kMaxLen);
	return out + "_" + hash;
}

/*
 * Write M's compiled form out as a plain-text .s file under MONO_TIER1_DUMP_DIR,
 * named after ENTRY_NAME, or do nothing if that variable is unset. This is a
 * second, independent codegen run - over its own clone of M, through a freshly
 * built TargetMachine from the same host_target_machine_builder() the engine
 * JITs with - rather than a tee on the real compile, so it cannot perturb the
 * MonoIRCompiler pipeline (shared mutable pass state, the EH side channel) that
 * actually produces the code the runtime executes. Every failure path here
 * warns to stderr and returns; a dump going wrong must never take a real
 * compile down with it.
 */
static void
dump_tier1_asm (const Module &m, StringRef entry_name)
{
	const char *dir = tier1_dump_dir ();
	if (!dir)
		return;

	std::unique_ptr<Module> clone = CloneModule (m);

	Expected<std::unique_ptr<TargetMachine>> tm =
		host_target_machine_builder ().createTargetMachine ();
	if (!tm) {
		fprintf (stderr, "mono llvm engine: MONO_TIER1_DUMP_DIR: %s\n",
		         toString (tm.takeError ()).c_str ());
		return;
	}
	auto *ltm = static_cast<LLVMTargetMachine *> (tm->get ());
	clone->setDataLayout (ltm->createDataLayout ());

	SmallString<256> path (dir);
	sys::path::append (path, sanitize_dump_filename (entry_name) + ".s");

	std::error_code ec;
	raw_fd_ostream out (path, ec, sys::fs::OF_Text);
	if (ec) {
		fprintf (stderr, "mono llvm engine: MONO_TIER1_DUMP_DIR: failed to open '%s': %s\n",
		         path.c_str (), ec.message ().c_str ());
		return;
	}

	legacy::PassManager pm;
	if (ltm->addPassesToEmitFile (pm, out, nullptr, CodeGenFileType::AssemblyFile)) {
		fprintf (stderr, "mono llvm engine: MONO_TIER1_DUMP_DIR: target does not support "
		         "emitting assembly\n");
		return;
	}
	pm.run (*clone);
}

namespace {

/* ---- .mono_lsda emitter --------------------------------------------------
 *
 * MonoLSDAStreamer is the object streamer MonoIRCompiler installs in place of
 * the stock createMCObjectStreamer one. It is a plain MCELFStreamer in every
 * respect but one: at finishImpl(), just before the base class writes the
 * object, it emits a target-neutral `.mono_lsda` section (plan 12 2) built from
 * the clauses the C2 MonoEHGatherPass gathered into the shared side channel.
 *
 * The base MCELFStreamer is byte-for-byte the object streamer the C1 pipeline
 * used (createMCObjectStreamer routes ELF through createELFStreamer, which is
 * `new MCELFStreamer + setRelaxAll`; x86-64 registers no object target streamer
 * that changes the bytes - verified). So for a NON-EH module the side channel is
 * empty, no `.mono_lsda` is emitted, and the output stays byte-identical to
 * SimpleCompiler's (the compiler-equivalence invariant).
 *
 * func_begin anchoring (resolves plan 12 9's [UNVERIFIED] flag): each side-
 * channel function carries its MF name, and every offset is a difference against
 * Ctx.getOrCreateSymbol(that name) - the SAME MCSymbol the AsmPrinter emits at
 * the function's entry (on ELF the mangled name equals the IR name). This keys
 * the anchor to the specific entry function by name, NOT to "the first emitted
 * label" (probe2's heuristic), which is what makes it correct for mono's multi-
 * symbol modules (the method plus its GC safepoint poll): a second function's
 * label never captures the wrong base. Because that anchor and the try/handler
 * labels all live in .text, the writer folds each difference to a constant and
 * the section carries zero relocations.
 *
 * A declined function (CAP-EH-0: a filter clause, the one thing the gather
 * still flags) gets NO record, so the load side sees no `.mono_lsda` for it and
 * declines cleanly to the classic JIT. A function the gather marked confirmed-
 * clean (mono-has-eh-clauses, but every landing pad optimized away - see
 * MonoEHGatherPass) still gets a record, just with a zero entry count: a valid,
 * empty table the load side can tell apart from "absent because declined".
 */
class MonoLSDAStreamer : public MCELFStreamer {
public:
	MonoLSDAStreamer (MCContext &ctx, std::unique_ptr<MCAsmBackend> tab,
	                  std::unique_ptr<MCObjectWriter> ow,
	                  std::unique_ptr<MCCodeEmitter> emitter, MonoEHSideChannel &sc)
		: MCELFStreamer (ctx, std::move (tab), std::move (ow), std::move (emitter)),
		  sc_ (sc)
	{
	}

	void finishImpl () override
	{
		MCContext &ctx = getContext ();
		bool section_open = false;
		unsigned records_emitted = 0;

		for (const MonoEHFunctionClauses &fn : sc_.functions) {
			/*
			 * Declined (CAP-EH-0: a filter clause) functions get no record: the
			 * load side must then decline, never publish a partial table. A
			 * confirmed-clean function with zero clauses (see MonoEHGatherPass)
			 * still gets a record below, with a zero entry count.
			 */
			if (fn.declined)
				continue;

			/*
			 * ONE-METHOD-PER-MODULE INVARIANT, enforced loudly. The `.mono_lsda`
			 * section carries NO function identity: records are concatenated from
			 * offset 0, and the load side (C4) reads the section from the start and
			 * attributes it to the ONE method being finalized. That is sound only
			 * because a JIT module holds exactly one EH-bearing function - mono
			 * compiles one method per LLVM module, and the sibling globals it also
			 * emits (gc.safepoint_poll, mono_personality) have no landing pads so
			 * the gather never records them. If a future slice ever puts a SECOND
			 * EH function in a module, a silent concatenation would let C4
			 * misattribute function-1's clause geometry to the method (a CAP-EH-0
			 * silent mis-catch). Convert that architectural regression into an
			 * immediate abort here instead - the same posture as the #16 memory-
			 * manager reclaim-ordering invariant (report_fatal_error, not a guess).
			 */
			if (++records_emitted > 1)
				report_fatal_error (
					"mono: multiple EH functions in one JIT module - "
					".mono_lsda attribution is ambiguous");

			/*
			 * The code-relative anchor: the entry function's own symbol, by name.
			 * This is the exact MCSymbol the AsmPrinter emitted at function entry
			 * (ELF mangling is identity here), so the differences below fold to
			 * .text-internal constants.
			 */
			MCSymbol *func_begin = ctx.getOrCreateSymbol (fn.function);

			/*
			 * Create/switch into `.mono_lsda` lazily - only once, and only if at
			 * least one function actually contributes a record. A module with an
			 * empty (or wholly declined) side channel emits no section at all, so
			 * a non-EH object is byte-identical to the C1 output.
			 */
			if (!section_open) {
				MCSectionELF *s = ctx.getELFSection (".mono_lsda", ELF::SHT_PROGBITS,
				                                     ELF::SHF_ALLOC);
				switchSection (s);
				section_open = true;
			}

			auto off_from_begin = [&] (const MCSymbol *sym) -> const MCExpr * {
				return MCBinaryExpr::createSub (
					MCSymbolRefExpr::create (sym, ctx),
					MCSymbolRefExpr::create (func_begin, ctx), ctx);
			};

			/*
			 * The finally body ranges MonoFinallyRangePass recorded for this
			 * same function, written into the same record as extra entries. They
			 * are not protected regions - the runtime's thread-abort guard is
			 * their only consumer - so they carry a marker kind and the reader
			 * skips them when it builds dispatch clauses.
			 */
			const MonoEHFinallyFunction *finally_fn = nullptr;
			for (const MonoEHFinallyFunction &f : sc_.finally_functions) {
				if (f.function == fn.function) {
					finally_fn = &f;
					break;
				}
			}
			std::size_t nbodies = finally_fn ? finally_fn->bodies.size () : 0;

			/* Header: magic 'MLSD', version 2, count (one entry per invoke range). */
			emitIntValue (0x4d4c5344u, 4);
			emitIntValue (2, 2);
			emitIntValue (fn.clauses.size () + nbodies, 2);

			for (const MonoEHClause &c : fn.clauses) {
				/* try_start_off: begin - func_begin. */
				emitValue (off_from_begin (c.try_begin), 4);
				/* try_len: end - begin. */
				emitValue (MCBinaryExpr::createSub (
					           MCSymbolRefExpr::create (c.try_end, ctx),
					           MCSymbolRefExpr::create (c.try_begin, ctx), ctx),
				           4);
				/* handler_off: handler - func_begin. */
				emitValue (off_from_begin (c.handler), 4);
				/* clause_index: the IL clause index, an absolute scalar. */
				emitIntValue ((uint32_t) c.clause_index, 4);
				/* kind: the clause's IL flags (self-describing v2; 0 for catch). */
				emitIntValue ((uint32_t) c.kind, 4);
			}

			for (std::size_t i = 0; i < nbodies; ++i) {
				const MonoEHFinallyBody &b = finally_fn->bodies[i];

				/* try_start_off/try_len: the handler body's one PC range. */
				emitValue (off_from_begin (b.body_begin), 4);
				emitValue (MCBinaryExpr::createSub (
					           MCSymbolRefExpr::create (b.body_end, ctx),
					           MCSymbolRefExpr::create (b.body_begin, ctx), ctx),
				           4);
				/* handler_off: unused - a body range names no landing pad. */
				emitIntValue (0, 4);
				emitIntValue ((uint32_t) b.clause_index, 4);
				emitIntValue (MONO_LSDA_KIND_FINALLY_BODY, 4);
			}
		}

		MCELFStreamer::finishImpl ();
	}

private:
	MonoEHSideChannel &sc_;
};

} // anonymous namespace

/* ---- custom IR compiler --------------------------------------------------
 *
 * MonoIRCompiler replaces LLJIT's default IR compiler. With this engine's zero
 * compile threads, LLJIT::createCompileFunction would otherwise build a
 * TMOwningSimpleCompiler (a SimpleCompiler owning one TargetMachine), whose
 * operator() is nothing but TM.addPassesToEmitMC + PM.run.
 *
 * The reason to own the object-emission pipeline is the EH port: C2 schedules a
 * MachineFunctionPass after addMachinePasses() and C3 swaps createMCObjectStreamer
 * for a custom MCStreamer that writes a .mono_lsda section. C2 (current) adds the
 * read-only MonoEHGatherPass after addMachinePasses(); it emits nothing and does
 * not modify the MachineFunction, so a non-EH module (no landing pads) still
 * produces an object byte-identical to SimpleCompiler's. C3 will supply the custom
 * streamer. That equivalence is asserted by the compiler-equivalence check in
 * test-llvm-engine.cpp.
 *
 * THREADING: like ConcurrentIRCompiler (CompileUtils.h), a fresh TargetMachine is
 * built from the JITTargetMachineBuilder on every operator() call, so the compiler
 * carries no mutable cross-call state and is safe under compile threads. The engine
 * is synchronous today (0 compile threads; engine.hpp), so per-call construction is
 * not required now - but it means turning on compile threads later needs no change
 * here, strictly better than TMOwningSimpleCompiler sharing one mutable TM.
 */
class MonoIRCompiler : public IRCompileLayer::IRCompiler {
public:
	explicit MonoIRCompiler (JITTargetMachineBuilder jtmb)
		: IRCompiler (irManglingOptionsFromTargetOptions (jtmb.getOptions ())),
		  jtmb_ (std::move (jtmb))
	{
	}

	Expected<std::unique_ptr<MemoryBuffer>> operator() (Module &m) override
	{
		Expected<std::unique_ptr<TargetMachine>> tm = jtmb_.createTargetMachine ();
		if (!tm)
			return tm.takeError ();
		/*
		 * JITTargetMachineBuilder::createTargetMachine always yields an
		 * LLVMTargetMachine (that is the only TargetMachine subclass the target
		 * registry constructs), whose createPassConfig / addPassesToEmitMC surface
		 * this pipeline replicates.
		 */
		auto *ltm = static_cast<LLVMTargetMachine *> (tm->get ());

		/*
		 * The EH-gather side channel is a stack local of this one operator() call
		 * (plan 12 1.4): one module, one thread, no cross-call state. C2 populates
		 * it and stops there; C3 consumes it in a custom streamer. Today nothing
		 * reads it after emission - a non-EH module leaves it empty.
		 */
		MonoEHSideChannel eh_side_channel;

		SmallVector<char, 0> obj_buffer;
		{
			raw_svector_ostream obj_stream (obj_buffer);
			if (Error err = emit_object (*ltm, m, obj_stream, eh_side_channel))
				return std::move (err);
		}

		/*
		 * Same SmallVectorMemoryBuffer wrapping SimpleCompiler::operator() uses,
		 * including the "-jitted-objectbuffer" name suffix. The buffer name is not
		 * part of the object bytes, so it does not affect byte-equivalence.
		 */
		return std::make_unique<SmallVectorMemoryBuffer> (
			std::move (obj_buffer),
			m.getModuleIdentifier () + "-jitted-objectbuffer",
			/*RequiresNullTerminator=*/ false);
	}

private:
	/* The C2 test hook drives emit_object directly to read back the side channel. */
	friend Expected<MonoEHSideChannel> gather_eh_sidechannel (Module &m);

	/*
	 * A faithful hand-inline of LLVMTargetMachine::addPassesToEmitMC followed by
	 * PM.run - the exact recipe SimpleCompiler drives, kept open so C2/C3 can (a)
	 * pm.add a MachineFunctionPass right after addMachinePasses() and (b) replace
	 * createMCObjectStreamer with a custom MCStreamer subclass. Any drift from the
	 * stock method between LLVM versions is a silent codegen difference (plan 12 8,
	 * "highest-tax item"); the equivalence test is what guards against it.
	 */
	static Error emit_object (LLVMTargetMachine &ltm, Module &m, raw_pwrite_stream &out,
	                          MonoEHSideChannel &eh_side_channel)
	{
		legacy::PassManager pm;

		/*
		 * addPassesToGenerateCode: the TargetPassConfig and MMI (added in that
		 * order, PassConfig first), then instruction selection and the machine
		 * passes. SimpleCompiler leaves DisableVerify at addPassesToEmitMC's
		 * default (true), so match that.
		 */
		auto *mmiwp = new MachineModuleInfoWrapperPass (&ltm);
		TargetPassConfig *tpc = ltm.createPassConfig (pm);
		tpc->setDisableVerify (true);
		pm.add (tpc);
		pm.add (mmiwp);
		if (tpc->addISelPasses ())
			return make_error<StringError> (
				"target does not support instruction selection",
				inconvertibleErrorCode ());
		tpc->addMachinePasses ();

		/*
		 * C2: the EH-gather pass runs after the machine passes and before the
		 * AsmPrinter, so it sees the final landing-pad set. It reads only and
		 * emits nothing (it populates eh_side_channel), so for a non-EH function
		 * (no landing pads) it is inert and the emitted object stays byte-identical
		 * to SimpleCompiler's - asserted by compiler-equivalence in
		 * test-llvm-engine.cpp.
		 */
		pm.add (new MonoEHGatherPass (&eh_side_channel));

		/*
		 * Record the PC ranges each finally handler body occupies, for the
		 * runtime's thread-abort guard. This must run after every pass that can
		 * move or duplicate code, so it goes here with the gather rather than
		 * inside addMachinePasses ().
		 */
		pm.add (new MonoFinallyRangePass (&eh_side_channel));

		tpc->setInitialized ();

		/*
		 * The AsmPrinter must emit into the MCContext the MMI created, not a fresh
		 * one - that is the external-context contract addPassesToEmitMC relies on
		 * (and the seam C3 uses to feed the custom streamer the same context).
		 */
		MCContext *ctx = &mmiwp->getMMI ().getContext ();

		const MCSubtargetInfo &sti = *ltm.getMCSubtargetInfo ();
		const MCRegisterInfo &mri = *ltm.getMCRegisterInfo ();
		std::unique_ptr<MCCodeEmitter> mce (
			ltm.getTarget ().createMCCodeEmitter (*ltm.getMCInstrInfo (), *ctx));
		std::unique_ptr<MCAsmBackend> mab (
			ltm.getTarget ().createMCAsmBackend (sti, mri, ltm.Options.MCOptions));
		if (!mce || !mab)
			return make_error<StringError> ("target does not support MC emission",
			                                inconvertibleErrorCode ());

		/*
		 * C3: MonoLSDAStreamer in place of the stock createMCObjectStreamer. It is
		 * a plain MCELFStreamer that additionally writes `.mono_lsda` from
		 * eh_side_channel at finishImpl(); driven by the very side channel the
		 * MonoEHGatherPass above populated.
		 *
		 * This construction reproduces exactly what createMCObjectStreamer does for
		 * ELF: it routes through createELFStreamer, which is `new MCELFStreamer`
		 * followed by setRelaxAll(MCRelaxAll). The IncrementalLinkerCompatible and
		 * DWARFMustBeAtTheEnd flags are consumed only by the COFF/MachO arms of
		 * createMCObjectStreamer, never the ELF one, so they do not apply here; and
		 * x86-64 registers no object target streamer that alters the bytes (all
		 * verified against createMCObjectStreamer for a non-EH module). So a non-EH
		 * module stays byte-identical to SimpleCompiler's output.
		 */
		std::unique_ptr<MCObjectWriter> ow = mab->createObjectWriter (out);
		auto lsda_streamer = std::make_unique<MonoLSDAStreamer> (
			*ctx, std::move (mab), std::move (ow), std::move (mce), eh_side_channel);
		if (ltm.Options.MCOptions.MCRelaxAll)
			lsda_streamer->getAssembler ().setRelaxAll (true);
		std::unique_ptr<MCStreamer> streamer (std::move (lsda_streamer));

		FunctionPass *printer = ltm.getTarget ().createAsmPrinter (ltm, std::move (streamer));
		if (!printer)
			return make_error<StringError> ("target does not support an AsmPrinter",
			                                inconvertibleErrorCode ());
		pm.add (printer);

		pm.run (m);
		return Error::success ();
	}

	JITTargetMachineBuilder jtmb_;
};

/*
 * Test hook (test-llvm-engine.cpp), declared in engine.hpp. Compile `m` to an
 * object through MonoIRCompiler from the same host JITTargetMachineBuilder the
 * engine JITs with. Not part of the engine's runtime surface.
 */
Expected<std::unique_ptr<MemoryBuffer>>
compile_object_with_mono_compiler (Module &m)
{
	MonoIRCompiler compiler (host_target_machine_builder ());
	return compiler (m);
}

/*
 * C2 test hook (declared in engine.hpp). Run the MonoIRCompiler object-emission
 * pipeline - MonoEHGatherPass and all - over `m` from the same host
 * JITTargetMachineBuilder the engine JITs with, and hand back the populated side
 * channel. The object bytes are emitted (the pass only runs as part of a real
 * codegen pipeline) and then discarded. This is the exact pass the runtime path
 * runs, so the gathered clauses the test asserts on are what the runtime gathers.
 */
Expected<MonoEHSideChannel>
gather_eh_sidechannel (Module &m)
{
	Expected<std::unique_ptr<TargetMachine>> tm =
		host_target_machine_builder ().createTargetMachine ();
	if (!tm)
		return tm.takeError ();
	auto *ltm = static_cast<LLVMTargetMachine *> (tm->get ());
	m.setDataLayout (ltm->createDataLayout ());

	MonoEHSideChannel sc;
	SmallVector<char, 0> obj_buffer;
	{
		raw_svector_ostream obj_stream (obj_buffer);
		if (Error err = MonoIRCompiler::emit_object (*ltm, m, obj_stream, sc))
			return std::move (err);
	}
	return sc;
}

/*
 * Size of each address-space reservation the JIT memory manager carves methods
 * out of. Just under 2 GiB (2 GiB - 64 KiB): the strict-less-than-INT32_MAX
 * ceiling that keeps every intra-slab displacement inside a signed 32-bit field
 * (see the memory-manager comment in the constructor below).
 */
static constexpr size_t kSlabReservationGranularity = 0x7FFF0000;

MonoLLVMJIT::MonoLLVMJIT ()
	: tsctx_ (std::make_unique<LLVMContext> ())
{
	ensure_native_target ();

	LLJITBuilder builder;
	builder.setJITTargetMachineBuilder (host_target_machine_builder ());
	/*
	 * Replace LLJIT's default IR compiler (TMOwningSimpleCompiler at 0 compile
	 * threads) with MonoIRCompiler. Functionally identical output today (C1); the
	 * EH port hooks a MachineFunctionPass and a custom MCStreamer into its
	 * pipeline (C2/C3). LLJIT hands the creator the very JITTargetMachineBuilder
	 * set just above, so the target-machine options (code model Small+PIC, host
	 * CPU/features, O3) are preserved exactly - no re-derivation.
	 */
	builder.setCompileFunctionCreator (
		[] (JITTargetMachineBuilder JTMB)
			-> Expected<std::unique_ptr<IRCompileLayer::IRCompiler>> {
			return std::make_unique<MonoIRCompiler> (std::move (JTMB));
		});
	/*
	 * The JITLink object-linking layer (llvm/ExecutionEngine/Orc/ObjectLinkingLayer)
	 * over an InProcessMemoryManager the layer OWNS. This replaces the legacy
	 * RTDyldObjectLinkingLayer + SectionMemoryManager. Two plugins do what the old
	 * MonoJitMemoryManager and NotifyLoaded hook did:
	 *   - EHFrameRegistrationPlugin registers each object's .eh_frame with the host
	 *     unwinder (__register_frame) - the eh-frame hook the old memory manager
	 *     carried - and deregisters it before reclamation (the ordering the old
	 *     destructor asserted; see release_owner ()). It is driven by
	 *     MonoEHFrameRegistrar (doc 26 J3, above), which forwards both calls to a
	 *     stock InProcessEHFrameRegistrar (host registration is unchanged) and
	 *     additionally records every register/deregister for
	 *     eh_frame_registry_stats ()'s benefit - the substitute for the dropped
	 *     ~MonoJitMemoryManager assert.
	 *   - MonoObjectLinkingPlugin ports every metadata capture (code size, .eh_frame
	 *     / .llvm_stackmaps / .mono_lsda ranges, reloc audit) plus the keep-live
	 *     that rescues the symbol-less .mono_lsda / .llvm_stackmaps sections from
	 *     jitlink::prune() - the JITLink equivalent of the old
	 *     setProcessAllSections(true).
	 * The code model is Small+PIC (host_target_machine_builder, J5), so a
	 * reference JITLink cannot prove is in range is not emitted as a bare narrow
	 * relocation - it is rerouted through a PLT stub or GOT slot with a full
	 * 64-bit outgoing edge, correct at any distance; the reloc audit checks that
	 * invariant on the resolved edges.
	 */
	builder.setObjectLinkingLayerCreator (
		[] (ExecutionSession &es, const Triple &) -> Expected<std::unique_ptr<ObjectLayer>> {
			/*
			 * Back the layer with one bounded slab reservation, bump-allocated,
			 * instead of the previous one-mmap-per-method InProcessMemoryManager.
			 * MapperJITLinkMemoryManager reserves kSlabReservationGranularity of
			 * address space up front (a single PROT_READ|WRITE anonymous mmap that
			 * stays non-resident until code is emitted into it - Linux demand-pages
			 * the untouched pages) and carves every compiled method out of it,
			 * only growing with a fresh reservation if one slab fills. That bounds
			 * the process's VMA count over a long tiered-promotion run, which the
			 * per-method path let grow toward vm.max_map_count.
			 *
			 * The granularity is just under 2 GiB so any two blocks inside a single
			 * slab stay within a signed-32-bit displacement of each other - the
			 * range optimizeGOTAndStubAccesses needs to relax a GOT/PLT indirection
			 * back to a direct reference. A full 2 GiB would put a block at offset 0
			 * and one at the end exactly one byte out of range.
			 */
			auto mm = orc::MapperJITLinkMemoryManager::CreateWithMapper<
				orc::InProcessMemoryMapper> (kSlabReservationGranularity);
			if (!mm)
				return mm.takeError ();
			auto layer = std::make_unique<ObjectLinkingLayer> (es, std::move (*mm));
			/*
			 * mono's translator creates externally-linked but UNNAMED globals
			 * (get_jit_callee() is called with an empty name for icall and
			 * MONO_PATCH_INFO_ABS callees). Those have no name in the IR, so
			 * they are absent from the symbol table ORC derives its
			 * materialization responsibility from - the backend only invents a
			 * name (__unnamed_N) when it emits the object. Relocations against
			 * them then fail with "Failed to materialize symbols: __unnamed_1".
			 *
			 * Auto-claiming makes the layer take responsibility for symbols that
			 * appear in the emitted object but were not declared in the IR, which
			 * is exactly this case. (The legacy MCJIT engine never hit it because
			 * it resolved globals by GlobalValue* via getPointerToGlobal(); ORCv2
			 * resolves only by name.)
			 *
			 * This is safe ONLY because compile() gives every module its own
			 * JITDylib: the invented __unnamed_N names are assigned in emission
			 * order, so they are identical across modules and would collide the
			 * moment anything consolidates modules into one dylib.
			 */
			layer->setAutoClaimResponsibilityForObjectSymbols (true);
			layer->addPlugin (std::make_unique<EHFrameRegistrationPlugin> (
				es, std::make_unique<MonoEHFrameRegistrar> ()));
			layer->addPlugin (std::make_unique<MonoObjectLinkingPlugin> ());
			return layer;
		});

	jit_ = cantFail (builder.create ());

	/*
	 * A bare dylib for explicitly-registered runtime helpers. We deliberately
	 * do NOT link the LLJIT main dylib (which carries the default process-symbol
	 * generator) into our compiled modules; instead each module links only to
	 * this dylib. Result: JIT'd code can reach helpers registered via
	 * register_symbol(), and nothing else - no -rdynamic/process-symbol search,
	 * as the README requires for the real backend.
	 */
	helpers_jd_ = &jit_->getExecutionSession ().createBareJITDylib ("mono.helpers");

	register_c_runtime_symbols ();
}

/*
 * Register the C-runtime routines that LLVM's own code generation can synthesize
 * calls to. These are NOT mono icalls: the translator never emits them by name.
 * They appear because the backend lowers IR intrinsics into libc calls - notably
 * llvm.memcpy/memmove/memset, which SelectionDAG turns into calls to memcpy(),
 * memmove() and memset() whenever the size is not a small constant.
 *
 * The legacy engine got these for free, because RTDyld's default memory manager
 * falls back to searching the host process for any unresolved symbol. This engine
 * deliberately has no process-symbol generator (see helpers_jd_ above), so
 * anything the JIT needs must be registered - which means this list is required,
 * and also that a missing entry fails loudly ("Symbols not found") instead of
 * silently binding to whatever the process happens to export.
 *
 * Which routines the backend picks is decided by TargetLibraryInfo built from
 * the TARGET MACHINE's triple (the host, via detectHost ()), not from the
 * module's triple - so these fire in the JIT regardless of what the module says.
 *
 * Deliberately absent, having been checked: sqrt/fabs/copysign and the bit
 * intrinsics (ctpop/ctlz/cttz/bswap) always expand inline on x86-64; the i128
 * helpers (__udivti3 and friends) are unreachable because mono emits no i128;
 * and _Unwind_Resume is held off by the EH-clause exclusion.
 *
 * One trap worth knowing: bcmp is NOT in this list only because the JIT module
 * sets no target triple. InstCombine rewrites memcmp(..) == 0 into bcmp as soon
 * as a GNU triple is present, so setting one without adding bcmp here turns
 * into a mystery abort.
 */
void
MonoLLVMJIT::register_c_runtime_symbols ()
{
	using d1 = double (*) (double);
	using d2 = double (*) (double, double);
	using d3 = double (*) (double, double, double);
	using f1 = float (*) (float);
	using f2 = float (*) (float, float);
	using f3 = float (*) (float, float, float);

	static const struct { const char *name; void *addr; } c_runtime[] = {
		/* Lowered from llvm.memcpy / llvm.memmove / llvm.memset. */
		{ "memcpy",   (void *) (uintptr_t) &::memcpy },
		{ "memmove",  (void *) (uintptr_t) &::memmove },
		{ "memset",   (void *) (uintptr_t) &::memset },
		{ "memcmp",   (void *) (uintptr_t) &::memcmp },

		/*
		 * Math libcalls. Confirmed by compiling every intrinsic mono emits and
		 * checking the emitted calls on every x86-64 variant (baseline, v2, v3,
		 * host): these have NO inline expansion and always become calls.
		 * frem lowers to fmod; the rest come straight from the llvm.* intrinsics.
		 */
		{ "sin",      (void *) (uintptr_t) (d1) &::sin },
		{ "sinf",     (void *) (uintptr_t) (f1) &::sinf },
		{ "cos",      (void *) (uintptr_t) (d1) &::cos },
		{ "cosf",     (void *) (uintptr_t) (f1) &::cosf },
		{ "exp",      (void *) (uintptr_t) (d1) &::exp },
		{ "expf",     (void *) (uintptr_t) (f1) &::expf },
		{ "exp2",     (void *) (uintptr_t) (d1) &::exp2 },
		{ "exp2f",    (void *) (uintptr_t) (f1) &::exp2f },
		{ "log",      (void *) (uintptr_t) (d1) &::log },
		{ "logf",     (void *) (uintptr_t) (f1) &::logf },
		{ "log2",     (void *) (uintptr_t) (d1) &::log2 },
		{ "log2f",    (void *) (uintptr_t) (f1) &::log2f },
		{ "log10",    (void *) (uintptr_t) (d1) &::log10 },
		{ "log10f",   (void *) (uintptr_t) (f1) &::log10f },
		{ "pow",      (void *) (uintptr_t) (d2) &::pow },
		{ "powf",     (void *) (uintptr_t) (f2) &::powf },
		{ "fmod",     (void *) (uintptr_t) (d2) &::fmod },
		{ "fmodf",    (void *) (uintptr_t) (f2) &::fmodf },

		/*
		 * The DAG combiner merges a sin and a cos of the same value into ONE
		 * sincos call - common in trig/rotation code and impossible to predict
		 * from the intrinsics mono emits.
		 */
		{ "sincos",   (void *) (uintptr_t) &::sincos },
		{ "sincosf",  (void *) (uintptr_t) &::sincosf },

		/*
		 * These normally expand inline, but only when the host supports the
		 * instruction: floor/ceil/trunc need SSE4.1 (roundsd) and fma needs
		 * +fma. On a feature-masked VM, container or emulator they fall back to
		 * libcalls, so registering them is cheap insurance against a crash that
		 * would only reproduce on some machines.
		 */
		{ "floor",    (void *) (uintptr_t) (d1) &::floor },
		{ "floorf",   (void *) (uintptr_t) (f1) &::floorf },
		{ "ceil",     (void *) (uintptr_t) (d1) &::ceil },
		{ "ceilf",    (void *) (uintptr_t) (f1) &::ceilf },
		{ "trunc",    (void *) (uintptr_t) (d1) &::trunc },
		{ "truncf",   (void *) (uintptr_t) (f1) &::truncf },
		{ "fma",      (void *) (uintptr_t) (d3) &::fma },
		{ "fmaf",     (void *) (uintptr_t) (f3) &::fmaf },

		/*
		 * Not emitted by the translator at all - SimplifyLibCalls rewrites
		 * pow(2.0, itofp(x)) and exp2(itofp(x)) into ldexp(1.0, x) whenever an
		 * integer is converted to float purely to feed a power-of-two, which
		 * is exactly the shape of level/LOD scaling code (radius * 2^-level).
		 * ldexp itself has no x86 instruction, so it's always a libcall.
		 */
		{ "ldexp",    (void *) (uintptr_t) (double (*) (double, int)) &::ldexp },
		{ "ldexpf",   (void *) (uintptr_t) (float (*) (float, int)) &::ldexpf },

		/*
		 * Same family as ldexp above, from the same optimizePow()/optimizeExp2()
		 * machinery in SimplifyLibCalls: pow(10.0, x) unconditionally becomes
		 * exp10(x), no fast-math flags required, so it fires on any llvm.pow
		 * call mono emits for a literal base-10 Math.Pow. exp10 has no x86
		 * instruction either.
		 */
		{ "exp10",    (void *) (uintptr_t) (d1) &::exp10 },
		{ "exp10f",   (void *) (uintptr_t) (f1) &::exp10f },

		/*
		 * DAGCombiner rewrites pow(x, 1.0/3.0) into a cbrt() call - but only
		 * under the nsz+ninf+nnan+afn fast-math flags, which mono only ever
		 * sets when the runtime is started with --ffast-math (off by default,
		 * see mono_use_fast_math). Registered anyway, same reasoning as
		 * floor/ceil/trunc/fma above: cheap insurance against a codegen path
		 * that only reproduces under a specific runtime flag.
		 */
		{ "cbrt",     (void *) (uintptr_t) (d1) &::cbrt },
		{ "cbrtf",    (void *) (uintptr_t) (f1) &::cbrtf },
	};

	for (const auto &sym : c_runtime)
		register_symbol (sym.name, sym.addr);
}

MonoLLVMJIT::~MonoLLVMJIT () = default;

/*
 * Set once the singleton exists. Read by get_singleton_if_created (), which
 * must not itself trigger construction - a domain unload in a process that
 * never JITted anything would otherwise build a whole LLJIT (target machine,
 * helper dylib, ~40 absoluteSymbols) only to discover it has nothing to free.
 */
static std::atomic<MonoLLVMJIT *> g_singleton {nullptr};

MonoLLVMJIT *
MonoLLVMJIT::get_singleton ()
{
	static MonoLLVMJIT *instance = [] () {
		auto *jit = new MonoLLVMJIT ();
		g_singleton.store (jit, std::memory_order_release);
		return jit;
	} ();
	return instance;
}

MonoLLVMJIT *
MonoLLVMJIT::get_singleton_if_created ()
{
	return g_singleton.load (std::memory_order_acquire);
}

/*
 * Tear down everything compiled under OWNER. Declared in engine.hpp, where the
 * caller's obligation to prove the code dead is spelled out.
 *
 * ExecutionSession::removeJITDylibs () is the right mechanism rather than
 * JITDylib::clear () or a ResourceTracker: clear() drops the symbol table but
 * leaves the JITDylib object itself in the session forever, and a
 * ResourceTracker is a sub-dylib granularity we do not need because compile ()
 * already gives every module a dylib of its own. removeJITDylibs () does both
 * halves - it removes the dylib from the session AND runs every registered
 * ResourceManager's handleRemoveResources, which for ObjectLinkingLayer runs
 * each plugin's notifyRemovingResources - EHFrameRegistrationPlugin::
 * notifyRemovingResources deregisters the object's .eh_frame (via the
 * InProcessEHFrameRegistrar) - and then deallocates the object's JITLink
 * allocation, i.e. unmaps its code and data.
 *
 * The deregister-before-unmap ORDER is a structural guarantee of
 * ObjectLinkingLayer::handleRemoveResources (plugin notifications run before the
 * memory manager deallocates), but it can no longer be asserted from a per-object
 * destructor: the memory is freed by the ONE shared InProcessMemoryManager the
 * layer owns, not by a per-object manager, so there is no local point at which to
 * check "my deregister ran before my unmap" (doc 26 Q5). The substitute is a
 * reclamation integration test (compile -> run -> release_owner -> assert the
 * .eh_frame range was deregistered and is no longer live) driving
 * eh_frame_registry_stats () - see MonoEHFrameRegistrar above and
 * test-reclamation-deregisters-eh-frame in test-llvm-engine.cpp (doc 26 J4).
 */
uint64_t
MonoLLVMJIT::release_owner (MonoDomain *owner)
{
	std::vector<JITDylibSP> jds;

	if (!owner)
		return 0;

	{
		std::lock_guard<std::mutex> lock (owners_mutex_);
		auto it = owners_.find (owner);
		if (it == owners_.end ())
			return 0;
		jds.reserve (it->second.size ());
		for (JITDylib *jd : it->second)
			jds.push_back (jd);
		/*
		 * Erase the map entry under the same lock that published it. The
		 * domain's storage is freed moments after mono calls us, so leaving the
		 * key behind would let a later domain allocated at the same address
		 * inherit this one's dylib list - the dangling-key/address-reuse hazard
		 * that task #29 hit with MonoMethod * keys, except that here it would
		 * hand a live domain a list of already-unmapped dylibs.
		 */
		owners_.erase (it);
	}

	if (jds.empty ())
		return 0;

	uint64_t n = jds.size ();
	/*
	 * cantFail is correct here rather than lax: removeJITDylibs only fails if a
	 * removed dylib is still in another dylib's link order, and the only link
	 * edge this engine ever creates points the other way (ours -> helpers). A
	 * failure would mean that invariant had been broken, which is not something
	 * to swallow while unmapping executable pages.
	 */
	cantFail (jit_->getExecutionSession ().removeJITDylibs (std::move (jds)));

	return n;
}

LLVMContext &
MonoLLVMJIT::context ()
{
	return *tsctx_.getContext ();
}


void
MonoLLVMJIT::register_symbol (StringRef name, void *addr)
{
	std::lock_guard<std::mutex> lock (named_symbols_mutex_);

	auto it = named_symbols_.find (name.str ());
	if (it != named_symbols_.end ()) {
		/*
		 * Every caller here names a target that is stable for the life of the
		 * process UNDER NORMAL OPERATION - but mono/mini's own regression
		 * harness (mini_regression_step (), driver.c) deliberately wipes the
		 * classic JIT's jit_trampoline_hash / jit_code_hash between opt-level
		 * passes to force a clean recompile, which makes
		 * mono_create_jit_trampoline ()/mono_icall_get_wrapper_full () hand
		 * back a FRESH address for a method or icall whose name we already
		 * registered. That new address is just as correct as the old one -
		 * every trampoline this engine ever names forwards to the same
		 * target for as long as the domain lives, and an orphaned one is
		 * never unmapped - so once a name is on file we keep the first
		 * address rather than treat the mismatch as the real bug it would be
		 * anywhere else (a genuine collision between two distinct targets,
		 * which mono_llvm_method_symbol ()'s own disambiguation already rules
		 * out).
		 */
		return;
	}

	auto &es = jit_->getExecutionSession ();
	MangleAndInterner mangle (es, jit_->getDataLayout ());

	SymbolMap symbols;
	symbols[mangle (name)] = ExecutorSymbolDef (
		ExecutorAddr (reinterpret_cast<uint64_t> (addr)),
		JITSymbolFlags::Exported | JITSymbolFlags::Absolute);

	cantFail (helpers_jd_->define (absoluteSymbols (std::move (symbols))));
	named_symbols_.emplace (name.str (), addr);
	symbols_by_addr_.emplace (addr, name.str ());
}

const char *
MonoLLVMJIT::resolve_symbol_name (void *addr)
{
	std::lock_guard<std::mutex> lock (named_symbols_mutex_);

	auto it = symbols_by_addr_.find (addr);
	return it != symbols_by_addr_.end () ? it->second.c_str () : nullptr;
}

void
MonoLLVMJIT::optimize (Function *func)
{
	/*
	 * `func` only identifies the module to optimize; we run the full per-module
	 * -O2 pipeline over the whole module in place, so the caller's subsequent
	 * codegen (which clones this module) and the "Optimized LLVM IR" dump both
	 * see the optimized IR.
	 */
	Module *module = func->getParent ();
	module->setDataLayout (jit_->getDataLayout ());

	/*
	 * Give the PassBuilder a TargetMachine. Without one, TargetTransformInfo is
	 * a no-op and the cost-model-driven parts of -O2 (loop vectorize/unroll, the
	 * inliner's cost model, and a raft of InstCombine/codegen-prepare decisions)
	 * are silently disabled - which would gut the point of running -O2 at all.
	 * This is the SAME builder the codegen path JITs with: host_target_machine_
	 * builder () is what LLJIT is constructed from (see MonoLLVMJIT's ctor) and
	 * what MonoIRCompiler re-derives its TargetMachine from per compile, so the
	 * optimizer and the code generator agree on the host target exactly.
	 */
	std::unique_ptr<TargetMachine> tm =
		cantFail (host_target_machine_builder ().createTargetMachine ());

	/*
	 * Registered before the PassBuilder so that if MONO_LLVM_DUMP_PASS_IR is
	 * set, every default-pipeline pass - including the ones the PassBuilder
	 * adds internally - gets its IR dumped after it runs. See passes/pass-dump.hpp;
	 * a no-op when the env var isn't set.
	 */
	PassInstrumentationCallbacks pic;
	register_pass_ir_dumper (pic);

	PassBuilder pb (tm.get (), PipelineTuningOptions (), std::nullopt, &pic);
	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cgam;
	ModuleAnalysisManager mam;
	pb.registerModuleAnalyses (mam);
	pb.registerCGSCCAnalyses (cgam);
	pb.registerFunctionAnalyses (fam);
	pb.registerLoopAnalyses (lam);
	pb.crossRegisterProxies (lam, fam, cgam, mam);

	/*
	 * Drops the class-init barriers a body ends up with more than one of, which
	 * is mostly what inlining leaves behind. Has to be registered before the
	 * pipeline is built - it hangs itself off an extension point inside the
	 * function simplification pipeline. See passes/elide-class-init.hpp.
	 */
	register_class_init_elision (pb, fam);

	/*
	 * The stock -O2 pipeline with mono's tier-1 inliner in place of LLVM's own
	 * inlining stage - the callee bodies that stage would need are not in the
	 * module until our pass puts them there. See passes/inliner.hpp.
	 */
	ModulePassManager mpm =
		build_tier1_pipeline (pb, pic, OptimizationLevel::O2);
	mpm.run (*module, mam);
}

CompileResult
MonoLLVMJIT::compile (Function *entry,
                      ArrayRef<GlobalVariable *> callee_vars,
                      uint64_t *callee_addrs,
                      StringRef eh_symbol,
                      MonoDomain *owner)
{
	/* Snapshot the names we need to resolve. */
	std::string entry_name = entry->getName ().str ();
	std::vector<std::string> var_names;
	var_names.reserve (callee_vars.size ());
	for (auto *gv : callee_vars)
		var_names.push_back (gv->getName ().str ());
	std::string eh_name = eh_symbol.str ();

	/*
	 * MONO_TIER1_DUMP_DIR debugging aid: off entry->getParent(), the module the
	 * translator built and already ran mono_llvm_optimize_method() over, i.e.
	 * exactly what the clone below is about to hand the JIT. A no-op (a cached
	 * getenv check) unless the variable is set.
	 */
	dump_tier1_asm (*entry->getParent (), entry_name);

	/*
	 * Hand the JIT a private CLONE of the caller's module, not the module
	 * itself. LLJIT::addIRModule consumes and eventually frees the module it is
	 * given; the caller (mono's translator) keeps using its original module
	 * after compile() returns - e.g. mono_llvm_remove_gc_safepoint_poll (see
	 * donor mini-llvm.c right after the mono_llvm_compile_method call). Cloning
	 * keeps the caller's module valid and owned by the caller; the JIT owns and
	 * frees the clone. (The legacy MCJIT engine achieved the same "LLVM never
	 * frees mono's module" invariant via module.release()+NotifyCompiled, which
	 * ORCv2 has no equivalent for - addIRModule always takes ownership.)
	 */
	std::unique_ptr<Module> clone = CloneModule (*entry->getParent ());
	clone->setDataLayout (jit_->getDataLayout ());

	/*
	 * A fresh JITDylib per compiled module. This isolates per-method symbols
	 * that would otherwise collide across methods (notably the "mono_eh_frame"
	 * global, emitted with the same name by every method) and gives us the
	 * donor's per-module "findSymbolIn" semantics for free: a lookup in this
	 * dylib can only resolve to this module's definitions, falling through the
	 * link order to the runtime-helper symbols in the helpers dylib.
	 */
	auto &es = jit_->getExecutionSession ();
	/*
	 * createBareJITDylib, NOT createJITDylib.
	 *
	 * The difference is Platform::setupJITDylib (), which LLJIT's generic
	 * LLVM-IR platform implements by adding a whole platform-runtime IR MODULE -
	 * the __lljit.run_atexits/cxa_atexit machinery - to every dylib it sets up.
	 * That module is never materialized here (nothing ever looks those symbols
	 * up, and this engine never calls LLJIT::initialize), but it is retained for
	 * the life of the dylib. Measured on `--regression generics.exe`, 866
	 * promoted methods: createJITDylib retains 18.6 MB of heap, 22.0 KB per
	 * method, against 132 KB of machine code for the whole run.
	 * createBareJITDylib retains 76 KB across all 866 - 90 bytes each - and cuts
	 * process RSS from 133.0 MB to 112.6 MB with the same 2054/2054 test result.
	 *
	 * What the platform setup would otherwise give us is the atexit/initializer
	 * support and a __dso_handle definition. mono's JIT modules use none of it:
	 * they have no global constructors and no cxa_atexit calls, and this engine
	 * has no initialize()/deinitialize() call anywhere. (An earlier comment here
	 * claimed a bare dylib "segfaults on addIRModule". That is not true of LLVM
	 * 18.1.3 - the whole regression suite passes on bare dylibs.)
	 *
	 * A fresh dylib PER MODULE is still what we want, and is now nearly free. It
	 * isolates per-method symbols that would otherwise collide across methods
	 * (the auto-claimed __unnamed_N names, and the "mono_eh_frame" global), gives
	 * the donor's per-module "findSymbolIn" lookup semantics, and - the point of
	 * this change - makes the dylib the unit of reclamation. Its link order gets
	 * only helpers_jd_ (explicit runtime helpers), not the LLJIT main dylib and
	 * its process-symbol generator.
	 */
	std::string jd_name = "mono.jit." + std::to_string (module_counter_++);
	JITDylib &jd = es.createBareJITDylib (jd_name);
	jd.addToLinkOrder (*helpers_jd_);

	/*
	 * Record the dylib against its owner BEFORE anything is materialized into
	 * it, so that a release_owner () for this key can never miss a dylib that
	 * already holds mapped code.
	 */
	if (owner) {
		std::lock_guard<std::mutex> lock (owners_mutex_);
		owners_[owner].push_back (&jd);
	}

	cantFail (jit_->addIRModule (jd, ThreadSafeModule (std::move (clone), tsctx_)));

	/*
	 * STEP 3b: cantFail() aborts on a resolution failure (e.g. an icall helper
	 * that was never register_symbol()'d). 3b will want a recoverable path here
	 * - propagate the llvm::Error out so the translator can fall back to tier-0
	 * - rather than aborting the process.
	 */
	CompileResult result;
	result.entry = cantFail (jit_->lookup (jd, entry_name)).getValue ();

	/*
	 * Materialization has run by now, so NotifyLoaded has deposited this
	 * module's object facts under its dylib name. Drain them: pick out the entry
	 * function's own size (the module may also carry the GC safepoint poll,
	 * whose size we do not want) and the .eh_frame for the EH port.
	 */
	{
		std::lock_guard<std::mutex> lock (g_object_info_mutex);
		auto entry_it = g_object_info.find (jd_name);
		if (entry_it != g_object_info.end ()) {
			const ObjectInfo &info = entry_it->second;
			auto size_it = info.func_sizes.find (entry_name);
			if (size_it != info.func_sizes.end ())
				result.code_size = size_it->second;
			result.eh_frame = info.eh_frame;
			result.stackmaps = info.stackmaps;
			result.mono_lsda = info.mono_lsda;
			g_object_info.erase (entry_it);
		}
	}

	for (size_t i = 0; i < var_names.size (); ++i)
		callee_addrs[i] = cantFail (jit_->lookup (jd, var_names[i])).getValue ();

	/*
	 * The "mono_eh_frame" global only exists when the module was produced by
	 * the FORKED LLVM, whose MonoEHFrame emission synthesized it. Against
	 * unmodified LLVM 18 there is no such global: stock LLVM emits a standard
	 * DWARF .eh_frame section instead (reported in result.eh_frame), and
	 * consuming that is not ported yet - which is why methods with EH clauses
	 * currently bail to the classic JIT.
	 *
	 * So a missing eh symbol is the normal case today, not an error: report
	 * "no mono-format EH info" rather than aborting the process.
	 */
	if (!eh_name.empty ()) {
		if (auto sym = jit_->lookup (jd, eh_name))
			result.mono_eh_frame = sym->getValue ();
		else
			consumeError (sym.takeError ());
	}

	return result;
}

} // namespace mono

/* ==========================================================================
 * extern "C" mono boundary. Thin adapters over mono::MonoLLVMJIT; the method-
 * compile entry points are reachable only from the (still-stubbed) translator,
 * so they are never hit at runtime until step 3b, but they link cleanly.
 * ========================================================================== */

#include <llvm-c/Core.h>

#include "backend.h"

extern "C" {

void
mono_llvm_jit_init (void)
{
	mono::MonoLLVMJIT::get_singleton ();
}

void
mono_llvm_jit_register_symbol (const char *name, gpointer addr)
{
	mono::MonoLLVMJIT::get_singleton ()->register_symbol (name, addr);
}

const char *
mono_llvm_jit_resolve_symbol_name (gpointer addr)
{
	/*
	 * get_singleton_if_created (), not get_singleton (): a disassembly
	 * request should never be the thing that stands up the JIT engine. If
	 * nothing has been compiled through it yet, ADDR can't be registered
	 * either, so there's nothing to find.
	 */
	mono::MonoLLVMJIT *jit = mono::MonoLLVMJIT::get_singleton_if_created ();
	return jit ? jit->resolve_symbol_name (addr) : nullptr;
}

MonoEERef
mono_llvm_create_ee (LLVMExecutionEngineRef *ee)
{
	/*
	 * MUST return NULL (matching the legacy engine). The engine is a process-wide
	 * singleton reached via get_singleton() internally, so there is no per-EE
	 * handle to hand back. Crucially, the donor stores this return value in
	 * module->mono_ee (a MonoEERef*) and, at teardown, calls
	 * mono_llvm_dispose_ee(module->mono_ee) - which writes NULL *through* that
	 * pointer. Returning a real pointer here would make dispose_ee scribble NULL
	 * over the singleton's first member (jit_), corrupting it. Returning NULL
	 * makes that write a guarded no-op. The donor never dereferences mono_ee (it
	 * only stores it, passes it to compile_method - which ignores it - and to
	 * dispose_ee), so NULL is safe. *ee is left untouched.
	 */
	(void) ee;
	return NULL;
}

void
mono_llvm_dispose_ee (MonoEERef *mono_ee)
{
	/*
	 * The engine singleton lives for the process lifetime; nothing per-EE to
	 * release. Because create_ee returns NULL, mono_ee is NULL here and this is a
	 * no-op - it never writes through a live pointer. (If create_ee ever returned
	 * non-NULL, this NULL store would corrupt whatever it pointed at.)
	 */
	if (mono_ee)
		*mono_ee = NULL;
}

void
mono_llvm_optimize_method (LLVMValueRef method)
{
	mono::MonoLLVMJIT::get_singleton ()->optimize (llvm::unwrap<llvm::Function> (method));
}

gpointer
mono_llvm_compile_method (MonoEERef mono_ee, MonoCompile *cfg, LLVMValueRef method,
                          int nvars, LLVMValueRef *callee_vars, gpointer *callee_addrs,
                          gpointer *eh_frame, guint32 *code_size_out,
                          gpointer *dwarf_eh_frame_out, guint32 *dwarf_eh_frame_size_out,
                          gpointer *stackmaps_out, guint32 *stackmaps_size_out,
                          gpointer *mono_lsda_out, guint32 *mono_lsda_size_out)
{
	(void) mono_ee;

	auto *jit = mono::MonoLLVMJIT::get_singleton ();
	auto *entry = llvm::unwrap<llvm::Function> (method);

	llvm::SmallVector<llvm::GlobalVariable *, 8> vars;
	vars.reserve (nvars);
	for (int i = 0; i < nvars; ++i)
		vars.push_back (llvm::unwrap<llvm::GlobalVariable> (callee_vars[i]));

	std::vector<uint64_t> addrs (nvars);

	/*
	 * cfg->domain is the lifetime key: it is the domain this method's code is
	 * published into (mini_tiered_promote () swaps the body into
	 * domain->jit_code_hash under that same domain), and mono_domain_free ()
	 * destroys that domain's own code manager a few dozen lines after it calls
	 * us back through mono_llvm_jit_release_domain ().
	 */
	mono::CompileResult res = jit->compile (entry, vars, addrs.data (), "mono_eh_frame",
	                                        cfg ? cfg->domain : NULL);

	for (int i = 0; i < nvars; ++i)
		callee_addrs[i] = (gpointer) (gsize) addrs[i];
	if (eh_frame)
		*eh_frame = (gpointer) (gsize) res.mono_eh_frame;
	if (code_size_out)
		*code_size_out = (guint32) res.code_size;
	if (dwarf_eh_frame_out)
		*dwarf_eh_frame_out = (gpointer) res.eh_frame.addr;
	if (dwarf_eh_frame_size_out)
		*dwarf_eh_frame_size_out = (guint32) res.eh_frame.size;
	if (stackmaps_out)
		*stackmaps_out = (gpointer) res.stackmaps.addr;
	if (stackmaps_size_out)
		*stackmaps_size_out = (guint32) res.stackmaps.size;
	if (mono_lsda_out)
		*mono_lsda_out = (gpointer) res.mono_lsda.addr;
	if (mono_lsda_size_out)
		*mono_lsda_size_out = (guint32) res.mono_lsda.size;

	return (gpointer) (gsize) res.entry;
}

guint32
mono_llvm_jit_release_domain (MonoDomain *domain)
{
	/*
	 * Nothing was ever JITted, so there is nothing to reclaim and no reason to
	 * build an engine to discover that.
	 */
	mono::MonoLLVMJIT *jit = mono::MonoLLVMJIT::get_singleton_if_created ();

	if (!jit || !domain)
		return 0;

	return (guint32) jit->release_owner (domain);
}

void
mono_llvm_set_unhandled_exception_handler (void)
{
	/*
	 * No-op, matching the legacy JIT engine. Registered as a JIT icall at
	 * startup whenever ENABLE_LLVM is defined; the real unhandled-exception
	 * path is wired by step 3b.
	 */
}

} /* extern "C" */
