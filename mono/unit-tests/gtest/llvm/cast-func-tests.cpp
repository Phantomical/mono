/*
 * Tests for LowerCastFuncPass, which writes a type test back as the probe the
 * runtime reads.
 *
 * Pure LLVM. A site whose class operand carries no marker gets the null check,
 * the cache probe and the wrapper, and none of the inline test - which is the
 * arm that needs no runtime to build or to read.
 */

#include "passes/cast-func.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>

#include <gtest/gtest.h>

#include <memory>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// A module holding one cast site, either as a call or as an invoke with a
/// handler around it.
struct CastModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	Function *wrapper = nullptr;
	BasicBlock *pad = nullptr;
	CallBase *site = nullptr;

	CastModule (bool throw_on_fail, bool protect = false)
	{
		module = std::make_unique<Module> ("casts", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *i64 = Type::getInt64Ty (*context);

		// The wrapper's own signature spells the class and the cache as
		// integers, which is what the runtime registered the icall with.
		wrapper = Function::Create (FunctionType::get (ptr, { ptr, i64, i64 }, false),
		                            GlobalValue::ExternalLinkage, "isinst_wrapper",
		                            module.get ());

		caller = Function::Create (FunctionType::get (ptr, { ptr }, false),
		                           GlobalValue::ExternalLinkage, "caller",
		                           module.get ());
		caller->setPersonalityFn (
			Function::Create (FunctionType::get (Type::getInt32Ty (*context), true),
		                          GlobalValue::ExternalLinkage, "personality",
		                          module.get ()));

		auto *klass = new GlobalVariable (*module, Type::getInt8Ty (*context), false,
		                                  GlobalValue::ExternalLinkage, nullptr,
		                                  "mono_class_Thing");
		auto *cache = new GlobalVariable (
			*module, ptr, false, GlobalValue::InternalLinkage,
			ConstantPointerNull::get (cast<PointerType> (ptr)), "cast_cache");

		BasicBlock *entry = BasicBlock::Create (*context, "entry", caller);
		BasicBlock *tail = BasicBlock::Create (*context, "tail", caller);
		IRBuilder<> b (entry);
		Value *args[] = { caller->getArg (0), klass, cache, wrapper };
		Function *decl = cast_func_decl (*module, throw_on_fail);

		if (!protect) {
			site = b.CreateCall (decl, args);
			b.CreateBr (tail);
		} else {
			pad = BasicBlock::Create (*context, "pad", caller);
			site = b.CreateInvoke (decl, tail, pad, args);

			b.SetInsertPoint (pad);

			auto *caught = b.CreateLandingPad (
				StructType::get (ptr, Type::getInt32Ty (*context)), 0);

			caught->setCleanup (true);
			b.CreateRet (ConstantPointerNull::get (cast<PointerType> (ptr)));
		}

		b.SetInsertPoint (tail);
		b.CreateRet (site);
	}

	void lower ()
	{
		ModuleAnalysisManager mam;

		LowerCastFuncPass ().run (*module, mam);
	}

	std::string text () const
	{
		std::string printed;
		raw_string_ostream out (printed);

		module->print (out, nullptr);
		return printed;
	}

	unsigned count (StringRef needle) const
	{
		std::string text = this->text ();
		unsigned seen = 0;
		size_t at = 0;

		while ((at = text.find (needle.str (), at)) != std::string::npos) {
			++seen;
			at += needle.size ();
		}

		return seen;
	}
};

TEST (CastFuncTest, ACallSiteBecomesTheProbeAndTheWrapper)
{
	CastModule m (/*throw_on_fail=*/false);

	m.lower ();

	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
	EXPECT_EQ (m.module->getFunction (cast_isinst_name), nullptr);

	// The null check, the cache read and the wrapper the miss falls back to.
	EXPECT_EQ (m.count ("icmp eq ptr"), 1u) << m.text ();
	EXPECT_EQ (m.count ("%cached_vtable = load"), 1u) << m.text ();
	EXPECT_EQ (m.count ("call ptr @isinst_wrapper"), 1u) << m.text ();
	EXPECT_EQ (m.count ("%cast_result = phi"), 1u) << m.text ();
}

// isinst caches a refusal in bit 0 of the same slot, so its probe masks that
// bit off before it compares and then reads it to pick between obj and null.
TEST (CastFuncTest, IsinstReadsTheRefusalBitFromItsCache)
{
	CastModule m (/*throw_on_fail=*/false);

	m.lower ();

	ASSERT_FALSE (verifyModule (*m.module, &errs ()));
	EXPECT_EQ (m.count ("and i64"), 1u) << m.text ();
	EXPECT_EQ (m.count ("-2"), 1u) << m.text ();
	EXPECT_EQ (m.count ("%answered_no = trunc"), 1u) << m.text ();
	EXPECT_EQ (m.count ("select i1"), 1u) << m.text ();
}

// castclass raises instead of answering null, so its slot holds the vtable on
// its own and the probe compares the word as it stands.
TEST (CastFuncTest, CastclassReadsItsSlotWholeAndMasksNothing)
{
	CastModule m (/*throw_on_fail=*/true);

	m.lower ();

	ASSERT_FALSE (verifyModule (*m.module, &errs ()));
	EXPECT_EQ (m.count ("and i64"), 0u);
	EXPECT_EQ (m.count ("answered_no"), 0u);
	EXPECT_EQ (m.count ("select i1"), 0u);
	EXPECT_EQ (m.count ("call ptr @isinst_wrapper"), 1u);
}

/*
 * A clause around the site makes it an invoke, and the wrapper is what can
 * raise. So the wrapper inherits the unwind edge, and the block it lands in
 * takes the site's place among the pad's predecessors.
 */
TEST (CastFuncTest, AProtectedSiteLeavesTheWrapperOnTheUnwindEdge)
{
	CastModule m (/*throw_on_fail=*/false, /*protect=*/true);

	m.lower ();

	ASSERT_FALSE (verifyModule (*m.module, &errs ()));
	EXPECT_EQ (m.count ("invoke ptr @isinst_wrapper"), 1u);
	EXPECT_EQ (m.count ("unwind label %pad"), 1u);

	// The probe itself raises nothing, so nothing else reaches the pad.
	unsigned edges = 0;

	for (BasicBlock &block : *m.caller)
		for (const BasicBlock *to : successors (&block))
			if (to == m.pad)
				++edges;

	EXPECT_EQ (edges, 1u);
}

// Neither form reads the cache for a null reference, because the probe loads
// the object's vtable and both wrappers answer null without one.
TEST (CastFuncTest, TheNullCheckStandsInFrontOfEveryVtableRead)
{
	CastModule m (/*throw_on_fail=*/false);

	m.lower ();

	ASSERT_FALSE (verifyModule (*m.module, &errs ()));

	BasicBlock &entry = m.caller->getEntryBlock ();
	auto *branch = dyn_cast<BranchInst> (entry.getTerminator ());

	ASSERT_NE (branch, nullptr);
	ASSERT_TRUE (branch->isConditional ());

	// The taken edge answers null, and it reads nothing off the object.
	for (Instruction &in : entry)
		EXPECT_FALSE (isa<LoadInst> (in)) << "the null check reads the object";
}

} // namespace
} // namespace test
} // namespace mono
