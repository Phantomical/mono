/*
 * test-llvm-il-debug.cpp: unit tests for the tier-1 IL-offset debug info.
 *
 * The translator records a method's native_offset -> il_offset map as debug
 * info: each OP_IL_SEQ_POINT sets a DILocation whose line is the IL offset plus
 * IL_OFFSET_LINE_BIAS (il-line-table.cpp), and the engine reads it back out of
 * the emitted object into CompileResult::il_lines / ::il_inline_frames
 * (parse_il_debug_info (), engine.cpp).
 *
 * Nothing about execution goes wrong when that map is wrong - it only makes
 * stack traces lie - so it needs checking directly rather than via a corpus.
 * These build modules whose debug info has the shape the translator produces,
 * JIT them through the real engine, and assert on what comes back.
 *
 * Two shapes are covered because two different things can break:
 *
 *  - HAND-BUILT inlinedAt chains pin the readback contract exactly. The scope
 *    and inlinedAt of every location are written out here, so the expected
 *    answer is not a guess about what the optimizer did.
 *  - one ALWAYSINLINE case runs the real pipeline, so the chain comes from
 *    LLVM's own inliner rather than from this file. That is what would catch
 *    the emission side silently stopping to produce inline records at all.
 */

#include "config.h"

#include <stdio.h>

#ifdef ENABLE_LLVM

#include <cstdint>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
/*
 * libtool compiles this with -DPIC, and PassBuilder's constructor names one of
 * its parameters PIC. Nothing in this file wants the macro.
 */
#undef PIC
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>

#include "mini/llvm/engine.hpp"
#include "llvm/il-line-table.hpp"

using namespace llvm;
using mono::IL_OFFSET_LINE_BIAS;
using mono::MonoLLVMJIT;

/* ------------------------------------------------------------ reporting */

static int passes, failures;

typedef enum { TEST_PASS, TEST_FAIL } TestResult;

static void
report (const char *name, TestResult r)
{
	if (r == TEST_PASS) {
		passes ++;
		printf ("ok   %s\n", name);
	} else {
		failures ++;
		printf ("FAIL %s\n", name);
	}
}

#define CHECK(cond)                                                             \
	do {                                                                    \
		if (!(cond)) {                                                  \
			printf ("     check failed: %s (at %s:%d)\n",           \
			        #cond, __FILE__, __LINE__);                     \
			return TEST_FAIL;                                       \
		}                                                               \
	} while (0)

/* ------------------------------------------------------------ fixtures */

/*
 * A module with one compile unit, shaped the way IlDebugModule shapes it: DWARF
 * 4, FullDebug (inline records live in `.debug_info`, not the line table).
 */
struct DebugFixture {
	orc::ThreadSafeContext tsctx;
	std::unique_ptr<Module> module;
	std::unique_ptr<DIBuilder> di;
	DIFile *file = nullptr;
	DICompileUnit *cu = nullptr;

	explicit DebugFixture (const char *name)
		: tsctx (std::make_unique<LLVMContext> ())
	{
		module = std::make_unique<Module> (name, ctx ());
		module->addModuleFlag (Module::Warning, "Dwarf Version", 4);
		module->addModuleFlag (Module::Warning, "Debug Info Version",
		                       DEBUG_METADATA_VERSION);
		di = std::make_unique<DIBuilder> (*module);
		file = di->createFile ("mono-tier1", ".");
		cu = di->createCompileUnit (dwarf::DW_LANG_C99, file, "mono tier-1",
		                            /*isOptimized=*/ true, "", 0, StringRef (),
		                            DICompileUnit::FullDebug);
	}

	DISubprogram *subprogram (const char *name)
	{
		DISubroutineType *type = di->createSubroutineType (di->getOrCreateTypeArray ({}));
		return di->createFunction (cu, name, name, file, 1, type, 1, DINode::FlagZero,
		                           DISubprogram::SPFlagDefinition | DISubprogram::SPFlagOptimized);
	}

	LLVMContext &ctx ()
	{
		LLVMContext *raw = nullptr;

		tsctx.withContextDo ([&] (LLVMContext *c) { raw = c; });
		return *raw;
	}
};

/* i64 NAME(i64) with a body long enough that its locations land at distinct PCs. */
static Function *
make_fn (Module *m, const char *name, Function::LinkageTypes linkage = Function::ExternalLinkage)
{
	LLVMContext &ctx = m->getContext ();
	Type *i64 = Type::getInt64Ty (ctx);
	FunctionType *fty = FunctionType::get (i64, {i64}, false);
	return Function::Create (fty, linkage, name, m);
}

/* Find the row covering NATIVE_OFFSET, or nullptr. */
static const mono::MonoIlLineRow *
line_at (const std::vector<mono::MonoIlLineRow> &rows, uint32_t native_offset)
{
	const mono::MonoIlLineRow *found = nullptr;
	for (const mono::MonoIlLineRow &r : rows) {
		if (r.native_offset <= native_offset)
			found = &r;
		else
			break;
	}
	return found;
}

/* ---------------------------------------------------- no inlining at all */

/*
 * A plain function: every IL offset the translator set must come back, ascending
 * by native offset, and nothing may claim to be inlined.
 */
static TestResult
test_lines_without_inlining (MonoLLVMJIT *jit)
{
	DebugFixture f ("selftest.il.lines");
	DISubprogram *sp = f.subprogram ("Root:Run (int)");

	Function *fn = make_fn (f.module.get (), "il_lines_root");
	fn->setSubprogram (sp);
	BasicBlock *bb = BasicBlock::Create (f.ctx (), "entry", fn);
	IRBuilder<> b (bb);

	Type *i64 = Type::getInt64Ty (f.ctx ());
	/*
	 * Volatile stores, so each IL offset keeps a machine instruction of its own:
	 * plain arithmetic here would be folded to a single instruction and the test
	 * would be measuring the optimizer rather than the readback.
	 */
	Value *slot = b.CreateAlloca (i64);
	Value *acc = &*fn->arg_begin ();
	const uint32_t il_offsets [] = { 0, 7, 0x1a, 0x2f };
	for (uint32_t il : il_offsets) {
		b.SetCurrentDebugLocation (
			DebugLoc (DILocation::get (f.ctx (), il + IL_OFFSET_LINE_BIAS, 1, sp)));
		b.CreateStore (ConstantInt::get (i64, il + 3), slot, /*isVolatile=*/ true);
	}
	b.CreateRet (acc);
	f.di->finalize ();

	mono::CompileResult res = jit->compile (fn, {}, nullptr, "", f.tsctx);
	CHECK (res.entry != 0);
	CHECK (res.il_inline_frames.empty ());
	CHECK (!res.il_lines.empty ());

	/* Ascending, and single-valued per native offset. */
	for (size_t i = 1; i < res.il_lines.size (); ++i)
		CHECK (res.il_lines [i - 1].native_offset < res.il_lines [i].native_offset);

	/* Every IL offset set above must appear; none may be invented. */
	for (uint32_t il : il_offsets) {
		bool seen = false;
		for (const mono::MonoIlLineRow &r : res.il_lines)
			if (r.il_offset == il)
				seen = true;
		CHECK (seen);
	}
	for (const mono::MonoIlLineRow &r : res.il_lines) {
		bool known = false;
		for (uint32_t il : il_offsets)
			if (r.il_offset == il)
				known = true;
		CHECK (known);
	}
	return TEST_PASS;
}

/* ------------------------------------------------- collapsed marker runs */

/*
 * The property the stackmap-marker scheme could not provide: several IL offsets
 * at ONE native offset must resolve to the LAST one in code order, not to an
 * arbitrary member of the run. Nothing is emitted between these locations, so
 * they all land on the same PC.
 */
static TestResult
test_collapsed_run_takes_last (MonoLLVMJIT *jit)
{
	DebugFixture f ("selftest.il.collapsed");
	DISubprogram *sp = f.subprogram ("Root:Collapse (int)");

	Function *fn = make_fn (f.module.get (), "il_collapsed_root");
	fn->setSubprogram (sp);
	BasicBlock *bb = BasicBlock::Create (f.ctx (), "entry", fn);
	IRBuilder<> b (bb);

	/* A run of locations with no code between them, then the instruction. */
	const uint32_t run [] = { 0x10, 0x11, 0x12, 0x13 };
	for (uint32_t il : run)
		b.SetCurrentDebugLocation (
			DebugLoc (DILocation::get (f.ctx (), il + IL_OFFSET_LINE_BIAS, 1, sp)));
	Value *v = b.CreateAdd (&*fn->arg_begin (),
	                        ConstantInt::get (Type::getInt64Ty (f.ctx ()), 1));
	b.CreateRet (v);
	f.di->finalize ();

	mono::CompileResult res = jit->compile (fn, {}, nullptr, "", f.tsctx);
	CHECK (res.entry != 0);
	CHECK (!res.il_lines.empty ());

	/* No row may report one of the superseded offsets in the run. */
	for (const mono::MonoIlLineRow &r : res.il_lines) {
		CHECK (r.il_offset != 0x10);
		CHECK (r.il_offset != 0x11);
		CHECK (r.il_offset != 0x12);
	}
	/* And the survivor must be the last one. */
	bool saw_last = false;
	for (const mono::MonoIlLineRow &r : res.il_lines)
		if (r.il_offset == 0x13)
			saw_last = true;
	CHECK (saw_last);
	return TEST_PASS;
}

/* ------------------------------------------------ hand-built inline chain */

/*
 * One level of inlining, written out by hand: the instruction's scope is the
 * CALLEE's subprogram and its inlinedAt is a location in the ROOT.
 *
 * The split is the whole contract. il_lines must report the ROOT's offset -
 * a stack frame for the root has to name the root's own call site - while the
 * callee's offset and identity come back as an inline frame.
 */
static TestResult
test_inline_chain_one_level (MonoLLVMJIT *jit)
{
	DebugFixture f ("selftest.il.inline1");
	DISubprogram *root_sp = f.subprogram ("Root:Caller (int)");
	DISubprogram *callee_sp = f.subprogram ("Leaf:Callee (int)");

	Function *fn = make_fn (f.module.get (), "il_inline1_root");
	fn->setSubprogram (root_sp);
	BasicBlock *bb = BasicBlock::Create (f.ctx (), "entry", fn);
	IRBuilder<> b (bb);
	Type *i64 = Type::getInt64Ty (f.ctx ());

	/* Plain root code first, at root IL 4. */
	b.SetCurrentDebugLocation (DebugLoc (DILocation::get (f.ctx (), 4 + IL_OFFSET_LINE_BIAS, 1, root_sp)));
	Value *v = b.CreateAdd (&*fn->arg_begin (), ConstantInt::get (i64, 5));

	/* Then a body inlined from Leaf:Callee at root IL 9, callee IL 0x21. */
	DILocation *call_site = DILocation::get (f.ctx (), 9 + IL_OFFSET_LINE_BIAS, 1, root_sp);
	DILocation *inlined = DILocation::get (f.ctx (), 0x21 + IL_OFFSET_LINE_BIAS, 1,
	                                       callee_sp, call_site);
	b.SetCurrentDebugLocation (DebugLoc (inlined));
	v = b.CreateMul (v, ConstantInt::get (i64, 7));

	b.SetCurrentDebugLocation (DebugLoc (DILocation::get (f.ctx (), 0x30 + IL_OFFSET_LINE_BIAS, 1, root_sp)));
	b.CreateRet (v);
	f.di->finalize ();

	mono::CompileResult res = jit->compile (fn, {}, nullptr, "", f.tsctx);
	CHECK (res.entry != 0);
	CHECK (!res.il_lines.empty ());
	CHECK (res.il_inline_frames.size () == 1);

	const mono::MonoIlInlineRow &inl = res.il_inline_frames [0];
	CHECK (inl.depth == 0);
	CHECK (inl.il_offset == 0x21);
	CHECK (inl.method == "Leaf:Callee (int)");

	/* At that very offset the method's own map must say the ROOT's call site. */
	const mono::MonoIlLineRow *line = line_at (res.il_lines, inl.native_offset);
	CHECK (line != nullptr);
	CHECK (line->il_offset == 9);

	/* The callee's IL offset must never appear as one of the root's own. */
	for (const mono::MonoIlLineRow &r : res.il_lines)
		CHECK (r.il_offset != 0x21);
	return TEST_PASS;
}

/*
 * Two levels: Root inlined Mid, which had inlined Leaf. The chain must come back
 * leaf-first, with the root's own offset kept out of it.
 */
static TestResult
test_inline_chain_two_levels (MonoLLVMJIT *jit)
{
	DebugFixture f ("selftest.il.inline2");
	DISubprogram *root_sp = f.subprogram ("Root:Outer (int)");
	DISubprogram *mid_sp = f.subprogram ("Mid:Middle (int)");
	DISubprogram *leaf_sp = f.subprogram ("Leaf:Inner (int)");

	Function *fn = make_fn (f.module.get (), "il_inline2_root");
	fn->setSubprogram (root_sp);
	BasicBlock *bb = BasicBlock::Create (f.ctx (), "entry", fn);
	IRBuilder<> b (bb);
	Type *i64 = Type::getInt64Ty (f.ctx ());

	DILocation *root_call = DILocation::get (f.ctx (), 0x12 + IL_OFFSET_LINE_BIAS, 1, root_sp);
	DILocation *mid_call = DILocation::get (f.ctx (), 0x34 + IL_OFFSET_LINE_BIAS, 1, mid_sp, root_call);
	DILocation *leaf_loc = DILocation::get (f.ctx (), 0x56 + IL_OFFSET_LINE_BIAS, 1, leaf_sp, mid_call);

	b.SetCurrentDebugLocation (DebugLoc (leaf_loc));
	Value *v = b.CreateAdd (&*fn->arg_begin (), ConstantInt::get (i64, 3));
	b.SetCurrentDebugLocation (DebugLoc (DILocation::get (f.ctx (), 0x40 + IL_OFFSET_LINE_BIAS, 1, root_sp)));
	b.CreateRet (v);
	f.di->finalize ();

	mono::CompileResult res = jit->compile (fn, {}, nullptr, "", f.tsctx);
	CHECK (res.entry != 0);
	CHECK (res.il_inline_frames.size () == 2);

	/* Leaf-first: depth 0 is the innermost body. */
	CHECK (res.il_inline_frames [0].depth == 0);
	CHECK (res.il_inline_frames [0].il_offset == 0x56);
	CHECK (res.il_inline_frames [0].method == "Leaf:Inner (int)");

	CHECK (res.il_inline_frames [1].depth == 1);
	CHECK (res.il_inline_frames [1].il_offset == 0x34);
	CHECK (res.il_inline_frames [1].method == "Mid:Middle (int)");

	/* Both frames describe the same address. */
	CHECK (res.il_inline_frames [0].native_offset == res.il_inline_frames [1].native_offset);

	const mono::MonoIlLineRow *line = line_at (res.il_lines, res.il_inline_frames [0].native_offset);
	CHECK (line != nullptr);
	CHECK (line->il_offset == 0x12);
	return TEST_PASS;
}

/* ------------------------------------------------- the real inliner path */

/*
 * The one case where LLVM builds the chain rather than this file: a real callee
 * with its own subprogram, marked alwaysinline, folded into the root by the
 * pipeline. This is what breaks if the emission side stops giving materialized
 * callees subprograms - the code would still run, and every other test here
 * would still pass, because they write their chains by hand.
 */
static TestResult
test_inline_chain_from_real_inlining (MonoLLVMJIT *jit)
{
	DebugFixture f ("selftest.il.realinline");
	DISubprogram *root_sp = f.subprogram ("Root:RealCaller (int)");
	DISubprogram *callee_sp = f.subprogram ("Leaf:RealCallee (int)");
	Type *i64 = Type::getInt64Ty (f.ctx ());

	Function *callee = make_fn (f.module.get (), "il_real_callee", Function::InternalLinkage);
	callee->setSubprogram (callee_sp);
	callee->addFnAttr (Attribute::AlwaysInline);
	{
		BasicBlock *cbb = BasicBlock::Create (f.ctx (), "entry", callee);
		IRBuilder<> cb (cbb);
		cb.SetCurrentDebugLocation (
			DebugLoc (DILocation::get (f.ctx (), 0x66 + IL_OFFSET_LINE_BIAS, 1, callee_sp)));
		cb.CreateRet (cb.CreateMul (&*callee->arg_begin (), ConstantInt::get (i64, 9)));
	}

	Function *fn = make_fn (f.module.get (), "il_real_root");
	fn->setSubprogram (root_sp);
	{
		BasicBlock *bb = BasicBlock::Create (f.ctx (), "entry", fn);
		IRBuilder<> b (bb);
		b.SetCurrentDebugLocation (
			DebugLoc (DILocation::get (f.ctx (), 0x14 + IL_OFFSET_LINE_BIAS, 1, root_sp)));
		Value *v = b.CreateCall (callee, {&*fn->arg_begin ()});
		b.SetCurrentDebugLocation (
			DebugLoc (DILocation::get (f.ctx (), 0x20 + IL_OFFSET_LINE_BIAS, 1, root_sp)));
		b.CreateRet (v);
	}
	f.di->finalize ();
	CHECK (!verifyModule (*f.module, &errs ()));

	/*
	 * Inline it here rather than hoping the engine's pipeline does: whether the
	 * tier-1 pipeline inlines an alwaysinline callee is its business, and this
	 * test is about what LLVM's inliner records when it fires.
	 */
	{
		PassBuilder pb;
		LoopAnalysisManager lam;
		FunctionAnalysisManager fam;
		CGSCCAnalysisManager cgam;
		ModuleAnalysisManager mam;
		pb.registerModuleAnalyses (mam);
		pb.registerCGSCCAnalyses (cgam);
		pb.registerFunctionAnalyses (fam);
		pb.registerLoopAnalyses (lam);
		pb.crossRegisterProxies (lam, fam, cgam, mam);
		ModulePassManager mpm;
		mpm.addPass (AlwaysInlinerPass ());
		mpm.run (*f.module, mam);
	}

	/*
	 * PRECONDITION, not a result: if the call survived, the rest of this test
	 * would pass vacuously by finding no inline frames and expecting none.
	 * (Calling the compiled code proves nothing here - the callee is in the same
	 * module, so a surviving call returns the same answer.)
	 */
	bool call_remains = false;
	for (BasicBlock &blk : *fn)
		for (Instruction &i : blk)
			if (isa<CallInst> (&i))
				call_remains = true;
	CHECK (!call_remains);

	mono::CompileResult res = jit->compile (fn, {}, nullptr, "", f.tsctx);
	CHECK (res.entry != 0);
	auto compiled = reinterpret_cast<int64_t (*) (int64_t)> (res.entry);
	CHECK (compiled (2) == 18);

	CHECK (!res.il_inline_frames.empty ());
	bool saw_callee = false;
	for (const mono::MonoIlInlineRow &r : res.il_inline_frames) {
		if (r.method == "Leaf:RealCallee (int)") {
			saw_callee = true;
			CHECK (r.depth == 0);
			CHECK (r.il_offset == 0x66);
			/* The root's own map still names the root's call site there. */
			const mono::MonoIlLineRow *line = line_at (res.il_lines, r.native_offset);
			CHECK (line != nullptr);
			CHECK (line->il_offset == 0x14);
		}
	}
	CHECK (saw_callee);

	/* The callee's IL offset must not have leaked into the root's own map. */
	for (const mono::MonoIlLineRow &r : res.il_lines)
		CHECK (r.il_offset != 0x66);
	return TEST_PASS;
}

/* --------------------------------------------------------- no debug info */

/*
 * A function with no debug info must produce no rows. The readback has to be
 * able to say "nothing here" - inventing a row would attribute native code to
 * an IL offset that was never recorded.
 */
static TestResult
test_no_debug_info_yields_nothing (MonoLLVMJIT *jit)
{
	auto owned = std::make_unique<LLVMContext> ();
	LLVMContext &ctx = *owned;
	orc::ThreadSafeContext tsctx (std::move (owned));
	auto module = std::make_unique<Module> ("selftest.il.nodebug", ctx);
	Function *fn = make_fn (module.get (), "il_nodebug_root");
	BasicBlock *bb = BasicBlock::Create (ctx, "entry", fn);
	IRBuilder<> b (bb);
	b.CreateRet (b.CreateAdd (&*fn->arg_begin (),
	                          ConstantInt::get (Type::getInt64Ty (ctx), 1)));

	mono::CompileResult res = jit->compile (fn, {}, nullptr, "", tsctx);
	CHECK (res.entry != 0);
	CHECK (res.il_lines.empty ());
	CHECK (res.il_inline_frames.empty ());
	return TEST_PASS;
}

/* ------------------------------------------------------------------ main */

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_il_debug_main (void);

int
test_llvm_il_debug_main (void)
{
	MonoLLVMJIT *jit = MonoLLVMJIT::get_singleton ();

	passes = failures = 0;

	report ("lines-without-inlining", test_lines_without_inlining (jit));
	report ("collapsed-run-takes-last", test_collapsed_run_takes_last (jit));
	report ("inline-chain-one-level", test_inline_chain_one_level (jit));
	report ("inline-chain-two-levels", test_inline_chain_two_levels (jit));
	report ("inline-chain-from-real-inlining", test_inline_chain_from_real_inlining (jit));
	report ("no-debug-info-yields-nothing", test_no_debug_info_yields_nothing (jit));

	printf ("%d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}

#endif /* ENABLE_LLVM */
