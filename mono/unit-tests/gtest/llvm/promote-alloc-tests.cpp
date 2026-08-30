/*
 * Tests for promote_allocations (), which moves an object nothing outside the
 * function reaches into a frame slot.
 *
 * Each case gives the pass a module and reads the decision back: whether the
 * allocation site is still there, and whether a load kept the mark that says the
 * word it reads does not change. What the frame slot is made of is the pass's
 * own business, so no case reads the type, the name or the zeroing of the slot.
 *
 * A refusal case changes one thing about an object the pass would otherwise
 * take, so the case names the one gate that refused it.
 *
 * Pure LLVM. Each module declares its own allocator, so no runtime and no
 * collector stands under the walk.
 */

#include "passes/alloc-func.hpp"
#include "passes/promote-alloc.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SourceMgr.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// What every module below declares.
///
/// The two allocation declarations carry the attributes `alloc_func_decl ()`
/// puts on them. `noalias` is the one the cases rest on, because it is what
/// tells alias analysis that a fresh object is not one the caller already held.
constexpr const char *preamble = R"(
declare noalias ptr @"mono.alloc.object"(ptr, i64, ptr)
    memory(argmem: read, inaccessiblemem: readwrite)
declare noalias ptr @"mono.alloc.object.kept"(ptr, i64, ptr)
    memory(argmem: read, inaccessiblemem: readwrite)
declare void @opaque(ptr)
declare i32 @personality(...)
!0 = !{}
)";

/// A module the pass has run over.
struct Promoted {
	LLVMContext context;
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	bool changed = false;

	explicit Promoted (const std::string &ir)
	{
		SMDiagnostic problem;

		module = parseAssemblyString (ir + preamble, problem, context);

		if (module == nullptr) {
			std::string complaint;
			raw_string_ostream out (complaint);

			problem.print ("", out);
			ADD_FAILURE () << complaint;
			return;
		}

		caller = module->getFunction ("caller");

		if (caller == nullptr) {
			ADD_FAILURE () << "the module declares no caller";
			return;
		}

		ModuleAnalysisManager mam;
		FunctionAnalysisManager fam;
		LoopAnalysisManager lam;
		CGSCCAnalysisManager cgam;
		PassBuilder pb;

		pb.registerModuleAnalyses (mam);
		pb.registerFunctionAnalyses (fam);
		pb.registerLoopAnalyses (lam);
		pb.registerCGSCCAnalyses (cgam);
		pb.crossRegisterProxies (lam, fam, cgam, mam);

		changed = !PromoteAllocationsPass ().run (*caller, fam).areAllPreserved ();

		std::string complaint;
		raw_string_ostream out (complaint);

		EXPECT_FALSE (verifyModule (*module, &out)) << complaint;
	}

	/// How many sites the allocation declaration \p name still has.
	unsigned allocations (StringRef name = alloc_object_name) const
	{
		const Function *decl = module->getFunction (name);

		return decl == nullptr ? 0 : unsigned (decl->getNumUses ());
	}

	/// How many frame slots the caller holds.
	unsigned slots () const
	{
		unsigned seen = 0;

		for (const Instruction &in : instructions (*caller))
			seen += isa<AllocaInst> (&in) ? 1 : 0;

		return seen;
	}

	/// Whether the load the module calls \p name still states that the word it
	/// reads does not change.
	bool marked (StringRef name) const
	{
		for (const Instruction &in : instructions (*caller))
			if (isa<LoadInst> (in) && in.getName () == name)
				return in.getMetadata (LLVMContext::MD_invariant_group) != nullptr;

		ADD_FAILURE () << "no load named " << name.str ();
		return false;
	}
};

TEST (PromoteAllocTest, TakesAnObjectNothingReadsOutOfTheHeap)
{
	Promoted m (R"(
define void @caller(ptr %ref, ptr %other, i64 %size) {
  %obj = call ptr @"mono.alloc.object"(ptr null, i64 48, ptr null)
  ret void
}
)");

	EXPECT_TRUE (m.changed);
	EXPECT_EQ (m.allocations (), 0u);
	EXPECT_EQ (m.slots (), 1u);
}

TEST (PromoteAllocTest, KeepsAnObjectAStoreHandsAway)
{
	// The store writes the object into memory the pass cannot see the end of,
	// so the object outlives the frame.
	Promoted m (R"(
define void @caller(ptr %ref, ptr %other, i64 %size) {
  %obj = call ptr @"mono.alloc.object"(ptr null, i64 48, ptr null)
  store ptr %obj, ptr %other, align 8
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.allocations (), 1u);
	EXPECT_EQ (m.slots (), 0u);
}

TEST (PromoteAllocTest, KeepsAnObjectACallCanHold)
{
	Promoted m (R"(
define void @caller(ptr %ref, ptr %other, i64 %size) {
  %obj = call ptr @"mono.alloc.object"(ptr null, i64 48, ptr null)
  call void @opaque(ptr %obj)
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.allocations (), 1u);
}

TEST (PromoteAllocTest, KeepsAnObjectATurnOfALoopMakes)
{
	// One slot serves the whole frame, so a turn would hand back the object the
	// turn before it still holds.
	Promoted m (R"(
define void @caller(ptr %ref, ptr %other, i64 %size) {
entry:
  %again = icmp ne ptr %other, null
  br label %header

header:
  br i1 %again, label %body, label %done

body:
  %obj = call ptr @"mono.alloc.object"(ptr null, i64 48, ptr null)
  br label %header

done:
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.allocations (), 1u);
	EXPECT_EQ (m.slots (), 0u);
}

TEST (PromoteAllocTest, KeepsAnObjectLargerThanTheLimit)
{
	// The size comes from the limit itself, so raising the limit does not turn
	// this case into one the pass takes.
	Promoted m (R"(
define void @caller(ptr %ref, ptr %other, i64 %size) {
  %obj = call ptr @"mono.alloc.object"(ptr null, i64 )"
	            + std::to_string (promote_alloc_limit + 1) + R"(, ptr null)
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.allocations (), 1u);
}

TEST (PromoteAllocTest, KeepsAnObjectOfNoStatedSize)
{
	Promoted m (R"(
define void @caller(ptr %ref, ptr %other, i64 %size) {
  %obj = call ptr @"mono.alloc.object"(ptr null, i64 %size, ptr null)
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.allocations (), 1u);
}

TEST (PromoteAllocTest, KeepsAnObjectTheProgramCanTellWasAllocated)
{
	// The kept form is what a class with a finalizer, with weak fields, or one
	// that can answer with a proxy allocates through.
	Promoted m (R"(
define void @caller(ptr %ref, ptr %other, i64 %size) {
  %obj = call ptr @"mono.alloc.object.kept"(ptr null, i64 48, ptr null)
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.allocations (alloc_object_kept_name), 1u);
	EXPECT_EQ (m.slots (), 0u);
}

TEST (PromoteAllocTest, KeepsAnObjectAllocatedInsideATry)
{
	// An invoke names the pads its edges reach, and taking the site away asks
	// for a repair of each. emit_protected_call () gives a site inside a try
	// this shape.
	Promoted m (R"(
define void @caller(ptr %ref, ptr %other, i64 %size) personality ptr @personality {
entry:
  %obj = invoke ptr @"mono.alloc.object"(ptr null, i64 48, ptr null)
          to label %no_throw unwind label %pad

no_throw:
  ret void

pad:
  %caught = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %caught
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.allocations (), 1u);
	EXPECT_EQ (m.slots (), 0u);
}

TEST (PromoteAllocTest, KeepsTheMarkOnAReadOfThePromotedObject)
{
	// !invariant.group ties the read to the store the object's own allocation
	// makes, not to a value pinned at function entry, so the frame slot the
	// object moves to needs no fixup for it.
	Promoted m (R"(
define void @caller(ptr %ref, ptr %other, i64 %size) {
  %obj = call ptr @"mono.alloc.object"(ptr null, i64 48, ptr null)
  %vtable = load ptr, ptr %obj, align 8, !invariant.group !0
  ret void
}
)");

	ASSERT_TRUE (m.changed);
	EXPECT_TRUE (m.marked ("vtable"));
}

} // namespace
} // namespace test
} // namespace mono
