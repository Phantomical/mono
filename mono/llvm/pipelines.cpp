#include "pipelines.hpp"
#include "arch/arch.hpp"
#include "jit.hpp"
#include "passes/array-address.hpp"
#include "passes/array-shape.hpp"
#include "passes/clamp-frame-align.hpp"
#include "passes/class-init.hpp"
#include "passes/cast-func.hpp"
#include "passes/devirtualize.hpp"
#include "passes/fold-cast.hpp"
#include "passes/inline-copies.hpp"
#include "passes/lower-builtins.hpp"
#include "passes/profile-counter-promoter.hpp"
#include "passes/profile-counters.hpp"
#include "passes/restore-tail-position.hpp"
#include "passes/rgctx-dedup.hpp"
#include "passes/rgctx-fetch.hpp"
#include "passes/tier-counter.hpp"
#include "passes/top-down-inline.hpp"
#include "passes/vtable-func.hpp"
#include <llvm/IR/PassManager.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/ProfileSummary.h>
#include <llvm/Pass.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/Instrumentation/InstrProfiling.h>
#include <llvm/Transforms/Instrumentation/PGOInstrumentation.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Utils/CountVisits.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Utils/LibCallsShrinkWrap.h>
#include <llvm/Transforms/Scalar/Reassociate.h>
#include <llvm/Transforms/Scalar/LICM.h>
#include <llvm/Transforms/Scalar/LoopRotation.h>
#include <llvm/Transforms/Scalar/SimpleLoopUnswitch.h>
#include <llvm/Transforms/Scalar/LoopIdiomRecognize.h>
#include <llvm/Transforms/Scalar/IndVarSimplify.h>
#include <llvm/Transforms/Scalar/LoopDeletion.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>
#include <llvm/Transforms/Scalar/MemCpyOptimizer.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/BDCE.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/Sink.h>
#include <llvm/Transforms/IPO/Annotation2Metadata.h>
#include <llvm/Transforms/IPO/ForceFunctionAttrs.h>
#include <llvm/Transforms/IPO/InferFunctionAttrs.h>
#include <llvm/Transforms/Scalar/LowerExpectIntrinsic.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/Instrumentation.h>
#include <llvm/Transforms/Scalar/AnnotationRemarks.h>
#include <llvm/Analysis/InstCount.h>
#include <llvm/Analysis/FunctionPropertiesAnalysis.h>
#include <llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h>
#include <llvm/Transforms/Scalar/ConstraintElimination.h>
#include <llvm/Transforms/Scalar/CorrelatedValuePropagation.h>
#include <llvm/Transforms/Scalar/DFAJumpThreading.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/JumpThreading.h>
#include <llvm/Transforms/Scalar/LoopInstSimplify.h>
#include <llvm/Transforms/Scalar/LoopSimplifyCFG.h>
#include <llvm/Transforms/Scalar/MergedLoadStoreMotion.h>
#include <llvm/Transforms/Utils/MoveAutoInit.h>
#include <llvm/Transforms/Scalar/TailRecursionElimination.h>
#include <llvm/Transforms/Vectorize/VectorCombine.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/ProfileData/InstrProfWriter.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <string>

namespace mono {

namespace {

/// Where buildPgoUsePipeline () mounts the counts for the reader to open.
constexpr const char *profile_file = "/mono.profdata";

/// The summary a body whose entry reads \p entry is weighed against.
///
/// ProfileSummaryInfo takes its two thresholds off percentiles of the counts a
/// summary describes: the hot one at the cutoff LLVM calls
/// profile-summary-cutoff-hot and the cold one at profile-summary-cutoff-cold.
/// Naming a count at each cutoff is what sets them, and the counts here are the
/// entry and nothing else, so a block is read against the body it is in:
/// anything a tenth as often as the entry is cold, and anything ten times as
/// often is hot. An entry is neither, so a body is not hot for being in this
/// pipeline.
///
/// It has to be built from \p entry alone. A summary is a module flag, and the
/// tier-2 inliner links a candidate's module into the root's, where two flags
/// that disagree are a hard error. Both modules share one LLVMContext, so a
/// summary that names the same numbers uniques to the same node and the two
/// agree.
std::unique_ptr<llvm::ProfileSummary>
summary_for (uint64_t entry)
{
	uint64_t hot = entry * 10;
	uint64_t cold = std::max<uint64_t> (entry / 10, 1);

	// Ordered by cutoff, because getEntryForPercentile () answers with the
	// first entry that reaches the percentile it is given.
	llvm::SummaryEntryVector detailed {
		{ 990000, hot, 1 },
		{ 999999, cold, 2 },
		{ 1000000, cold, 2 },
	};

	return std::make_unique<llvm::ProfileSummary> (
		llvm::ProfileSummary::PSK_Instr, detailed, /*TotalCount=*/entry * 100,
		/*MaxCount=*/hot, /*MaxInternalCount=*/hot, /*MaxFunctionCount=*/entry,
		/*NumCounts=*/2, /*NumFunctions=*/1);
}

/// Gives every body that has counts the same entry count, and hands the
/// thresholds a summary on the same scale.
///
/// BlockFrequencyInfo answers a block's count as the entry count times the
/// block's frequency, so one entry count for every body scales the counts and
/// leaves the frequencies where they were. What that takes out is the
/// difference between a body tier 2 took for its calls and one it took for a
/// loop: the second has an entry that is small beside its own loop, and a call
/// site there reads cold and gets a budget almost nothing clears.
///
/// Both sides of that question have to move together. Every hot and cold answer
/// weighs a block's count against a threshold the summary carries, and the
/// summary PGOInstrumentationUse wrote describes the counts as they were
/// counted. Leaving it puts an entry of ten thousand beside blocks in the
/// millions, and what follows is a program laid out cold: 13 of 24 bodies moved
/// from .text.hot to .text.unlikely, and every shape of one benchmark lost
/// 5-10%.
///
/// A body with no counts keeps none. It is laid out on static frequencies, and
/// an entry count would say the profile described it.
class NormalizeProfilePass : public llvm::PassInfoMixin<NormalizeProfilePass> {
	uint64_t entry_;

public:
	explicit NormalizeProfilePass (uint64_t entry) : entry_ (entry) {}

	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &)
	{
		for (llvm::Function &f : m)
			if (!f.isDeclaration () && f.getEntryCount ().has_value ())
				f.setEntryCount (entry_);

		/*
		 * Whatever this module holds, and even where it holds no counts at all.
		 * The summary is a module flag and the tier-2 inliner links a
		 * candidate's module into the root's, so a module that kept the one
		 * PGOInstrumentationUse wrote disagrees with a module that took this
		 * one, and the link is a hard error rather than a merge.
		 */
		m.setProfileSummary (summary_for (entry_)->getMD (m.getContext ()),
		                     llvm::ProfileSummary::PSK_Instr);

		// The counts every later question reads are what this moves.
		return llvm::PreservedAnalyses::none ();
	}
};

/// A profile holding no records, in the format the reader opens.
///
/// Built once. It is a header and an empty index, and every lookup against it
/// answers "no profile data available for function" - which is what a method
/// that gathered no counts wants, and which nothing warns about while
/// `-pgo-warn-missing-function` is off.
llvm::StringRef
profile_with_no_records ()
{
	static const std::string bytes = [] {
		llvm::InstrProfWriter writer;

		// The reader checks this against what the instrumentation wrote.
		llvm::consumeError (writer.mergeProfileKind (llvm::InstrProfKind::IRInstrumentation));

		return writer.writeBuffer ()->getBuffer ().str ();
	}();

	return bytes;
}

static void
addAnnotationRemarksPass (llvm::ModulePassManager &MPM)
{
	llvm::FunctionPassManager FPM;
	FPM.addPass (llvm::AnnotationRemarksPass ());

	if (llvm::AreStatisticsEnabled ()) {
		FPM.addPass (llvm::InstCountPass ());
		FPM.addPass (llvm::FunctionPropertiesStatisticsPass ());
	}

	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (std::move (FPM)));
}
} // namespace

MonoPipelineTuningOptions::MonoPipelineTuningOptions () = default;

MonoPipelineTuningOptions
MonoPipelineTuningOptions::forTier1 ()
{
	MonoPipelineTuningOptions options;

	// What LLVM sets these to below O2. Tier 1 builds at O1 and is where nearly
	// all code stays, so what a vectorizer costs it is compile latency.
	options.LoopVectorization = false;
	options.SLPVectorization = false;

	return options;
}

MonoPipelineTuningOptions
MonoPipelineTuningOptions::forTier2 ()
{
	MonoPipelineTuningOptions options;

	// What LLVM sets these to at O3. It raises them from the level in a
	// function local to PassBuilder.cpp, so tier 2 sets them itself. The
	// optimization stage in buildTier2Pipeline () is what reads them.
	options.LoopVectorization = true;
	options.SLPVectorization = true;

	// leaving this as true causes link errors
	options.CallGraphProfile = false;

	return options;
}

llvm::IntrusiveRefCntPtr<OneFileFS>
makeProfileFileSystem ()
{
	return llvm::IntrusiveRefCntPtr<OneFileFS> (new OneFileFS (llvm::MemoryBuffer::getMemBuffer (
		profile_with_no_records (), profile_file, /*RequiresNullTerminator=*/false)));
}

OneFileFS::CurrentFileGuard
pushProfile (OneFileFS &fs, llvm::ArrayRef<uint8_t> profile)
{
	llvm::StringRef bytes =
		profile.empty ()
			? profile_with_no_records ()
			: llvm::StringRef ((const char *) profile.data (), profile.size ());

	// Not a copy. The guard is what says the bytes are still there.
	return fs.set (llvm::MemoryBuffer::getMemBuffer (bytes, profile_file,
	                                                 /*RequiresNullTerminator=*/false));
}

MonoPassBuilder::MonoPassBuilder (llvm::TargetMachine *TM, OneFileFS *ProfileFS,
                                  llvm::PassInstrumentationCallbacks *PIC,
                                  MonoPipelineTuningOptions PTO)
	: llvm::PassBuilder (TM, PTO, std::nullopt, PIC), TM (TM), PTO (PTO),
	  ProfileFS (ProfileFS)
{
	/*
	 * Here rather than at a place in either pipeline, because what settles a
	 * dispatch site is the simplification around it: SROA and EarlyCSE forward
	 * the store at an allocation to the read of the vtable, and GVN forwards it
	 * across a block boundary that separates the two. The peephole point sits behind each
	 * of those rounds and in front of the SCCP and InstCombine runs that read
	 * the direct call this leaves.
	 *
	 * Registered before either pipeline is built, since that is when the
	 * callbacks are asked for.
	 */
	registerPeepholeEPCallback (
		[] (llvm::FunctionPassManager &FPM, llvm::OptimizationLevel) {
			// In front of DevirtualizePass, because answering a type test is
			// what delivers the allocation a chain's receiver comes from.
			FPM.addPass (mono::FoldCastPass ());
			FPM.addPass (mono::DevirtualizePass ());
		});
}

llvm::FunctionPassManager
MonoPassBuilder::buildCommonFunctionSimplificationPipeline ()
{
	auto optLevel = llvm::OptimizationLevel::O1;

	// This mirrors LLVM's O1 function simplification pipeline. It runs before
	// PGO instrumentation is added or used, so it must stay identical between
	// tier1 and tier2.

	llvm::FunctionPassManager FPM;

	if (llvm::AreStatisticsEnabled ())
		FPM.addPass (llvm::CountVisitsPass ());

	FPM.addPass (llvm::LowerExpectIntrinsicPass ());

	// The rest is the O1 function optimization pipeline, see
	// PassBuilder::buildO1FunctionSimplificationPipeline for the justifications.

	FPM.addPass (llvm::SROAPass (llvm::SROAOptions::ModifyCFG));
	FPM.addPass (llvm::EarlyCSEPass (/* UseMemorySSA = */ true));
	FPM.addPass (
		llvm::SimplifyCFGPass (llvm::SimplifyCFGOptions ().convertSwitchRangeToICmp (true)));
	FPM.addPass (llvm::InstCombinePass ());
	FPM.addPass (llvm::LibCallsShrinkWrapPass ());

	invokePeepholeEPCallbacks (FPM, optLevel);

	FPM.addPass (
		llvm::SimplifyCFGPass (llvm::SimplifyCFGOptions ().convertSwitchRangeToICmp (true)));
	FPM.addPass (llvm::ReassociatePass ());

	llvm::LoopPassManager LPM1, LPM2;

	LPM1.addPass (llvm::LICMPass (PTO.LicmMssaOptCap, PTO.LicmMssaNoAccForPromotionCap,
	                              /* AllowSpeculation = */ false));
	LPM1.addPass (llvm::LoopRotatePass (true, false));
	LPM1.addPass (llvm::SimpleLoopUnswitchPass ());

	LPM2.addPass (llvm::LoopIdiomRecognizePass ());
	LPM2.addPass (llvm::IndVarSimplifyPass ());

	invokeLateLoopOptimizationsEPCallbacks (LPM2, optLevel);

	LPM2.addPass (llvm::LoopDeletionPass ());
	LPM2.addPass (llvm::LoopFullUnrollPass (optLevel.getSpeedupLevel (),
	                                        /* OnlyWhenForced= */ !PTO.LoopUnrolling,
	                                        PTO.ForgetAllSCEVInLoopUnroll));

	invokeLoopOptimizerEndEPCallbacks (LPM2, optLevel);

	FPM.addPass (llvm::createFunctionToLoopPassAdaptor (std::move (LPM1), /*UseMemorySSA=*/true));
	FPM.addPass (llvm::InstCombinePass ());
	FPM.addPass (llvm::createFunctionToLoopPassAdaptor (std::move (LPM2), /*UseMemorySSA=*/false));

	FPM.addPass (llvm::SROAPass (llvm::SROAOptions::ModifyCFG));
	FPM.addPass (llvm::MemCpyOptPass ());
	FPM.addPass (llvm::SCCPPass ());
	FPM.addPass (llvm::BDCEPass ());
	FPM.addPass (llvm::InstCombinePass ());
	invokePeepholeEPCallbacks (FPM, optLevel);

	// Left out: this backend does not emit coroutine intrinsics.
	// FPM.addPass(llvm::CoroElidePass());

	invokeScalarOptimizerLateEPCallbacks (FPM, optLevel);

	FPM.addPass (llvm::ADCEPass ());
	FPM.addPass (
		llvm::SimplifyCFGPass (llvm::SimplifyCFGOptions ().convertSwitchRangeToICmp (true)));
	FPM.addPass (llvm::InstCombinePass ());

	return FPM;
}

llvm::ModulePassManager
MonoPassBuilder::buildCommonModuleSimplificationPipeline ()
{
	llvm::ModulePassManager MPM;

	MPM.addPass (llvm::Annotation2MetadataPass ());
	MPM.addPass (llvm::ForceFunctionAttrsPass ());
	MPM.addPass (llvm::InferFunctionAttrsPass ());

	MPM.addPass (mono::ArrayAddressPass ());
	MPM.addPass (mono::LowerBuiltinsPass ());

	// Make sure that the always-inline functions we add get inlined early on
	// before simplifications and PGO counters are added.
	MPM.addPass (llvm::AlwaysInlinerPass (/*InsertLifetime=*/true));
	MPM.addPass (mono::StripInlineCopiesPass ());

	// A dimension the method's own IL settled, in front of the simplification
	// that then optimizes the reads this writes.
	MPM.addPass (mono::ArrayShapePass (/*finalize=*/false));

	auto CommonFPM = buildCommonFunctionSimplificationPipeline ();
	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (std::move (CommonFPM),
	                                                      PTO.EagerlyInvalidateAnalyses));

	/*
	 * And a dimension that arrived with a fold. The translator gives every
	 * argument an alloca, so an inlined parameter is a load until SROA has run
	 * above, whatever the caller passed. Both tiers lower at both points, so a
	 * body carries the same CFG into the PGO hash whichever tier compiled it.
	 */
	MPM.addPass (mono::ArrayShapePass (/*finalize=*/true));

	return MPM;
}

llvm::ModulePassManager
MonoPassBuilder::buildPgoInstrumentationPipeline ()
{
	llvm::ModulePassManager MPM;

	// A body that cannot promote gets no counters. The thunks, the thrower and
	// the dispatcher are all in the module and none of them ever reaches
	// tier 2.
	MPM.addPass (mono::ProfileSelectPass ());
	MPM.addPass (llvm::PGOInstrumentationGen ());

	// Promotion wants each loop to have a preheader and exits that only it
	// branches to, and the translator gives it neither.
	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (llvm::LoopSimplifyPass ()));
	MPM.addPass (mono::ProfileCounterPromoterPass ());

	MPM.addPass (mono::ProfileGatherPass ());

	/*
	 * Every counter update is an atomicrmw, the write-backs the promoter left
	 * behind included. Threads share a body's counters, so an update written
	 * as a load and a store loses a count when two of them reach one block
	 * together.
	 */
	MPM.addPass (llvm::InstrProfilingLoweringPass (llvm::InstrProfOptions{ .Atomic = true }));
	MPM.addPass (mono::ProfileLocalizePass ());

	return MPM;
}

llvm::ModulePassManager
MonoPassBuilder::buildPgoUsePipeline ()
{
	llvm::ModulePassManager MPM;

	/*
	 * The reader takes a path rather than a buffer, so the counts reach it as
	 * a file. Which counts those are is settled per run rather than here: the
	 * file system serves whatever the running compile pushed, so one pipeline
	 * built once reads a different method's counts each time.
	 *
	 * This file system serves that buffer for every path it is asked for, so
	 * the name below only has to be one the reader will open.
	 */
	MPM.addPass (llvm::PGOInstrumentationUse (profile_file, "", /*IsCS=*/false, ProfileFS));

	// Behind the reader, which is what writes the entry counts this replaces,
	// and in front of the summary the thresholds are read against.
	if (uint64_t entry = profile_entry_count ())
		MPM.addPass (NormalizeProfilePass (entry));

	/*
	 * The summary the weights are read against. LLVM caches it here for the
	 * same reason, in addPGOInstrPasses: nothing downstream asks for it, and
	 * without it every hot and cold question answers the same way. The tier-2
	 * inliner reads it straight off the module analysis manager.
	 */
	MPM.addPass (llvm::RequireAnalysisPass<llvm::ProfileSummaryAnalysis, llvm::Module> ());

	return MPM;
}

llvm::ModulePassManager
MonoPassBuilder::buildTier1Pipeline ()
{
	auto MPM = buildCommonModuleSimplificationPipeline ();

	if (PTO.EnablePGO)
		MPM.addPass (buildPgoInstrumentationPipeline ());

	/*
	 * Behind the instrumentation, so that both tiers hash a CFG with the type
	 * tests still one call each. Tier 2 lowers behind its inliner instead, and
	 * neither tier has lowered by the time it takes the hash.
	 *
	 * In front of TierCounterPass, which turns the calls that can unwind into
	 * invokes on to the counter's own pad. The wrapper this writes is one of
	 * them.
	 */
	MPM.addPass (mono::LowerCastFuncPass ());

	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (mono::ClassInitPass ()));
	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (mono::RgctxDedupPass ()));
	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (mono::RestoreTailPositionPass ()));

	/*
	 * Behind the PGO instrumentation, so that the counts carry over to tier 2,
	 * which has no tiering counter of its own. The counters' blocks are then
	 * outside the CFG the instrumentation hashed.
	 *
	 * Behind RestoreTailPositionPass as well, because a body with a loop writes
	 * its count back at each exit. A write-back in front of the ret that pass
	 * looks for hides the shape it repairs, and the tail call is lost.
	 *
	 * Behind RgctxDedupPass for the weights. That pass takes fetches out, and a
	 * weight read in front of it counts instructions the body never runs.
	 */
	if (PTO.EnablePromotion)
		MPM.addPass (mono::TierCounterPass ());

	MPM.addPass (mono::RgctxFetchPass ());

	// In front of the ABI lowering, which rewrites the calls this leaves, and of
	// codegen, which has no lowering for the declaration at all.
	MPM.addPass (mono::LowerVTableFuncPass ());
	MPM.addPass (arch::MonoAbiPass ());

	// Behind the ABI lowering, which makes an alloca of its own for a value the
	// convention passes in memory.
	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (mono::ClampFrameAlignPass ()));

	addAnnotationRemarksPass (MPM);

	return MPM;
}

llvm::ModulePassManager
MonoPassBuilder::buildTier2SimplificationPipeline ()
{
	auto MPM = buildCommonModuleSimplificationPipeline ();

	if (PTO.EnablePGO)
		MPM.addPass (buildPgoUsePipeline ());

	// Behind the counts, so that the tier the profile was gathered at and the
	// tier reading it back hash the same CFG. A check this drops sits on an
	// invoke where its class has a handler around it, and an edge is what the
	// hash is over.
	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (mono::ClassInitPass ()));

	// Behind the counts for the same reason, and in front of the pipeline
	// below, which then optimizes one fetch rather than several.
	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (mono::RgctxDedupPass ()));

	/*
	 * O3 before anything weighs a call site, and that is what a cost model
	 * needs: freshly translated managed IR is a null check on every
	 * dereference and a bounds check on every element, and a threshold read
	 * against it is spent before any of the real work is costed.
	 */
	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (
		buildTier2FunctionSimplificationPipeline (), PTO.EagerlyInvalidateAnalyses));

	return MPM;
}

llvm::ModulePassManager
MonoPassBuilder::buildTier2MaterializePipeline ()
{
	return buildTier2SimplificationPipeline ();
}

llvm::ModulePassManager
MonoPassBuilder::buildTier2Pipeline ()
{
	auto MPM = buildTier2SimplificationPipeline ();

	MPM.addPass (mono::TopDownInlinerPass (*TM, buildTier2MaterializePipeline (),
	                                       buildTier2FunctionSimplificationPipeline ()));

	MPM.addPass (mono::StripInlineCopiesPass ());

	/*
	 * Behind the inliner, so the cost model weighs a callee with its type tests
	 * still one call each, and so a test the inliner made answerable has been
	 * answered. In front of the optimization pipeline, which then reads the
	 * probe this writes as ordinary IR.
	 */
	MPM.addPass (mono::LowerCastFuncPass ());

	/*
	 * InstCombine sinks a load only into a block whose unique predecessor is
	 * the load's own block. It does no alias analysis for the move. So a load
	 * of one array element's field stays above the next array's bounds check,
	 * and SLP covers the split with an insertelement gather. SinkingPass asks
	 * alias analysis instead. LLVM's O3 pipeline does not run it, so this
	 * extension point puts it in front of the vectorizers.
	 */
	registerVectorizerStartEPCallback (
		[] (llvm::FunctionPassManager &FPM, llvm::OptimizationLevel) {
			FPM.addPass (llvm::SinkingPass ());
		});

	MPM.addPass (buildModuleOptimizationPipeline (llvm::OptimizationLevel::O3,
	                                              llvm::ThinOrFullLTOPhase::None));

	llvm::FunctionPassManager FPM;

	// Again, because unrolling and jump threading copied whatever the run in
	// the common pipeline left standing.
	FPM.addPass (mono::ClassInitPass ());
	FPM.addPass (mono::RgctxDedupPass ());

	// Last, because what it repairs is the pipeline's own doing.
	FPM.addPass (mono::RestoreTailPositionPass ());
	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (std::move (FPM)));

	MPM.addPass (mono::RgctxFetchPass ());

	// In front of the ABI lowering, for the reason tier 1 gives.
	MPM.addPass (mono::LowerVTableFuncPass ());
	MPM.addPass (arch::MonoAbiPass ());

	// Behind the ABI lowering, for the reason tier 1 gives.
	MPM.addPass (llvm::createModuleToFunctionPassAdaptor (mono::ClampFrameAlignPass ()));

	addAnnotationRemarksPass (MPM);

	return MPM;
}

llvm::FunctionPassManager
MonoPassBuilder::buildTier2FunctionSimplificationPipeline ()
{
	auto optLevel = llvm::OptimizationLevel::O3;

	// This mirrors LLVM's O3 function simplification pipeline.

	llvm::FunctionPassManager FPM;

	if (llvm::AreStatisticsEnabled ())
		FPM.addPass (llvm::CountVisitsPass ());

	FPM.addPass (llvm::SROAPass (llvm::SROAOptions::ModifyCFG));
	FPM.addPass (llvm::EarlyCSEPass (/* UseMemorySSA = */ true));

	// SpeculativeExecutionPass is built asking for a divergent target, which
	// amd64 is not, so it is left out rather than walked over for nothing.

	FPM.addPass (llvm::JumpThreadingPass ());
	FPM.addPass (llvm::CorrelatedValuePropagationPass ());
	FPM.addPass (
		llvm::SimplifyCFGPass (llvm::SimplifyCFGOptions ().convertSwitchRangeToICmp (true)));
	FPM.addPass (llvm::InstCombinePass ());
	FPM.addPass (llvm::AggressiveInstCombinePass ());
	FPM.addPass (llvm::LibCallsShrinkWrapPass ());

	invokePeepholeEPCallbacks (FPM, optLevel);

	// LLVM puts PGOMemOPSizeOpt here, under a profile it reads memop sizes
	// from. Value profiling is off, so there are none to read.

	/*
	 * LLVM reads this flag off PGOOpt, which a builder of our own never sets.
	 * Left unset, TailCallElimPass would leave a converted recursion's entry
	 * count stale. This pipeline always runs after PGOInstrumentationUse, so
	 * it passes true directly instead.
	 */
	FPM.addPass (llvm::TailCallElimPass (/* UpdateFunctionEntryCount = */ true));
	FPM.addPass (
		llvm::SimplifyCFGPass (llvm::SimplifyCFGOptions ().convertSwitchRangeToICmp (true)));
	FPM.addPass (llvm::ReassociatePass ());
	FPM.addPass (llvm::ConstraintEliminationPass ());

	llvm::LoopPassManager LPM1, LPM2;

	LPM1.addPass (llvm::LoopInstSimplifyPass ());
	LPM1.addPass (llvm::LoopSimplifyCFGPass ());
	LPM1.addPass (llvm::LICMPass (PTO.LicmMssaOptCap, PTO.LicmMssaNoAccForPromotionCap,
	                              /* AllowSpeculation = */ false));
	LPM1.addPass (llvm::LoopRotatePass (true, false));

	// The second one, and the one that does the hoisting. The first runs with
	// speculation off because rotation has not happened yet.
	LPM1.addPass (llvm::LICMPass (PTO.LicmMssaOptCap, PTO.LicmMssaNoAccForPromotionCap,
	                              /* AllowSpeculation = */ true));
	LPM1.addPass (llvm::SimpleLoopUnswitchPass (/* NonTrivial = */ true));

	LPM2.addPass (llvm::LoopIdiomRecognizePass ());
	LPM2.addPass (llvm::IndVarSimplifyPass ());

	invokeLateLoopOptimizationsEPCallbacks (LPM2, optLevel);

	LPM2.addPass (llvm::LoopDeletionPass ());
	LPM2.addPass (llvm::LoopFullUnrollPass (optLevel.getSpeedupLevel (),
	                                        /* OnlyWhenForced= */ !PTO.LoopUnrolling,
	                                        PTO.ForgetAllSCEVInLoopUnroll));

	invokeLoopOptimizerEndEPCallbacks (LPM2, optLevel);

	FPM.addPass (llvm::createFunctionToLoopPassAdaptor (std::move (LPM1), /*UseMemorySSA=*/true));
	FPM.addPass (
		llvm::SimplifyCFGPass (llvm::SimplifyCFGOptions ().convertSwitchRangeToICmp (true)));
	FPM.addPass (llvm::InstCombinePass ());
	FPM.addPass (llvm::createFunctionToLoopPassAdaptor (std::move (LPM2), /*UseMemorySSA=*/false));

	FPM.addPass (llvm::SROAPass (llvm::SROAOptions::ModifyCFG));
	FPM.addPass (llvm::VectorCombinePass (/* TryEarlyFoldsOnly = */ true));
	FPM.addPass (llvm::MergedLoadStoreMotionPass ());
	FPM.addPass (llvm::GVNPass ());
	FPM.addPass (llvm::SCCPPass ());
	FPM.addPass (llvm::BDCEPass ());
	FPM.addPass (llvm::InstCombinePass ());

	invokePeepholeEPCallbacks (FPM, optLevel);

	FPM.addPass (llvm::JumpThreadingPass ());
	FPM.addPass (llvm::CorrelatedValuePropagationPass ());
	FPM.addPass (llvm::ADCEPass ());
	FPM.addPass (llvm::MemCpyOptPass ());
	FPM.addPass (llvm::DSEPass ());
	FPM.addPass (llvm::MoveAutoInitPass ());
	FPM.addPass (llvm::createFunctionToLoopPassAdaptor (
		llvm::LICMPass (PTO.LicmMssaOptCap, PTO.LicmMssaNoAccForPromotionCap,
	                    /* AllowSpeculation = */ true),
		/*UseMemorySSA=*/true));

	// Left out: this backend does not emit coroutine intrinsics.
	// FPM.addPass(llvm::CoroElidePass());

	invokeScalarOptimizerLateEPCallbacks (FPM, optLevel);

	// Four options rather than the one the O1 pipeline's closing SimplifyCFG
	// takes.
	FPM.addPass (llvm::SimplifyCFGPass (llvm::SimplifyCFGOptions ()
	                                        .convertSwitchRangeToICmp (true)
	                                        .convertSwitchToArithmetic (true)
	                                        .hoistCommonInsts (true)
	                                        .sinkCommonInsts (true)));
	FPM.addPass (llvm::InstCombinePass ());

	invokePeepholeEPCallbacks (FPM, optLevel);

	return FPM;
}

} // namespace mono
