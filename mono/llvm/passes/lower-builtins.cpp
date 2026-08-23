/**
 * \file
 * \brief Rewriting `mono.builtin.*` calls into the calls they stand for.
 *
 * Each builtin declaration carries the name of the function it stands for, so
 * the rewrite is a matter of restating the site against that function with the
 * arguments the real shape wants. The rewrite reads none of mono's metadata:
 * what the translator could not say in the call it said on the declaration.
 */

#include "lower-builtins.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {
namespace {

/// Rewrites the builtin call into the call it stands for.
///
/// A string constructor keeps the this every instance method's signature has -
/// the runtime gives it a null and its body never reads it - so the call it
/// stands for is the same call with that null in front.
void
lower_string_constructor (CallBase *site, Function *target)
{
	IRBuilder<> b (site);
	SmallVector<Value *, 8> args;

	args.push_back (Constant::getNullValue (PointerType::get (site->getContext (), 0)));
	args.append (site->arg_begin (), site->arg_end ());

	CallBase *lowered;
	if (auto *invoke = dyn_cast<InvokeInst> (site)) {
		/*
		 * The site keeps its destinations, so the string a constructor throws
		 * out of stays catchable exactly where the constructor was called.
		 */
		lowered = b.CreateInvoke (target, invoke->getNormalDest (),
		                          invoke->getUnwindDest (), args);
	} else {
		CallInst *call = b.CreateCall (target, args);

		/* A managed frame is observable. emit_protected_call says why. */
		call->setTailCallKind (CallInst::TCK_NoTail);
		lowered = call;
	}

	lowered->setCallingConv (target->getCallingConv ());
	site->replaceAllUsesWith (lowered);
	site->eraseFromParent ();
}

} // namespace

PreservedAnalyses
LowerBuiltinsPass::run (Module &m, ModuleAnalysisManager &)
{
	SmallVector<Function *, 4> decls;

	for (Function &f : m)
		if (f.isDeclaration () && f.getName ().starts_with (builtin_prefix))
			decls.push_back (&f);

	if (decls.empty ())
		return PreservedAnalyses::all ();

	for (Function *decl : decls) {
		StringRef kind = decl->getFnAttribute (builtin_attribute).getValueAsString ();
		StringRef name =
			decl->getFnAttribute (builtin_target_attribute).getValueAsString ();
		Function *target = m.getFunction (name);

		if (target == nullptr)
			report_fatal_error (Twine ("builtin ") + decl->getName ()
			                    + " stands for " + name + ", which is not declared");

		SmallVector<CallBase *, 8> sites;

		for (User *user : decl->users ())
			if (auto *site = dyn_cast<CallBase> (user))
				sites.push_back (site);

		if (kind == builtin_string_constructor) {
			if (target->arg_size () != decl->arg_size () + 1)
				report_fatal_error (Twine ("string constructor ") + name
				                    + " does not take a this and the arguments of "
				                    + decl->getName ());

			for (CallBase *site : sites)
				lower_string_constructor (site, target);
		} else {
			report_fatal_error (Twine ("unknown ") + builtin_attribute + " kind '"
			                    + kind + "' on " + decl->getName ());
		}

		/* Anything left is a use no lowering understands, so fail loudly. */
		if (!decl->use_empty ())
			report_fatal_error (Twine ("unlowered use of ") + decl->getName ());
		decl->eraseFromParent ();
	}

	return PreservedAnalyses::none ();
}

} // namespace mono
