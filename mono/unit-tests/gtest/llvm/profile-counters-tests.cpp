/*
 * Tests for ProfileAtomicPass, which makes each counter update the
 * instrumentation lowering left non-atomic one atomicrmw again.
 *
 * Pure LLVM: the pass names no metadata, so neither do these.
 */

#include "passes/profile-counters.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/ProfileData/InstrProf.h>

#include <gtest/gtest.h>

#include <memory>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// A module with an array of counters and a function that adds to them.
///
/// The shapes the cases build are the ones the lowering leaves standing: a
/// load, an add and a store on one slot of `__profc_`, an atomicrmw where
/// promotion already put one, and the same group of three on a global that is
/// not a counter.
struct CounterModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Type *i64 = nullptr;
	GlobalVariable *counters = nullptr;
	GlobalVariable *other = nullptr;
	Function *body = nullptr;
	std::unique_ptr<IRBuilder<>> in_body;

	explicit CounterModule (unsigned slots = 4)
	{
		module = std::make_unique<Module> ("counters", *context);
		i64 = Type::getInt64Ty (*context);

		ArrayType *array = ArrayType::get (i64, slots);
		std::string name = (getInstrProfCountersVarPrefix () + "Some_Method").str ();

		counters = new GlobalVariable (*module, array, false,
		                               GlobalValue::PrivateLinkage,
		                               Constant::getNullValue (array), name);
		other = new GlobalVariable (*module, i64, false,
		                            GlobalValue::PrivateLinkage,
		                            Constant::getNullValue (i64), "a_global");

		body = Function::Create (FunctionType::get (Type::getVoidTy (*context), false),
		                         GlobalValue::ExternalLinkage, "body", module.get ());
		in_body = std::make_unique<IRBuilder<>> (
			BasicBlock::Create (*context, "entry", body));
	}

	/// The address of one slot, as the lowering writes it.
	Value *slot (unsigned index)
	{
		return in_body->CreateConstInBoundsGEP2_64 (counters->getValueType (),
		                                            counters, 0, index);
	}

	/// Adds \p step to \p address as a load, an add and a store.
	void add_in_three_steps (Value *address, uint64_t step = 1)
	{
		Value *old = in_body->CreateLoad (i64, address, "pgocount");

		in_body->CreateStore (in_body->CreateAdd (old, ConstantInt::get (i64, step)),
		                      address);
	}

	void run ()
	{
		in_body->CreateRetVoid ();

		ModuleAnalysisManager mam;
		PassBuilder pb;

		pb.registerModuleAnalyses (mam);
		ProfileAtomicPass ().run (*module, mam);
	}

	/// Every atomicrmw the body holds, in the order they stand in.
	std::vector<AtomicRMWInst *> atomics () const
	{
		std::vector<AtomicRMWInst *> found;

		for (Instruction &i : instructions (*body))
			if (auto *rmw = dyn_cast<AtomicRMWInst> (&i))
				found.push_back (rmw);

		return found;
	}

	unsigned count_of (unsigned opcode) const
	{
		unsigned n = 0;

		for (Instruction &i : instructions (*body))
			if (i.getOpcode () == opcode)
				n++;

		return n;
	}
};

} // namespace

TEST (ProfileAtomic, AReadAddWriteOnACounterBecomesOneAtomicAdd)
{
	CounterModule m;

	m.add_in_three_steps (m.slot (0));
	m.run ();

	std::vector<AtomicRMWInst *> atomics = m.atomics ();

	ASSERT_EQ (atomics.size (), 1u);
	EXPECT_EQ (atomics[0]->getOperation (), AtomicRMWInst::Add);
	EXPECT_EQ (atomics[0]->getOrdering (), AtomicOrdering::Monotonic);
	EXPECT_EQ (atomics[0]->getValOperand (),
	           ConstantInt::get (m.i64, 1));
	// The three it replaces are gone rather than left for a later pass.
	EXPECT_EQ (m.count_of (Instruction::Load), 0u);
	EXPECT_EQ (m.count_of (Instruction::Store), 0u);
	EXPECT_EQ (m.count_of (Instruction::Add), 0u);
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

/*
 * A promoted counter is written back once for a whole loop, so the value it
 * adds is a register rather than a constant. The pass has to keep it.
 */
TEST (ProfileAtomic, ARegisterStepIsKept)
{
	CounterModule m;
	Value *address = m.slot (2);
	Value *accumulated = m.in_body->CreateAdd (ConstantInt::get (m.i64, 3),
	                                           ConstantInt::get (m.i64, 4),
	                                           "pgocount.promoted");
	Value *old = m.in_body->CreateLoad (m.i64, address, "pgocount");

	m.in_body->CreateStore (m.in_body->CreateAdd (old, accumulated), address);
	m.run ();

	std::vector<AtomicRMWInst *> atomics = m.atomics ();

	ASSERT_EQ (atomics.size (), 1u);
	EXPECT_EQ (atomics[0]->getValOperand (), accumulated);
	EXPECT_EQ (atomics[0]->getPointerOperand (), address);
}

TEST (ProfileAtomic, EachSlotIsTakenSeparately)
{
	CounterModule m;

	m.add_in_three_steps (m.slot (0));
	m.add_in_three_steps (m.slot (1));
	m.add_in_three_steps (m.slot (3));
	m.run ();

	EXPECT_EQ (m.atomics ().size (), 3u);
	EXPECT_EQ (m.count_of (Instruction::Store), 0u);
}

/*
 * What promotion already took. The pass must not count it twice, and an
 * atomicrmw is not the group of three it looks for.
 */
TEST (ProfileAtomic, AnAtomicUpdateIsLeftAlone)
{
	CounterModule m;
	Value *address = m.slot (0);

	m.in_body->CreateAtomicRMW (AtomicRMWInst::Add, address,
	                            ConstantInt::get (m.i64, 1), MaybeAlign (),
	                            AtomicOrdering::Monotonic);
	m.run ();

	ASSERT_EQ (m.atomics ().size (), 1u);
	EXPECT_EQ (m.atomics ()[0]->getOrdering (), AtomicOrdering::Monotonic);
}

/*
 * The counters are the only globals this pass owns. A translated body adds to
 * a static field with the same three instructions.
 */
TEST (ProfileAtomic, AGlobalThatIsNotACounterIsLeftAlone)
{
	CounterModule m;

	m.add_in_three_steps (m.other);
	m.run ();

	EXPECT_EQ (m.atomics ().size (), 0u);
	EXPECT_EQ (m.count_of (Instruction::Load), 1u);
	EXPECT_EQ (m.count_of (Instruction::Store), 1u);
}

/*
 * A load whose value goes somewhere else as well is not a counter update on
 * its own, and folding it into an atomicrmw would drop that other reader.
 */
TEST (ProfileAtomic, ALoadWithASecondReaderIsLeftAlone)
{
	CounterModule m;
	Value *address = m.slot (1);
	Value *old = m.in_body->CreateLoad (m.i64, address, "pgocount");

	m.in_body->CreateStore (m.in_body->CreateAdd (old, ConstantInt::get (m.i64, 1)),
	                        address);
	m.in_body->CreateStore (old, m.other);
	m.run ();

	EXPECT_EQ (m.atomics ().size (), 0u);
	EXPECT_EQ (m.count_of (Instruction::Load), 1u);
}

} // namespace test
} // namespace mono
