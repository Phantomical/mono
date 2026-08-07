/**
 * \file
 * \brief The sites the translator hands to LowerBuiltinsPass.
 *
 * A method whose call shape is not the one its metadata reads is emitted as a
 * call to a `mono.builtin.*` declaration saying what the site means, and the
 * pass settles how it is reached. This file is where that knowledge lives: an
 * opcode that can reach such a method asks for the meaning and never spells
 * the shape.
 */

#include "method-to-llvm.hpp"
#include "passes/lower-builtins.hpp"
#include "runtime-error.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/metadata.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <string>
#include <vector>

namespace mono {

/// The object CTOR builds out of ARGS, which are its arguments without the
/// this.
///
/// A string cannot be allocated before its length is known, so its constructor
/// compiles as a creator: the runtime hands it a null this and it returns the
/// string it made. Asking the builtin for the object keeps that shape out of
/// the opcodes - all they have to know is that a creator hands back what it
/// built, which is what newobj wanted anyway.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::emit_creator (MonoIrBuilder &builder, MonoMethod *ctor,
                                 llvm::ArrayRef<llvm::Value *> args)
{
	/*
	 * A string constructor is an icall, and what the runtime publishes for it
	 * is the wrapper that calls the static creator behind it - a method this
	 * backend compiles, with the same shape the constructor's own creator
	 * signature has.
	 */
	llvm::Expected<llvm::Function *> target =
		create_method_decl (icall_wrapper_target (ctor));
	if (!target)
		return target.takeError ();

	/* Whatever it is called, a creator hands back an object nothing else holds. */
	(*target)->addRetAttr (llvm::Attribute::NoAlias);

	llvm::FunctionType *shape = (*target)->getFunctionType ();

	if (shape->getNumParams () != args.size () + 1)
		return invalid_il ("wrong number of arguments to a constructor");

	/*
	 * One declaration per creator: the name it stands for is unique already,
	 * and a shared module can have declared it under an earlier emitter.
	 */
	std::string name =
		(llvm::Twine (builtin_prefix) + builtin_creator + "." + (*target)->getName ())
			.str ();
	llvm::Function *decl = module->getFunction (name);

	if (decl == nullptr) {
		std::vector<llvm::Type *> params (shape->param_begin () + 1,
		                                  shape->param_end ());

		decl = llvm::Function::Create (
			llvm::FunctionType::get (shape->getReturnType (), params,
		                                 shape->isVarArg ()),
			llvm::GlobalValue::ExternalLinkage, name, module);
		decl->addFnAttr (llvm::Attribute::get (context (), builtin_attribute,
		                                       builtin_creator));
		decl->addFnAttr (llvm::Attribute::get (context (), builtin_target_attribute,
		                                       (*target)->getName ()));
	}

	return emit_protected_call (builder, decl, args);
}

/// A plain call to a creator, which leaves the object it built on the stack.
///
/// Ordinary IL reaches a constructor through newobj. The one place that calls
/// one directly is the runtime-invoke wrapper: reflection has no instance to
/// hand over either, so the wrapper pushes a placeholder this, calls the
/// constructor and stores what comes back.
llvm::Error
MethodLLVMEmitter::emit_creator_call (MonoIrBuilder &builder, MonoMethod *ctor,
                                      MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	/* The placeholder goes no further; the creator's own this is the pass's. */
	llvm::Expected<llvm::Value *> created =
		emit_creator (builder, ctor, llvm::ArrayRef (*args).drop_front (sig->hasthis));

	if (!created)
		return created.takeError ();

	pop_stack (sig->param_count + sig->hasthis);
	push_stack (*created, mono_marshal_get_string_ctor_signature (ctor)->ret);
	return llvm::Error::success ();
}

} // namespace mono
