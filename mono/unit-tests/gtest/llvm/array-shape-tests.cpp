/*
 * Tests for the two halves of `mono.array.shape.*`: the fold reads such a site
 * out of the array where it names dimension zero, and the lowering puts back on
 * the accessor every site that is left.
 *
 * Both read MonoArray's layout out of mono's headers, which accessor and
 * which fallback method off the declaration, and the dimension and the exception
 * token off the site. None of that needs a runtime, so these cases run with none
 * under them. What each case counts is the header reads, not the offsets they
 * are made at.
 */

#include "cl-opt-override.hpp"
#include "passes/array-shape.hpp"

#include "analysis/constant-values.hpp"
#include "pipelines.hpp"

#include <llvm/IR/Constants.h>
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

/// A module holding one shape site, asking for the dimension the caller names.
struct ShapeModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	Function *target = nullptr;
	std::string decl_name;

	ShapeModule (StringRef kind, int dimension)
	{
		module = std::make_unique<Module> ("array-shape", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *i32 = Type::getInt32Ty (*context);

		target = Function::Create (FunctionType::get (i32, { ptr, i32 }, false),
		                           GlobalValue::ExternalLinkage,
		                           "System.Array:GetLength", module.get ());

		decl_name =
			(Twine (array_shape_prefix) + kind + "." + target->getName ()).str ();

		// The accessor's own arguments, and the exception token behind them.
		Function *decl =
			Function::Create (FunctionType::get (i32, { ptr, i32, i32 }, false),
		                          GlobalValue::ExternalLinkage, decl_name,
		                          module.get ());

		decl->addFnAttr (Attribute::get (*context, array_shape_attribute, kind));
		decl->addFnAttr (Attribute::get (*context, array_shape_target_attribute,
		                                 target->getName ()));

		caller = Function::Create (FunctionType::get (i32, { ptr }, false),
		                           GlobalValue::ExternalLinkage, "caller",
		                           module.get ());

		BasicBlock *entry = BasicBlock::Create (*context, "entry", caller);
		IRBuilder<> b (entry);

		b.CreateRet (b.CreateCall (decl, { caller->getArg (0),
		                                   ConstantInt::get (i32, dimension),
		                                   ConstantInt::get (i32, 1) }));
	}

	void run ()
	{
		lower_array_shapes (*module);
		ASSERT_FALSE (verifyModule (*module, &errs ()));
		ASSERT_EQ (module->getFunction (decl_name), nullptr);
	}

	/// Counts the loads the pass left in the caller, and how many of them carry
	/// `!invariant.group`.
	void count_loads (unsigned *total, unsigned *tagged)
	{
		*total = 0;
		*tagged = 0;
		for (Instruction &i : instructions (*caller)) {
			auto *load = dyn_cast<LoadInst> (&i);

			if (load == nullptr)
				continue;
			++*total;
			if (load->hasMetadata (LLVMContext::MD_invariant_group))
				++*tagged;
		}
	}

	/// Counts the calls to the accessor the caller still makes.
	unsigned count_calls ()
	{
		unsigned calls = 0;

		for (Instruction &i : instructions (*caller)) {
			auto *site = dyn_cast<CallBase> (&i);

			if (site != nullptr && site->getCalledFunction () == target)
				++calls;
		}

		return calls;
	}
};

/// The managers a fold asks its analyses through, held for as long as the call.
struct Analyses {
	ModuleAnalysisManager mam;
	CGSCCAnalysisManager cgam;
	FunctionAnalysisManager fam;
	LoopAnalysisManager lam;
	PassBuilder pb;

	Analyses ()
	{
		// All four, because MemorySSA asks alias analysis for GlobalsAA, which
		// is a module analysis it reaches through the proxy.
		pb.registerModuleAnalyses (mam);
		pb.registerCGSCCAnalyses (cgam);
		pb.registerFunctionAnalyses (fam);
		pb.registerLoopAnalyses (lam);
		pb.crossRegisterProxies (lam, fam, cgam, mam);
		register_mono_analyses (fam);
	}
};

/// Dimension zero is read out of the array: the bounds pointer, then either
/// max_length or the first dimension's length. Only the bounds pointer is
/// tagged while `tag_non_pointer_invariant_group ()` is off: `max_length` and
/// the first dimension's length are not pointers.
TEST (ArrayShape, ZeroReadsTheHeader)
{
	ShapeModule m (array_shape_length, 0);
	unsigned total = 0, tagged = 0;

	ASSERT_NO_FATAL_FAILURE (m.run ());
	m.count_loads (&total, &tagged);
	EXPECT_EQ (total, 3u);
	EXPECT_EQ (tagged, 1u);
	EXPECT_EQ (m.count_calls (), 0u);
}

/// The two non-pointer reads join the bounds pointer once
/// `-mono-invariant-group-nonptr` turns the non-pointer case back on.
TEST (ArrayShape, ZeroReadsTheHeaderFullyTaggedWhenEnabled)
{
	BoolOptionOverride enabled ("mono-invariant-group-nonptr", true);
	ShapeModule m (array_shape_length, 0);
	unsigned total = 0, tagged = 0;

	ASSERT_NO_FATAL_FAILURE (m.run ());
	m.count_loads (&total, &tagged);
	EXPECT_EQ (total, 3u);
	EXPECT_EQ (tagged, total);
	EXPECT_EQ (m.count_calls (), 0u);
}

/// A lower bound is zero where there is no bounds vector, so that arm reads
/// nothing. The remaining read, the lower bound itself, is not a pointer, so
/// only the bounds pointer is tagged while
/// `tag_non_pointer_invariant_group ()` is off.
TEST (ArrayShape, ZeroLowerBoundReadsTheBoundsOnly)
{
	ShapeModule m (array_shape_lower_bound, 0);
	unsigned total = 0, tagged = 0;

	ASSERT_NO_FATAL_FAILURE (m.run ());
	m.count_loads (&total, &tagged);
	EXPECT_EQ (total, 2u);
	EXPECT_EQ (tagged, 1u);
	EXPECT_EQ (m.count_calls (), 0u);
}

/// The lower-bound read joins the bounds pointer once
/// `-mono-invariant-group-nonptr` turns the non-pointer case back on.
TEST (ArrayShape, ZeroLowerBoundFullyTaggedWhenEnabled)
{
	BoolOptionOverride enabled ("mono-invariant-group-nonptr", true);
	ShapeModule m (array_shape_lower_bound, 0);
	unsigned total = 0, tagged = 0;

	ASSERT_NO_FATAL_FAILURE (m.run ());
	m.count_loads (&total, &tagged);
	EXPECT_EQ (total, 2u);
	EXPECT_EQ (tagged, total);
	EXPECT_EQ (m.count_calls (), 0u);
}

/// Any other dimension needs the rank test the accessor makes, so the site
/// goes back onto it.
TEST (ArrayShape, OtherDimensionsKeepTheCall)
{
	ShapeModule m (array_shape_length, 1);
	unsigned total = 0, tagged = 0;

	ASSERT_NO_FATAL_FAILURE (m.run ());
	m.count_loads (&total, &tagged);
	EXPECT_EQ (total, 0u);
	EXPECT_EQ (m.count_calls (), 1u);
}

/// A fold leaves a site it cannot read standing, so that a later round still
/// gets to read the dimension.
TEST (ArrayShape, AFoldKeepsASiteItCannotRead)
{
	ShapeModule m (array_shape_length, 1);

	Analyses analyses;

	fold_array_shapes (*m.module->getFunction ("caller"), analyses.fam);
	ASSERT_FALSE (verifyModule (*m.module, &errs ()));
	EXPECT_NE (m.module->getFunction (m.decl_name), nullptr);
	EXPECT_EQ (m.count_calls (), 0u);
}

} // namespace
} // namespace test
} // namespace mono
