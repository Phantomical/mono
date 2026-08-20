/**
 * \file
 * \brief Getting the IRPGO counters in a tier-1 body back out again.
 *
 * LLVM's own instrumentation writes the counters and its own profile reader
 * consumes them, but the two normally meet through a file a process writes at
 * exit. Nothing here exits, so these passes record what the reader will ask for
 * and make the counter array reachable while the code is still running.
 */

#ifndef MONO_LLVM_PASSES_PROFILE_COUNTERS_HPP
#define MONO_LLVM_PASSES_PROFILE_COUNTERS_HPP

#include <llvm/IR/PassManager.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mono {

/// One instrumented function's counter array.
struct ProfileSite {
	/// The name the profile reader keys on.
	std::string name;
	/// The hash of the CFG the counter indices were assigned over. The reader
	/// drops a record whose hash disagrees with the function it is applied to.
	uint64_t hash = 0;
	uint32_t counters = 0;
};

/// What the passes below recorded for the module this thread last ran the
/// pipeline over. The caller empties it before each run.
std::vector<ProfileSite> &profile_sites ();

/// Marks every function that will not promote as one to leave uninstrumented.
///
/// Must also run in front of the reader, which skips the same functions and
/// would otherwise warn about each one it has no record for.
class ProfileSelectPass : public llvm::PassInfoMixin<ProfileSelectPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

/// Records the name and hash each instrumented function was given.
///
/// Run between the instrumentation and its lowering. Both values are arguments
/// of the increment intrinsics, and the lowering replaces those with arithmetic
/// on the counter array.
class ProfileGatherPass : public llvm::PassInfoMixin<ProfileGatherPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

/// Makes every global the lowering wrote local to the module.
///
/// Run behind the lowering. It marks some of them external and comdat, for a
/// static linker to merge across objects and a profile runtime to find at
/// process exit. Here the counters are read straight out of the running code,
/// so nothing looks for them by name, and left visible they are symbols the JIT
/// promises from the IR and then links with flags that do not match.
class ProfileLocalizePass : public llvm::PassInfoMixin<ProfileLocalizePass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

} // namespace mono

#endif
