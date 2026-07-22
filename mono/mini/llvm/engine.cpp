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
 *   1. The pure-LLVM engine core (class mono::MonoLLVMJIT + MonoJitMemoryManager).
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

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <llvm/ADT/StringMap.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/CodeGen/AsmPrinter.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineFunctionPass.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
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
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/Utils/Cloning.h>

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
	 * The loaded `.gcc_except_table` (Itanium LSDA) section, or {nullptr,0} if the
	 * module emitted none. Present only for a method LLVM gave a personalityFn and
	 * an invoke/landingpad; the EH port (M2) decodes it via
	 * mono::decode_gcc_except_table into the method's MonoJitExceptionInfo[]. Same
	 * {addr,size} shape as eh_frame, captured by the same section-name loop.
	 */
	EhFrameInfo gcc_except_table;
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

static void
accumulate_reloc_audit (const object::ObjectFile &obj)
{
	RelocAudit one = audit_relocations (obj);

	std::lock_guard<std::mutex> lock (g_reloc_audit_mutex);
	bool first = g_jit_reloc_audit.truncating == 0 && one.truncating != 0;
	g_jit_reloc_audit.total += one.total;
	g_jit_reloc_audit.truncating += one.truncating;
	if (g_jit_reloc_audit.first_offender.empty ())
		g_jit_reloc_audit.first_offender = one.first_offender;

	/*
	 * Say so once. Silent address truncation is the exact failure mode this
	 * audit exists to catch, and on a release-mode LLVM nothing else would
	 * report it. Warn rather than abort: the engine is a compiler backend and
	 * killing the process on a diagnostic is worse than a wrong-looking JIT
	 * that the unit test will fail on.
	 *
	 * Worth knowing that this is a warning BEFORE the fact, not a post-mortem:
	 * jitLinkForORC runs loadObject -> OnLoaded (which is what calls us) ->
	 * finalizeAsync -> resolveRelocations, so the message is emitted before
	 * RTDyld performs the truncating write.
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
 * Collected object facts, keyed by the name of the JITDylib the module was
 * added to (compile() gives every module its own, uniquely named).
 *
 * DELIBERATELY NOT a thread_local. The NotifyLoaded hook runs on whichever
 * thread materializes the module, which stops being the calling thread the
 * moment the JIT is configured with compile threads (what tiering wants). A
 * thread-local channel would then leave the caller reading an empty value - a
 * zero code_size - so the keyed map is what makes this survive that change.
 */
static std::mutex g_object_info_mutex;
static std::map<std::string, ObjectInfo> g_object_info;

/*
 * Harvest the per-function sizes and the .eh_frame location out of the freshly
 * emitted object. Called from the object layer's NotifyLoaded hook.
 *
 * NOTE: sizes are keyed by the RAW ELF symbol name, while compile() looks the
 * entry up through ORC, which applies the DataLayout's global prefix. Those
 * coincide on ELF x86-64 (the prefix is empty) but would diverge on a platform
 * that uses one (Mach-O's leading '_'), silently yielding code_size 0. Mangle
 * the key here if this engine is ever ported to such a target.
 */
static void
capture_object_info (orc::MaterializationResponsibility &r, const object::ObjectFile &obj,
                     const RuntimeDyld::LoadedObjectInfo &loaded)
{
	ObjectInfo info;

	/*
	 * Check the relocation widths of what LLVM just emitted. This is the real
	 * guard on the "sections may be mapped anywhere" assumption the stock
	 * SectionMemoryManager forces on us; see RelocAudit above.
	 */
	accumulate_reloc_audit (obj);

	if (isa<object::ELFObjectFileBase> (&obj)) {
		for (const object::SymbolRef &sym : obj.symbols ()) {
			Expected<object::SymbolRef::Type> type = sym.getType ();
			if (!type) {
				consumeError (type.takeError ());
				continue;
			}
			if (*type != object::SymbolRef::ST_Function)
				continue;

			Expected<StringRef> name = sym.getName ();
			if (!name) {
				consumeError (name.takeError ());
				continue;
			}

			uint64_t size = object::ELFSymbolRef (sym).getSize ();
			if (size)
				info.func_sizes[name->str ()] = size;
		}
	}

	/*
	 * Locate a loaded section by name and return where it landed, {nullptr,0} if
	 * absent. Used for .eh_frame (EH port) and .llvm_stackmaps (gshared this-slot,
	 * #15); the EH port's .gcc_except_table can reuse it too.
	 */
	auto capture_named_section = [&obj, &loaded] (StringRef want) -> EhFrameInfo {
		EhFrameInfo r;
		for (const object::SectionRef &sec : obj.sections ()) {
			Expected<StringRef> name = sec.getName ();
			if (!name) {
				consumeError (name.takeError ());
				continue;
			}
			if (*name != want)
				continue;
			r.addr = (uint8_t *) (uintptr_t) loaded.getSectionLoadAddress (sec);
			r.size = sec.getSize ();
			break;
		}
		return r;
	};

	info.eh_frame = capture_named_section (".eh_frame");
	info.stackmaps = capture_named_section (".llvm_stackmaps");
	info.gcc_except_table = capture_named_section (".gcc_except_table");
	info.mono_lsda = capture_named_section (".mono_lsda");

	std::lock_guard<std::mutex> lock (g_object_info_mutex);
	g_object_info[r.getTargetJITDylib ().getName ()] = std::move (info);
}

/*
 * Custom RTDyld memory manager. Subclasses SectionMemoryManager (which does
 * correct mmap + W^X finalization) and adds the .eh_frame capture hook.
 *
 * The object linking layer constructs one of these PER OBJECT, so the capture
 * is naturally per-module.
 *
 * STEP 3b - mono-owned code allocation: to make mono's code manager own the
 * JIT code (mono_mem_manager_code_reserve), override allocateCodeSection() here
 * to route through the current MonoCompile's mem_manager (the legacy engine
 * passed it via a thread-local cfg). For this milestone SectionMemoryManager's
 * own RWX allocation is used, which is self-contained and keeps the engine core
 * free of any mono dependency.
 */
class MonoJitMemoryManager : public SectionMemoryManager {
public:
	/*
	 * ORDERING INVARIANT, always on. Reclaiming a dylib must deregister its
	 * .eh_frame with the host unwinder BEFORE the memory holding those FDEs is
	 * unmapped, and the unmapping is exactly what this destructor does (via
	 * ~SectionMemoryManager). __register_frame's registry is process-global, so
	 * an FDE left registered over freed - and later reused - memory is reachable
	 * from any unwind anywhere in the process, not just from mono's.
	 *
	 * The order is RTDyldObjectLinkingLayer::handleRemoveResources's to get
	 * right (deregisterEHFrames then destroy the memory manager); this asserts
	 * it held rather than trusting a library-internal sequencing across LLVM
	 * upgrades. The check is O(1) on two bools set on this object's own code
	 * path - no counters, no globals - and fires a fatal error rather than
	 * unmapping live FDEs, because a stale process-global FDE is not something
	 * to let slide.
	 */
	~MonoJitMemoryManager () override
	{
		if (registered_eh_ && !deregistered_eh_)
			llvm::report_fatal_error (
				".eh_frame unmapped before deregisterEHFrames - JIT reclamation ordering broke");
	}

	void registerEHFrames (uint8_t *Addr, uint64_t LoadAddr, size_t Size) override
	{
		/*
		 * Register with the host (libgcc/libunwind) __register_frame so JITted
		 * code can unwind during this milestone. The EH port replaces this base
		 * call with mono-native registration.
		 *
		 * The section is NOT captured here: the memory manager is constructed
		 * per object by a factory that receives no context, so it cannot tell
		 * which module it belongs to. capture_object_info() takes it from the
		 * object file instead, where the owning JITDylib is known.
		 */
		registered_eh_ = true;
		SectionMemoryManager::registerEHFrames (Addr, LoadAddr, Size);
	}

	void deregisterEHFrames () override
	{
		deregistered_eh_ = true;
		SectionMemoryManager::deregisterEHFrames ();
	}

private:
	/* For the teardown-ordering invariant in the destructor above. */
	bool registered_eh_ = false;
	bool deregistered_eh_ = false;
};

/* ---- singleton bootstrap -------------------------------------------------- */

static std::once_flag g_targets_once;

static void
ensure_native_target ()
{
	std::call_once (g_targets_once, [] {
		InitializeNativeTarget ();
		InitializeNativeTargetAsmPrinter ();
		InitializeNativeTargetAsmParser ();
	});
}

/*
 * No code model is pinned; the engine uses LLVM's default. On x86-64 that
 * default is Large for a 64-bit JIT (getEffectiveX86CodeModel: JIT && Is64Bit
 * -> Large), which keeps every reference 64-bit-wide so a stock
 * SectionMemoryManager can place sections anywhere above 4 GB. That invariant
 * is not assumed - it is checked on the emitted relocations by
 * audit_relocations() here and the reloc-widths case in
 * mono/unit-tests/test-llvm-engine.cpp.
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

/* ---- EH clause gather pass (C2) ------------------------------------------
 *
 * MonoEHGatherPass runs after addMachinePasses() and before the AsmPrinter (so
 * it sees the final landing-pad set). For each landing pad it records the invoke
 * range, the handler label and the IL clause_index - recovered in-process from
 * the type_info_N global's i32 initializer (mono's clause-index smuggling) - into
 * a per-compile side channel. It EMITS NOTHING and never modifies the
 * MachineFunction (runOnMachineFunction returns false, and it preserves all
 * analyses), so scheduling it leaves the emitted object byte-identical for a
 * non-EH module. C3 turns the side channel into a .mono_lsda section.
 *
 * Robustness (CAP-EH-0): it must not assert or crash on any module. A landing
 * pad missing a begin/end/lpad label, a type_info that is not a GlobalVariable
 * with a ConstantInt initializer, or a negative (filter) / zero (cleanup) TypeId
 * is recorded as unexpected (declined / has_filter) so a later slice can decline
 * the method - it is never a fatal error here.
 *
 * A static char ID is all the legacy PassManager needs - no INITIALIZE_PASS
 * (plan 12 1.3).
 */
namespace {

char g_mono_eh_gather_pass_id = 0;

class MonoEHGatherPass : public MachineFunctionPass {
public:
	explicit MonoEHGatherPass (MonoEHSideChannel &sc)
		: MachineFunctionPass (g_mono_eh_gather_pass_id), sc_ (sc)
	{
	}

	StringRef getPassName () const override { return "Mono EH clause gather"; }

	/* Read-only: never disturb anything the AsmPrinter will emit. */
	void getAnalysisUsage (AnalysisUsage &au) const override
	{
		au.setPreservesAll ();
		MachineFunctionPass::getAnalysisUsage (au);
	}

	bool runOnMachineFunction (MachineFunction &mf) override
	{
		const std::vector<LandingPadInfo> &pads = mf.getLandingPads ();

		/* Inert on a non-EH function: no landing pads -> nothing recorded. */
		if (pads.empty ())
			return false;

		const std::vector<const GlobalValue *> &type_infos = mf.getTypeInfos ();

		MonoEHFunctionClauses fn;
		fn.function = mf.getName ().str ();

		for (const LandingPadInfo &lp : pads) {
			const MCSymbol *handler = lp.LandingPadLabel;
			if (!handler)
				fn.declined = true;

			/*
			 * A cleanup (finally/fault) landing pad is out of the catch-only
			 * milestone and must decline, exactly like a filter. But the
			 * per-TypeId loop below - the only other place fn.declined is set -
			 * never sees a cleanup marker for a PURE-cleanup pad: LLVM pushes the
			 * implicit cleanup TypeId 0 onto LandingPadInfo::TypeIds only when the
			 * pad ALSO has clauses (MachineFunction::addLandingPad: `isCleanup ()
			 * && getNumClauses () != 0`), so a setCleanup(true) pad with zero
			 * clauses leaves TypeIds empty and the loop runs zero iterations,
			 * returning {declined:false, clauses:[]}. Read the cleanup bit
			 * straight off the IR LandingPadInst the pad was lowered from - the
			 * exact instruction addLandingPad inspects - so the decline is set
			 * for both the pure-cleanup and the catch+cleanup shapes, and the
			 * `declined` side channel is correct on its own rather than relying on
			 * MonoLSDAStreamer's downstream empty-clauses guard.
			 */
			if (const BasicBlock *bb =
			        lp.LandingPadBlock ? lp.LandingPadBlock->getBasicBlock () : nullptr) {
				if (const auto *lpi =
				        dyn_cast_or_null<LandingPadInst> (bb->getFirstNonPHI ()))
					if (lpi->isCleanup ())
						fn.declined = true;
			}

			/*
			 * BeginLabels/EndLabels carry ONE (begin,end) pair PER INVOKE that
			 * unwinds to this landing pad (SmallVector<MCSymbol*,1>): mono's
			 * emit_call (translator-emit.cpp) issues one LLVMBuildInvoke2 per
			 * protected call in the try - including the implicit null/bounds/div
			 * checks that lower to throw-call invokes - all converging on the
			 * clause's single handler landing pad. So a try with N protected calls
			 * yields ONE landing pad with N invoke ranges. We MUST emit one clause
			 * per invoke range: keeping only the first would publish a
			 * [try_start,try_end) covering only the first call, so a throw from the
			 * 2nd+ call is not is_address_protected and the handler silently never
			 * runs. mono's model supports this directly - is_address_protected
			 * scans all clauses and takes the first PC match, so multiple ei with
			 * the same clause_index/handler over disjoint ranges is expected.
			 * .mono_lsda is therefore "one entry per invoke range"; C3/C4 honor it.
			 *
			 * The two vectors are paired by index (an LLVM invariant). If they ever
			 * disagree in length, decline rather than mispair; likewise a landing
			 * pad with no invoke range at all is malformed.
			 */
			if (lp.BeginLabels.size () != lp.EndLabels.size ())
				fn.declined = true;
			size_t nranges = std::min (lp.BeginLabels.size (), lp.EndLabels.size ());
			if (nranges == 0)
				fn.declined = true;

			for (size_t i = 0; i < nranges; ++i) {
				/*
				 * The invoke range and handler entry are the same MCSymbol*s the
				 * AsmPrinter emits into .text; C3 turns them into
				 * func_begin-relative offsets. A missing label is malformed -
				 * record the decline, do not crash.
				 */
				const MCSymbol *begin = lp.BeginLabels[i];
				const MCSymbol *end = lp.EndLabels[i];
				if (!begin || !end)
					fn.declined = true;

				for (int type_id : lp.TypeIds) {
					if (type_id <= 0) {
						/*
						 * type_id < 0 is a filter (exception-specification), == 0
						 * a cleanup action - neither is a catch clause, and both
						 * are out of the catch-only milestone. Flag and skip.
						 */
						if (type_id < 0)
							fn.has_filter = true;
						fn.declined = true;
						continue;
					}

					MonoEHClause clause;
					clause.try_begin = begin;
					clause.try_end = end;
					clause.handler = handler;

					/*
					 * mono's clause-index smuggling: TypeIds are 1-based indices
					 * into getTypeInfos(); the referenced type_info_N global's i32
					 * initializer is the IL clause index. Recover it in-process -
					 * no ttype-table deref, no relocation dependency.
					 */
					if ((size_t) type_id <= type_infos.size ()) {
						const GlobalValue *gv = type_infos[type_id - 1];
						if (const auto *var = dyn_cast_or_null<GlobalVariable> (gv)) {
							if (var->hasInitializer ()) {
								if (const auto *ci = dyn_cast<ConstantInt> (
								        var->getInitializer ())) {
									clause.clause_index = (int) ci->getSExtValue ();
									clause.clause_resolved = true;
								}
							}
						}
					}
					if (!clause.clause_resolved)
						fn.declined = true;

					fn.clauses.push_back (clause);
				}
			}
		}

		sc_.functions.push_back (std::move (fn));
		return false;
	}

private:
	MonoEHSideChannel &sc_;
};

/* ---- .mono_lsda emitter (C3) ---------------------------------------------
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
 * A declined function (CAP-EH-0: a missing label, an unresolvable clause index,
 * a filter/cleanup TypeId - anything the gather flagged) gets NO record, so the
 * load side sees no `.mono_lsda` for it and declines cleanly to the classic JIT.
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
			 * Declined (CAP-EH-0) or clause-less functions get no record: the
			 * load side must then decline, never publish a partial table.
			 */
			if (fn.declined || fn.clauses.empty ())
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

			/* Header: magic 'MLSD', version 1, count (one entry per invoke range). */
			emitIntValue (0x4d4c5344u, 4);
			emitIntValue (1, 2);
			emitIntValue (fn.clauses.size (), 2);

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
		pm.add (new MonoEHGatherPass (eh_side_channel));

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
 * Test hooks for the C1 compiler-equivalence check (test-llvm-engine.cpp),
 * declared in engine.hpp. Compile `m` to an object two ways - through
 * MonoIRCompiler and through LLVM's stock TMOwningSimpleCompiler - from the SAME
 * host JITTargetMachineBuilder, so the test can assert the two objects are
 * byte-identical: the proof that swapping in MonoIRCompiler is observably inert.
 * Not part of the engine's runtime surface.
 */
Expected<std::unique_ptr<MemoryBuffer>>
compile_object_with_mono_compiler (Module &m)
{
	MonoIRCompiler compiler (host_target_machine_builder ());
	return compiler (m);
}

Expected<std::unique_ptr<MemoryBuffer>>
compile_object_with_simple_compiler (Module &m)
{
	Expected<std::unique_ptr<TargetMachine>> tm =
		host_target_machine_builder ().createTargetMachine ();
	if (!tm)
		return tm.takeError ();
	/* TMOwningSimpleCompiler is exactly what LLJIT builds by default at 0 threads. */
	TMOwningSimpleCompiler compiler (std::move (*tm));
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
	 * set just above, so the target-machine options (code model Large, host
	 * CPU/features, O3) are preserved exactly - no re-derivation.
	 */
	builder.setCompileFunctionCreator (
		[] (JITTargetMachineBuilder JTMB)
			-> Expected<std::unique_ptr<IRCompileLayer::IRCompiler>> {
			return std::make_unique<MonoIRCompiler> (std::move (JTMB));
		});
	/*
	 * Force the RTDyld object-linking layer with our custom memory manager.
	 * LLJIT would default to RTDyldObjectLinkingLayer on ELF/amd64 anyway, but
	 * we must inject MonoJitMemoryManager to get the .eh_frame hook.
	 */
	builder.setObjectLinkingLayerCreator (
		[] (ExecutionSession &es, const Triple &) -> Expected<std::unique_ptr<ObjectLayer>> {
			auto layer = std::make_unique<RTDyldObjectLinkingLayer> (
				es, [] () { return std::make_unique<MonoJitMemoryManager> (); });
			/*
			 * The emitted object is the only place the per-function machine-code
			 * size is available (ELF st_size). mono needs it for cfg->code_len,
			 * which sizes the method's MonoJitInfo - a zero-length jit-info makes
			 * mini_jit_info_table_find() unable to find the method at all.
			 */
			layer->setNotifyLoaded (
				[] (orc::MaterializationResponsibility &r, const object::ObjectFile &obj,
				    const RuntimeDyld::LoadedObjectInfo &loaded) {
					capture_object_info (r, obj, loaded);
				});
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
			/*
			 * Map EVERY section, not just those RTDyld deems required for
			 * execution. The custom-emit `.mono_lsda` clause table (plan 12) is
			 * SHF_ALLOC but has NO incoming relocations or symbols, so with the
			 * default (ProcessAllSections=false) RTDyld skips allocating it and
			 * getSectionLoadAddress() returns 0 - the section is present in the
			 * object yet unmapped, so the load-time reader sees a null pointer and
			 * every clause-bearing method declines. (Contrast `.gcc_except_table`,
			 * which loads only because the `.eh_frame` FDE references it.) Forcing
			 * all sections gives `.mono_lsda` a live load address; it costs only a
			 * few extra bytes of mapped, non-executable metadata per module.
			 */
			layer->setProcessAllSections (true);
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
 * ResourceManager's handleRemoveResources, which for RTDyldObjectLinkingLayer
 * means deregisterEHFrames () followed by destroying the object's
 * SectionMemoryManager, i.e. unmapping its code and data. That deregister-
 * before-unmap ordering is asserted in ~MonoJitMemoryManager.
 */
uint64_t
MonoLLVMJIT::release_owner (void *owner)
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
		 * Erase the map entry under the same lock that published it. The key is
		 * a MonoDomain * whose storage is freed moments after mono calls us, so
		 * leaving it behind would let a later domain allocated at the same
		 * address inherit this one's dylib list - the dangling-key/address-reuse
		 * hazard that task #29 hit with MonoMethod * keys, except that here it
		 * would hand a live domain a list of already-unmapped dylibs.
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
	auto &es = jit_->getExecutionSession ();
	MangleAndInterner mangle (es, jit_->getDataLayout ());

	SymbolMap symbols;
	symbols[mangle (name)] = ExecutorSymbolDef (
		ExecutorAddr (reinterpret_cast<uint64_t> (addr)),
		JITSymbolFlags::Exported | JITSymbolFlags::Absolute);

	cantFail (helpers_jd_->define (absoluteSymbols (std::move (symbols))));
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

	PassBuilder pb (tm.get ());
	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cgam;
	ModuleAnalysisManager mam;
	pb.registerModuleAnalyses (mam);
	pb.registerCGSCCAnalyses (cgam);
	pb.registerFunctionAnalyses (fam);
	pb.registerLoopAnalyses (lam);
	pb.crossRegisterProxies (lam, fam, cgam, mam);

	ModulePassManager mpm =
		pb.buildPerModuleDefaultPipeline (OptimizationLevel::O2);
	mpm.run (*module, mam);
}

CompileResult
MonoLLVMJIT::compile (Function *entry,
                      ArrayRef<GlobalVariable *> callee_vars,
                      uint64_t *callee_addrs,
                      StringRef eh_symbol,
                      void *owner)
{
	/* Snapshot the names we need to resolve. */
	std::string entry_name = entry->getName ().str ();
	std::vector<std::string> var_names;
	var_names.reserve (callee_vars.size ());
	for (auto *gv : callee_vars)
		var_names.push_back (gv->getName ().str ());
	std::string eh_name = eh_symbol.str ();

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
			result.gcc_except_table = info.gcc_except_table;
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
mono_llvm_jit_register_symbol (const char *name, void *addr)
{
	mono::MonoLLVMJIT::get_singleton ()->register_symbol (name, addr);
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
                          gpointer *gcc_except_table_out, guint32 *gcc_except_table_size_out,
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
	if (gcc_except_table_out)
		*gcc_except_table_out = (gpointer) res.gcc_except_table.addr;
	if (gcc_except_table_size_out)
		*gcc_except_table_size_out = (guint32) res.gcc_except_table.size;
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
