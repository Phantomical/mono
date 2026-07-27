/*
 * test-llvm-class-init.cpp: unit tests for the class-init barrier redundancy
 * analysis in mono/mini/llvm/passes/elide-class-init.cpp.
 *
 * find_redundant_class_inits () answers "has every path here already asked the
 * runtime to initialize this class?" from two facts: a trigger call for the same
 * class, and a check that found the class initialized. Either is enough on its
 * own, and they meet at merges - which is the whole point, since a completed
 * barrier establishes the fact on both of its arms.
 *
 * Getting it wrong in the permissive direction drops a cctor, so the cases below
 * lean on the negatives as hard as the positives: different classes, one-armed
 * coverage, the unwind side of a throwing trigger, and tags whose IR no longer
 * decodes.
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
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
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
using llvm::CallBase;
using llvm::CallInst;
using llvm::Function;
using llvm::IRBuilder;
using llvm::Module;

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

	/* An `if (arg > 0)` fork, for building merges that carry no class-init fact. */
	std::pair<BasicBlock *, BasicBlock *> fork (BasicBlock *in)
	{
		IRBuilder<> b (in);
		BasicBlock *left = block ("left");
		BasicBlock *right = block ("right");

		b.CreateCondBr (b.CreateICmpSGT (func->getArg (0), b.getInt32 (0)), left, right);
		return {left, right};
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

struct Barrier {
	Check check;
	CallInst *trigger;
	/* Where both arms land, and where the caller keeps building. */
	BasicBlock *join;
};

/* A whole barrier - check, guarded trigger, join - appended to IN. */
static Barrier
emit_barrier (Harness &h, BasicBlock *in, std::uint64_t vtable, const char *klass,
              const Shape &shape = {})
{
	Barrier out;

	out.check = emit_check (h, in, vtable, klass, shape);
	out.join = h.block ("join");
	out.trigger = emit_trigger (h, out.check.not_inited, vtable, klass, shape.tag_trigger);

	IRBuilder<> (out.check.not_inited).CreateBr (out.join);
	IRBuilder<> (out.check.inited).CreateBr (out.join);
	return out;
}

/* ------------------------------------------------------------- checking */

/*
 * Run the analysis and assert that exactly the listed calls came back, in the
 * listed order (the analysis reports in program order).
 */
static void
expect_redundant (const char *what, Harness &h, const std::vector<const CallBase *> &want)
{
	cases_run ++;

	std::string err;
	llvm::raw_string_ostream os (err);
	if (llvm::verifyFunction (*h.func, &os)) {
		fail (what, ("test built invalid IR: " + os.str ()).c_str ());
		return;
	}

	auto got = mono::find_redundant_class_inits (*h.func);

	if (got.size () != want.size ()) {
		char buf [128];
		snprintf (buf, sizeof (buf), "expected %zu redundant trigger(s), got %zu",
		          want.size (), got.size ());
		fail (what, buf);
		return;
	}

	for (std::size_t i = 0; i < want.size (); i ++) {
		if (got [i].call != want [i]) {
			char buf [128];
			snprintf (buf, sizeof (buf), "redundant trigger %zu is not the expected call", i);
			fail (what, buf);
		}
	}
}

static void
expect_none (const char *what, Harness &h)
{
	expect_redundant (what, h, {});
}

/* ------------------------------------------------- a preceding trigger call */

static void
cases_prior_call (void)
{
	{
		/* Back-to-back triggers for one class: the second cannot do anything. */
		Harness h ("two-triggers-one-block");
		emit_trigger (h, h.entry, VTABLE_A, "A");
		CallInst *second = emit_trigger (h, h.entry, VTABLE_A, "A");
		IRBuilder<> (h.entry).CreateBr (h.exit);

		expect_redundant ("prior-call-same-block", h, {second});
	}

	{
		/* Same, one block apart. */
		Harness h ("two-triggers-two-blocks");
		emit_trigger (h, h.entry, VTABLE_A, "A");
		BasicBlock *next = h.block ("next");
		IRBuilder<> (h.entry).CreateBr (next);
		CallInst *second = emit_trigger (h, next, VTABLE_A, "A");
		IRBuilder<> (next).CreateBr (h.exit);

		expect_redundant ("prior-call-across-blocks", h, {second});
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
		auto [left, right] = h.fork (h.entry);

		emit_trigger (h, left, VTABLE_A, "A");
		IRBuilder<> (left).CreateBr (h.exit);
		emit_trigger (h, right, VTABLE_A, "A");
		IRBuilder<> (right).CreateBr (h.exit);

		expect_none ("prior-call-sibling-arm", h);
	}

	{
		/* Nor does it cover what follows the merge - the other arm has no fact. */
		Harness h ("trigger-on-one-arm-then-join");
		auto [left, right] = h.fork (h.entry);
		BasicBlock *join = h.block ("join");

		emit_trigger (h, left, VTABLE_A, "A");
		IRBuilder<> (left).CreateBr (join);
		IRBuilder<> (right).CreateBr (join);
		emit_trigger (h, join, VTABLE_A, "A");
		IRBuilder<> (join).CreateBr (h.exit);

		expect_none ("one-armed-merge", h);
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

/* ------------------------------------------- a preceding initialized check */

static void
cases_initialized_check (void)
{
	{
		/* Down the arm where the byte was already set, a trigger is dead weight. */
		Harness h ("trigger-on-inited-edge");
		Check check = emit_check (h, h.entry, VTABLE_A, "A");
		CallInst *call = emit_trigger (h, check.inited, VTABLE_A, "A");
		IRBuilder<> (check.inited).CreateBr (h.exit);
		IRBuilder<> (check.not_inited).CreateBr (h.exit);

		expect_redundant ("inited-edge-covers", h, {call});
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
		Barrier first = emit_barrier (h, h.entry, VTABLE_A, "A");
		IRBuilder<> (first.join).CreateBr (h.exit);

		expect_none ("lone-barrier-is-clean", h);
	}

	{
		/* A trigger inside the cold arm, after the guarded one, is covered. */
		Harness h ("trigger-inside-cold-arm");
		Check check = emit_check (h, h.entry, VTABLE_A, "A");
		emit_trigger (h, check.not_inited, VTABLE_A, "A");
		CallInst *extra = emit_trigger (h, check.not_inited, VTABLE_A, "A");
		IRBuilder<> (check.not_inited).CreateBr (h.exit);
		IRBuilder<> (check.inited).CreateBr (h.exit);

		expect_redundant ("cold-arm-second-trigger", h, {extra});
	}
}

/* ------------------------------------------------- the two facts meeting */

static void
cases_facts_meet (void)
{
	{
		/*
		 * The case the whole analysis exists for. Past a completed barrier the
		 * class is initialized whichever arm was taken - one checked and found
		 * it set, the other called the trigger - so a second barrier for the
		 * same class is redundant even though neither half of the first
		 * dominates it.
		 */
		Harness h ("barrier-after-barrier");
		Barrier first = emit_barrier (h, h.entry, VTABLE_A, "A");
		Barrier second = emit_barrier (h, first.join, VTABLE_A, "A");
		IRBuilder<> (second.join).CreateBr (h.exit);

		expect_redundant ("barrier-covers-a-later-barrier", h, {second.trigger});
	}

	{
		/* And a bare trigger past the join, with no second check at all. */
		Harness h ("trigger-after-barrier");
		Barrier first = emit_barrier (h, h.entry, VTABLE_A, "A");
		CallInst *later = emit_trigger (h, first.join, VTABLE_A, "A");
		IRBuilder<> (first.join).CreateBr (h.exit);

		expect_redundant ("barrier-covers-a-later-trigger", h, {later});
	}

	{
		/* A barrier for A says nothing about B. */
		Harness h ("barrier-then-other-class");
		Barrier first = emit_barrier (h, h.entry, VTABLE_A, "A");
		Barrier second = emit_barrier (h, first.join, VTABLE_B, "B");
		IRBuilder<> (second.join).CreateBr (h.exit);

		expect_none ("barrier-does-not-cover-another-class", h);
	}

	{
		/*
		 * Three in a row: the second is covered by the first, and the third by
		 * either of them. Reported in program order.
		 */
		Harness h ("three-barriers");
		Barrier a = emit_barrier (h, h.entry, VTABLE_A, "A");
		Barrier b = emit_barrier (h, a.join, VTABLE_A, "A");
		Barrier c = emit_barrier (h, b.join, VTABLE_A, "A");
		IRBuilder<> (c.join).CreateBr (h.exit);

		expect_redundant ("chain-of-barriers", h, {b.trigger, c.trigger});
	}

	{
		/*
		 * The two facts arriving from different arms of an unrelated `if`: one
		 * side ran a trigger, the other took a check's yes edge. Either is
		 * enough, so the merge has it.
		 */
		Harness h ("mixed-facts-merge");
		auto [left, right] = h.fork (h.entry);
		BasicBlock *join = h.block ("join");

		emit_trigger (h, left, VTABLE_A, "A");
		IRBuilder<> (left).CreateBr (join);

		Check check = emit_check (h, right, VTABLE_A, "A");
		IRBuilder<> (check.inited).CreateBr (join);
		/* The arm that found it clear runs the trigger, as a barrier would. */
		emit_trigger (h, check.not_inited, VTABLE_A, "A");
		IRBuilder<> (check.not_inited).CreateBr (join);

		CallInst *after = emit_trigger (h, join, VTABLE_A, "A");
		IRBuilder<> (join).CreateBr (h.exit);

		expect_redundant ("call-and-check-meet", h, {after});
	}

	{
		/* But if one arm only *checked* and let the clear case through
		 * uncovered, the merge has nothing. */
		Harness h ("half-covered-merge");
		auto [left, right] = h.fork (h.entry);
		BasicBlock *join = h.block ("join");

		emit_trigger (h, left, VTABLE_A, "A");
		IRBuilder<> (left).CreateBr (join);

		Check check = emit_check (h, right, VTABLE_A, "A");
		IRBuilder<> (check.inited).CreateBr (join);
		IRBuilder<> (check.not_inited).CreateBr (join);

		emit_trigger (h, join, VTABLE_A, "A");
		IRBuilder<> (join).CreateBr (h.exit);

		expect_none ("uncovered-arm-poisons-the-merge", h);
	}

	{
		/*
		 * A loop. The header's trigger is not covered - the first iteration
		 * arrives from the entry with nothing behind it - but the body's is,
		 * from the header on every iteration.
		 */
		Harness h ("trigger-in-a-loop");
		BasicBlock *header = h.block ("header");
		BasicBlock *body = h.block ("body");
		IRBuilder<> (h.entry).CreateBr (header);

		emit_trigger (h, header, VTABLE_A, "A");
		IRBuilder<> hb (header);
		hb.CreateCondBr (hb.CreateICmpSGT (h.func->getArg (0), hb.getInt32 (0)), body, h.exit);

		CallInst *in_body = emit_trigger (h, body, VTABLE_A, "A");
		IRBuilder<> (body).CreateBr (header);

		expect_redundant ("loop-header-covers-the-body", h, {in_body});
	}

	{
		/*
		 * A trigger reached only from a loop back edge, with no fact ahead of
		 * it. Interior blocks start out optimistically covered, so this is the
		 * case that catches a fixpoint that never runs.
		 */
		Harness h ("bare-loop");
		BasicBlock *header = h.block ("header");
		BasicBlock *body = h.block ("body");
		IRBuilder<> (h.entry).CreateBr (header);

		IRBuilder<> hb (header);
		hb.CreateCondBr (hb.CreateICmpSGT (h.func->getArg (0), hb.getInt32 (0)), body, h.exit);

		emit_trigger (h, body, VTABLE_A, "A");
		IRBuilder<> (body).CreateBr (header);

		expect_none ("loop-does-not-cover-itself", h);
	}
}

/* ---------------------------------------------------- a throwing trigger */

static void
cases_invoke (void)
{
	/*
	 * A trigger inside a try region is an invoke. Its normal edge carries the
	 * fact; its unwind edge is where the cctor threw, and there the class is
	 * emphatically not initialized - a barrier on that path still has to run.
	 */
	Harness h ("invoking-trigger");
	llvm::LLVMContext &ctx = h.ctx;

	auto *personality = llvm::Function::Create (
		llvm::FunctionType::get (llvm::Type::getInt32Ty (ctx), true),
		Function::ExternalLinkage, "__gxx_personality_v0", h.module.get ());
	h.func->setPersonalityFn (personality);

	BasicBlock *normal = h.block ("normal");
	BasicBlock *lpad = h.block ("lpad");

	IRBuilder<> b (h.entry);
	llvm::InvokeInst *invoke = b.CreateInvoke (h.trigger_callee (), normal, lpad,
	                                           {b.getInt64 (VTABLE_A)});
	h.tag (invoke, "mono.class-init", "A");

	IRBuilder<> lb (lpad);
	auto *clause_ty = llvm::StructType::get (b.getPtrTy (), b.getInt32Ty ());
	lb.CreateLandingPad (clause_ty, 0)->setCleanup (true);
	emit_trigger (h, lpad, VTABLE_A, "A");
	IRBuilder<> (lpad).CreateBr (h.exit);

	CallInst *after = emit_trigger (h, normal, VTABLE_A, "A");
	IRBuilder<> (normal).CreateBr (h.exit);

	expect_redundant ("invoke-covers-only-its-normal-edge", h, {after});
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

		expect_redundant ("inverted-check-yes-edge", h, {call});
	}

	{
		/* And the trigger on an inverted check's cold arm still is not covered. */
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

		expect_redundant ("gep-form-address", h, {call});
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

		expect_redundant ("no-expect-wrapper", h, {call});
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
		auto [left, right] = h.fork (h.entry);
		h.tag (h.entry->getTerminator (), "mono.class-init-check", "A");

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
		emit_trigger (h, h.entry, VTABLE_A, "A");
		IRBuilder<> b (h.entry);
		CallInst *dynamic = b.CreateCall (h.trigger_callee (),
		                                  {b.CreateZExt (h.func->getArg (0), b.getInt64Ty ())});
		h.tag (dynamic, "mono.class-init", "A");
		b.CreateBr (h.exit);

		expect_none ("non-constant-vtable-ignored", h);
	}

	{
		/* Dead code is covered vacuously, which is true and useless; it stays
		 * out of the result rather than inviting a transform into it. */
		Harness h ("unreachable-trigger");
		BasicBlock *orphan = h.block ("orphan");
		IRBuilder<> (h.entry).CreateBr (h.exit);
		emit_trigger (h, orphan, VTABLE_A, "A");
		IRBuilder<> (orphan).CreateBr (h.exit);

		expect_none ("unreachable-block-not-reported", h);
	}
}

/* ------------------------------------------------------------- the transform */

/* Calls to the trigger still present in BB (or in the whole function). */
static unsigned
triggers_in (BasicBlock *bb)
{
	unsigned n = 0;

	for (llvm::Instruction &ins : *bb) {
		auto *call = llvm::dyn_cast<CallBase> (&ins);
		if (call && call->getCalledFunction () &&
		    call->getCalledFunction ()->getName () == "mono_generic_class_init")
			n ++;
	}
	return n;
}

static unsigned
triggers_in (Function *f)
{
	unsigned n = 0;

	for (BasicBlock &bb : *f)
		n += triggers_in (&bb);
	return n;
}

static void
run_elision (Harness &h)
{
	llvm::PassBuilder pb;
	llvm::FunctionAnalysisManager fam;

	pb.registerFunctionAnalyses (fam);
	mono::register_class_init_elision (pb, fam);

	llvm::FunctionPassManager fpm;
	fpm.addPass (mono::ClassInitElisionPass ());
	fpm.run (*h.func, fam);
}

/* Run the transform and check how many triggers it left behind. */
static void
expect_left (const char *what, Harness &h, unsigned want)
{
	cases_run ++;

	run_elision (h);

	std::string err;
	llvm::raw_string_ostream os (err);
	if (llvm::verifyFunction (*h.func, &os)) {
		fail (what, ("the transform produced invalid IR: " + os.str ()).c_str ());
		return;
	}

	unsigned got = triggers_in (h.func);
	if (got != want) {
		char buf [128];
		snprintf (buf, sizeof (buf), "expected %u trigger(s) left, got %u", want, got);
		fail (what, buf);
	}
}

static void
cases_transform (void)
{
	{
		/* Of two back-to-back triggers, the first survives. */
		Harness h ("elide-back-to-back");
		emit_trigger (h, h.entry, VTABLE_A, "A");
		emit_trigger (h, h.entry, VTABLE_A, "A");
		IRBuilder<> (h.entry).CreateBr (h.exit);

		expect_left ("transform-back-to-back", h, 1);
	}

	{
		/* The whole point: a second barrier past the first one's join. */
		Harness h ("elide-barrier-after-barrier");
		Barrier first = emit_barrier (h, h.entry, VTABLE_A, "A");
		Barrier second = emit_barrier (h, first.join, VTABLE_A, "A");
		IRBuilder<> (second.join).CreateBr (h.exit);

		expect_left ("transform-second-barrier", h, 1);

		if (triggers_in (first.check.not_inited) != 1)
			fail ("transform-second-barrier", "the covering barrier's trigger was removed");
		if (triggers_in (second.check.not_inited) != 0)
			fail ("transform-second-barrier", "the covered barrier's trigger survived");
		/*
		 * The guard the trigger leaves behind is not this pass's to remove -
		 * its arm is empty now and the SimplifyCFG behind it in the pipeline
		 * folds the pair away.
		 */
		if (!llvm::isa<BranchInst> (second.check.branch))
			fail ("transform-second-barrier", "the check branch was disturbed");
	}

	{
		/* Nothing redundant, nothing touched. */
		Harness h ("elide-lone-barrier");
		Barrier only = emit_barrier (h, h.entry, VTABLE_A, "A");
		IRBuilder<> (only.join).CreateBr (h.exit);

		expect_left ("transform-leaves-a-lone-barrier", h, 1);
	}

	{
		/* Two classes: each keeps its own barrier. */
		Harness h ("elide-two-classes");
		Barrier a = emit_barrier (h, h.entry, VTABLE_A, "A");
		Barrier b = emit_barrier (h, a.join, VTABLE_B, "B");
		IRBuilder<> (b.join).CreateBr (h.exit);

		expect_left ("transform-keeps-both-classes", h, 2);
	}

	{
		/* A chain collapses to the first one in a single run. */
		Harness h ("elide-chain");
		Barrier a = emit_barrier (h, h.entry, VTABLE_A, "A");
		Barrier b = emit_barrier (h, a.join, VTABLE_A, "A");
		Barrier c = emit_barrier (h, b.join, VTABLE_A, "A");
		IRBuilder<> (c.join).CreateBr (h.exit);

		expect_left ("transform-collapses-a-chain", h, 1);
	}

	{
		/*
		 * A redundant trigger that is an invoke. Deleting it means rewriting a
		 * terminator: the normal edge becomes an unconditional branch and the
		 * handler loses a predecessor.
		 */
		Harness h ("elide-invoke");
		llvm::LLVMContext &ctx = h.ctx;

		auto *personality = llvm::Function::Create (
			llvm::FunctionType::get (llvm::Type::getInt32Ty (ctx), true),
			Function::ExternalLinkage, "__gxx_personality_v0", h.module.get ());
		h.func->setPersonalityFn (personality);

		emit_trigger (h, h.entry, VTABLE_A, "A");

		BasicBlock *normal = h.block ("normal");
		BasicBlock *lpad = h.block ("lpad");
		IRBuilder<> b (h.entry);
		llvm::InvokeInst *invoke = b.CreateInvoke (h.trigger_callee (), normal, lpad,
		                                           {b.getInt64 (VTABLE_A)});
		h.tag (invoke, "mono.class-init", "A");

		IRBuilder<> lb (lpad);
		lb.CreateLandingPad (llvm::StructType::get (b.getPtrTy (), b.getInt32Ty ()), 0)
			->setCleanup (true);
		IRBuilder<> (lpad).CreateBr (h.exit);
		IRBuilder<> (normal).CreateBr (h.exit);

		expect_left ("transform-invoke", h, 1);

		auto *br = llvm::dyn_cast<BranchInst> (h.entry->getTerminator ());
		if (!br || br->isConditional () || br->getSuccessor (0) != normal)
			fail ("transform-invoke", "the invoke was not replaced by a branch to its normal dest");
		if (!llvm::pred_empty (lpad))
			fail ("transform-invoke", "the handler kept its predecessor");
	}

	{
		/*
		 * A tag on a call whose result someone is using. The real trigger
		 * returns void, so this is a tag the pass does not understand, and
		 * deleting the call would take a live value with it. The analysis
		 * happily calls the second one covered; the transform has to decline.
		 */
		cases_run ++;

		Harness h ("elide-used-result");
		IRBuilder<> b (h.entry);

		auto valued = h.module->getOrInsertFunction ("class_init_with_a_result",
		                                             b.getInt32Ty (), b.getInt64Ty ());
		CallInst *first = b.CreateCall (valued, {b.getInt64 (VTABLE_A)});
		h.tag (first, "mono.class-init", "A");
		CallInst *second = b.CreateCall (valued, {b.getInt64 (VTABLE_A)});
		h.tag (second, "mono.class-init", "A");

		auto sink = h.module->getOrInsertFunction ("sink", b.getVoidTy (), b.getInt32Ty ());
		b.CreateCall (sink, {second});
		b.CreateBr (h.exit);

		if (mono::find_redundant_class_inits (*h.func).size () != 1)
			fail ("transform-declines-a-used-result", "expected the analysis to flag the second call");

		run_elision (h);

		std::string err;
		llvm::raw_string_ostream os (err);
		if (llvm::verifyFunction (*h.func, &os))
			fail ("transform-declines-a-used-result",
			      ("the transform produced invalid IR: " + os.str ()).c_str ());
		if (second->getParent () != h.entry)
			fail ("transform-declines-a-used-result", "a call with a live result was deleted");
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

	if (result.redundant ().size () != 1)
		fail ("analysis-manager-result", "expected exactly 1 redundant trigger");
	if (!result.is_redundant (second))
		fail ("analysis-manager-lookup", "the covered call was not reported redundant");
	if (result.is_redundant (first))
		fail ("analysis-manager-lookup", "the covering call was itself reported redundant");
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
	cases_initialized_check ();
	cases_facts_meet ();
	cases_invoke ();
	cases_shapes ();
	cases_transform ();
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
