/**
 * \file
 * \brief MonoJit - the ORCv2/LLJIT execution engine for the LLVM-only backend.
 */

#include "jit.hpp"

#include "arch/arch.hpp"
#include "compiler.hpp"
#include "nearmem.hpp"

#include "../mini/llvm/il-line-table.hpp"
#include "passes/array-address.hpp"
#include "passes/lower-builtins.hpp"
#include "passes/restore-tail-position.hpp"
#include "stubs.hpp"

#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/MapperJITLinkMemoryManager.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/OrcABISupport.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/Scalar/TailRecursionElimination.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
	/*
	 * Printed and left by hand rather than through report_fatal_error, which
	 * ends in exit() when it is told not to produce a crash diagnostic. exit()
	 * runs the static destructors of every C++ library loaded into the
	 * process, LLVM's own among them, while the threads that are still
	 * compiling are using what those destructors free.
	 */
	static const char msg[] =
		"LLVM ERROR: a method failed to compile on first call\n";
	[[maybe_unused]] ssize_t written = write (2, msg, sizeof (msg) - 1);
	fflush (nullptr);
	_exit (1);
}

namespace {

/// How much of what this backend produces the IR verifier gets to see.
enum class VerifyLevel {
	/// Nothing is checked.
	off,
	/// The translator's output, every pass written here, and the module
	/// codegen is handed.
	mono,
	/// The above plus every stock pass in the optimization pipeline.
	each,
};

/*
 * MONO_LLVM_JIT_VERIFY picks the level, defaulting to `mono` against an LLVM
 * built with assertions - the configuration that is being checked rather than
 * shipped. Malformed IR is worth that: it does not crash codegen, it
 * miscompiles, because the register allocator reads whatever the broken value
 * happened to leave behind.
 */
VerifyLevel
verify_level ()
{
	static VerifyLevel level = [] {
#ifdef MONO_LLVM_ASSERTIONS
		constexpr VerifyLevel unset = VerifyLevel::mono;
#else
		constexpr VerifyLevel unset = VerifyLevel::off;
#endif
		const char *v = std::getenv ("MONO_LLVM_JIT_VERIFY");

		if (v == nullptr)
			return unset;

		StringRef setting (v);
		if (setting == "0" || setting == "off")
			return VerifyLevel::off;
		if (setting == "each" || setting == "all")
			return VerifyLevel::each;
		return VerifyLevel::mono;
	}();

	return level;
}

/*
 * The verifier's diagnostics name the offending instruction and nothing else,
 * which in a JIT that compiles thousands of methods leaves out both the method
 * and what had just run over it. Print those, and the module the failure is
 * about, so this is diagnosable from the log rather than by bisecting.
 */
[[noreturn]] void
report_broken_ir (const Module &m, StringRef when, StringRef diagnostics)
{
	errs () << "mono: broken IR " << when << ", in " << m.getModuleIdentifier ()
	        << "\n"
	        << diagnostics << m << "\n";
	report_fatal_error ("mono: IR verification failed", /*GenCrashDiag=*/false);
}

void
verify_or_die (const Module &m, StringRef when)
{
	std::string diagnostics;
	raw_string_ostream os (diagnostics);

	if (verifyModule (m, &os))
		report_broken_ir (m, when, diagnostics);
}

/// The passes this backend writes, by the name the pass instrumentation reports
/// them under. A break introduced by one of these is a bug here.
bool
is_mono_pass (StringRef pass)
{
	return pass == ArrayAddressPass::name () ||
	       pass == LowerBuiltinsPass::name () ||
	       pass == RestoreTailPositionPass::name () ||
	       pass == arch::LegacyAbiPass::name ();
}

} // namespace

/*
 * Reduce the just-compiled (not yet linked) object's `.debug_line` to per-
 * function rows: the IL offset in effect at each offset into each function.
 *
 * Reading the INPUT object rather than the linked graph is what forces the hook
 * this runs from: the debug sections are not SHF_ALLOC, so JITLink neither
 * allocates nor relocates them and there would be nothing to read on the other
 * side. In a relocatable object DWARFContext applies the debug-section
 * relocations itself, so a row's address is already an offset within `.text`.
 *
 * Several rows landing on one address is what a run of IL instructions collapses
 * to once the optimizer is done with it. They arrive in code order, so the last
 * one is the offset in effect there; keeping that one is what makes the map
 * single-valued, and it agrees with the "most recent point execution passed"
 * lookup that reads the map back.
 */
static void
parse_il_line_table (MemoryBufferRef obj_buf,
                     std::map<std::string, std::vector<IlLineRow>> &out)
{
	Expected<std::unique_ptr<object::ObjectFile>> obj =
		object::ObjectFile::createObjectFile (obj_buf);

	if (!obj) {
		consumeError (obj.takeError ());
		return;
	}

	std::unique_ptr<DWARFContext> dw = DWARFContext::create (**obj);
	if (!dw)
		return;

	struct FuncRange {
		uint64_t start;
		uint64_t size;
		std::string name;
	};
	std::vector<FuncRange> funcs;

	for (const object::SymbolRef &sym : (*obj)->symbols ()) {
		Expected<object::SymbolRef::Type> type = sym.getType ();
		Expected<StringRef> name = sym.getName ();
		Expected<uint64_t> value = sym.getValue ();

		if (!type || !name || !value) {
			consumeError (joinErrors (
				type ? Error::success () : type.takeError (),
				joinErrors (name ? Error::success () : name.takeError (),
				            value ? Error::success () : value.takeError ())));
			continue;
		}
		if (*type != object::SymbolRef::ST_Function)
			continue;

		funcs.push_back ({ *value, object::ELFSymbolRef (sym).getSize (),
		                   name->str () });
	}

	if (funcs.empty ())
		return;

	for (const std::unique_ptr<DWARFUnit> &cu : dw->compile_units ()) {
		const DWARFDebugLine::LineTable *lt = dw->getLineTableForUnit (cu.get ());

		if (!lt)
			continue;

		for (const DWARFDebugLine::Row &row : lt->Rows) {
			/*
			 * Line 0 is DWARF's "no source location" - what an instruction the
			 * translator never attributed produces. The bias keeps a real IL
			 * offset of 0 from looking like one.
			 */
			if (row.EndSequence || row.Line == 0)
				continue;

			const FuncRange *owner = nullptr;

			for (const FuncRange &f : funcs) {
				if (row.Address.Address < f.start)
					continue;
				if (f.size && row.Address.Address >= f.start + f.size)
					continue;
				owner = &f;
				break;
			}
			if (!owner)
				continue;

			IlLineRow line;
			line.native_offset =
				(uint32_t) (row.Address.Address - owner->start);
			line.il_offset = (uint32_t) (row.Line - IL_OFFSET_LINE_BIAS);

			std::vector<IlLineRow> &rows = out[owner->name];

			if (!rows.empty ()
			    && rows.back ().native_offset == line.native_offset)
				rows.back () = line;
			else
				rows.push_back (line);
		}
	}

	/*
	 * Rows are ascending by address within a sequence but need not be across
	 * sequences, since LLVM lays blocks out as it likes. The runtime binary-
	 * searches these, so sort - stably, so the last-row-wins choice above
	 * survives.
	 */
	for (auto &kv : out)
		std::stable_sort (kv.second.begin (), kv.second.end (),
		                  [] (const IlLineRow &a, const IlLineRow &b) {
			                  return a.native_offset < b.native_offset;
		                  });
}

/*
 * Reads, for every linked object, where the pieces the runtime needs landed:
 * each defined function's extent and the two mono side-table sections. Keyed by
 * the per-compile dylib, whose name is unique, so a method compiled twice never
 * collides with itself.
 *
 * The side-table sections carry no symbols and nothing references them, so
 * JITLink's pruning would drop them; the pre-prune pass marks their blocks
 * live. The read itself runs post-fixup, when addresses are final - and with
 * the in-process memory manager those addresses are readable memory in this
 * process from finalization on.
 */
class MonoJit::ObjectCapturePlugin : public ObjectLinkingLayer::Plugin {
public:
	struct Extents {
		std::vector<std::pair<std::string, std::pair<const uint8_t *, size_t>>>
			functions;
		const uint8_t *clause_table = nullptr;
		size_t clause_table_size = 0;
		const uint8_t *guard_table = nullptr;
		size_t guard_table_size = 0;
		const uint8_t *unwind_table = nullptr;
		size_t unwind_table_size = 0;
		/// Each defined function's line table, by name.
		std::map<std::string, std::vector<IlLineRow>> il_lines;
	};

	/*
	 * The one hook that sees the object bytes, which is the only place
	 * `.debug_line` exists: it is not SHF_ALLOC, so it never reaches the
	 * LinkGraph the passes below run over. Upstream marks this deprecated and
	 * promises "a proper mechanism for capturing object buffers"; there is not
	 * one yet, so this is the mechanism.
	 */
	void notifyMaterializing (MaterializationResponsibility &mr,
	                          jitlink::LinkGraph &, jitlink::JITLinkContext &,
	                          MemoryBufferRef input_object) override
	{
		std::map<std::string, std::vector<IlLineRow>> lines;

		parse_il_line_table (input_object, lines);
		if (lines.empty ())
			return;

		std::lock_guard<std::mutex> lock (mutex_);
		il_lines_[mr.getTargetJITDylib ().getName ()] = std::move (lines);
	}

	void modifyPassConfig (MaterializationResponsibility &mr,
	                       jitlink::LinkGraph &g,
	                       jitlink::PassConfiguration &config) override
	{
		std::string dylib = mr.getTargetJITDylib ().getName ();

		config.PrePrunePasses.push_back ([] (jitlink::LinkGraph &graph) -> Error {
			for (jitlink::Section &section : graph.sections ()) {
				if (section.getName () != ".mono_lsda"
				    && section.getName () != ".mono_guards"
				    && section.getName () != ".mono_unwind")
					continue;
				for (jitlink::Block *block : section.blocks ())
					graph.addAnonymousSymbol (*block, 0,
					                          block->getSize (), false,
					                          /*IsLive=*/true);
			}
			return Error::success ();
		});

		config.PostFixupPasses.push_back (
			[this, dylib] (jitlink::LinkGraph &graph) -> Error {
				Extents extents;

				for (jitlink::Section &section : graph.sections ()) {
					jitlink::SectionRange range (section);

					if (section.getName () == ".mono_lsda") {
						extents.clause_table =
							range.getStart ().toPtr<const uint8_t *> ();
						extents.clause_table_size = range.getSize ();
					} else if (section.getName () == ".mono_guards") {
						extents.guard_table =
							range.getStart ().toPtr<const uint8_t *> ();
						extents.guard_table_size = range.getSize ();
					} else if (section.getName () == ".mono_unwind") {
						extents.unwind_table =
							range.getStart ().toPtr<const uint8_t *> ();
						extents.unwind_table_size = range.getSize ();
					}
				}

				for (jitlink::Symbol *sym : graph.defined_symbols ()) {
					if (!sym->hasName () || !sym->isCallable ())
						continue;
					extents.functions.emplace_back (
						std::string (*sym->getName ()),
						std::make_pair (
							sym->getAddress ().toPtr<const uint8_t *> (),
							(size_t) sym->getSize ()));
				}

				std::lock_guard<std::mutex> lock (mutex_);
				captured_[dylib] = std::move (extents);
				return Error::success ();
			});
	}

	/// The extents captured for DYLIB's one object, surrendered to the caller.
	std::optional<Extents> take (StringRef dylib)
	{
		std::lock_guard<std::mutex> lock (mutex_);
		auto it = captured_.find (std::string (dylib));

		if (it == captured_.end ())
			return std::nullopt;

		Extents extents = std::move (it->second);
		captured_.erase (it);

		/*
		 * Captured by the other hook, before linking, so it is merged here
		 * rather than written into the same slot.
		 */
		if (auto lines = il_lines_.find (std::string (dylib));
		    lines != il_lines_.end ()) {
			extents.il_lines = std::move (lines->second);
			il_lines_.erase (lines);
		}

		return extents;
	}

	Error notifyFailed (MaterializationResponsibility &) override
	{
		return Error::success ();
	}
	Error notifyRemovingResources (JITDylib &, ResourceKey) override
	{
		return Error::success ();
	}
	void notifyTransferringResources (JITDylib &, ResourceKey, ResourceKey) override
	{
	}

private:
	std::mutex mutex_;
	std::map<std::string, Extents> captured_;
	std::map<std::string, std::map<std::string, std::vector<IlLineRow>>> il_lines_;
};

/*
 * The one JITLink memory manager every MonoJit links through.
 *
 * There is a MonoJit per appdomain, and this reserves address space in 16MB
 * units out of a pool that is one gigabyte wide (nearmem.hpp). One of these per
 * domain would therefore claim 16MB no other domain could touch and cap the
 * process at 63 domains that have compiled anything; shared, a domain's ranges
 * return to a common free list when its linker goes down and any live domain
 * can take them.
 *
 * Nothing domain-owned lives in here. It holds address ranges, not symbols or
 * relocations, and a range only reaches the free list after JITLink has run the
 * allocation's deallocation actions - so the code that was there is already
 * unreachable and deregistered before anyone else can be given the memory.
 *
 * Leaked on purpose: the ObjectLinkingLayers that borrow it are destroyed
 * whenever their domain is unloaded, and it has to outlive every one of them.
 */
static Expected<jitlink::JITLinkMemoryManager *>
shared_memory_manager ()
{
	static std::mutex mutex;
	static MapperJITLinkMemoryManager *shared = nullptr;

	std::lock_guard<std::mutex> lock (mutex);

	if (shared == nullptr) {
		auto mapper = NearMemoryMapper::Create ();

		if (!mapper)
			return mapper.takeError ();
		shared = new MapperJITLinkMemoryManager (16 * 1024 * 1024,
		                                         std::move (*mapper));
	}

	return shared;
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

static std::mutex g_options_mutex;
static std::vector<std::string> g_options;

void
MonoJit::add_option (StringRef opt)
{
	std::lock_guard<std::mutex> lock (g_options_mutex);

	g_options.push_back (opt.starts_with ("-") ? opt.str () : "-" + opt.str ());
}

/*
 * cl::ParseCommandLineOptions () is all-at-once - each call re-parses argv from
 * scratch - so the queued options are handed over in one batch, and only once.
 * Passing an error stream is what keeps a bad option from calling exit () out
 * from under the runtime.
 */
static Error
apply_options ()
{
	static bool applied = false;

	std::lock_guard<std::mutex> lock (g_options_mutex);
	if (applied || g_options.empty ())
		return Error::success ();
	applied = true;

	std::vector<const char *> argv {"mono"};
	for (const std::string &opt : g_options)
		argv.push_back (opt.c_str ());

	if (!cl::ParseCommandLineOptions ((int) argv.size (), argv.data (), "",
	                                  &errs ()))
		return createStringError (inconvertibleErrorCode (),
		                          "llvm rejected an option given with --llvm-opt");
	return Error::success ();
}

/*
 * The host target configuration every compile uses, detected once.
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
	static const JITTargetMachineBuilder jtmb = [] {
		ensure_native_target ();

		auto b = cantFail (JITTargetMachineBuilder::detectHost ());
		b.setCodeGenOptLevel (CodeGenOptLevel::None);
		b.setCPU (std::string (sys::getHostCPUName ()));
		b.setCodeModel (CodeModel::Small);
		b.setRelocationModel (Reloc::PIC_);

		/*
		 * If codegen ever reaches an LLVM `unreachable` (a translator bug, or
		 * UB the IL could not rule out), a `ud2` beats falling through into
		 * whatever bytes come next.
		 */
		b.getOptions ().TrapUnreachable = true;

		StringMap<bool> features = sys::getHostCPUFeatures ();
		std::vector<std::string> feature_vec;
		for (auto &kv : features)
			if (kv.second)
				feature_vec.push_back ((Twine ("+") + kv.first ()).str ());
		b.addFeatures (feature_vec);
		return b;
	}();
	return jtmb;
}

/*
 * One per compile thread, because building one is far from free - the X86
 * subtarget alone resolves a ~200-entry feature string against the implication
 * graph and then builds every lowering and legalizer table behind it, which for
 * methods the size the translator emits costs more than compiling them. A
 * TargetMachine is not safe to share between threads (this is why ORC's stock
 * ConcurrentIRCompiler builds one per module), but reusing one for module after
 * module on a single thread is exactly what SimpleCompiler does.
 */
TargetMachine &
host_target_machine ()
{
	static thread_local std::unique_ptr<TargetMachine> tm =
		cantFail (host_target_machine_builder ().createTargetMachine ());
	return *tm;
}

bool
ir_verification_enabled ()
{
	return verify_level () != VerifyLevel::off;
}

void
MonoJit::run_tier0_pipeline (Module &m)
{
	VerifyLevel verify = verify_level ();
	PassInstrumentationCallbacks pic;

	if (verify != VerifyLevel::off) {
		verify_or_die (m, "as translated");
		pic.registerAfterPassCallback (
			[&m, verify] (StringRef pass, Any, const PreservedAnalyses &) {
				if (verify == VerifyLevel::each || is_mono_pass (pass))
					verify_or_die (
						m, ("after pass \"" + pass + "\"").str ());
			});
	}

	/*
	 * A TargetMachine so TargetTransformInfo is real; without one the
	 * cost-model-driven parts of the pipeline silently no-op.
	 */
	PassBuilder pb (&host_target_machine (), PipelineTuningOptions (),
	                std::nullopt, &pic);
	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cgam;
	ModuleAnalysisManager mam;
	pb.registerModuleAnalyses (mam);
	pb.registerCGSCCAnalyses (cgam);
	pb.registerFunctionAnalyses (fam);
	pb.registerLoopAnalyses (lam);
	pb.crossRegisterProxies (lam, fam, cgam, mam);

	ModulePassManager mpm;

	/*
	 * Before the pipeline, so the optimizer sees the element arithmetic;
	 * after it, so it works over natural-typed calls and only what survives
	 * is lowered to the legacy boundary convention.
	 */
	mpm.addPass (ArrayAddressPass ());
	mpm.addPass (LowerBuiltinsPass ());

	/*
	 * The function simplification pipeline rather than the whole O1 module
	 * pipeline: a module here is a single method, so the module and CGSCC
	 * layers have nothing to work on - no internal function to specialize, and
	 * no callee body to inline, since every call the translator emits leaves
	 * the module by symbol. Running them anyway costs a large fraction of
	 * tier-0 compile time.
	 */
	FunctionPassManager fpm = pb.buildFunctionSimplificationPipeline (
		OptimizationLevel::O1, ThinOrFullLTOPhase::None);

	/*
	 * At O1 this is the one pass that only the module pipeline would have run,
	 * and it is load-bearing: it marks the entry thunk's call to the method
	 * body as a tail call, which is what lets the thunk leave no frame behind.
	 * Without it every method entered through its thunk - anything the runtime
	 * calls, so every reflection invoke - shows up twice in a stack trace.
	 */
	fpm.addPass (TailCallElimPass ());

	/*
	 * Last, because what it repairs is the pipeline's own doing: SimplifyCFG
	 * merges a function's returning blocks into one, which turns the ret a tail
	 * call needs to sit in front of into a branch and leaves the marker meaning
	 * nothing. It steps around musttail, whose adjacency is a verifier rule, and
	 * around nothing else.
	 */
	fpm.addPass (RestoreTailPositionPass ());
	mpm.addPass (createModuleToFunctionPassAdaptor (std::move (fpm)));
	mpm.addPass (arch::LegacyAbiPass ());
	mpm.run (m, mam);
}

Expected<std::unique_ptr<MonoJit>>
MonoJit::create ()
{
	ensure_native_target ();

	if (Error err = apply_options ())
		return std::move (err);

	LLJITBuilder builder;
	builder.setJITTargetMachineBuilder (host_target_machine_builder ());

	/*
	 * JITLink, not the RTDyldObjectLinkingLayer LLJIT still defaults to on
	 * ELF, with memory placed low so mini's rel32 call patching can always
	 * reach what gets published here (nearmem.hpp). LLJIT's generic platform
	 * setup attaches its eh-frame registration plugin to this layer on its
	 * own.
	 */
	builder.setObjectLinkingLayerCreator (
		[] (ExecutionSession &es) -> Expected<std::unique_ptr<ObjectLayer>> {
			auto memmgr = shared_memory_manager ();

			if (!memmgr)
				return memmgr.takeError ();
			return std::make_unique<ObjectLinkingLayer> (es, **memmgr);
		});

	/*
	 * The compiler that carries mono's clause gather and side-table emission
	 * along with stock codegen; SimpleCompiler emits neither.
	 */
	builder.setCompileFunctionCreator (
		[] (JITTargetMachineBuilder jtmb)
			-> Expected<std::unique_ptr<IRCompileLayer::IRCompiler>> {
			return std::make_unique<MethodObjectCompiler> (std::move (jtmb));
		});

	auto jit = builder.create ();
	if (!jit)
		return jit.takeError ();

	std::unique_ptr<MonoJit> self (new MonoJit (std::move (*jit)));

	self->capture_ = std::make_shared<ObjectCapturePlugin> ();
	static_cast<ObjectLinkingLayer &> (self->jit_->getObjLinkingLayer ())
		.addPlugin (self->capture_);

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

	auto redirectable = make_stub_manager (es);
	if (!redirectable)
		return redirectable.takeError ();
	self->redirectable_ = std::move (*redirectable);

	auto callbacks =
		LocalJITCompileCallbackManager<arch::LazyEntryABI>::Create (
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

const Triple &
MonoJit::triple () const
{
	return jit_->getExecutionSession ().getTargetTriple ();
}

Error
MonoJit::register_symbol (StringRef name, void *addr)
{
	std::lock_guard<std::mutex> lock (named_symbols_mutex_);

	auto it = named_symbols_.find (name.str ());
	if (it != named_symbols_.end ()) {
		/*
		 * A name is a promise about what it stands for. Two addresses under one
		 * name means a caller built a name that is not unique, and the first
		 * definition is the one every later module would silently link against -
		 * so say so here rather than emit code that reads the wrong object.
		 */
		if (it->second != addr)
			return createStringError (
				inconvertibleErrorCode (),
				"symbol %s already stands for a different address",
				name.str ().c_str ());
		return Error::success ();
	}

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

Error
MonoJit::create_stub (StringRef name, void *target)
{
	ExecutionSession &es = jit_->getExecutionSession ();

	SymbolMap dests;
	dests[es.intern (name)] = {
		ExecutorAddr::fromPtr (target),
		JITSymbolFlags::Exported | JITSymbolFlags::Callable,
	};

	return redirectable_->createRedirectableSymbols (
		stubs_->getDefaultResourceTracker (), std::move (dests));
}

Error
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
	/*
	 * A stub only has a slot to write once its object has been emitted, and
	 * stubs are defined without being materialized: the sibling of the stub
	 * that fired may never have been reached by any link. Look it up first -
	 * a no-op once emitted - or the redirect has nothing to write to.
	 */
	Expected<void *> addr = stub_address (name);
	if (!addr)
		return addr.takeError ();

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

Expected<CompiledMethod>
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
	JITDylib &jd = jit_->getExecutionSession ().createBareJITDylib (jd_name);
	jd.addToLinkOrder (*helpers_);
	jd.addToLinkOrder (*stubs_);

	if (Error err = jit_->addIRModule (jd, std::move (tsm)))
		return std::move (err);

	Expected<ExecutorAddr> sym = jit_->lookup (jd, entry);
	if (!sym)
		return sym.takeError ();

	std::optional<ObjectCapturePlugin::Extents> extents = capture_->take (jd_name);
	if (!extents)
		return createStringError (inconvertibleErrorCode (),
		                          "no object was captured while compiling %s",
		                          entry.str ().c_str ());

	CompiledMethod compiled;
	compiled.entry = sym->toPtr<void *> ();
	compiled.dylib = &jd;
	compiled.clause_table = extents->clause_table;
	compiled.clause_table_size = extents->clause_table_size;
	compiled.guard_table = extents->guard_table;
	compiled.guard_table_size = extents->guard_table_size;
	compiled.unwind_table = extents->unwind_table;
	compiled.unwind_table_size = extents->unwind_table_size;

	for (auto &[name, extent] : extents->functions) {
		if (name == entry) {
			compiled.code = extent.first;
			compiled.code_size = extent.second;
		}
	}
	compiled.functions = std::move (extents->functions);

	if (auto lines = extents->il_lines.find (entry.str ());
	    lines != extents->il_lines.end ())
		compiled.il_lines = std::move (lines->second);

	if (compiled.code == nullptr)
		return createStringError (inconvertibleErrorCode (),
		                          "the linked object for %s does not define it",
		                          entry.str ().c_str ());

	return compiled;
}

Error
MonoJit::remove_dylibs (const std::vector<JITDylib *> &dylibs)
{
	if (dylibs.empty ())
		return Error::success ();

	std::vector<JITDylibSP> owned (dylibs.begin (), dylibs.end ());

	return jit_->getExecutionSession ().removeJITDylibs (std::move (owned));
}

Error
MonoJit::undefine_stubs (const std::vector<std::string> &names)
{
	if (names.empty ())
		return Error::success ();

	ExecutionSession &es = jit_->getExecutionSession ();
	SymbolNameSet symbols;

	for (const std::string &name : names)
		symbols.insert (es.intern (name));

	/*
	 * Undefine before reclaiming: a stub has to be unreachable by name before
	 * its block can be handed to the next method along.
	 */
	if (Error err = stubs_->remove (symbols))
		return err;

	redirectable_->discard (*stubs_, symbols);
	return Error::success ();
}

} // namespace mono
