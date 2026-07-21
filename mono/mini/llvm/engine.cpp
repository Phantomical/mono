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
#include <llvm/ExecutionEngine/Orc/Core.h>
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
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MemoryBuffer.h>
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
};

/* ---- relocation audit ----------------------------------------------------
 *
 * See engine.hpp for what this guards and why the emitted relocations, rather
 * than the target machine's reported code model, are the observable that is
 * checked.
 */

/*
 * Does this x86-64 relocation force a full 64-bit address through a field
 * narrower than 64 bits, with no indirection to absorb the range?
 *
 * INCLUDED, and why each one is genuinely unsafe here:
 *   - R_X86_64_32 / R_X86_64_32S: absolute 32-bit. Cannot name a section above
 *     4 GB at all. Small-model codegen emits 32S for, among other things, a
 *     jump-table base address.
 *   - R_X86_64_PC32: 32-bit displacement resolved directly by RuntimeDyld
 *     against the real load address, guarded only by assert(isInt<32>()). The
 *     small model emits one per function in .eh_frame, which encodes its FDE
 *     pc-begin as DW_EH_PE_pcrel|sdata4 - a displacement from the .eh_frame
 *     allocation to the code allocation. The large model emits PC64 there.
 *   - the 16/8-bit forms, for completeness; codegen never emits them here.
 *
 * EXCLUDED - and note carefully that this is a PRAGMATIC exclusion, not a
 * safety argument:
 *   - R_X86_64_PLT32 and R_X86_64_GOTPCREL/GOTPCRELX/REX_GOTPCRELX are left
 *     out because the large model never emits them (measured: zero across a
 *     603-module corpus of real mono methods), so including them would buy no
 *     coverage while risking noise. They are NOT safe in principle. RTDyld
 *     does interpose - a stub for PLT32, a GOT entry for GOTPCREL* - but the
 *     GOT is not local to the code: RuntimeDyldELF rewrites GOTPCREL* into a
 *     PC32 against the GOT section, which it allocates through
 *     allocateDataSection(IsReadOnly=false). In SectionMemoryManager that is
 *     the RWDataMem group, a different set of mmap slabs from the CodeMem
 *     group the referring code lives in (see the three MemoryGroup members of
 *     SectionMemoryManager). So a code->GOT reference is a 32-bit displacement
 *     across two independent allocations and hits exactly the same bare
 *     assert(isInt<32>()) as everything above. The PLT32 stub's own GOT load
 *     has the same shape.
 *
 *     CONSEQUENCE, stated plainly: this auditor has a false-negative gap. If
 *     some future codegen change made the large model emit GOTPCREL*, the
 *     audit would stay silent on a genuinely unsafe relocation. It is tolerable
 *     today only because a code-model regression also flips every function's
 *     .eh_frame to PC32 (603 of 603 modules in the corpus), which the audit
 *     does catch - i.e. the gap is masked by a louder signal, not closed.
 *   - R_X86_64_GOT32: a 32-bit offset within the GOT, not an address.
 *
 * Non-x86-64 targets are not classified at all; see audit_relocations().
 */
static bool
x86_64_reloc_truncates_address (uint64_t type)
{
	switch (type) {
	case ELF::R_X86_64_32:
	case ELF::R_X86_64_32S:
	case ELF::R_X86_64_PC32:
	case ELF::R_X86_64_16:
	case ELF::R_X86_64_PC16:
	case ELF::R_X86_64_8:
	case ELF::R_X86_64_PC8:
		return true;
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

	/* Locate the loaded .eh_frame for the EH port. */
	for (const object::SectionRef &sec : obj.sections ()) {
		Expected<StringRef> name = sec.getName ();
		if (!name) {
			consumeError (name.takeError ());
			continue;
		}
		if (*name != ".eh_frame")
			continue;
		info.eh_frame.addr = (uint8_t *) (uintptr_t) loaded.getSectionLoadAddress (sec);
		info.eh_frame.size = sec.getSize ();
		break;
	}

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
 * WHY THE LARGE CODE MODEL IS PINNED, AND WHY ONLY ON x86-64.
 *
 * The legacy LLVM 6 shim (mini-llvm-cpp.cpp, no longer built) says mono
 * "requires" JIT code to be allocated with MAP_32BIT. That requirement was
 * about the SMALL code model: small-model x86-64 codegen reaches its targets
 * through 32-bit-wide relocations, which cannot span an arbitrary distance.
 * RuntimeDyld only assert()s on such an overflow and the shipped LLVM has
 * assertions off, so the failure would be a silent truncation.
 *
 * This engine does not need MAP_32BIT, and the reason is the code model, not
 * luck about where mmap lands. Measured over ~13,700 loaded sections across
 * the mini regression suite (basic/generics/exceptions/objects/iltests/
 * arrays/basic-math, plus a MONO_TIERED=1 run):
 *
 *   - relocation histogram: 28,775 R_X86_64_64 (.ltext/.rodata) and
 *     4,701 R_X86_64_PC64 (.eh_frame). Nothing narrower.
 *   - every section landed ABOVE 4 GB (0x7f10_a5cb_f000 .. 0x7fd5_1e09_c000);
 *     not one below. Low-address allocation is emphatically NOT what makes
 *     this work - the code model is.
 *   - the section names are .ltext / .ldata, the large-model prefixes, which
 *     is the model's own signature in the object file.
 *
 * That is what lets this engine use a stock SectionMemoryManager.
 *
 * WHAT THE PIN DOES AND DOES NOT BUY. On x86-64 it is a no-op today: LLVM's
 * X86 backend already picks Large for a 64-bit JIT with no model requested
 * (getEffectiveX86CodeModel: `if (JIT) return Is64Bit ? Large : Small`), and
 * JITTargetMachineBuilder::createTargetMachine passes JIT=true. Confirmed on
 * LLVM 18.1.3 - with and without this call the emitted object is identical.
 * Its only value is to remove the dependency on that internal default. Note
 * that it cannot be *self-checked*: X86TargetMachine returns a requested model
 * verbatim, so reading the model back off the target machine after pinning it
 * only proves that setCodeModel() works. The invariant is instead checked
 * where it is real - on the relocations of the emitted object, by
 * accumulate_reloc_audit() here and by the reloc-widths case in
 * mono/unit-tests/test-llvm-engine.cpp.
 *
 * WHY THE GATE. This function is arch-agnostic (detectHost()) and engine.cpp
 * is built under a plain `if ENABLE_LLVM` with no arch condition, so it runs
 * on every host mono has a port for. Measured against this same libLLVM-18.1.3
 * by emitting one probe module per triple twice, with and without the pin, and
 * diffing the object bytes and the relocation histogram. JIT default code
 * model, then the observed effect of pinning Large:
 *
 *   x86_64   Large  -> objects byte-identical; pin is a no-op
 *   aarch64  Large  -> objects byte-identical; pin is a no-op
 *   armv7    Small  -> objects byte-identical; the pin is accepted and then
 *                      ignored by the ARM backend
 *   s390x    Medium -> objects byte-identical for the probe. The effective
 *                      model does become Large, but no codegen difference was
 *                      observed; the codegen effect is NOT characterised.
 *   i686     Small  -> objects DIFFER: the external call goes from R_386_PLT32
 *                      to an absolute R_386_32. "Large" is meaningless in a
 *                      32-bit address space anyway.
 *   ppc64le  Small  -> objects DIFFER: .TOC. addressing changes
 *                      REL16_HA/REL16_LO/TOC16_DS -> REL64/TOC16_HA/TOC16_LO_DS
 *   riscv32/64 Small -> pinning Large ABORTS the process during codegen:
 *                      "LLVM ERROR: Unsupported code model for lowering".
 *                      Note createTargetMachine() ACCEPTS it silently; the
 *                      abort lands on the first method compiled.
 *
 * So outside x86-64 the pin ranges from pointless to fatal, and nowhere does
 * it buy anything: the two arches that would want Large already default to it.
 * Restricting the pin to x86-64 is therefore the whole of its correct scope -
 * it is exactly the target whose relocation behaviour has been analysed (see
 * x86_64_reloc_truncates_address) and whose small-model failure mode motivated
 * it. aarch64 is deliberately left on its (already Large) default rather than
 * pinned, so that a port which needs a different model there is not silently
 * overridden by an x86-derived assumption.
 *
 * OPEN, NOT SOLVED, FOR OTHER 64-BIT PORTS: on ppc64le/riscv64/s390x the JIT
 * default is not Large, so the "sections may live anywhere above 4 GB"
 * assumption is unproven there. The relocation audit is x86-64-only and will
 * report "did not look" on those hosts. Porting the engine to them means
 * analysing their relocation sets and, if needed, constraining allocation -
 * NOT pinning Large, which two of the three reject outright.
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
	if (jtmb.getTargetTriple ().getArch () == Triple::x86_64)
		jtmb.setCodeModel (CodeModel::Large);
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

MonoLLVMJIT::MonoLLVMJIT ()
	: tsctx_ (std::make_unique<LLVMContext> ())
{
	ensure_native_target ();

	LLJITBuilder builder;
	builder.setJITTargetMachineBuilder (host_target_machine_builder ());
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
	Module *module = func->getParent ();
	module->setDataLayout (jit_->getDataLayout ());

	PassBuilder pb;
	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cgam;
	ModuleAnalysisManager mam;
	pb.registerModuleAnalyses (mam);
	pb.registerCGSCCAnalyses (cgam);
	pb.registerFunctionAnalyses (fam);
	pb.registerLoopAnalyses (lam);
	pb.crossRegisterProxies (lam, fam, cgam, mam);

	FunctionPassManager fpm = pb.buildFunctionSimplificationPipeline (
		OptimizationLevel::O2, ThinOrFullLTOPhase::None);
	fpm.run (*func, fam);
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
                          gpointer *dwarf_eh_frame_out, guint32 *dwarf_eh_frame_size_out)
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
