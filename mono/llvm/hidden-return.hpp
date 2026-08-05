/**
 * \file
 * \brief The pointer a return too wide for the return registers travels through.
 *
 * A value type that will not fit in the return registers has to come back
 * through a pointer the caller supplies. Left to itself LLVM invents that
 * pointer while lowering the call, and the slot it points at is a fresh stack
 * object in the caller's own frame - which is why such a call can never become
 * a jump: handing the frame away would leave the callee writing into dead
 * stack, so the backend quietly drops the tail call.
 *
 * Spelling the pointer out in the IR instead - a void prototype whose leading
 * parameter is `sret` - is what makes the return tail-callable, because a tail
 * site can then forward the caller's *own* incoming pointer, which lives in an
 * ancestor frame and outlives the jump. X86 recognises exactly that shape
 * (mayBeSRetTailCallCompatible) and still refuses a forwarded local.
 *
 * The prototype is derived from the signature, so a declaration and a body
 * always agree on it without either asking the other.
 */

#ifndef MONO_LLVM_HIDDEN_RETURN_HPP
#define MONO_LLVM_HIDDEN_RETURN_HPP

#include <llvm/IR/Argument.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

#include <llvm/ADT/SmallVector.h>

namespace mono {

namespace detail {

/// The return registers of one file, and how many of them an aggregate's
/// leaves have claimed so far.
struct ReturnRegisters {
	static constexpr unsigned integer_count = 3; ///< RAX, RDX, RCX
	static constexpr unsigned sse_count = 4;     ///< XMM0 - XMM3

	unsigned integer = 0;
	unsigned sse = 0;
	bool unclassifiable = false;

	bool exhausted () const
	{
		return unclassifiable || integer > integer_count || sse > sse_count;
	}
};

/// Count the return registers T's leaves claim into USE.
///
/// LLVM flattens an aggregate return into its scalar leaves and gives each leaf
/// a register of its own, so what decides the demotion is how many leaves there
/// are and which file each rides in - not the aggregate's size. Padding counts:
/// the flattening is over the IR type, and that is where the translator spelled
/// the padding out.
inline void
count_return_registers (llvm::Type *t, ReturnRegisters &use)
{
	if (use.exhausted ())
		return;

	if (auto *st = llvm::dyn_cast<llvm::StructType> (t)) {
		for (llvm::Type *element : st->elements ())
			count_return_registers (element, use);
		return;
	}

	if (auto *at = llvm::dyn_cast<llvm::ArrayType> (t)) {
		ReturnRegisters one;

		count_return_registers (at->getElementType (), one);
		use.unclassifiable |= one.unclassifiable;
		use.integer += one.integer * at->getNumElements ();
		use.sse += one.sse * at->getNumElements ();
		return;
	}

	if (t->isPointerTy ()) {
		use.integer++;
	} else if (t->isIntegerTy ()) {
		/* A wider integer legalizes into as many machine words. */
		use.integer += (t->getIntegerBitWidth () + 63) / 64;
	} else if (t->isFloatTy () || t->isDoubleTy () || t->isVectorTy ()) {
		use.sse++;
	} else {
		use.unclassifiable = true;
	}
}

} // namespace detail

/// Whether a return of TYPE is handed back through a hidden pointer rather than
/// in the return registers.
inline bool
returns_by_hidden_pointer (llvm::Type *type)
{
	/* Only an aggregate is ever flattened; everything else has its own register. */
	if (!type->isStructTy () && !type->isArrayTy ())
		return false;

	detail::ReturnRegisters use;

	detail::count_return_registers (type, use);
	return use.exhausted ();
}

/// What F's hidden return pointer points at, or null when F returns its value
/// the ordinary way.
inline llvm::Type *
hidden_return_type (const llvm::Function *f)
{
	return f->getParamStructRetType (0);
}

/// The hidden return pointer F is entered with, or null.
inline llvm::Argument *
hidden_return_pointer (llvm::Function *f)
{
	return hidden_return_type (f) != nullptr ? f->getArg (0) : nullptr;
}

/// TYPE with its hidden return pointer folded back into the return, which is the
/// shape the signature described before this convention took it apart.
inline llvm::FunctionType *
natural_prototype (llvm::FunctionType *type, llvm::Type *hidden)
{
	llvm::SmallVector<llvm::Type *, 8> params (type->param_begin () + 1,
	                                           type->param_end ());

	return llvm::FunctionType::get (hidden, params, type->isVarArg ());
}

/// TYPE with the hidden pointer a return of HIDDEN travels through spelled out
/// as its leading parameter.
inline llvm::FunctionType *
hidden_return_prototype (llvm::FunctionType *type, llvm::Type *hidden)
{
	llvm::SmallVector<llvm::Type *, 8> params;

	params.push_back (llvm::PointerType::get (hidden->getContext (), 0));
	params.append (type->param_begin (), type->param_end ());

	return llvm::FunctionType::get (llvm::Type::getVoidTy (hidden->getContext ()), params,
	                                type->isVarArg ());
}

/// The attributes the hidden return pointer carries: what it points at, and that
/// it is the only name for that memory the callee is given.
inline llvm::AttributeSet
hidden_return_attributes (llvm::LLVMContext &ctx, llvm::Type *hidden)
{
	llvm::AttrBuilder ab (ctx);

	ab.addStructRetAttr (hidden);
	ab.addAttribute (llvm::Attribute::NoAlias);
	return llvm::AttributeSet::get (ctx, ab);
}

} // namespace mono

#endif /* MONO_LLVM_HIDDEN_RETURN_HPP */
