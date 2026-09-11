/**
 * \file
 * \brief Giving FastISel an `unreachable` it selects.
 */

#include "trap-unreachable.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {
namespace {

// The TargetMachine's own settings, asked here so a trap goes in exactly where
// codegen would have emitted one. `host_target_machine_builder ()` turns
// `TrapUnreachable` on and leaves `NoTrapAfterNoreturn` off.
constexpr bool trap_unreachable = true;
constexpr bool no_trap_after_noreturn = false;

} // namespace

PreservedAnalyses
TrapUnreachablePass::run (Function &f, FunctionAnalysisManager &fam)
{
	SmallVector<UnreachableInst *, 8> ends;

	for (BasicBlock &block : f)
		if (auto *end = dyn_cast<UnreachableInst> (block.getTerminator ()))
			if (end->shouldLowerToTrap (trap_unreachable, no_trap_after_noreturn))
				ends.push_back (end);

	if (ends.empty ())
		return PreservedAnalyses::all ();

	Function *trap
		= Intrinsic::getOrInsertDeclaration (f.getParent (), Intrinsic::trap);

	for (UnreachableInst *end : ends) {
		IRBuilder<> b (end);

		b.SetCurrentDebugLocation (end->getDebugLoc ());
		b.CreateCall (trap);
	}

	PreservedAnalyses preserved;

	preserved.preserveSet<CFGAnalyses> ();
	return preserved;
}

} // namespace mono
