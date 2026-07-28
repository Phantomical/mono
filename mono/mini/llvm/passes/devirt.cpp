/**
 * \file
 * \brief Implementation of the tier-1 exact devirtualization pass.
 *
 * See devirt.hpp for what this is for. The shape of the work is:
 *
 *   1. find the calls the translator tagged "mono.virtcall";
 *   2. prove the receiver's exact class, or give up on that site;
 *   3. ask the runtime what (declared method, exact class) resolves to;
 *   4. point the call at that method's trampoline symbol.
 *
 * Step 4 is a plain operand rewrite - the callee changes and nothing else, right
 * down to the imt argument an interface site passes in `nest`, which is left in
 * place for the callee to ignore. The call keeps its block, so an `invoke` keeps
 * its unwind edge and the EH structure is untouched. That is the whole reason
 * this is cheap: a guarded devirtualization would have to split the site in two
 * and duplicate those edges.
 */

#include "devirt.hpp"

#include "inliner-support.hpp"

#ifdef PIC
#undef PIC
#endif

#include <string>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace mono {

namespace {

/*
 * Tracing, gated on MONO_DEVIRT_TRACE. The interesting output here is the
 * refusals rather than the successes: "this site stayed indirect, and here is
 * which check stopped it" is what says whether the walk is too weak or the
 * runtime-side gates too strict, and a body that computes correct answers looks
 * identical either way.
 */
bool
trace_enabled ()
{
	/* Read once for the process; concurrent tier-1 compiles all pass through here. */
	static const bool enabled = getenv ("MONO_DEVIRT_TRACE") != nullptr;
	return enabled;
}

void
trace (const char *what, MonoMethod *method, MonoClass *klass = nullptr)
{
	if (!trace_enabled ())
		return;

	char *name = mono_method_full_name (method, TRUE);

	errs () << "[devirt] " << what << " " << name;
	if (klass) {
		char *recv = mono_class_full_name (klass);

		errs () << " on " << recv;
		g_free (recv);
	}
	errs () << "\n";
	g_free (name);
}

/* One decoded "mono.virtcall" tag. */
struct VirtCallTag {
	StringRef kind;          /* "vtable", "imt" or "delegate" */
	MonoMethod *declared;
	unsigned this_pindex;
};

bool
decode_tag (const CallBase &cb, VirtCallTag *out)
{
	MDNode *md = cb.getMetadata ("mono.virtcall");
	if (!md || md->getNumOperands () != 3)
		return false;

	auto *kind = dyn_cast<MDString> (md->getOperand (0));
	auto *method = dyn_cast<ConstantAsMetadata> (md->getOperand (1));
	auto *pindex = dyn_cast<ConstantAsMetadata> (md->getOperand (2));
	if (!kind || !method || !pindex)
		return false;

	out->kind = kind->getString ();
	out->declared = (MonoMethod *) (gsize) cast<ConstantInt> (method->getValue ())->getZExtValue ();
	out->this_pindex = cast<ConstantInt> (pindex->getValue ())->getZExtValue ();
	return out->declared != nullptr;
}

/*
 * The class an allocation site produces, or NULL if CB is not one.
 *
 * The front end passes the MonoVTable as a literal to the managed allocator
 * (handle_alloc () in method-to-ir.c), and the inliner refuses to fold wrappers,
 * so an allocator call reliably survives into optimized IR as an opaque call
 * with that constant still in place. LLVM cannot see through it - the vtable
 * field is written inside the allocator - which is exactly why the class has to
 * be read off the argument rather than off a load of the object.
 */
MonoClass *
allocated_class (const CallBase &cb)
{
	Function *callee = cb.getCalledFunction ();
	if (!callee || cb.arg_empty ())
		return nullptr;

	std::string name = callee->getName ().str ();
	MonoMethod *method = managed_method_from_symbol (name.c_str ());
	if (!method || method->wrapper_type != MONO_WRAPPER_ALLOC)
		return nullptr;

	Value *arg = cb.getArgOperand (0);
	/* The argument is an intptr, but a constant can arrive folded through a cast. */
	if (auto *expr = dyn_cast<ConstantExpr> (arg))
		if (expr->getOpcode () == Instruction::IntToPtr || expr->getOpcode () == Instruction::PtrToInt)
			arg = expr->getOperand (0);

	auto *vtable_const = dyn_cast<ConstantInt> (arg);
	if (!vtable_const)
		return nullptr;

	auto *vtable = (MonoVTable *) (gsize) vtable_const->getZExtValue ();
	return vtable ? vtable->klass : nullptr;
}

/*
 * What a walk of one value concluded.
 *
 * Cycle has to be distinct from Unknown. A loop-carried phi reaches itself, and
 * that self-edge carries no information - folding it into Unknown would sink
 * every phi in a loop, while folding it into "agrees with the others" is how
 * you get an unprovable arm silently ignored, which is a miscompile. So it is
 * its own answer: contributes nothing, proves nothing.
 */
enum class Proof { Unknown, Cycle, Exact };

struct ClassProof {
	Proof kind;
	MonoClass *klass;        /* only meaningful when kind == Exact */

	static ClassProof unknown () { return { Proof::Unknown, nullptr }; }
	static ClassProof cycle () { return { Proof::Cycle, nullptr }; }
	static ClassProof exact (MonoClass *k) { return { Proof::Exact, k }; }
};

/*
 * Prove the exact class of V.
 *
 * "Exact" is the whole point: a base-class answer is not good enough, since the
 * slot a subclass dispatches to may differ. Every path that merges values must
 * therefore agree - a phi whose arms disagree proves nothing rather than their
 * common base.
 */
ClassProof
prove_class (Value *v, SmallPtrSetImpl<Value *> &visited, unsigned depth = 0)
{
	/*
	 * The visited set already guarantees termination; this only bounds how deep
	 * the C++ recursion can go on a pathological chain of merges.
	 */
	static const unsigned MaxDepth = 32;

	if (depth > MaxDepth)
		return ClassProof::unknown ();

	v = v->stripPointerCasts ();

	if (!visited.insert (v).second)
		return ClassProof::cycle ();

	if (auto *cb = dyn_cast<CallBase> (v)) {
		MonoClass *klass = allocated_class (*cb);

		return klass ? ClassProof::exact (klass) : ClassProof::unknown ();
	}

	if (auto *phi = dyn_cast<PHINode> (v)) {
		MonoClass *agreed = nullptr;

		for (Value *in : phi->incoming_values ()) {
			ClassProof p = prove_class (in, visited, depth + 1);

			if (p.kind == Proof::Unknown)
				return ClassProof::unknown ();
			if (p.kind == Proof::Cycle)
				continue;
			if (agreed && agreed != p.klass)
				return ClassProof::unknown ();
			agreed = p.klass;
		}

		/* Every arm was a cycle: no information, but nothing contradicted either. */
		return agreed ? ClassProof::exact (agreed) : ClassProof::cycle ();
	}

	if (auto *sel = dyn_cast<SelectInst> (v)) {
		ClassProof t = prove_class (sel->getTrueValue (), visited, depth + 1);
		ClassProof f = prove_class (sel->getFalseValue (), visited, depth + 1);

		if (t.kind == Proof::Cycle)
			return f;
		if (f.kind == Proof::Cycle)
			return t;
		if (t.kind != Proof::Exact || f.kind != Proof::Exact || t.klass != f.klass)
			return ClassProof::unknown ();
		return t;
	}

	return ClassProof::unknown ();
}

/*
 * Whether CB can be rewritten at all, independently of what it resolves to.
 */
const char *
site_refusal (const CallBase &cb, const VirtCallTag &tag)
{
	/*
	 * A delegate's "slot" is MonoDelegate.invoke_impl, so the target follows
	 * from the delegate's creation site rather than from the receiver's class -
	 * a different oracle entirely, and a later slice.
	 */
	if (tag.kind == "delegate")
		return "refuse-site-kind";

	if (tag.this_pindex >= cb.arg_size ())
		return "refuse-site-no-receiver";

	/*
	 * A musttail call's callee has to keep matching the caller's own signature
	 * exactly, so repointing it is not a question of the operand alone.
	 */
	if (auto *ci = dyn_cast<CallInst> (&cb))
		if (ci->isMustTailCall ())
			return "refuse-site-musttail";

	return nullptr;
}

} // namespace

unsigned
devirtualize (Module &module, const Tier1Root &root)
{
	if (!root.cfg || !root.module)
		return 0;

	/*
	 * Collected first, then rewritten: resolving a target adds its declaration
	 * to the module, and growing the module's function list while walking it is
	 * not worth reasoning about.
	 */
	SmallVector<CallBase *, 16> sites;

	for (Function &func : module) {
		if (func.isDeclaration ())
			continue;

		for (Instruction &inst : instructions (func)) {
			auto *cb = dyn_cast<CallBase> (&inst);

			/*
			 * A site an earlier round resolved keeps its tag - the rewrite
			 * only changes the callee - so anything already direct is done.
			 */
			if (!cb || cb->getCalledFunction ())
				continue;
			if (cb->getMetadata ("mono.virtcall"))
				sites.push_back (cb);
		}
	}

	unsigned rewritten = 0;

	for (CallBase *cb : sites) {
		VirtCallTag tag;
		if (!decode_tag (*cb, &tag))
			continue;

		if (const char *why = site_refusal (*cb, tag)) {
			trace (why, tag.declared);
			continue;
		}

		Value *receiver = cb->getArgOperand (tag.this_pindex);

		SmallPtrSet<Value *, 8> visited;
		ClassProof recv = prove_class (receiver, visited);
		if (recv.kind != Proof::Exact) {
			trace ("refuse-receiver-unknown", tag.declared);
			continue;
		}

		/*
		 * The vtable load this rewrite makes dead is also the site's null check:
		 * mini_emit_method_call_full () emits an explicit CHECK_THIS only on the
		 * paths it resolves statically, and lets the faulting load stand in for
		 * it on the virtual one. Once nothing uses the loaded vtable, that load
		 * is deleted and a null receiver would reach the callee instead of
		 * raising NullReferenceException.
		 *
		 * Today this cannot fire - prove_class () only succeeds when every path
		 * to the receiver is an allocator call, and those are marked nonnull -
		 * so the check is here to keep that true as origins are added, not
		 * because it is currently reachable. A future origin that is not
		 * self-evidently non-null (a field load, say) must either be refused
		 * here or grow a real null check at the rewrite.
		 */
		if (!isKnownNonZero (receiver, module.getDataLayout (), 0, nullptr, cb)) {
			trace ("refuse-receiver-maybe-null", tag.declared, recv.klass);
			continue;
		}

		const char *reason = nullptr;
		MonoMethod *target = resolve_exact_virtual_target (tag.declared, recv.klass, &reason);
		if (!target) {
			trace (reason, tag.declared, recv.klass);
			continue;
		}

		/*
		 * An interface or generic-virtual site carries an imt argument in `nest`,
		 * which nothing downstream has any use for. It stays anyway: the
		 * declaration is built to the site's own type, so the call is repointed
		 * rather than rebuilt, and the body materialize_callee () hands back
		 * accepts the parameter and ignores it. Inlining and DCE take it and
		 * everything that computed it away.
		 *
		 * What that cannot absorb is a target wanting an rgctx of its own in that
		 * parameter, since an imt argument is not one.
		 */
		if (target_needs_rgctx (target, root)) {
			trace ("refuse-target-needs-rgctx", target, recv.klass);
			continue;
		}

		Function *decl = direct_callee_decl (target, *cb, root);
		if (!decl) {
			trace ("refuse-no-declaration", target, recv.klass);
			continue;
		}

		cb->setCalledFunction (decl);
		rewritten++;
		trace ("devirt", target, recv.klass);
	}

	return rewritten;
}

} // namespace mono
