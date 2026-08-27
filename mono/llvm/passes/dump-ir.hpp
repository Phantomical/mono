/**
 * \file
 * \brief Printing a module from inside a pipeline, where the pipeline puts
 * this pass.
 */

#ifndef MONO_LLVM_PASSES_DUMP_IR_HPP
#define MONO_LLVM_PASSES_DUMP_IR_HPP

#include <llvm/IR/PassManager.h>

#include "mono/mini/jit-dump.hpp"

namespace mono {

/// Prints each method the module publishes, under the dump point this pass
/// carries.
///
/// The points around a pipeline print from outside the run, so a stage in the
/// middle of one has no other way to be read. Add this only where
/// `dump_point_enabled ()` says the point is on: it walks the module and builds
/// a name for each body, and a compile that prints nothing must not pay for
/// that.
class DumpIRPass : public llvm::PassInfoMixin<DumpIRPass> {
public:
	explicit DumpIRPass (DumpPoint point) : point_ (point) {}

	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);

	/// A pass instrumentation can skip a pass that is not required, and a dump
	/// the command line asked for must not be one it skips.
	static bool isRequired () { return true; }

private:
	DumpPoint point_;
};

} // namespace mono

#endif
