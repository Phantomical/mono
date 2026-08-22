#ifndef __MONO_LLVM_PIPELINES_HPP__
#define __MONO_LLVM_PIPELINES_HPP__

#include "util/one-file-vfs.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/PGOOptions.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Target/TargetMachine.h>

namespace mono {

class InlineCandidates;

/// Builds the file system a tier-2 pipeline reads its counts through.
///
/// The pipeline holds this rather than a profile, so one built once can read a
/// different method's counts on each run. A compile pushes its own with
/// pushProfile () and keeps the guard for as long as the pipeline can run.
///
/// A run that pushes nothing reads a profile holding no records, so the reader
/// always has a file to open. That matters more than it looks: the reader
/// raises a DS_Error for a profile it cannot open, and an error diagnostic no
/// handler takes calls exit (1).
llvm::IntrusiveRefCntPtr<OneFileFS> makeProfileFileSystem ();

/// Makes \p profile the counts the next run against \p fs reads, until the
/// guard goes.
///
/// The bytes are not copied, so they have to outlive the guard, and the guard
/// has to outlive the run.
///
/// Empty is a method that gathered no counts, which is ordinary. It reads the
/// same profile with no records that an unpushed run does, so the method comes
/// back missing and the body keeps the static frequencies.
OneFileFS::CurrentFileGuard pushProfile (OneFileFS &fs, llvm::ArrayRef<uint8_t> profile);

/// Tunable parameters for passes in the mono pipeline.
///
/// A tier gets its own, because the settings LLVM reads off these are the ones
/// it would otherwise pick from the optimization level, and the two tiers do
/// not build for the same one.
class MonoPipelineTuningOptions : public llvm::PipelineTuningOptions {
public:
	/// LLVM's own defaults, which are what it uses at O2 and above.
	///
	/// PassBuilder settles the level-dependent ones in
	/// setupOptionsForPipelineAlias (), which is private to it, so a builder of
	/// ours starts from these whatever level it goes on to build for. Prefer a
	/// tier's own settings below.
	MonoPipelineTuningOptions ();

	/// The settings each tier's builder is made with.
	static MonoPipelineTuningOptions forTier1 ();
	static MonoPipelineTuningOptions forTier2 ();

	/// Whether a tier-1 body gathers counts and a tier-2 body is laid out
	/// against them. Off leaves both tiers on the static frequencies.
	bool EnablePGO = true;

	/// Whether a tier-1 body carries the counter that asks for tier 2.
	///
	/// Separate from EnablePGO: a body can promote off a plain entry count with
	/// no profile behind it, and turning the profile off must not stop it.
	bool EnablePromotion = true;
};

/// This class builds the pipelines used for various tiers.
///
/// It is a builder and nothing else: a pipeline it returns owns what it needs,
/// so the builder can go as soon as the pipeline is built. The target machine
/// and the file system are the caller's, and both have to outlive every
/// pipeline built against them.
class MonoPassBuilder : public llvm::PassBuilder {
	llvm::TargetMachine *TM;
	MonoPipelineTuningOptions PTO;
	OneFileFS *ProfileFS;

public:
	MonoPassBuilder (llvm::TargetMachine *TM, OneFileFS *ProfileFS,
	                 llvm::PassInstrumentationCallbacks *PIC = nullptr,
	                 MonoPipelineTuningOptions PTO = MonoPipelineTuningOptions ());

	/// Build the pipeline for the requested tier.
	///
	/// The tier-2 pipeline reads which engine to ask about inlining from
	/// InlineCandidatesAnalysis, which the analysis manager it runs under has
	/// to have registered.
	llvm::ModulePassManager buildTier1Pipeline ();
	llvm::ModulePassManager buildTier2Pipeline ();

	/// Build what a candidate the tier-2 inliner materializes is put through.
	///
	/// The inliner translates a candidate into a module of its own and runs
	/// this over it, so the cost model weighs a body in the shape a tier-1
	/// body has. That shape is what the profile is keyed on: the counter
	/// indices were assigned over the CFG this pipeline leaves behind, so a
	/// candidate put through anything else takes its weights from nothing.
	llvm::ModulePassManager buildTier2MaterializePipeline ();

private:
	/// This is the common simplification pipeline that runs before we apply profiling
	/// statistics.
	///
	/// It needs to be exactly the same between tier1 and tier2 or else the profile
	/// counts will not be able to be applied.
	llvm::FunctionPassManager buildCommonFunctionSimplificationPipeline ();
	llvm::ModulePassManager buildCommonModuleSimplificationPipeline ();

	llvm::ModulePassManager buildPgoInstrumentationPipeline ();
	llvm::ModulePassManager buildPgoUsePipeline ();

	// A separate simplification pipeline that runs after the common one
	llvm::FunctionPassManager buildTier2FunctionSimplificationPipeline ();
	llvm::ModulePassManager buildTier2SimplificationPipeline ();
};

} // namespace mono

#endif
