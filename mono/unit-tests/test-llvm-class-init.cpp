/*
 * test-llvm-class-init.cpp: unit tests for the class-init barrier redundancy
 * analysis in mono/mini/llvm/passes/elide-class-init.cpp.
 *
 * find_redundant_class_inits () answers "is this mono_generic_class_init call
 * already guaranteed to have happened?" from two dominating facts: an earlier
 * trigger call for the same class, or an earlier check that found the class
 * initialized. Getting it wrong in the permissive direction drops a cctor, so
 * the cases below lean on the negatives as hard as the positives - different
 * classes, the barrier's own guarded call, a check whose "yes" edge does not
 * actually dominate, and tags whose IR no longer decodes.
 *
 * The IR is assembled here by hand rather than translated from IL: the point is
 * to pin the analysis against shapes the optimizer can produce (an inverted
 * check, a folded address, a missing llvm.expect), not just the one shape the
 * translator emits today.
 */

#include "config.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#ifdef ENABLE_LLVM

#include <mono/metadata/abi-details.h>
#include <mono/metadata/object-internals.h>

/* Has to come after the mono headers - mono-tls.h puts PIC back in scope. */
#ifdef PIC
#undef PIC
#endif

#include "mini/llvm/passes/elide-class-init.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

using llvm::BasicBlock;
using llvm::BranchInst;
using llvm::CallInst;
using llvm::Function;
using llvm::IRBuilder;
using llvm::Module;

using mono::ClassInitFactKind;
using mono::RedundantClassInit;

/* ------------------------------------------------------------ reporting */

static int failures;
static int cases_run;

static void
fail (const char *what, const char *detail)
{
	printf ("FAIL %s: %s\n", what, detail);
	failures ++;
}

/* --------------------------------------------------------- IR assembly */

/*
 * Two fake vtable addresses. Any value works - the analysis only ever compares
 * them - but they are page-aligned and obviously synthetic so a stray one in a
 * failure message is recognizable.
 */
static const std::uint64_t VTABLE_A = 0x1000000;
static const std::uint64_t VTABLE_B = 0x2000000;

static std::uint64_t
inited_addr (std::uint64_t vtable)
{
	return vtable + MONO_STRUCT_OFFSET (MonoVTable, initialized);
}

/* How to spell a barrier. The defaults are what the translator emits. */
struct Shape {
	/* `icmp eq ..., 0` with the successors swapped - what SimplifyCFG leaves
	 * behind when it inverts a branch. Means the same thing. */
	bool inverted = false;
	/* The translator wraps the compare in llvm.expect to weight the branch. */
	bool expect = true;
	/* Load through a getelementptr instead of a folded constant address: the
	 * front-end's in-body barrier, versus the prologue guard. */
	bool gep_address = false;
	bool tag_check = true;
	bool tag_trigger = true;
};

struct Harness {
	llvm::LLVMContext ctx;
	std::unique_ptr<Module> module;
	Function *func = nullptr;
	BasicBlock *entry = nullptr;
	BasicBlock *exit = nullptr;

	explicit Harness (const char *name)
	{
		module = std::make_unique<Module> (name, ctx);
		/* The host layout the engine optimizes against; the analysis reads
		 * pointer sizes out of it. */
		module->setDataLayout ("e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-"
		                       "i128:128-f80:128-n8:16:32:64-S128");

		auto *sig = llvm::FunctionType::get (llvm::Type::getVoidTy (ctx),
		                                     {llvm::Type::getInt32Ty (ctx)}, false);
		func = Function::Create (sig, Function::ExternalLinkage, name, module.get ());
		entry = BasicBlock::Create (ctx, "entry", func);
		exit = BasicBlock::Create (ctx, "exit", func);
		IRBuilder<> (exit).CreateRetVoid ();
	}

	BasicBlock *block (const char *name)
	{
		return BasicBlock::Create (ctx, name, func, exit);
	}

	llvm::FunctionCallee trigger_callee ()
	{
		return module->getOrInsertFunction ("mono_generic_class_init",
		                                    llvm::Type::getVoidTy (ctx),
		                                    llvm::Type::getInt64Ty (ctx));
	}

	void tag (llvm::Instruction *ins, const char *kind, const char *klass)
	{
		ins->setMetadata (kind, llvm::MDNode::get (ctx, llvm::MDString::get (ctx, klass)));
	}
};

/* A `mono_generic_class_init (vtable)` call appended to IN. */
static CallInst *
emit_trigger (Harness &h, BasicBlock *in, std::uint64_t vtable, const char *klass, bool tagged = true)
{
	IRBuilder<> b (in);
	CallInst *call = b.CreateCall (h.trigger_callee (), {b.getInt64 (vtable)});

	if (tagged)
		h.tag (call, "mono.class-init", klass);
	return call;
}

struct Check {
	BranchInst *branch;
	/* The successor taken when the class turned out to be initialized already. */
	BasicBlock *inited;
	BasicBlock *not_inited;
};

/*
 * The test on `vtable->initialized`, appended to IN, branching to two fresh
 * blocks. The caller terminates both.
 */
static Check
emit_check (Harness &h, BasicBlock *in, std::uint64_t vtable, const char *klass,
            const Shape &shape = {})
{
	IRBuilder<> b (in);
	llvm::Type *byte = b.getInt8Ty ();
	llvm::PointerType *ptr = b.getPtrTy ();

	llvm::Constant *addr;
	if (shape.gep_address)
		addr = llvm::ConstantExpr::getGetElementPtr (
			byte, llvm::ConstantExpr::getIntToPtr (b.getInt64 (vtable), ptr),
			b.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, initialized)));
	else
		addr = llvm::ConstantExpr::getIntToPtr (b.getInt64 (inited_addr (vtable)), ptr);

	llvm::Value *loaded = b.CreateLoad (byte, addr);
	llvm::Value *cond = shape.inverted ? b.CreateICmpEQ (loaded, b.getInt8 (0))
	                                   : b.CreateICmpNE (loaded, b.getInt8 (0));

	if (shape.expect) {
		llvm::Function *expect = llvm::Intrinsic::getDeclaration (
			h.module.get (), llvm::Intrinsic::expect, {b.getInt1Ty ()});
		cond = b.CreateCall (expect, {cond, b.getInt1 (!shape.inverted)});
	}

	Check out;
	out.inited = h.block ("inited");
	out.not_inited = h.block ("notinited");
	/* Inverting the compare swaps which successor the "yes" answer takes. */
	out.branch = shape.inverted ? b.CreateCondBr (cond, out.not_inited, out.inited)
	                            : b.CreateCondBr (cond, out.inited, out.not_inited);

	if (shape.tag_check)
		h.tag (out.branch, "mono.class-init-check", klass);
	return out;
}

/*
 * A whole barrier - check, guarded trigger, join - appended to IN. Returns the
 * join block, which is where a caller keeps building.
 */
static BasicBlock *
emit_barrier (Harness &h, BasicBlock *in, std::uint64_t vtable, const char *klass,
              const Shape &shape = {})
{
	Check check = emit_check (h, in, vtable, klass, shape);
	BasicBlock *join = h.block ("join");

	emit_trigger (h, check.not_inited, vtable, klass, shape.tag_trigger);
	IRBuilder<> (check.not_inited).CreateBr (join);
	IRBuilder<> (check.inited).CreateBr (join);
	return join;
}

/* ------------------------------------------------------------- checking */

static std::vector<RedundantClassInit>
analyze (const char *what, Harness &h)
{
	std::string err;
	llvm::raw_string_ostream os (err);

	if (llvm::verifyFunction (*h.func, &os)) {
		fail (what, ("test built invalid IR: " + os.str ()).c_str ());
		return {};
	}

	llvm::DominatorTree dt (*h.func);
	auto found = mono::find_redundant_class_inits (*h.func, dt);
	return std::vector<RedundantClassInit> (found.begin (), found.end ());
}

/* Assert that nothing in H is redundant. */
static void
expect_none (const char *what, Harness &h)
{
	cases_run ++;

	auto found = analyze (what, h);
	if (!found.empty ()) {
		char buf [128];
		snprintf (buf, sizeof (buf), "expected no redundant trigger, got %zu", found.size ());
		fail (what, buf);
	}
}

/* Assert that CALL, and only it, is redundant, for the stated reason. */
static void
expect_one (const char *what, Harness &h, const CallInst *call, ClassInitFactKind kind,
            const llvm::Instruction *fact, std::uint64_t vtable)
{
	cases_run ++;

	auto found = analyze (what, h);
	if (found.size () != 1) {
		char buf [128];
		snprintf (buf, sizeof (buf), "expected exactly 1 redundant trigger, got %zu",
		          found.size ());
		fail (what, buf);
		return;
	}

	const RedundantClassInit &got = found [0];
	if (got.call != call)
		fail (what, "a different trigger call than expected was reported redundant");
	if (got.kind != kind)
		fail (what, kind == ClassInitFactKind::PriorCall
		                ? "expected a prior-call fact, got an initialized-edge one"
		                : "expected an initialized-edge fact, got a prior-call one");
	if (got.fact != fact)
		fail (what, "reported the wrong dominating instruction");
	if (got.vtable != vtable)
		fail (what, "reported the wrong vtable");
}

/* ------------------------------------------------- a dominating trigger call */

static void
cases_prior_call (void)
{
	{
		/* Back-to-back triggers for one class: the second cannot do anything. */
		Harness h ("two-triggers-one-block");
		CallInst *first = emit_trigger (h, h.entry, VTABLE_A, "A");
		CallInst *second = emit_trigger (h, h.entry, VTABLE_A, "A");
		IRBuilder<> (h.entry).CreateBr (h.exit);

		expect_one ("prior-call-same-block", h, second, ClassInitFactKind::PriorCall,
		            first, VTABLE_A);
	}

	{
		/* Same, one block apart. */
		Harness h ("two-triggers-two-blocks");
		CallInst *first = emit_trigger (h, h.entry, VTABLE_A, "A");
		BasicBlock *next = h.block ("next");
		IRBuilder<> (h.entry).CreateBr (next);
		CallInst *second = emit_trigger (h, next, VTABLE_A, "A");
		IRBuilder<> (next).CreateBr (h.exit);

		expect_one ("prior-call-across-blocks", h, second, ClassInitFactKind::PriorCall,
		            first, VTABLE_A);
	}

	{
		/* Two classes, two cctors: neither trigger stands in for the other. */
		Harness h ("two-triggers-two-classes");
		emit_trigger (h, h.entry, VTABLE_A, "A");
		emit_trigger (h, h.entry, VTABLE_B, "B");
		IRBuilder<> (h.entry).CreateBr (h.exit);

		expect_none ("prior-call-different-class", h);
	}

	{
		/* A trigger on one arm of an `if` does not cover the other arm. */
		Harness h ("trigger-on-one-arm");
		IRBuilder<> b (h.entry);
		BasicBlock *left = h.block ("left");
		BasicBlock *right = h.block ("right");
		b.CreateCondBr (b.CreateICmpSGT (h.func->getArg (0), b.getInt32 (0)), left, right);

		emit_trigger (h, left, VTABLE_A, "A");
		IRBuilder<> (left).CreateBr (h.exit);
		emit_trigger (h, right, VTABLE_A, "A");
		IRBuilder<> (right).CreateBr (h.exit);

		expect_none ("prior-call-sibling-arm", h);
	}

	{
		/* An untagged trigger is not a fact - the analysis never sees it. */
		Harness h ("untagged-first-trigger");
		emit_trigger (h, h.entry, VTABLE_A, "A", /* tagged */ false);
		emit_trigger (h, h.entry, VTABLE_A, "A");
		IRBuilder<> (h.entry).CreateBr (h.exit);

		expect_none ("prior-call-untagged-ignored", h);
	}
}

/* ------------------------------------------- a dominating initialized edge */

static void
cases_initialized_edge (void)
{
	{
		/* Down the arm where the byte was already set, a trigger is dead weight. */
		Harness h ("trigger-on-inited-edge");
		Check check = emit_check (h, h.entry, VTABLE_A, "A");
		CallInst *call = emit_trigger (h, check.inited, VTABLE_A, "A");
		IRBuilder<> (check.inited).CreateBr (h.exit);
		IRBuilder<> (check.not_inited).CreateBr (h.exit);

		expect_one ("inited-edge-dominates", h, call, ClassInitFactKind::InitializedEdge,
		            check.branch, VTABLE_A);
	}

	{
		/* The other arm is exactly where the trigger belongs. */
		Harness h ("trigger-on-notinited-edge");
		Check check = emit_check (h, h.entry, VTABLE_A, "A");
		emit_trigger (h, check.not_inited, VTABLE_A, "A");
		IRBuilder<> (check.not_inited).CreateBr (h.exit);
		IRBuilder<> (check.inited).CreateBr (h.exit);

		expect_none ("notinited-edge-is-not-a-fact", h);
	}

	{
		/* A's check says nothing about B. */
		Harness h ("inited-edge-other-class");
		Check check = emit_check (h, h.entry, VTABLE_A, "A");
		emit_trigger (h, check.inited, VTABLE_B, "B");
		IRBuilder<> (check.inited).CreateBr (h.exit);
		IRBuilder<> (check.not_inited).CreateBr (h.exit);

		expect_none ("inited-edge-different-class", h);
	}

	{
		/* A whole barrier on its own is exactly what it should be. */
		Harness h ("lone-barrier");
		BasicBlock *join = emit_barrier (h, h.entry, VTABLE_A, "A");
		IRBuilder<> (join).CreateBr (h.exit);

		expect_none ("lone-barrier-is-clean", h);
	}

	{
		/*
		 * Two complete barriers for one class in sequence. Neither single fact
		 * from the first reaches the second - the join merges the arm that
		 * checked with the arm that called - so this analysis leaves it alone
		 * even though the second barrier is, on every path, unnecessary.
		 * Catching it needs the two facts met at the join, not dominance.
		 */
		Harness h ("barrier-after-barrier");
		BasicBlock *join = emit_barrier (h, h.entry, VTABLE_A, "A");
		BasicBlock *second = emit_barrier (h, join, VTABLE_A, "A");
		IRBuilder<> (second).CreateBr (h.exit);

		expect_none ("join-of-a-barrier-is-not-a-single-dominator", h);
	}

	{
		/* A trigger inside the cold arm of a barrier for the same class is,
		 * though - the guarded call dominates it. */
		Harness h ("trigger-inside-cold-arm");
		Check check = emit_check (h, h.entry, VTABLE_A, "A");
		CallInst *guarded = emit_trigger (h, check.not_inited, VTABLE_A, "A");
		CallInst *extra = emit_trigger (h, check.not_inited, VTABLE_A, "A");
		IRBuilder<> (check.not_inited).CreateBr (h.exit);
		IRBuilder<> (check.inited).CreateBr (h.exit);

		expect_one ("cold-arm-second-trigger", h, extra, ClassInitFactKind::PriorCall,
		            guarded, VTABLE_A);
	}
}

/* --------------------------------------------- shapes the optimizer produces */

static void
cases_shapes (void)
{
	{
		/*
		 * An inverted check: `icmp eq ..., 0` with the successors swapped. The
		 * metadata is identical, so reading the polarity off the successor order
		 * would pick the wrong edge here and license eliding a live trigger.
		 */
		Shape shape;
		shape.inverted = true;

		Harness h ("inverted-check");
		Check check = emit_check (h, h.entry, VTABLE_A, "A", shape);
		CallInst *call = emit_trigger (h, check.inited, VTABLE_A, "A");
		IRBuilder<> (check.inited).CreateBr (h.exit);
		IRBuilder<> (check.not_inited).CreateBr (h.exit);

		expect_one ("inverted-check-yes-edge", h, call, ClassInitFactKind::InitializedEdge,
		            check.branch, VTABLE_A);
	}

	{
		/* And the trigger on an inverted check's cold arm still is not redundant. */
		Shape shape;
		shape.inverted = true;

		Harness h ("inverted-check-cold-arm");
		Check check = emit_check (h, h.entry, VTABLE_A, "A", shape);
		emit_trigger (h, check.not_inited, VTABLE_A, "A");
		IRBuilder<> (check.not_inited).CreateBr (h.exit);
		IRBuilder<> (check.inited).CreateBr (h.exit);

		expect_none ("inverted-check-no-edge", h);
	}

	{
		/*
		 * The front-end's barrier loads through a getelementptr while the
		 * prologue guard folds the offset into the constant. Both name the same
		 * byte, so a gep-form check covers a trigger for the same class.
		 */
		Shape shape;
		shape.gep_address = true;

		Harness h ("gep-address-check");
		Check check = emit_check (h, h.entry, VTABLE_A, "A", shape);
		CallInst *call = emit_trigger (h, check.inited, VTABLE_A, "A");
		IRBuilder<> (check.inited).CreateBr (h.exit);
		IRBuilder<> (check.not_inited).CreateBr (h.exit);

		expect_one ("gep-form-address", h, call, ClassInitFactKind::InitializedEdge,
		            check.branch, VTABLE_A);
	}

	{
		/* llvm.expect is a hint, not part of the test. */
		Shape shape;
		shape.expect = false;

		Harness h ("check-without-expect");
		Check check = emit_check (h, h.entry, VTABLE_A, "A", shape);
		CallInst *call = emit_trigger (h, check.inited, VTABLE_A, "A");
		IRBuilder<> (check.inited).CreateBr (h.exit);
		IRBuilder<> (check.not_inited).CreateBr (h.exit);

		expect_one ("no-expect-wrapper", h, call, ClassInitFactKind::InitializedEdge,
		            check.branch, VTABLE_A);
	}

	{
		/* An untagged check is invisible, the same way an untagged trigger is. */
		Shape shape;
		shape.tag_check = false;

		Harness h ("untagged-check");
		Check check = emit_check (h, h.entry, VTABLE_A, "A", shape);
		emit_trigger (h, check.inited, VTABLE_A, "A");
		IRBuilder<> (check.inited).CreateBr (h.exit);
		IRBuilder<> (check.not_inited).CreateBr (h.exit);

		expect_none ("untagged-check-ignored", h);
	}

	{
		/*
		 * A tag that outlived its IR. Passes hoist and merge branches without
		 * touching metadata, so a `mono.class-init-check` can end up on a branch
		 * that tests something else entirely; the condition, not the tag, has to
		 * be what decides.
		 */
		Harness h ("stale-check-tag");
		IRBuilder<> b (h.entry);
		BasicBlock *left = h.block ("left");
		BasicBlock *right = h.block ("right");
		BranchInst *br = b.CreateCondBr (b.CreateICmpSGT (h.func->getArg (0), b.getInt32 (0)),
		                                 left, right);
		h.tag (br, "mono.class-init-check", "A");

		emit_trigger (h, left, VTABLE_A, "A");
		IRBuilder<> (left).CreateBr (h.exit);
		IRBuilder<> (right).CreateBr (h.exit);

		expect_none ("stale-check-tag-does-not-decode", h);
	}

	{
		/*
		 * A trigger whose vtable is not a constant - what shared generic code
		 * produces, where the vtable comes out of the rgctx. There is no class to
		 * compare, so it is neither a fact nor a candidate.
		 */
		Harness h ("non-constant-trigger");
		CallInst *first = emit_trigger (h, h.entry, VTABLE_A, "A");
		IRBuilder<> b (h.entry);
		CallInst *dynamic = b.CreateCall (h.trigger_callee (),
		                                  {b.CreateZExt (h.func->getArg (0), b.getInt64Ty ())});
		h.tag (dynamic, "mono.class-init", "A");
		b.CreateBr (h.exit);
		(void) first;

		expect_none ("non-constant-vtable-ignored", h);
	}
}

/* ------------------------------------------------ the pass-manager wrapper */

static void
cases_analysis_manager (void)
{
	cases_run ++;

	Harness h ("through-the-analysis-manager");
	CallInst *first = emit_trigger (h, h.entry, VTABLE_A, "A");
	CallInst *second = emit_trigger (h, h.entry, VTABLE_A, "A");
	IRBuilder<> (h.entry).CreateBr (h.exit);

	llvm::PassBuilder pb;
	llvm::FunctionAnalysisManager fam;
	pb.registerFunctionAnalyses (fam);
	fam.registerPass ([] { return mono::ClassInitElisionAnalysis (); });

	auto &result = fam.getResult<mono::ClassInitElisionAnalysis> (*h.func);

	if (result.redundant ().size () != 1) {
		fail ("analysis-manager-result", "expected exactly 1 redundant trigger");
		return;
	}

	const RedundantClassInit *found = result.lookup (second);
	if (!found)
		fail ("analysis-manager-lookup", "lookup () did not find the redundant call");
	else if (found->fact != first)
		fail ("analysis-manager-lookup", "lookup () reported the wrong dominating fact");

	if (result.lookup (first))
		fail ("analysis-manager-lookup", "the dominating call was itself reported redundant");
}

/* ------------------------------------------------------------ entry point */

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_class_init_main (void);

int
test_llvm_class_init_main (void)
{
	failures = 0;
	cases_run = 0;

	setvbuf (stdout, nullptr, _IOLBF, 0);

	cases_prior_call ();
	cases_initialized_edge ();
	cases_shapes ();
	cases_analysis_manager ();

	printf ("%d cases run, %d failed\n", cases_run, failures);
	return failures ? 1 : 0;
}

#else /* !ENABLE_LLVM */

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_class_init_main (void);

int
test_llvm_class_init_main (void)
{
	return 0;
}

#endif /* ENABLE_LLVM */
