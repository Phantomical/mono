/**
 * \file
 * \brief Emitting `mono.builtin.creator` calls for a string constructor.
 *
 * A string constructor returns the string it builds instead of filling in an
 * instance. `passes/lower-builtins.hpp` says what the `creator` builtin means
 * and how the pass lowers it. This file emits that call. `newobj` reaches it
 * once allocation is skipped. A direct `call` reaches it from the
 * runtime-invoke wrapper.
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

/// Calls a constructor that returns the object it builds instead of filling in
/// an instance.
///
/// \param args  the constructor arguments, without the this.
///
/// A string constructor cannot fill in an instance because its length is not
/// known before the constructor reads its arguments. Asking for the object
/// here keeps that shape out of the opcode that calls it.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::emit_creator (MonoIrBuilder &builder, MonoMethod *ctor,
                                 llvm::ArrayRef<llvm::Value *> args)
{
	// Callers do not agree on whether ctor already names its wrapper, so ask
	// again here.
	llvm::Expected<llvm::Function *> target =
		create_method_decl (icall_wrapper_target (ctor));
	if (!target)
		return target.takeError ();

	// A creator hands back an object nothing else holds.
	(*target)->addRetAttr (llvm::Attribute::NoAlias);

	llvm::FunctionType *shape = (*target)->getFunctionType ();

	if (shape->getNumParams () != args.size () + 1)
		return invalid_il ("wrong number of arguments to a constructor");

	// One declaration per creator name in the module. A filter body gets
	// its own MethodLLVMEmitter, but it shares this module with the method
	// body. If that other emitter already declared the creator, this
	// lookup finds it here.
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

/// A plain call to a creator leaves the object it built on the stack.
///
/// Ordinary IL reaches a constructor through newobj. Only the runtime-invoke
/// wrapper calls a constructor directly, since reflection has no instance to
/// hand over either. It pushes a placeholder this, calls the constructor, and
/// stores what comes back.
llvm::Error
MethodLLVMEmitter::emit_creator_call (MonoIrBuilder &builder, MonoMethod *ctor,
                                      MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	// The placeholder goes no further. The creator's own this comes from the pass.
	llvm::Expected<llvm::Value *> created =
		emit_creator (builder, ctor, llvm::ArrayRef (*args).drop_front (sig->hasthis));

	if (!created)
		return created.takeError ();

	pop_stack (sig->param_count + sig->hasthis);
	push_stack (*created, mono_marshal_get_string_ctor_signature (ctor)->ret);
	return llvm::Error::success ();
}

} // namespace mono
