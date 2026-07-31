/**
 * \file
 * \brief MonoJit - the ORCv2/LLJIT execution engine for the LLVM-only backend.
 */

#include "jit.hpp"

#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

using namespace llvm;
using namespace llvm::orc;

namespace mono {

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
	 * bounded slab reservation (and the redirectable-stub manager, which
	 * wants this layer type) come later. LLJIT's generic platform setup
	 * attaches its eh-frame registration plugin to this layer on its own.
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

	self->helpers_ =
		&self->jit_->getExecutionSession ().createBareJITDylib ("mono.helpers");

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
	 * A dylib per module, resolving only through mono.helpers: JIT'd code can
	 * reach exactly what was registered, nothing else. Bare, because these
	 * modules carry no initializers for the platform to manage.
	 */
	std::string jd_name =
		("jd." + Twine (module_counter_.fetch_add (1)) + "." + entry).str ();
	JITDylib &jd =
		jit_->getExecutionSession ().createBareJITDylib (std::move (jd_name));
	jd.addToLinkOrder (*helpers_);

	if (Error err = jit_->addIRModule (jd, std::move (tsm)))
		return std::move (err);

	Expected<ExecutorAddr> sym = jit_->lookup (jd, entry);
	if (!sym)
		return sym.takeError ();

	return sym->toPtr<void *> ();
}

} // namespace mono
