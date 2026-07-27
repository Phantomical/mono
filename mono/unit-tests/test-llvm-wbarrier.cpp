/*
 * test-llvm-wbarrier.cpp: unit tests for the write-barrier lowering in
 * mono/mini/llvm/passes/wbarrier.cpp.
 *
 * The transform turns a tagged barrier call into a conditional card mark, and
 * almost everything that could go wrong with it is invisible at runtime until it
 * is far too late: a dropped mark loses an object, and the wrong memory ordering
 * loses one only occasionally, on one machine, under load. So the cases below
 * pin the parts that carry the argument rather than just "it produced some IR" -
 * that the load is unordered and the RMW is a release Or, that the bit index is
 * built from the shift and mask the collector was asked for, and that the mark
 * is the cold arm.
 *
 * The negatives matter as much: an untagged call must survive untouched, and so
 * must a barrier reached by invoke, since deleting one of those would take its
 * block's terminator with it. Every case verifies the function afterwards, which
 * is what actually catches that class of mistake.
 *
 * The pass is driven with a synthetic CardBitmap rather than through
 * mono::card_bitmap (): these tests link the sgen runtime, which reports no
 * bitmap at all, and the arithmetic is worth testing against values chosen to
 * make a wrong shift or a missing mask obvious.
 */

#include "config.h"

#include <cstdint>
#include <cstdio>
#include <memory>

#ifdef ENABLE_LLVM

#include <mono/metadata/object-internals.h>

/* Has to come after the mono headers - mono-tls.h puts PIC back in scope. */
#ifdef PIC
#undef PIC
#endif

#include "mini/llvm/passes/wbarrier.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

using llvm::AtomicOrdering;
using llvm::AtomicRMWInst;
using llvm::BasicBlock;
using llvm::BranchInst;
using llvm::CallInst;
using llvm::ConstantInt;
using llvm::Function;
using llvm::IRBuilder;
using llvm::LoadInst;
using llvm::Module;

/* ------------------------------------------------------------ reporting */

static int failures;
static int cases_run;

static void
fail (const char *what, const char *detail)
{
	printf ("FAIL %s: %s\n", what, detail);
	failures ++;
}

static void
check (const char *what, bool ok, const char *detail)
{
	cases_run ++;
	if (!ok)
		fail (what, detail);
}

/* --------------------------------------------------------- IR assembly */

/*
 * A bitmap that is nothing like the real one on purpose. The shift is not
 * boehm's LOG_HBLKSIZE and the mask is not a plausible PHT_ENTRIES-1, so a
 * lowering that hardcodes either instead of reading the config shows up as a
 * mismatch rather than passing by coincidence.
 */
static const std::uint64_t TABLE_ADDR = 0x7f0000000000ull;
static const unsigned TABLE_SHIFT = 11;
static const std::uint64_t TABLE_MASK = 0x3ffff;

static mono::CardBitmap
test_bitmap ()
{
	return mono::CardBitmap { TABLE_ADDR, TABLE_SHIFT, TABLE_MASK };
}

struct Harness {
	llvm::LLVMContext ctx;
	std::unique_ptr<Module> module;
	Function *func = nullptr;
	BasicBlock *entry = nullptr;

	explicit Harness (const char *name)
	{
		module = std::make_unique<Module> (name, ctx);
		/* The host layout the engine optimizes against; the lowering reads the
		 * pointer width out of it to size the bitmap's words. */
		module->setDataLayout ("e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-"
		                       "i128:128-f80:128-n8:16:32:64-S128");

		auto *sig = llvm::FunctionType::get (llvm::Type::getVoidTy (ctx),
		                                     {llvm::Type::getInt64Ty (ctx)}, false);
		func = Function::Create (sig, Function::ExternalLinkage, name, module.get ());
		entry = BasicBlock::Create (ctx, "entry", func);
	}

	llvm::FunctionCallee barrier_callee ()
	{
		return module->getOrInsertFunction ("mono_gc_wbarrier_generic_nostore_internal",
		                                    llvm::Type::getVoidTy (ctx),
		                                    llvm::Type::getInt64Ty (ctx));
	}

	/* The barrier as the translator leaves it: a call carrying `mono.wbarrier`. */
	CallInst *barrier (IRBuilder<> &b, llvm::Value *addr, bool tagged = true)
	{
		CallInst *call = b.CreateCall (barrier_callee (), {addr});
		if (tagged)
			call->setMetadata ("mono.wbarrier", llvm::MDNode::get (ctx, {}));
		return call;
	}

	void run ()
	{
		llvm::FunctionAnalysisManager fam;
		llvm::PassBuilder pb;
		pb.registerFunctionAnalyses (fam);
		mono::WriteBarrierLoweringPass (test_bitmap ()).run (*func, fam);
	}

	bool verifies () { return !llvm::verifyFunction (*func, &llvm::errs ()); }
};

/* ------------------------------------------------------------- counting */

template <typename T>
static int
count (Function &f)
{
	int n = 0;
	for (llvm::Instruction &ins : llvm::instructions (f))
		if (llvm::isa<T> (&ins))
			n ++;
	return n;
}

static AtomicRMWInst *
first_rmw (Function &f)
{
	for (llvm::Instruction &ins : llvm::instructions (f))
		if (auto *rmw = llvm::dyn_cast<AtomicRMWInst> (&ins))
			return rmw;
	return nullptr;
}

static LoadInst *
first_load (Function &f)
{
	for (llvm::Instruction &ins : llvm::instructions (f))
		if (auto *load = llvm::dyn_cast<LoadInst> (&ins))
			return load;
	return nullptr;
}

/* The conditional branch the lowering introduces, with its profile weights. */
static BranchInst *
guard_branch (Function &f)
{
	for (llvm::Instruction &ins : llvm::instructions (f)) {
		auto *br = llvm::dyn_cast<BranchInst> (&ins);
		if (br && br->isConditional () && br->getMetadata ("prof"))
			return br;
	}
	return nullptr;
}

/*
 * Whether V is reachable through the operand graph via an operation with OPCODE
 * and a constant operand equal to LITERAL - how the tests ask "was the shift by
 * the configured amount, and the mask the configured one".
 *
 * Both instructions and constant expressions count. An operation on entirely
 * constant operands - which the table base is, being a literal address - gets
 * folded to a ConstantExpr as it is built, so it never turns up as an
 * Instruction to look at.
 */
static bool
uses_constant (llvm::Value *v, unsigned opcode, std::uint64_t literal, int depth = 8)
{
	if (depth == 0)
		return false;

	llvm::User *user = nullptr;
	unsigned op = 0;

	if (auto *ins = llvm::dyn_cast<llvm::Instruction> (v)) {
		user = ins;
		op = ins->getOpcode ();
	} else if (auto *expr = llvm::dyn_cast<llvm::ConstantExpr> (v)) {
		user = expr;
		op = expr->getOpcode ();
	} else {
		return false;
	}

	if (op == opcode) {
		for (llvm::Value *o : user->operands ()) {
			auto *c = llvm::dyn_cast<ConstantInt> (o);
			if (c && c->getZExtValue () == literal)
				return true;
		}
	}

	for (llvm::Value *o : user->operands ())
		if (uses_constant (o, opcode, literal, depth - 1))
			return true;

	return false;
}

/* ------------------------------------------------------------ the cases */

/* The barrier becomes a load, a test, and a conditional atomic OR. */
static void
case_lowers_tagged_call ()
{
	Harness h ("lowers_tagged_call");
	IRBuilder<> b (h.entry);
	h.barrier (b, h.func->getArg (0));
	b.CreateRetVoid ();

	h.run ();

	check ("lowers_tagged_call", count<CallInst> (*h.func) == 0,
	       "the barrier call should be gone");
	check ("lowers_tagged_call", count<AtomicRMWInst> (*h.func) == 1,
	       "should have left exactly one atomic RMW");
	check ("lowers_tagged_call", count<LoadInst> (*h.func) == 1,
	       "should have left exactly one load");
	check ("lowers_tagged_call", h.verifies (), "result should verify");
}

/*
 * The orderings, which are the whole argument in wbarrier.hpp: an unordered
 * read, because a stale answer can only be stale in the harmless direction, and
 * a release OR, so a thread that sees the mark also sees the store behind it.
 */
static void
case_memory_orderings ()
{
	Harness h ("memory_orderings");
	IRBuilder<> b (h.entry);
	h.barrier (b, h.func->getArg (0));
	b.CreateRetVoid ();

	h.run ();

	LoadInst *load = first_load (*h.func);
	AtomicRMWInst *rmw = first_rmw (*h.func);

	check ("memory_orderings", load && load->isAtomic (), "the card read must be atomic");
	check ("memory_orderings", load && load->getOrdering () == AtomicOrdering::Unordered,
	       "the card read should be unordered");
	check ("memory_orderings", rmw && rmw->getOperation () == AtomicRMWInst::Or,
	       "the mark should be an OR");
	check ("memory_orderings", rmw && rmw->getOrdering () == AtomicOrdering::Release,
	       "the mark should have release ordering");
	check ("memory_orderings", load && rmw && load->getAlign () == rmw->getAlign (),
	       "read and mark should agree on alignment");
}

/* The index is built from the configured shift and mask, not from constants. */
static void
case_index_arithmetic ()
{
	Harness h ("index_arithmetic");
	IRBuilder<> b (h.entry);
	h.barrier (b, h.func->getArg (0));
	b.CreateRetVoid ();

	h.run ();

	LoadInst *load = first_load (*h.func);
	check ("index_arithmetic", load != nullptr, "expected a card read");
	if (!load)
		return;

	llvm::Value *addr = load->getPointerOperand ();

	check ("index_arithmetic",
	       uses_constant (addr, llvm::Instruction::LShr, TABLE_SHIFT),
	       "the address should be shifted by the configured amount");
	check ("index_arithmetic",
	       uses_constant (addr, llvm::Instruction::And, TABLE_MASK),
	       "the page index should be masked with the configured mask");
	check ("index_arithmetic",
	       uses_constant (addr, llvm::Instruction::IntToPtr, TABLE_ADDR),
	       "the table base should be the configured address");
}

/* Marking is the cold arm - the point of testing before storing. */
static void
case_mark_is_cold ()
{
	Harness h ("mark_is_cold");
	IRBuilder<> b (h.entry);
	h.barrier (b, h.func->getArg (0));
	b.CreateRetVoid ();

	h.run ();

	BranchInst *br = guard_branch (*h.func);
	check ("mark_is_cold", br != nullptr, "the guard should carry branch weights");
	if (!br)
		return;

	auto *prof = br->getMetadata ("prof");
	/* operand 0 is the "branch_weights" name, then one weight per successor. */
	check ("mark_is_cold", prof && prof->getNumOperands () == 3,
	       "expected two branch weights");
	if (!prof || prof->getNumOperands () != 3)
		return;

	auto weight = [&] (unsigned i) {
		auto *md = llvm::mdconst::dyn_extract<ConstantInt> (prof->getOperand (i));
		return md ? md->getZExtValue () : 0;
	};

	/* The true edge is the one that marks. It should be the unlikely one. */
	check ("mark_is_cold", weight (1) < weight (2),
	       "the marking edge should be weighted colder than the skip");
}

/* A call without the tag is not a barrier and must be left exactly as it is. */
static void
case_leaves_untagged_call ()
{
	Harness h ("leaves_untagged_call");
	IRBuilder<> b (h.entry);
	h.barrier (b, h.func->getArg (0), /* tagged */ false);
	b.CreateRetVoid ();

	h.run ();

	check ("leaves_untagged_call", count<CallInst> (*h.func) == 1,
	       "an untagged call should survive");
	check ("leaves_untagged_call", count<AtomicRMWInst> (*h.func) == 0,
	       "an untagged call should not be lowered");
	check ("leaves_untagged_call", h.verifies (), "result should verify");
}

/*
 * A barrier reached by invoke is skipped rather than lowered: deleting it would
 * leave its block without a terminator. Correct, just not inlined.
 */
static void
case_leaves_invoke ()
{
	Harness h ("leaves_invoke");

	/* An invoke needs a personality and a landing pad to be well formed. */
	auto *personality = llvm::Function::Create (
		llvm::FunctionType::get (llvm::Type::getInt32Ty (h.ctx), true),
		Function::ExternalLinkage, "__mono_personality", h.module.get ());
	h.func->setPersonalityFn (personality);

	BasicBlock *normal = BasicBlock::Create (h.ctx, "normal", h.func);
	BasicBlock *unwind = BasicBlock::Create (h.ctx, "unwind", h.func);

	IRBuilder<> b (h.entry);
	auto *inv = b.CreateInvoke (h.barrier_callee (), normal, unwind,
	                            {h.func->getArg (0)});
	inv->setMetadata ("mono.wbarrier", llvm::MDNode::get (h.ctx, {}));

	IRBuilder<> (normal).CreateRetVoid ();
	IRBuilder<> lb (unwind);
	auto *pad = lb.CreateLandingPad (
		llvm::StructType::get (llvm::PointerType::get (h.ctx, 0),
		                       llvm::Type::getInt32Ty (h.ctx)), 0);
	pad->setCleanup (true);
	lb.CreateRetVoid ();

	h.run ();

	check ("leaves_invoke", count<llvm::InvokeInst> (*h.func) == 1,
	       "an invoked barrier should survive");
	check ("leaves_invoke", count<AtomicRMWInst> (*h.func) == 0,
	       "an invoked barrier should not be lowered");
	check ("leaves_invoke", h.verifies (), "result should verify");
}

/* A run of barriers - a constructor's worth - all get lowered. */
static void
case_multiple_barriers ()
{
	Harness h ("multiple_barriers");
	IRBuilder<> b (h.entry);

	llvm::Value *base = h.func->getArg (0);
	for (int i = 0; i < 3; i ++) {
		llvm::Value *addr = b.CreateAdd (
			base, ConstantInt::get (llvm::Type::getInt64Ty (h.ctx), i * 8));
		h.barrier (b, addr);
	}
	b.CreateRetVoid ();

	h.run ();

	check ("multiple_barriers", count<CallInst> (*h.func) == 0,
	       "every barrier call should be gone");
	check ("multiple_barriers", count<AtomicRMWInst> (*h.func) == 3,
	       "each barrier should have left a mark");
	check ("multiple_barriers", h.verifies (), "result should verify");
}

/*
 * The wrapper's parameter is a native int today, but nothing about the lowering
 * depends on that, so a pointer-typed barrier has to work too.
 */
static void
case_pointer_argument ()
{
	Harness h ("pointer_argument");
	IRBuilder<> b (h.entry);

	auto callee = h.module->getOrInsertFunction (
		"wbarrier_ptr", llvm::Type::getVoidTy (h.ctx),
		llvm::PointerType::get (h.ctx, 0));
	llvm::Value *ptr = b.CreateIntToPtr (h.func->getArg (0),
	                                     llvm::PointerType::get (h.ctx, 0));
	CallInst *call = b.CreateCall (callee, {ptr});
	call->setMetadata ("mono.wbarrier", llvm::MDNode::get (h.ctx, {}));
	b.CreateRetVoid ();

	h.run ();

	check ("pointer_argument", count<AtomicRMWInst> (*h.func) == 1,
	       "a pointer-typed barrier should lower");
	check ("pointer_argument", h.verifies (), "result should verify");
}

/* --------------------------------------------------------------- driver */

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_wbarrier_main (void);

int
test_llvm_wbarrier_main (void)
{
	failures = 0;
	cases_run = 0;

	setvbuf (stdout, nullptr, _IOLBF, 0);

	case_lowers_tagged_call ();
	case_memory_orderings ();
	case_index_arithmetic ();
	case_mark_is_cold ();
	case_leaves_untagged_call ();
	case_leaves_invoke ();
	case_multiple_barriers ();
	case_pointer_argument ();

	printf ("%d cases run, %d failed\n", cases_run, failures);
	return failures ? 1 : 0;
}

#else /* !ENABLE_LLVM */

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_wbarrier_main (void);

int
test_llvm_wbarrier_main (void)
{
	return 0;
}

#endif /* ENABLE_LLVM */
