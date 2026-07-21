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
#include <mutex>
#include <string>
#include <vector>

#include <cstdint>
#include <cstdio>

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
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/Utils/Cloning.h>

using namespace llvm;
using namespace llvm::orc;

namespace mono {

/*
 * .eh_frame of the module currently being materialized, on the compiling
 * thread. RuntimeDyld calls MemoryManager::registerEHFrames() during the
 * synchronous lookup inside compile() (0 compile threads), so a thread_local is
 * a correct and simple channel from the per-object memory manager back to
 * compile(). Reset by compile() before each materialization.
 */
static thread_local EhFrameInfo t_current_eh_frame;

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
		/* Capture for step 3b's mono-side unwinder wiring. */
		t_current_eh_frame.addr = Addr;
		t_current_eh_frame.size = Size;
		/*
		 * Still register with the host (libgcc/libunwind) __register_frame so
		 * JITted code can unwind during this milestone. Step 3b replaces this
		 * base call with mono-native registration of the captured section.
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
			return std::make_unique<RTDyldObjectLinkingLayer> (
				es, [] () { return std::make_unique<MonoJitMemoryManager> (); });
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

const EhFrameInfo &
MonoLLVMJIT::last_eh_frame () const
{
	return t_current_eh_frame;
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

uint64_t
MonoLLVMJIT::compile (Function *entry,
                      ArrayRef<GlobalVariable *> callee_vars,
                      uint64_t *callee_addrs,
                      StringRef eh_symbol,
                      uint64_t *eh_out)
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
	JITDylib &jd = cantFail (es.createJITDylib ("mono.jit." + std::to_string (module_counter_++)));
	jd.addToLinkOrder (*helpers_jd_);

	t_current_eh_frame = EhFrameInfo{};

	cantFail (jit_->addIRModule (jd, ThreadSafeModule (std::move (clone), tsctx_)));

	/*
	 * STEP 3b: cantFail() aborts on a resolution failure (e.g. an icall helper
	 * that was never register_symbol()'d). 3b will want a recoverable path here
	 * - propagate the llvm::Error out so the translator can fall back to tier-0
	 * - rather than aborting the process.
	 */
	uint64_t body = cantFail (jit_->lookup (jd, entry_name)).getValue ();

	for (size_t i = 0; i < var_names.size (); ++i)
		callee_addrs[i] = cantFail (jit_->lookup (jd, var_names[i])).getValue ();

	/*
	 * The "mono_eh_frame" global only exists when the module was produced by
	 * the FORKED LLVM, whose MonoEHFrame emission synthesized it. Against
	 * unmodified LLVM 18 there is no such global: stock LLVM emits a standard
	 * DWARF .eh_frame section instead (captured separately by the memory
	 * manager, see last_eh_frame()), and consuming that is not ported yet -
	 * which is why methods with EH clauses currently bail to the classic JIT.
	 *
	 * So a missing eh symbol is the normal case today, not an error: report
	 * "no mono-format EH info" rather than aborting the process.
	 */
	if (!eh_name.empty () && eh_out) {
		*eh_out = 0;
		if (auto sym = jit_->lookup (jd, eh_name))
			*eh_out = sym->getValue ();
		else
			consumeError (sym.takeError ());
	}

	return body;
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
	uint64_t addr = jit->compile (fn, {}, nullptr, "", nullptr);
	SELFTEST_CHECK (addr != 0);
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
	uint64_t addr = jit->compile (fn, {}, nullptr, "", nullptr);
	SELFTEST_CHECK (addr != 0);
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
                          gpointer *eh_frame)
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
	uint64_t eh_addr = 0;

	uint64_t body = jit->compile (entry, vars, addrs.data (), "mono_eh_frame", &eh_addr);

	for (int i = 0; i < nvars; ++i)
		callee_addrs[i] = (gpointer) (gsize) addrs[i];
	if (eh_frame)
		*eh_frame = (gpointer) (gsize) eh_addr;

	return (gpointer) (gsize) body;
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
