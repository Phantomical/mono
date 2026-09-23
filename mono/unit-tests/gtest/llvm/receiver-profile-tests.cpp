/*
 * Tests for the receiver record: ReceiverProfilePass and its lowering at tier 1,
 * the helper the lowered code calls, and AnnotateReceiversPass at tier 2.
 *
 * Pure LLVM. A method id and a vtable are numbers the passes compare and never
 * dereference.
 */

#include "passes/receiver-profile.hpp"

#include "compile-state.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/ProfileData/InstrProf.h>
#include <llvm/Support/SourceMgr.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

extern "C" void mono_llvm_jit_record_receiver (mono::ReceiverRecord *record, void *vtable);

using namespace llvm;

namespace mono {
namespace test {
namespace {

constexpr uint64_t caller_id = 0x1000;
constexpr uint64_t callee_id = 0x2000;

/*
 * caller dispatches twice: once at its own IL offset 7, and once at offset 2 of
 * a callee inlined into it.
 */
constexpr const char *dispatches = R"(
define ptr @caller(ptr %obj, ptr %vt) #0 !dbg !5 {
entry:
  %f = call ptr @mono.vtable.func(ptr %vt, i32 3), !dbg !10
  %r = call ptr %f(ptr %obj), !dbg !10
  %g = call ptr @mono.imt.func(ptr %vt, i32 2, ptr null), !dbg !11
  %s = call ptr %g(ptr %obj, ptr nest null), !dbg !11
  ret ptr %r, !dbg !10
}

define ptr @uninstrumented(ptr %obj, ptr %vt) !dbg !7 {
entry:
  %f = call ptr @mono.vtable.func(ptr %vt, i32 3), !dbg !12
  %r = call ptr %f(ptr %obj), !dbg !12
  ret ptr %r, !dbg !12
}

declare ptr @mono.vtable.func(ptr, i32)
declare ptr @mono.imt.func(ptr, i32, ptr)

attributes #0 = { "mono-tier-threshold"="100" }

!llvm.module.flags = !{!0}
!llvm.dbg.cu = !{!1}
!mono.il.subprogram.ids = !{!20, !21, !22}

!0 = !{i32 2, !"Debug Info Version", i32 3}
!1 = distinct !DICompileUnit(language: DW_LANG_C99, file: !2, isOptimized: true, emissionKind: NoDebug)
!2 = !DIFile(filename: "t", directory: ".")
!3 = !DISubroutineType(types: !4)
!4 = !{}
!5 = distinct !DISubprogram(name: "caller", scope: !2, file: !2, type: !3, spFlags: DISPFlagDefinition, unit: !1)
!6 = distinct !DISubprogram(name: "callee", scope: !2, file: !2, type: !3, spFlags: DISPFlagDefinition, unit: !1)
!7 = distinct !DISubprogram(name: "uninstrumented", scope: !2, file: !2, type: !3, spFlags: DISPFlagDefinition, unit: !1)
!10 = !DILocation(line: 8, scope: !5)
!11 = !DILocation(line: 3, scope: !6, inlinedAt: !10)
!12 = !DILocation(line: 1, scope: !7)
!20 = !{!5, i64 4096}
!21 = !{!6, i64 8192}
!22 = !{!7, i64 12288}
)";

struct Parsed {
	LLVMContext context;
	std::unique_ptr<Module> module;
	ModuleAnalysisManager mam;

	Parsed ()
	{
		SMDiagnostic problem;

		module = parseAssemblyString (dispatches, problem, context);

		if (module == nullptr) {
			std::string complaint;
			raw_string_ostream out (complaint);

			problem.print ("", out);
			ADD_FAILURE () << complaint;
		}
	}

	void verify ()
	{
		std::string complaint;
		raw_string_ostream out (complaint);

		EXPECT_FALSE (verifyModule (*module, &out)) << complaint;
	}

	/// How many calls in \p f name \p callee.
	unsigned calls_to (StringRef f, StringRef callee)
	{
		unsigned found = 0;

		for (Instruction &i : instructions (*module->getFunction (f)))
			if (auto *call = dyn_cast<CallBase> (&i))
				if (Function *named = call->getCalledFunction ())
					found += named->getName () == callee;

		return found;
	}
};

TEST (ReceiverProfileTest, EachDispatchIsKeyedByTheIlThatWroteIt)
{
	Parsed ir;

	receiver_sites ().clear ();
	ReceiverProfilePass ().run (*ir.module, ir.mam);
	ir.verify ();

	ASSERT_EQ (receiver_sites ().size (), 1u);

	const ReceiverSites &sites = receiver_sites ().front ();

	EXPECT_EQ (sites.function, "caller");
	EXPECT_EQ (sites.first, 0u);
	ASSERT_EQ (sites.keys.size (), 2u);
	EXPECT_EQ (sites.keys[0], (ReceiverSiteKey { caller_id, 7 }));
	EXPECT_EQ (sites.keys[1], (ReceiverSiteKey { callee_id, 2 }));

	GlobalVariable *table = ir.module->getGlobalVariable ("mono_receivers", true);

	ASSERT_NE (table, nullptr);
	EXPECT_EQ (table->getSection (), receiver_section);
	EXPECT_EQ (ir.module->getDataLayout ().getTypeAllocSize (table->getValueType ()),
	           2 * sizeof (ReceiverRecord));

	EXPECT_EQ (ir.calls_to ("caller", "mono.profile.receiver"), 2u);
	EXPECT_EQ (ir.calls_to ("uninstrumented", "mono.profile.receiver"), 0u);
}

TEST (ReceiverProfileTest, TheLoweringLeavesOnlyTheHelper)
{
	Parsed ir;

	receiver_sites ().clear ();
	ReceiverProfilePass ().run (*ir.module, ir.mam);
	LowerReceiverProfilePass ().run (*ir.module, ir.mam);
	ir.verify ();

	EXPECT_EQ (ir.module->getFunction ("mono.profile.receiver"), nullptr);
	EXPECT_EQ (ir.calls_to ("caller", "mono_llvm_jit_record_receiver"), 2u);
}

void
record (ReceiverRecord &into, uintptr_t vtable, unsigned times = 1)
{
	for (unsigned i = 0; i < times; i++)
		mono_llvm_jit_record_receiver (&into, reinterpret_cast<void *> (vtable));
}

TEST (ReceiverProfileTest, TheHelperClaimsAnEntryPerClassThenReplacesTheLeastCounted)
{
	ReceiverRecord seen;

	std::memset (&seen, 0, sizeof (seen));

	for (uintptr_t vtable = 1; vtable <= ReceiverRecord::entries; vtable++)
		record (seen, vtable * 16);
	record (seen, 16);
	record (seen, 80);

	EXPECT_EQ (seen.seen[0].vtable.load (), 16u);
	EXPECT_EQ (seen.seen[0].count.load (), 2u);
	EXPECT_EQ (seen.seen[1].vtable.load (), 80u);
	EXPECT_EQ (seen.seen[1].count.load (), 2u);
	EXPECT_EQ (seen.seen[1].inherited.load (), 1u);

	for (unsigned i = 2; i < ReceiverRecord::entries; i++) {
		EXPECT_EQ (seen.seen[i].vtable.load (), (i + 1) * 16u);
		EXPECT_EQ (seen.seen[i].count.load (), 1u);
	}
}

TEST (ReceiverProfileTest, TheHotEntryFollowsTheHighestCount)
{
	ReceiverRecord seen;

	std::memset (&seen, 0, sizeof (seen));
	record (seen, 16);
	record (seen, 32, 2);

	EXPECT_EQ (seen.hot.load (), sizeof (ReceiverRecord::Entry));
}

TEST (ReceiverProfileTest, AClassArrivingAfterTheRecordFillsStillCounts)
{
	ReceiverRecord seen;

	std::memset (&seen, 0, sizeof (seen));

	for (uintptr_t vtable = 1; vtable <= ReceiverRecord::entries; vtable++)
		record (seen, vtable * 16);
	for (unsigned i = 0; i < 100; i++) {
		record (seen, 0x1000);
		if (i % 10 == 0)
			record (seen, 0x2000 + i * 16);
	}

	ReceiverCounts counts;

	counts.add (seen);

	auto late = std::find_if (counts.seen.begin (), counts.seen.end (),
	                          [] (const auto &s) { return s.first == 0x1000; });

	ASSERT_NE (late, counts.seen.end ());
	EXPECT_EQ (late->second, 100u);
	EXPECT_EQ (counts.total (), ReceiverRecord::entries + 110);
}

TEST (ReceiverProfileTest, CountsFromTwoRecordsOfOneSiteAddUp)
{
	ReceiverRecord a, b;

	std::memset (&a, 0, sizeof (a));
	std::memset (&b, 0, sizeof (b));
	a.seen[0].vtable = 16;
	a.seen[0].count = 3;
	b.seen[0].vtable = 32;
	b.seen[0].count = 1;
	b.seen[1].vtable = 16;
	b.seen[1].count = 7;
	b.seen[1].inherited = 5;

	ReceiverCounts counts;

	counts.add (a);
	counts.add (b);

	ASSERT_EQ (counts.seen.size (), 2u);
	EXPECT_EQ (counts.seen[0], std::make_pair (uint64_t (16), uint64_t (5)));
	EXPECT_EQ (counts.seen[1], std::make_pair (uint64_t (32), uint64_t (1)));
	EXPECT_EQ (counts.total (), 11u);
}

TEST (ReceiverProfileTest, AnnotationLandsOnTheSiteMostFrequentFirst)
{
	Parsed ir;

	auto recorded = [] (const ReceiverSiteKey &key) -> std::optional<ReceiverCounts> {
		if (!(key == ReceiverSiteKey { caller_id, 7 }))
			return std::nullopt;

		ReceiverCounts counts;

		counts.seen = { { 16, 10 }, { 32, 90 } };
		counts.other = 4;
		return counts;
	};
	CompileState state;

	state.receivers = recorded;

	{
		CompileScope compiling (state);

		AnnotateReceiversPass ().run (*ir.module, ir.mam);
	}
	ir.verify ();

	CallBase *vtable_site = nullptr, *imt_site = nullptr;

	for (Instruction &i : instructions (*ir.module->getFunction ("caller")))
		if (auto *call = dyn_cast<CallBase> (&i))
			if (Function *named = call->getCalledFunction ()) {
				if (named->getName () == "mono.vtable.func")
					vtable_site = call;
				if (named->getName () == "mono.imt.func")
					imt_site = call;
			}

	ASSERT_NE (vtable_site, nullptr);
	ASSERT_NE (imt_site, nullptr);

	uint64_t total = 0;
	SmallVector<InstrProfValueData, 4> seen =
		getValueProfDataFromInst (*vtable_site, IPVK_VTableTarget, UINT32_MAX, total);

	ASSERT_EQ (seen.size (), 2u);
	EXPECT_EQ (seen[0].Value, 32u);
	EXPECT_EQ (seen[0].Count, 90u);
	EXPECT_EQ (seen[1].Value, 16u);
	EXPECT_EQ (total, 104u);

	EXPECT_EQ (imt_site->getMetadata (LLVMContext::MD_prof), nullptr);
}

} // namespace
} // namespace test
} // namespace mono
