/*
 * Tests for ArrayAddressPass, which turns a `mono.array.address.*` site into a
 * bounds check for each dimension.
 *
 * Pure LLVM: the pass names no mono type, so neither do these. The layout the
 * translator reads off MonoArray is written here by hand.
 *
 * The cases are about the `!invariant.load` tag on the header reads. A lost
 * tag changes no result managed code can see. It costs each bounds check that
 * merges across a store to a managed field, so only a test that reads the IR
 * catches it.
 */

#include "passes/array-address.hpp"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// The amd64 layout of MonoArray and MonoArrayBounds, which is what the
/// translator writes on the declaration. The widths are the ones
/// `MONO_BIG_ARRAYS` leaves undefined, where `mono_array_size_t` is 4 bytes.
constexpr const char *szarray_spec =
	"rank=1,size=8,bounded=0,token=1,bounds=16,maxlen=24,maxlen_bytes=4,"
	"vector=32,stride=8,blen=0,blen_bytes=4,blb=4,blb_bytes=4";
constexpr const char *rect_spec =
	"rank=2,size=8,bounded=1,token=1,bounds=16,maxlen=24,maxlen_bytes=4,"
	"vector=32,stride=8,blen=0,blen_bytes=4,blb=4,blb_bytes=4";

/// A module holding one address site, either on a szarray or on a rectangular
/// array with a lower bound.
struct AddressModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;

	AddressModule (unsigned rank, const char *spec)
	{
		module = std::make_unique<Module> ("array-address", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *i32 = Type::getInt32Ty (*context);
		std::vector<Type *> params (1 + rank, i32);

		params[0] = ptr;

		std::string name =
			(Twine (array_address_prefix) + "r" + Twine (rank)).str ();
		Function *decl =
			Function::Create (FunctionType::get (ptr, params, false),
			                  GlobalValue::ExternalLinkage, name, module.get ());

		decl->addFnAttr (Attribute::get (*context, array_address_attribute, spec));

		caller = Function::Create (FunctionType::get (ptr, { ptr }, false),
		                           GlobalValue::ExternalLinkage, "caller",
		                           module.get ());

		BasicBlock *entry = BasicBlock::Create (*context, "entry", caller);
		IRBuilder<> b (entry);
		std::vector<Value *> args (1 + rank, ConstantInt::get (i32, 3));

		args[0] = caller->getArg (0);
		b.CreateRet (b.CreateCall (decl, args));
	}

	void run ()
	{
		ModuleAnalysisManager mam;

		ArrayAddressPass ().run (*module, mam);
		ASSERT_FALSE (verifyModule (*module, &errs ()));
	}

	/// Counts the loads the pass left in the caller, and how many of them carry
	/// `!invariant.load`.
	void count_loads (unsigned *total, unsigned *tagged)
	{
		*total = 0;
		*tagged = 0;
		for (Instruction &i : instructions (*caller)) {
			auto *load = dyn_cast<LoadInst> (&i);

			if (load == nullptr)
				continue;
			++*total;
			if (load->hasMetadata (LLVMContext::MD_invariant_load))
				++*tagged;
		}
	}
};

/// A szarray has no bounds vector, so the check reads only max_length.
TEST (ArrayAddress, SzarrayLengthIsInvariant)
{
	AddressModule m (1, szarray_spec);
	unsigned total = 0, tagged = 0;

	ASSERT_NO_FATAL_FAILURE (m.run ());
	m.count_loads (&total, &tagged);
	EXPECT_EQ (total, 1u);
	EXPECT_EQ (tagged, total);
}

/// A rectangular array reads the bounds pointer, then a length and a lower
/// bound for each of the two dimensions.
TEST (ArrayAddress, RectangularHeaderIsInvariant)
{
	AddressModule m (2, rect_spec);
	unsigned total = 0, tagged = 0;

	ASSERT_NO_FATAL_FAILURE (m.run ());
	m.count_loads (&total, &tagged);
	EXPECT_EQ (total, 5u);
	EXPECT_EQ (tagged, total);
}

/// The tag must not make the header reads speculatable. A load hoisted over
/// the null check on the array faults when the array is null.
TEST (ArrayAddress, HeaderLoadsAreNotSpeculatable)
{
	AddressModule m (2, rect_spec);

	ASSERT_NO_FATAL_FAILURE (m.run ());
	for (Instruction &i : instructions (*m.caller)) {
		auto *load = dyn_cast<LoadInst> (&i);

		if (load == nullptr)
			continue;
		EXPECT_FALSE (isSafeToSpeculativelyExecute (load));
	}
}

} // namespace
} // namespace test
} // namespace mono
