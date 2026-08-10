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
 * Spelling the pointer out in the IR instead - a void prototype with an `sret`
 * parameter - is what makes the return tail-callable, because a tail site can
 * then forward the caller's *own* incoming pointer, which lives in an ancestor
 * frame and outlives the jump. X86 recognises exactly that shape
 * (mayBeSRetTailCallCompatible) and still refuses a forwarded local.
 *
 * The pointer sits *behind* the first argument, at parameter 1, and takes
 * parameter 0 only when the signature has no argument at all. That is what
 * leaves the receiver in the first argument register, where the runtime's
 * trampolines insist on finding it - mono_arch_get_this_arg_from_call and the
 * unbox trampoline both read it straight out of ARG_REG1. Arity is only the
 * mechanism; the invariant behind it is that every signature something recovers
 * a receiver from has that receiver as its first argument, and a receiver is
 * always integer-class.
 *
 * Deciding it by arity rather than by whether the signature has a `this` is
 * what keeps one LLVM prototype from meaning two different things. A static
 * method taking one pointer and an instance method taking none are both
 * `void (ptr, ptr)`; if the index came from the signature they would disagree
 * about which pointer is the return slot, and a tail call between them - which
 * matching_call_abi () would wave through, since it compares the prototype -
 * would put it in the wrong register. It also keeps a declaration and the
 * wrapper standing behind it from disagreeing: a multidimensional array
 * accessor carries both `pinvoke` and `hasthis` while its wrapper carries only
 * `hasthis`, so any flavor-derived rule reads the two ends differently.
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

/// Which parameter of a prototype of COUNT parameters is the hidden return
/// pointer, given that it has one. The one place the position is decided.
inline unsigned
hidden_return_index (unsigned count)
{
	return count > 1 ? 1 : 0;
}

/// What F's hidden return pointer points at, or null when F returns its value
/// the ordinary way.
///
/// Only parameters 0 and 1 are asked: `sret` anywhere else is IR the verifier
/// rejects outright, so a third position would fail loudly rather than read here
/// as "no hidden return".
inline llvm::Type *
hidden_return_type (const llvm::Function *f)
{
	return f->getParamStructRetType (hidden_return_index (f->arg_size ()));
}

/// The hidden return pointer F is entered with, or null.
inline llvm::Argument *
hidden_return_pointer (llvm::Function *f)
{
	return hidden_return_type (f) != nullptr
	               ? f->getArg (hidden_return_index (f->arg_size ()))
	               : nullptr;
}

/// Where natural argument I lands in a prototype whose hidden return pointer is
/// at HIDDEN, which is past the end when there is none.
///
/// Not an offset: with the pointer at parameter 1, argument 0 sits in front of
/// it and everything after it is shifted by one.
inline unsigned
natural_parameter_index (unsigned i, unsigned hidden)
{
	return i + (i >= hidden ? 1 : 0);
}

/// Where natural argument I lands in F.
inline unsigned
natural_parameter_index (unsigned i, const llvm::Function *f)
{
	if (hidden_return_type (f) == nullptr)
		return i;
	return natural_parameter_index (i, hidden_return_index (f->arg_size ()));
}

/// TYPE with its hidden return pointer folded back into the return, which is the
/// shape the signature described before this convention took it apart.
inline llvm::FunctionType *
natural_prototype (llvm::FunctionType *type, llvm::Type *hidden)
{
	llvm::SmallVector<llvm::Type *, 8> params (type->param_begin (), type->param_end ());

	params.erase (params.begin () + hidden_return_index (type->getNumParams ()));
	return llvm::FunctionType::get (hidden, params, type->isVarArg ());
}

/// TYPE with the hidden pointer a return of HIDDEN travels through spelled out
/// as a parameter, behind the first argument.
inline llvm::FunctionType *
hidden_return_prototype (llvm::FunctionType *type, llvm::Type *hidden)
{
	llvm::SmallVector<llvm::Type *, 8> params (type->param_begin (), type->param_end ());

	params.insert (params.begin () + hidden_return_index (params.size () + 1),
	               llvm::PointerType::get (hidden->getContext (), 0));

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
