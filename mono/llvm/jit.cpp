/**
 * \file
 * \brief MonoJit - the ORCv2/LLJIT execution engine for the LLVM-only backend.
 */

#include "jit.hpp"

#include "stubs.hpp"

#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/OrcABISupport.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

#include <memory>

using namespace llvm;
using namespace llvm::orc;

namespace mono {

/*
 * Where a stub lands when the compile behind it failed. The trampoline has
 * already put the call's arguments back and jumped here, so this is running as
 * the method the caller asked for: there is no value it could return and no
 * caller that would know what to do with one.
 */
[[noreturn]] static void
lazy_compile_failed ()
{
	report_fatal_error ("a method failed to compile on first call", false);
}

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

/*
 * The host target configuration every compile uses.
 *
 * Code model Small with Reloc::PIC_ rather than the JIT default (Large):
 * JITLink reroutes any reference it cannot prove in-range through an in-graph
 * GOT slot or PLT stub whose outgoing edge is a full 64-bit pointer, so
 * Small+PIC is correct wherever sections land and considerably denser.
 *
 * CodeGenOptLevel::None is the tier-0 choice on purpose: it selects FastISel,
 * which is the cheap-and-cheerful instruction selection this tier wants - the
 * easy wins come from the O1 IR pipeline (run_tier0_pipeline), not from the
 * optimizing selector. FastISel falls back to SelectionDAG per block for
 * constructs it does not cover (musttail among them), which costs compile
 * time, never correctness.
 */
static JITTargetMachineBuilder
host_target_machine_builder ()
{
	ensure_native_target ();

	auto jtmb = cantFail (JITTargetMachineBuilder::detectHost ());
	jtmb.setCodeGenOptLevel (CodeGenOptLevel::None);
	jtmb.setCPU (std::string (sys::getHostCPUName ()));
	jtmb.setCodeModel (CodeModel::Small);
	jtmb.setRelocationModel (Reloc::PIC_);

	/*
	 * If codegen ever reaches an LLVM `unreachable` (a translator bug, or UB
	 * the IL could not rule out), a `ud2` beats falling through into whatever
	 * bytes come next.
	 */
	jtmb.getOptions ().TrapUnreachable = true;

	StringMap<bool> features = sys::getHostCPUFeatures ();
	std::vector<std::string> feature_vec;
	for (auto &kv : features)
		if (kv.second)
			feature_vec.push_back ((Twine ("+") + kv.first ()).str ());
	jtmb.addFeatures (feature_vec);
	return jtmb;
}

void
MonoJit::run_tier0_pipeline (Module &m)
{
	/*
	 * A TargetMachine so TargetTransformInfo is real; without one the
	 * cost-model-driven parts of the pipeline silently no-op.
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
		pb.buildPerModuleDefaultPipeline (OptimizationLevel::O1);
	mpm.run (m, mam);
}

Expected<std::unique_ptr<MonoJit>>
MonoJit::create ()
{
	ensure_native_target ();

	LLJITBuilder builder;
	builder.setJITTargetMachineBuilder (host_target_machine_builder ());

	/*
	 * JITLink, not the RTDyldObjectLinkingLayer LLJIT still defaults to on
	 * ELF. The layer's default in-process memory manager is enough for now; a
	 * bounded slab reservation for method bodies comes later. LLJIT's generic
	 * platform setup attaches its eh-frame registration plugin to this layer
	 * on its own.
	 */
	builder.setObjectLinkingLayerCreator (
		[] (ExecutionSession &es) -> Expected<std::unique_ptr<ObjectLayer>> {
			return std::make_unique<ObjectLinkingLayer> (es);
		});

	auto jit = builder.create ();
	if (!jit)
		return jit.takeError ();

	std::unique_ptr<MonoJit> self (new MonoJit (std::move (*jit)));

	/*
	 * Every added module goes through the tier-0 pipeline on its way to the
	 * compiler. The transform layer sits under LLJIT's init-helper layer, so
	 * this runs after any platform rewriting and immediately before codegen.
	 */
	self->jit_->getIRTransformLayer ().setTransform (
		[] (ThreadSafeModule tsm, MaterializationResponsibility &)
			-> Expected<ThreadSafeModule> {
			tsm.withModuleDo ([] (Module &m) { run_tier0_pipeline (m); });
			return std::move (tsm);
		});

	ExecutionSession &es = self->jit_->getExecutionSession ();
	self->helpers_ = &es.createBareJITDylib ("mono.helpers");
	self->stubs_ = &es.createBareJITDylib ("mono.stubs");

	auto redirectable = make_redirectable_symbol_manager (es);
	if (!redirectable)
		return redirectable.takeError ();
	self->redirectable_ = std::move (*redirectable);

	auto callbacks =
		LocalJITCompileCallbackManager<OrcX86_64_SysV>::Create (
			es, ExecutorAddr::fromPtr (&lazy_compile_failed));
	if (!callbacks)
		return callbacks.takeError ();
	self->callbacks_ = std::move (*callbacks);

	return std::move (self);
}

MonoJit::MonoJit (std::unique_ptr<LLJIT> jit)
	: jit_ (std::move (jit))
{
}

MonoJit::~MonoJit () = default;

const DataLayout &
MonoJit::data_layout () const
{
	return jit_->getDataLayout ();
}

Error
MonoJit::register_symbol (StringRef name, void *addr)
{
	std::lock_guard<std::mutex> lock (named_symbols_mutex_);

	auto it = named_symbols_.find (name.str ());
	if (it != named_symbols_.end ())
		return Error::success ();

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

Expected<void *>
MonoJit::create_stub (StringRef name, void *target)
{
	ExecutionSession &es = jit_->getExecutionSession ();

	SymbolMap dests;
	dests[es.intern (name)] = {
		ExecutorAddr::fromPtr (target),
		JITSymbolFlags::Exported | JITSymbolFlags::Callable,
	};

	if (Error err = redirectable_->createRedirectableSymbols (
	        stubs_->getDefaultResourceTracker (), std::move (dests)))
		return std::move (err);

	return stub_address (name);
}

Expected<void *>
MonoJit::create_lazy_stub (StringRef name, LazyCompileFunction compile)
{
	ExecutionSession &es = jit_->getExecutionSession ();

	/*
	 * ORC's callback takes a copyable std::function, and a compile closure
	 * carrying a module is move-only.
	 */
	auto shared = std::make_shared<LazyCompileFunction> (std::move (compile));
	std::string method = name.str ();

	/*
	 * ORC materializes this once however many threads arrive together, and
	 * hands them all the same answer, so the redirect below happens once too.
	 */
	Expected<ExecutorAddr> trampoline = callbacks_->getCompileCallback (
		[this, &es, method, shared] () -> ExecutorAddr {
			Expected<void *> code = (*shared) ();
			if (!code) {
				es.reportError (code.takeError ());
				return ExecutorAddr::fromPtr (&lazy_compile_failed);
			}

			if (Error err = redirect_stub (method, *code)) {
				es.reportError (std::move (err));
				return ExecutorAddr::fromPtr (&lazy_compile_failed);
			}

			return ExecutorAddr::fromPtr (*code);
		});
	if (!trampoline)
		return trampoline.takeError ();

	return create_stub (name, trampoline->toPtr<void *> ());
}

Error
MonoJit::redirect_stub (StringRef name, void *target)
{
	ExecutionSession &es = jit_->getExecutionSession ();

	SymbolMap dests;
	dests[es.intern (name)] = {
		ExecutorAddr::fromPtr (target),
		JITSymbolFlags::Exported | JITSymbolFlags::Callable,
	};

	return redirectable_->redirect (*stubs_, dests);
}

Expected<void *>
MonoJit::stub_address (StringRef name)
{
	Expected<ExecutorAddr> sym = jit_->lookup (*stubs_, name);
	if (!sym)
		return sym.takeError ();
	return sym->toPtr<void *> ();
}

Expected<void *>
MonoJit::compile (ThreadSafeModule tsm, StringRef entry)
{
	/*
	 * An assertions-on LLVM refuses to codegen a module whose layout
	 * disagrees with the target, and a fresh module has none at all.
	 */
	tsm.withModuleDo ([&] (Module &m) {
		if (m.getDataLayout ().isDefault ())
			m.setDataLayout (jit_->getDataLayout ());
	});

	/*
	 * A dylib per module, resolving only through mono.helpers and mono.stubs:
	 * JIT'd code can reach exactly what was registered and the methods that
	 * have been published, nothing else. Calls to another method bind to its
	 * stub by name, which is what keeps them correct across promotions. Bare,
	 * because these modules carry no initializers for the platform to manage.
	 */
	std::string jd_name =
		("jd." + Twine (module_counter_.fetch_add (1)) + "." + entry).str ();
	JITDylib &jd =
		jit_->getExecutionSession ().createBareJITDylib (std::move (jd_name));
	jd.addToLinkOrder (*helpers_);
	jd.addToLinkOrder (*stubs_);

	if (Error err = jit_->addIRModule (jd, std::move (tsm)))
		return std::move (err);

	Expected<ExecutorAddr> sym = jit_->lookup (jd, entry);
	if (!sym)
		return sym.takeError ();

	return sym->toPtr<void *> ();
}

} // namespace mono
