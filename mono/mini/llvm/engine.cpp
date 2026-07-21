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
 * Three parts:
 *   1. The pure-LLVM engine core (class mono::MonoLLVMJIT + MonoJitMemoryManager).
 *   2. mono_llvm_engine_run_selftest(): an extern "C" self-test that builds
 *      hand-crafted modules, JITs them through the real compile() path and
 *      checks the results. Driven by mono/unit-tests/test-llvm-engine.c.
 *   3. The extern "C" mono boundary: thin adapters that unwrap the llvm-c
 *      handles the translator passes and forward to the core.
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
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
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
		SectionMemoryManager::registerEHFrames (Addr, LoadAddr, Size);
	}

	void deregisterEHFrames () override
	{
		SectionMemoryManager::deregisterEHFrames ();
	}
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

static JITTargetMachineBuilder
host_target_machine_builder ()
{
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

MonoLLVMJIT *
MonoLLVMJIT::get_singleton ()
{
	static MonoLLVMJIT *instance = new MonoLLVMJIT ();
	return instance;
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
                      StringRef eh_symbol)
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
	 * createJITDylib (not bare) so the platform sets up the dylib for IR
	 * materialization; a bare dylib segfaults on addIRModule. Its link order
	 * gets only helpers_jd_ (explicit runtime helpers) - not the LLJIT main
	 * dylib and its process-symbol generator.
	 */
	std::string jd_name = "mono.jit." + std::to_string (module_counter_++);
	JITDylib &jd = cantFail (es.createJITDylib (jd_name));
	jd.addToLinkOrder (*helpers_jd_);

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

/* ---- self-test ------------------------------------------------------------
 * Exercises the engine core end to end: builds hand-crafted modules, JITs them
 * through the real compile() path, calls the results, and checks the values.
 * Exposed as an extern "C" entry (mono_llvm_engine_run_selftest) so mono's C
 * unit-test harness (mono/unit-tests/test-llvm-engine.c) can drive it without
 * touching the LLVM C++ API. Returns 0 on success, non-zero on failure.
 */

extern "C" int mono_llvm_engine_run_selftest (void);

using namespace llvm;
using mono::MonoLLVMJIT;

namespace {

/*
 * The runtime helper the second self-test registers. It has internal linkage
 * (static), so it is NOT a process symbol, and it is registered under a
 * deliberately un-mangled name that does not exist anywhere in the process.
 * Consequently: if the engine ever resolved externals via a process-symbol
 * search instead of our explicit register_symbol(), this name would fail to
 * resolve and the lookup inside compile() would abort. A passing call therefore
 * proves the JITed code reached exactly the pointer we registered - never a
 * process symbol.
 */
static int64_t
selftest_helper_impl (int64_t x)
{
	return x * 3 + 7;
}

static const char SELFTEST_HELPER_NAME[] = "mono$selftest$helper$absent_from_process";

#define SELFTEST_CHECK(cond)                                                    \
	do {                                                                    \
		if (!(cond)) {                                                  \
			fprintf (stderr, "mono llvm engine selftest FAILED: %s "\
			         "(at %s:%d)\n", #cond, __FILE__, __LINE__);   \
			return 1;                                              \
		}                                                              \
	} while (0)

/* int64 add_i64(int64 a, int64 b) { return a + b; } */
static int
selftest_arithmetic (MonoLLVMJIT *jit)
{
	LLVMContext &ctx = jit->context ();
	auto module = std::make_unique<Module> ("selftest.arith", ctx);
	Type *i64 = Type::getInt64Ty (ctx);
	FunctionType *fty = FunctionType::get (i64, {i64, i64}, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "add_i64", module.get ());
	BasicBlock *bb = BasicBlock::Create (ctx, "entry", fn);
	IRBuilder<> b (bb);
	auto arg = fn->arg_begin ();
	Value *a = &*arg++;
	Value *c = &*arg;
	b.CreateRet (b.CreateAdd (a, c));

	/* compile() clones the module; our unique_ptr keeps owning the original and
	 * frees it on scope exit - no release(), no leak, no double free. */
	mono::CompileResult res = jit->compile (fn, {}, nullptr, "");
	uint64_t addr = res.entry;
	SELFTEST_CHECK (addr != 0);
	/* The size channel must report a real body, not silently zero. */
	SELFTEST_CHECK (res.code_size > 0);
	auto compiled = reinterpret_cast<int64_t (*) (int64_t, int64_t)> (addr);
	SELFTEST_CHECK (compiled (20, 22) == 42);
	SELFTEST_CHECK (compiled (-5, 5) == 0);
	return 0;
}

/*
 * use_helper(x) = <registered helper>(x) + 1.
 * The IR's external callee is named SELFTEST_HELPER_NAME (absent from the
 * process) and registered to point at selftest_helper_impl (internal linkage).
 * Only our register_symbol() can satisfy that name, so correct results prove the
 * call resolved to the registered pointer, not to any process symbol.
 */
static int
selftest_registered_helper (MonoLLVMJIT *jit)
{
	jit->register_symbol (SELFTEST_HELPER_NAME, (void *) &selftest_helper_impl);

	LLVMContext &ctx = jit->context ();
	auto module = std::make_unique<Module> ("selftest.helper", ctx);
	Type *i64 = Type::getInt64Ty (ctx);
	FunctionType *helper_ty = FunctionType::get (i64, {i64}, false);
	Function *helper = Function::Create (helper_ty, Function::ExternalLinkage,
	                                     SELFTEST_HELPER_NAME, module.get ());
	FunctionType *fty = FunctionType::get (i64, {i64}, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "use_helper", module.get ());
	BasicBlock *bb = BasicBlock::Create (ctx, "entry", fn);
	IRBuilder<> b (bb);
	Value *x = &*fn->arg_begin ();
	Value *called = b.CreateCall (helper, {x});
	b.CreateRet (b.CreateAdd (called, ConstantInt::get (i64, 1)));

	jit->optimize (fn); /* also exercise the optimizer */

	/* compile() clones; keep owning the original (freed on scope exit). */
	mono::CompileResult res = jit->compile (fn, {}, nullptr, "");
	uint64_t addr = res.entry;
	SELFTEST_CHECK (addr != 0);
	SELFTEST_CHECK (res.code_size > 0);
	auto compiled = reinterpret_cast<int64_t (*) (int64_t)> (addr);
	/* selftest_helper_impl(x) = x*3+7, then +1. Only reachable via registration. */
	SELFTEST_CHECK (compiled (10) == 38); /* (10*3+7)+1 */
	SELFTEST_CHECK (compiled (0) == 8);   /* (0*3+7)+1  */
	return 0;
}

#undef SELFTEST_CHECK

} // namespace

extern "C" int
mono_llvm_engine_run_selftest (void)
{
	MonoLLVMJIT *jit = MonoLLVMJIT::get_singleton ();
	int rc = selftest_arithmetic (jit);
	if (rc)
		return rc;
	return selftest_registered_helper (jit);
}

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
                          gpointer *eh_frame, guint32 *code_size_out)
{
	(void) mono_ee;
	(void) cfg;

	auto *jit = mono::MonoLLVMJIT::get_singleton ();
	auto *entry = llvm::unwrap<llvm::Function> (method);

	llvm::SmallVector<llvm::GlobalVariable *, 8> vars;
	vars.reserve (nvars);
	for (int i = 0; i < nvars; ++i)
		vars.push_back (llvm::unwrap<llvm::GlobalVariable> (callee_vars[i]));

	std::vector<uint64_t> addrs (nvars);

	mono::CompileResult res = jit->compile (entry, vars, addrs.data (), "mono_eh_frame");

	for (int i = 0; i < nvars; ++i)
		callee_addrs[i] = (gpointer) (gsize) addrs[i];
	if (eh_frame)
		*eh_frame = (gpointer) (gsize) res.mono_eh_frame;
	if (code_size_out)
		*code_size_out = (guint32) res.code_size;

	return (gpointer) (gsize) res.entry;
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
