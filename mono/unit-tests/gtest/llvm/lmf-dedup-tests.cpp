/* Tests for LmfDedupPass using the IR shape emitted by the LMF helpers. */

#include "passes/lmf-dedup.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace llvm;

namespace mono {
namespace test {
namespace {

	/// A module with one caller and a stand-in for the TLS lmf_addr slot.
struct LmfDedupModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	Type *ptr = nullptr;
	Type *i64 = nullptr;
	Type *slot_ty = nullptr;
	GlobalVariable *lmf_addr = nullptr;

	LmfDedupModule ()
	{
		module = std::make_unique<Module> ("lmf-dedup", *context);

		ptr = PointerType::get (*context, 0);
		i64 = Type::getInt64Ty (*context);
		slot_ty = ArrayType::get (Type::getInt8Ty (*context), 24);

		caller = Function::Create (
			FunctionType::get (Type::getVoidTy (*context), { i64 }, false),
			GlobalValue::ExternalLinkage, "caller", module.get ());
		lmf_addr = new GlobalVariable (*module.get (), ptr, false,
		                               GlobalValue::ExternalLinkage, nullptr,
		                               "lmf_addr_slot");
	}

	BasicBlock *block (StringRef name) { return BasicBlock::Create (*context, name, caller); }

	/// Write one LMF push and return its slot.
	AllocaInst *push (IRBuilder<> &b)
	{
		AllocaInst *slot = b.CreateAlloca (slot_ty);
		Value *previous = b.CreateLoad (ptr, lmf_addr);

		b.CreateStore (previous, slot);

		Value *name = MetadataAsValue::get (
			*context, MDNode::get (*context, MDString::get (*context, "rbp")));
		Value *rbp_gep = b.CreateConstInBoundsGEP1_32 (b.getInt8Ty (), slot, 8);
		StoreInst *rbp_store = b.CreateStore (
			b.CreateIntrinsic (Intrinsic::read_register, { i64 }, { name }), rbp_gep);
		Value *rsp_gep = b.CreateConstInBoundsGEP1_32 (b.getInt8Ty (), slot, 16);
		StoreInst *rsp_store =
			b.CreateStore (b.CreatePtrToInt (b.CreateStackSave (), i64), rsp_gep);

		rbp_store->setMetadata ("mono.lmf.capture", MDNode::get (*context, {}));
		rsp_store->setMetadata ("mono.lmf.capture", MDNode::get (*context, {}));
		b.CreateStore (slot, lmf_addr);
		return slot;
	}

	/// Write the matching LMF pop.
	void pop (IRBuilder<> &b, AllocaInst *slot)
	{
		Value *previous = b.CreateLoad (ptr, slot);
		StoreInst *release = b.CreateStore (previous, lmf_addr);

		release->setMetadata ("mono.lmf.release", MDNode::get (*context, {}));
	}

	void dedup ()
	{
		PassBuilder pb;
		FunctionAnalysisManager fam;

		pb.registerFunctionAnalyses (fam);
		LmfDedupPass ().run (*caller, fam);
	}

	unsigned captures () const
	{
		unsigned found = 0;

		for (const Instruction &i : instructions (caller))
			if (i.getMetadata ("mono.lmf.capture") != nullptr)
				++found;
		return found;
	}

	unsigned allocas () const
	{
		unsigned found = 0;

		for (const Instruction &i : instructions (caller))
			if (isa<AllocaInst> (i))
				++found;
		return found;
	}
};

} // namespace

TEST (LmfDedup, ASecondPushDominatedByTheFirstsPopReusesItsSlot)
{
	LmfDedupModule m;
	IRBuilder<> b (m.block ("entry"));
	AllocaInst *first = m.push (b);

	m.pop (b, first);

	AllocaInst *second = m.push (b);

	m.pop (b, second);
	b.CreateRetVoid ();

	m.dedup ();
	EXPECT_FALSE (verifyFunction (*m.caller, &errs ()));
	EXPECT_EQ (m.allocas (), 1u);
	EXPECT_EQ (m.captures (), 2u);
}

TEST (LmfDedup, AChainOfSequentialPushesAllReuseTheFirstsSlot)
{
	LmfDedupModule m;
	IRBuilder<> b (m.block ("entry"));
	AllocaInst *first = m.push (b);

	m.pop (b, first);

	AllocaInst *second = m.push (b);

	m.pop (b, second);

	AllocaInst *third = m.push (b);

	m.pop (b, third);
	b.CreateRetVoid ();

	m.dedup ();
	EXPECT_EQ (m.allocas (), 1u);
	EXPECT_EQ (m.captures (), 2u);

	for (const Instruction &i : instructions (m.caller)) {
		if (const auto *alloca = dyn_cast<AllocaInst> (&i)) {
			EXPECT_EQ (alloca, first);
		}
	}
}

TEST (LmfDedup, ANestedPushKeepsItsOwnSlot)
{
	LmfDedupModule m;
	IRBuilder<> b (m.block ("entry"));
	AllocaInst *outer = m.push (b);
	AllocaInst *inner = m.push (b);

	m.pop (b, inner);
	m.pop (b, outer);
	b.CreateRetVoid ();

	m.dedup ();
	EXPECT_FALSE (verifyFunction (*m.caller, &errs ()));
	EXPECT_EQ (m.allocas (), 2u);
	EXPECT_EQ (m.captures (), 4u);
}

TEST (LmfDedup, PushesOnExclusiveBranchesEachKeepTheirOwnSlot)
{
	LmfDedupModule m;
	BasicBlock *entry = m.block ("entry");
	BasicBlock *left = m.block ("left");
	BasicBlock *right = m.block ("right");
	BasicBlock *join = m.block ("join");

	IRBuilder<> b (entry);

	b.CreateCondBr (ConstantInt::getTrue (*m.context), left, right);

	b.SetInsertPoint (left);

	AllocaInst *on_left = m.push (b);

	m.pop (b, on_left);
	b.CreateBr (join);

	b.SetInsertPoint (right);

	AllocaInst *on_right = m.push (b);

	m.pop (b, on_right);
	b.CreateBr (join);

	IRBuilder<> (join).CreateRetVoid ();

	m.dedup ();
	EXPECT_FALSE (verifyFunction (*m.caller, &errs ()));
	EXPECT_EQ (m.allocas (), 2u);
	EXPECT_EQ (m.captures (), 4u);
}

TEST (LmfDedup, ADynamicAllocaAnywhereInTheFunctionDisablesReuse)
{
	LmfDedupModule m;
	IRBuilder<> b (m.block ("entry"));
	AllocaInst *first = m.push (b);

	m.pop (b, first);
	b.CreateAlloca (b.getInt8Ty (), m.caller->getArg (0));

	AllocaInst *second = m.push (b);

	m.pop (b, second);
	b.CreateRetVoid ();

	m.dedup ();
	EXPECT_EQ (m.allocas (), 3u);
	EXPECT_EQ (m.captures (), 4u);
}

TEST (LmfDedup, ASlotWithOnlyOneCaptureStoreIsLeftAlone)
{
	LmfDedupModule m;
	IRBuilder<> b (m.block ("entry"));
	AllocaInst *first = m.push (b);

	m.pop (b, first);

	AllocaInst *second = b.CreateAlloca (m.slot_ty);
	Value *rbp_gep = b.CreateConstInBoundsGEP1_32 (b.getInt8Ty (), second, 8);
	StoreInst *rbp_store = b.CreateStore (ConstantInt::get (m.i64, 0), rbp_gep);

	rbp_store->setMetadata ("mono.lmf.capture", MDNode::get (*m.context, {}));
	m.pop (b, second);
	b.CreateRetVoid ();

	m.dedup ();
	EXPECT_EQ (m.allocas (), 2u);
}

} // namespace test
} // namespace mono
