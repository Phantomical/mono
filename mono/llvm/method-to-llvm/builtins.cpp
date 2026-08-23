/**
 * \file
 * \brief Emitting `mono.builtin.string_constructor` calls.
 *
 * A string constructor returns the string it builds instead of filling in an
 * instance. `passes/lower-builtins.hpp` says what the builtin means and how
 * the pass lowers it. This file emits that call. `newobj` reaches it once
 * allocation is skipped. A direct `call` reaches it from the runtime-invoke
 * wrapper.
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

/// Calls the string constructor ctor and returns the string it built.
///
/// \param args  the constructor arguments, without the this.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::emit_string_constructor (MonoIrBuilder &builder, MonoMethod *ctor,
                                            llvm::ArrayRef<llvm::Value *> args)
{
	// Callers do not agree on whether ctor already names its wrapper, so ask
	// again here.
	llvm::Expected<llvm::Function *> target =
		create_method_decl (icall_wrapper_target (ctor));
	if (!target)
		return target.takeError ();

	// A string constructor returns an object nothing else holds.
	(*target)->addRetAttr (llvm::Attribute::NoAlias);

	llvm::FunctionType *shape = (*target)->getFunctionType ();

	if (shape->getNumParams () != args.size () + 1)
		return invalid_il ("wrong number of arguments to a constructor");

	// One declaration per builtin name in the module. A filter body gets
	// its own MethodLLVMEmitter, but it shares this module with the method
	// body. If that other emitter already declared this one, the lookup
	// finds it here.
	std::string name =
		(llvm::Twine (builtin_prefix) + builtin_string_constructor + "." + (*target)->getName ())
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
		                                       builtin_string_constructor));
		decl->addFnAttr (llvm::Attribute::get (context (), builtin_target_attribute,
		                                       (*target)->getName ()));
	}

	return emit_protected_call (builder, decl, args);
}

llvm::Error
MethodLLVMEmitter::emit_string_constructor_call (MonoIrBuilder &builder, MonoMethod *ctor,
                                                 MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	// The placeholder goes no further. The null this comes from the pass.
	llvm::Expected<llvm::Value *> created =
		emit_string_constructor (builder, ctor,
		                         llvm::ArrayRef (*args).drop_front (sig->hasthis));

	if (!created)
		return created.takeError ();

	pop_stack (sig->param_count + sig->hasthis);
	push_stack (*created, mono_marshal_get_string_ctor_signature (ctor)->ret);
	return llvm::Error::success ();
}

} // namespace mono
