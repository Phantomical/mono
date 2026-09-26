/**
 * \file
 * \brief The address space object references and managed pointers live in.
 *
 * Object references and managed pointers use address space 1 so LLVM knows
 * they cannot be freed while live. Native pointers remain in address space 0.
 * Managed pointers have no dereferenceable attribute because they may point to
 * native memory.
 */

#ifndef MONO_LLVM_MANAGED_POINTER_HPP
#define MONO_LLVM_MANAGED_POINTER_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>

namespace mono {

constexpr unsigned object_address_space = 1;

/// Lets LLVM treat address-space-1 pointers as live GC references.
constexpr llvm::StringRef object_gc_strategy = "statepoint-example";

inline llvm::PointerType *
object_pointer_type (llvm::LLVMContext &ctx)
{
	return llvm::PointerType::get (ctx, object_address_space);
}

inline bool
is_object_pointer (const llvm::Type *type)
{
	return type->isPointerTy () && type->getPointerAddressSpace () == object_address_space;
}

/// Convert a pointer constant, preserving null, poison, and undef.
inline llvm::Constant *
constant_in_address_space (llvm::Constant *value, unsigned space)
{
	auto *type = llvm::PointerType::get (value->getContext (), space);

	if (llvm::isa<llvm::ConstantPointerNull> (value))
		return llvm::ConstantPointerNull::get (type);
	if (llvm::isa<llvm::PoisonValue> (value))
		return llvm::PoisonValue::get (type);
	if (llvm::isa<llvm::UndefValue> (value))
		return llvm::UndefValue::get (type);

	return llvm::ConstantExpr::getAddrSpaceCast (value, type);
}

/// Convert a pointer to `space` when needed.
inline llvm::Value *
in_address_space (llvm::IRBuilderBase &builder, llvm::Value *value, unsigned space)
{
	llvm::Type *type = value->getType ();

	if (!type->isPointerTy () || type->getPointerAddressSpace () == space)
		return value;
	if (auto *constant = llvm::dyn_cast<llvm::Constant> (value))
		return constant_in_address_space (constant, space);

	return builder.CreateAddrSpaceCast (value, llvm::PointerType::get (type->getContext (), space));
}

/// Replace a call site, casting the replacement result when necessary.
inline void
replace_site_result (llvm::CallBase *site, llvm::CallBase *call)
{
	llvm::Value *result = call;

	if (call->getType () != site->getType ()) {
		llvm::Instruction *at = call->getNextNode ();

		if (auto *invoke = llvm::dyn_cast<llvm::InvokeInst> (call))
			at = &*invoke->getNormalDest ()->getFirstInsertionPt ();

		llvm::IRBuilder<> after (at);

		result = call->getType ()->isPointerTy ()
		                 ? in_address_space (after, call,
		                                     site->getType ()->getPointerAddressSpace ())
		                 : after.CreateIntToPtr (call, site->getType ());
	}

	site->replaceAllUsesWith (result);
}

} // namespace mono

#endif
