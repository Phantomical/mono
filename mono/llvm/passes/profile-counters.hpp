/**
 * \file
 * \brief Getting the IRPGO counters in a tier-1 body back out again.
 *
 * LLVM's own instrumentation writes the counters and its profile reader
 * consumes them. The two normally meet through a file a process writes at
 * exit. A tier-2 compile wants the counts long before the process exits.
 * These passes record what the reader will ask for, and make the counter
 * array reachable while the code is still running.
 */

#ifndef MONO_LLVM_PASSES_PROFILE_COUNTERS_HPP
#define MONO_LLVM_PASSES_PROFILE_COUNTERS_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Value.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mono {

struct ProfileSite {
	/// The function this counts, by the name it carries in the IR.
	std::string function;
	/// The name the profile reader keys on. It is not the one above for a
	/// function with local linkage, which the reader qualifies.
	std::string name;
	/// The hash of the CFG the counter indices were assigned over. The reader
	/// drops a record whose hash disagrees with the function it is applied to.
	uint64_t hash = 0;
	uint32_t counters = 0;
};

/// What the passes below recorded for the module this thread last ran the
/// pipeline over. The caller empties it before each run.
std::vector<ProfileSite> &profile_sites ();

/// Where one instrumented function's counters landed, as the linked object says.
struct ProfileArray {
	/// What `__llvm_prf_data` keys the function's name under, which is what
	/// profile_name_key () returns for a ProfileSite's name.
	uint64_t name_key = 0;
	uint64_t hash = 0;
	const uint64_t *counters = nullptr;
	uint32_t count = 0;
};

/// Reads a linked object's `__llvm_prf_data` records.
///
/// Each record says where its own function's counters landed, so a module
/// holding several instrumented functions gives one entry each. Returns
/// nothing when the section is not a whole number of records. That is what a
/// disagreement with LLVM about the record layout looks like from here.
std::vector<ProfileArray> read_profile_arrays (const uint8_t *data, size_t size);

uint64_t profile_name_key (llvm::StringRef name);

/// Whether \p address reaches one of the counter arrays the instrumentation
/// wrote.
bool is_counter_address (const llvm::Value *address);

/// Marks every function that will not promote as one to leave uninstrumented.
///
/// The mark is `NoProfile`, which `PGOInstrumentationGen` obeys and
/// `PGOInstrumentationUse` does not - `skipPGOUse ()` tests only for a
/// declaration and for too many critical edges. So a function this pass marked
/// still reaches the reader, which finds no record for it, counts it in
/// `NumOfPGOMissing` and leaves it without weights. That is silent because
/// `-pgo-warn-missing-function` is off by default, and it is the right answer
/// for such a function either way.
class ProfileSelectPass : public llvm::PassInfoMixin<ProfileSelectPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

/// Records the name, hash, and counter count each instrumented function was
/// given.
///
/// Run between the instrumentation and its lowering. All three come from the
/// increment intrinsics, and the lowering replaces those calls with
/// arithmetic on the counter array.
class ProfileGatherPass : public llvm::PassInfoMixin<ProfileGatherPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

/// Makes each counter update the lowering left non-atomic one atomicrmw again.
///
/// Run behind the lowering. Threads share a body's counters, so a counter
/// written as a load and a store loses a count when two threads reach the same
/// block together. Only InstrProfOptions::Atomic decides which one the
/// lowering picks, for a promoted write-back and an untouched counter alike.
/// This tree turns that flag on, so the lowering never leaves this pass
/// anything to convert.
class ProfileAtomicPass : public llvm::PassInfoMixin<ProfileAtomicPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

/// Makes every global the profiling machinery wrote local to the module.
///
/// Run behind the lowering. The profiling machinery marks some of these
/// globals external and comdat, for a static linker to merge and a profile
/// runtime to find at process exit. Here the counters are read straight out
/// of the running code, so the JIT never looks them up by name. Left
/// visible, they are symbols the JIT promises from the IR and then links
/// with flags that do not match.
class ProfileLocalizePass : public llvm::PassInfoMixin<ProfileLocalizePass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

} // namespace mono

#endif
