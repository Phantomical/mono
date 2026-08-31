/*
 * Tests for FoldEmptyFinallyPass, which erases a finally's body markers and
 * thread-abort check once nothing survives between the markers.
 *
 * Pure LLVM: the pass names no metadata, so these hand-build the marker
 * calls and the abort-check shape exceptions.cpp writes around them, rather
 * than driving method_to_llvm () over a corpus.
 */

#include "passes/fold-empty-finally.hpp"

#include "mono_lsda_format.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace llvm;

namespace mono {
namespace test {
namespace {

std::uint64_t
begin_id (std::uint32_t clause)
{
	return MONO_LLVM_FINALLY_STACKMAP_ID_BASE | clause;
}

std::uint64_t
end_id (std::uint32_t clause)
{
	return MONO_LLVM_FINALLY_END_STACKMAP_ID_BASE | clause;
}

/// Plants a marker the way emit_finally_body_marker () does: an opening one
/// names its clause's guard, a closing one names nothing.
CallInst *
emit_marker (IRBuilder<> &b, std::uint64_t id, ArrayRef<Value *> vars = {})
{
	std::vector<Value *> args = { b.getInt64 (id), b.getInt32 (0) };

	args.insert (args.end (), vars.begin (), vars.end ());
	return b.CreateIntrinsic (Intrinsic::experimental_stackmap, {}, args);
}

/// Builds the load/branch/deliver/leaving shape emit_finally_abort_check ()
/// writes, starting at wherever b is positioned, and leaves b positioned at
/// the end of `leaving`.
void
build_abort_check (Function &f, IRBuilder<> &b, Value *guard, FunctionCallee finish)
{
	LLVMContext &ctx = f.getContext ();
	auto *deliver = BasicBlock::Create (ctx, "deliver", &f);
	auto *leaving = BasicBlock::Create (ctx, "leaving", &f);
	Value *flagged = b.CreateLoad (b.getInt8Ty (), guard, /* isVolatile */ true, "flagged");

	b.CreateCondBr (b.CreateICmpNE (flagged, b.getInt8 (0)), deliver, leaving);

	IRBuilder<> in_deliver (deliver);

	in_deliver.CreateCall (finish);
	in_deliver.CreateBr (leaving);

	b.SetInsertPoint (leaving);
}

bool
has_stackmap (Function &f)
{
	for (Instruction &i : instructions (f))
		if (auto *call = dyn_cast<IntrinsicInst> (&i);
		    call != nullptr && call->getIntrinsicID () == Intrinsic::experimental_stackmap)
			return true;

	return false;
}

bool
has_alloca (Function &f)
{
	for (Instruction &i : instructions (f))
		if (isa<AllocaInst> (i))
			return true;

	return false;
}

bool
calls (Function &f, Function *callee)
{
	for (Instruction &i : instructions (f))
		if (auto *call = dyn_cast<CallBase> (&i); call != nullptr && call->getCalledFunction () == callee)
			return true;

	return false;
}

void
run_pass (Function &f)
{
	FunctionAnalysisManager fam;

	FoldEmptyFinallyPass ().run (f, fam);
}

} // namespace

TEST (FoldEmptyFinally, EmptyBodyInOneBlockDropsMarkersAndAbortCheck)
{
	LLVMContext ctx;
	Module m ("empty-finally", ctx);
	FunctionCallee finish =
		m.getOrInsertFunction ("finish", FunctionType::get (Type::getVoidTy (ctx), false));
	auto *f = Function::Create (FunctionType::get (Type::getVoidTy (ctx), false),
	                            GlobalValue::ExternalLinkage, "f", &m);
	IRBuilder<> b (BasicBlock::Create (ctx, "entry", f));
	AllocaInst *guard = b.CreateAlloca (b.getInt8Ty (), nullptr, "guard0");

	b.CreateStore (b.getInt8 (0), guard);
	emit_marker (b, begin_id (0), { guard });
	// Nothing of the finally's own IL survives here - this is the shape the
	// bug report is about.
	emit_marker (b, end_id (0));
	build_abort_check (*f, b, guard, finish);
	b.CreateRetVoid ();

	run_pass (*f);

	EXPECT_FALSE (has_stackmap (*f));
	EXPECT_FALSE (has_alloca (*f));
	EXPECT_FALSE (calls (*f, cast<Function> (finish.getCallee ())));
	EXPECT_FALSE (verifyModule (m, &errs ()));
}

/// EmptyBodyInOneBlockDropsMarkersAndAbortCheck covers a body SimplifyCFG has
/// already merged into one block. This one spreads the markers and the abort
/// check back over the separate blocks exceptions.cpp itself builds, to
/// prove the walk does not depend on that merge having already happened.
TEST (FoldEmptyFinally, EmptyBodySpreadOverSeveralBlocksStillFolds)
{
	LLVMContext ctx;
	Module m ("empty-finally-multiblock", ctx);
	FunctionCallee finish =
		m.getOrInsertFunction ("finish", FunctionType::get (Type::getVoidTy (ctx), false));
	auto *f = Function::Create (FunctionType::get (Type::getVoidTy (ctx), false),
	                            GlobalValue::ExternalLinkage, "f", &m);
	auto *entry = BasicBlock::Create (ctx, "entry", f);
	auto *handler = BasicBlock::Create (ctx, "handler", f);
	auto *endfinally = BasicBlock::Create (ctx, "endfinally", f);
	IRBuilder<> b (entry);
	AllocaInst *guard = b.CreateAlloca (b.getInt8Ty (), nullptr, "guard0");

	b.CreateStore (b.getInt8 (0), guard);
	b.CreateBr (handler);

	b.SetInsertPoint (handler);
	emit_marker (b, begin_id (0), { guard });
	b.CreateBr (endfinally);

	b.SetInsertPoint (endfinally);
	emit_marker (b, end_id (0));
	build_abort_check (*f, b, guard, finish);
	b.CreateRetVoid ();

	run_pass (*f);

	EXPECT_FALSE (has_stackmap (*f));
	EXPECT_FALSE (has_alloca (*f));
	EXPECT_FALSE (calls (*f, cast<Function> (finish.getCallee ())));
	EXPECT_FALSE (verifyModule (m, &errs ()));
}

/// A finally with real work of its own keeps its markers and its abort
/// check - the fold only ever answers for a body that already has nothing
/// in it.
TEST (FoldEmptyFinally, RealWorkBetweenMarkersIsLeftAlone)
{
	LLVMContext ctx;
	Module m ("live-finally", ctx);
	FunctionCallee finish =
		m.getOrInsertFunction ("finish", FunctionType::get (Type::getVoidTy (ctx), false));
	FunctionCallee cleanup =
		m.getOrInsertFunction ("cleanup", FunctionType::get (Type::getVoidTy (ctx), false));
	auto *f = Function::Create (FunctionType::get (Type::getVoidTy (ctx), false),
	                            GlobalValue::ExternalLinkage, "f", &m);
	IRBuilder<> b (BasicBlock::Create (ctx, "entry", f));
	AllocaInst *guard = b.CreateAlloca (b.getInt8Ty (), nullptr, "guard0");

	b.CreateStore (b.getInt8 (0), guard);
	emit_marker (b, begin_id (0), { guard });
	b.CreateCall (cleanup);
	emit_marker (b, end_id (0));
	build_abort_check (*f, b, guard, finish);
	b.CreateRetVoid ();

	run_pass (*f);

	EXPECT_TRUE (has_stackmap (*f));
	EXPECT_TRUE (has_alloca (*f));
	EXPECT_TRUE (calls (*f, cast<Function> (cleanup.getCallee ())));
	EXPECT_FALSE (verifyModule (m, &errs ()));
}

/// Two openings naming the same clause is not a shape the front end ever
/// writes. The fold declines rather than guessing which one is real.
TEST (FoldEmptyFinally, AmbiguousOpeningMarkerIsLeftAlone)
{
	LLVMContext ctx;
	Module m ("ambiguous-finally", ctx);
	auto *f = Function::Create (FunctionType::get (Type::getVoidTy (ctx), false),
	                            GlobalValue::ExternalLinkage, "f", &m);
	IRBuilder<> b (BasicBlock::Create (ctx, "entry", f));
	AllocaInst *guard = b.CreateAlloca (b.getInt8Ty (), nullptr, "guard0");

	emit_marker (b, begin_id (0), { guard });
	emit_marker (b, begin_id (0), { guard });
	emit_marker (b, end_id (0));
	b.CreateRetVoid ();

	run_pass (*f);

	EXPECT_TRUE (has_stackmap (*f));
	EXPECT_TRUE (has_alloca (*f));
	EXPECT_FALSE (verifyModule (m, &errs ()));
}

/// A finally nested inside another one's body is what an outer clause's own
/// markers bracket, so the outer region only reads empty once the inner
/// clause's markers are gone. FoldEmptyFinallyPass has to take another round
/// after folding the inner one to see that.
TEST (FoldEmptyFinally, NestedEmptyFinallyFoldsBothOnce)
{
	LLVMContext ctx;
	Module m ("nested-empty-finally", ctx);
	auto *f = Function::Create (FunctionType::get (Type::getVoidTy (ctx), false),
	                            GlobalValue::ExternalLinkage, "f", &m);
	IRBuilder<> b (BasicBlock::Create (ctx, "entry", f));
	AllocaInst *outer_guard = b.CreateAlloca (b.getInt8Ty (), nullptr, "guard0");
	AllocaInst *inner_guard = b.CreateAlloca (b.getInt8Ty (), nullptr, "guard1");

	b.CreateStore (b.getInt8 (0), outer_guard);
	emit_marker (b, begin_id (0), { outer_guard });
	b.CreateStore (b.getInt8 (0), inner_guard);
	emit_marker (b, begin_id (1), { inner_guard });
	emit_marker (b, end_id (1));
	emit_marker (b, end_id (0));
	b.CreateRetVoid ();

	run_pass (*f);

	EXPECT_FALSE (has_stackmap (*f));
	EXPECT_FALSE (has_alloca (*f));
	EXPECT_FALSE (verifyModule (m, &errs ()));
}

} // namespace test
} // namespace mono
