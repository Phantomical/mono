#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/remoting.h"
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace mono {

/*
 * III.4.21  newobj - create a new object
 *
 *   Format     Assembly Format   Description
 *   73 <T>     newobj ctor       Allocate an uninitialized object or value type and
 *                                call ctor.
 *
 * Stack Transition:
 *
 *   ..., arg1, ... argN -> ..., obj
 *
 * Description:
 *
 *   The newobj instruction creates a new object or a new instance of a value type.
 *   ctor is a metadata token (a methodref or methodef that shall be marked as a
 *   constructor; see Partition II) that indicates the name, class, and signature of
 *   the constructor to call. If a constructor exactly matching the indicated name,
 *   class and signature cannot be found, MissingMethodException is thrown.
 *
 *   The newobj instruction allocates a new instance of the class associated with
 *   ctor and initializes all the fields in the new instance to 0 (of the proper
 *   type) or null as appropriate. It then calls the constructor with the given
 *   arguments along with the newly created instance. After the constructor has been
 *   called, the now initialized object reference is pushed on the stack.
 *
 *   From the constructor's point of view, the uninitialized object is argument 0 and
 *   the other arguments passed to newobj follow in order.
 *
 *   All zero-based, one-dimensional arrays are created using newarr, not newobj. On
 *   the other hand, all other arrays (more than one dimension, or one-dimensional
 *   but not zero-based) are created using newobj.
 *
 *   Value types are not usually created using newobj. They are usually allocated
 *   either as arguments or local variables, using newarr (for zero-based,
 *   one-dimensional arrays), or as fields of objects. Once allocated, they are
 *   initialized using initobj. However, the newobj instruction can be used to create
 *   a new instance of a value type on the stack, that can then be passed as an
 *   argument, stored in a local, etc.
 *
 * Exceptions:
 *
 *   System.InvalidOperationException is thrown if ctor's class is abstract.
 *
 *   System.MethodAccessException is thrown if ctor is inaccessible.
 *
 *   System.OutOfMemoryException is thrown if there is insufficient memory to satisfy
 *   the request.
 *
 *   System.MissingMethodException is thrown if a constructor method with the
 *   indicated name, class, and signature could not be found. This is typically
 *   detected when CIL is converted to native code, rather than at runtime.
 *
 * Correctness:
 *
 *   Correct CIL ensures that ctor is a valid methodref or methoddef token, and that
 *   the arguments on the stack are assignable-to (§I.8.7.3) the parameters of the
 *   constructor.
 */
llvm::Error
MethodLLVMEmitter::emit_newobj (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoMethod *> target = resolve_method (token);
	if (!target)
		return target.takeError ();

	MonoMethodSignature *sig = mono_method_signature_internal (*target);

	if (sig == nullptr)
		return invalid_il ("the constructor has no signature");
	if (!sig->hasthis || strcmp ((*target)->name, ".ctor") != 0)
		return invalid_il ("newobj needs an instance constructor");

	MonoClass *klass = (*target)->klass;

	/* Multi-dimensional and non-zero-based arrays construct through newobj. */
	if (m_class_get_rank (klass) != 0)
		return emit_array_newobj (builder, *target, sig);

	MonoMethod *ctor = *target;

#ifndef DISABLE_REMOTING
	/*
	 * Allocating a context-bound class hands back a transparent proxy, and the
	 * construction has to be remoted through it - that is what activates the
	 * object and gives the proxy its identity; running the constructor's body
	 * directly on the proxy would just scribble on it. The with-check wrapper
	 * remotes exactly the proxy case, and on the real object every other
	 * MarshalByRefObject-derived class allocates, it falls through to the
	 * plain constructor call.
	 */
	if (!m_class_is_valuetype (klass) && mono_class_is_marshalbyref (klass)) {
		ERROR_DECL (wrap_error);
		MonoMethod *checked =
			mono_marshal_get_remoting_invoke_with_check (ctor, wrap_error);

		if (!is_ok (wrap_error))
			return runtime_error (wrap_error);
		ctor = checked;
	}
#endif

	/* A synchronized constructor holds its lock over the body, not the allocation. */
	ctor = synchronized_target (ctor);

	llvm::Expected<llvm::Function *> declaration = create_method_decl (ctor);
	if (!declaration)
		return declaration.takeError ();

	size_t count = sig->param_count;

	if (stack.size () < count)
		return unbalanced_stack (count);

	/*
	 * The constructor sees the fresh instance as argument 0 and the operands
	 * follow, so the stack unwinds into positions 1..N.
	 */
	std::vector<llvm::Value *> args (count + 1);

	for (size_t i = 0; i < count; ++i) {
		llvm::Expected<llvm::Value *> converted =
			coerce_to_argument (builder, get_stack (count - 1 - i), sig->params[i]);

		if (!converted)
			return converted.takeError ();
		args[i + 1] = *converted;
	}

	MonoType *pushed = m_class_get_byval_arg (klass);
	llvm::Value *created = nullptr;
	llvm::AllocaInst *temp = nullptr;
	llvm::Type *slot = nullptr;
	llvm::Align align = type_alignment (pushed);

	/*
	 * A string cannot be allocated before its length is known, so its constructor
	 * is a creator: it builds the string rather than filling one in. There is
	 * nothing to allocate here - the object arrives from the call.
	 */
	if ((*target)->string_ctor) {
		llvm::Expected<llvm::Value *> created =
			emit_creator (builder, ctor, llvm::ArrayRef (args).drop_front ());

		if (!created)
			return created.takeError ();

		pop_stack (count);
		push_stack (*created, pushed);
		return llvm::Error::success ();
	}

	if (m_class_is_valuetype (klass)) {
		/*
		 * A value type constructs in place, so the instance is a zeroed slot
		 * and what gets pushed afterwards is the value read back out of it.
		 */
		llvm::Expected<llvm::Type *> type = convert_type (pushed);
		if (!type)
			return type.takeError ();

		MonoIrBuilder entry (entry_block, entry_block->begin ());

		slot = *type;
		temp = entry.CreateAlloca (slot);
		temp->setAlignment (align);
		builder.CreateMemSet (temp, builder.getInt8 (0),
		                      mono_class_value_size (klass, NULL), align);
		args[0] = temp;
	} else {
		llvm::Expected<llvm::Function *> allocate = object_new_decl ();

		if (!allocate)
			return allocate.takeError ();

		created = emit_protected_call (
			builder, *allocate,
			adapt_to_callee (builder, *allocate,
		                         {class_symbol (klass, "mono_vtable_")}));
		args[0] = created;
	}

	emit_protected_call (builder, *declaration, args);
	pop_stack (count);

	if (temp != nullptr)
		push_stack (builder.CreateAlignedLoad (slot, temp, align), pushed);
	else
		push_stack (created, pushed);

	return llvm::Error::success ();
}

namespace {

/// Mark CALLEE as the GC allocation it is, with the same reasoning
/// mono_array_new_specific's declaration documents: NoAlias and nothing more -
/// an allockind would let LLVM delete an unused allocation whose failure is
/// still a catchable OutOfMemoryException, and a Zeroed claim would fold loads
/// of the initialized header to zero. Deliberately not nounwind.
llvm::FunctionCallee
mark_gc_allocator (llvm::FunctionCallee callee)
{
	if (auto *function = llvm::dyn_cast<llvm::Function> (callee.getCallee ()))
		function->addRetAttr (llvm::Attribute::NoAlias);

	return callee;
}

} // namespace

/// The array shapes of newobj: rank above one, or explicit lower bounds. The
/// metadata constructor has no body - the runtime's array-new icalls implement it,
/// keyed by the constructor's method so they can recover the array class.
llvm::Error
MethodLLVMEmitter::emit_array_newobj (MonoIrBuilder &builder, MonoMethod *ctor,
                                      MonoMethodSignature *sig)
{
	MonoClass *klass = ctor->klass;
	size_t rank = m_class_get_rank (klass);
	size_t count = sig->param_count;

	if (stack.size () < count)
		return unbalanced_stack (count);
	if (count != rank && count != 2 * rank)
		return invalid_il ("an array constructor takes a length, or a bound and "
		                   "a length, per dimension");

	llvm::LLVMContext &ctx = context ();
	llvm::Type *word = llvm::Type::getIntNTy (ctx, TARGET_SIZEOF_VOID_P * 8);

	std::vector<llvm::Value *> operands (count);

	for (size_t i = 0; i < count; ++i) {
		llvm::Expected<llvm::Value *> converted =
			coerce_to_argument (builder, get_stack (count - 1 - i), sig->params[i]);

		if (!converted)
			return converted.takeError ();
		operands[i] = *converted;
	}

	bool int32_lengths = std::all_of (sig->params, sig->params + count,
	                                  [] (MonoType *p) { return p->type == MONO_TYPE_I4; });
	llvm::Value *result;

	if (count == rank && count <= 4 && int32_lengths) {
		/* One int32 length per dimension, few enough of them: the direct icalls. */
		constexpr MonoJitICallId by_rank[] = {
			MONO_JIT_ICALL_mono_array_new_1,
			MONO_JIT_ICALL_mono_array_new_2,
			MONO_JIT_ICALL_mono_array_new_3,
			MONO_JIT_ICALL_mono_array_new_4,
		};
		llvm::Expected<llvm::Function *> allocate =
			icall_wrapper_decl (by_rank[count - 1]);

		if (!allocate)
			return allocate.takeError ();
		mark_gc_allocator (*allocate);

		std::vector<llvm::Value *> args (count + 1);

		args[0] = method_symbol (ctor);
		std::copy (operands.begin (), operands.end (), args.begin () + 1);
		result = emit_protected_call (builder, *allocate,
		                              adapt_to_callee (builder, *allocate, args));
	} else {
		/*
		 * mono_array_new_n_icall wants the lower bounds first and the lengths
		 * after, while the constructor interleaves them per dimension - so a
		 * (bound, length) list deinterleaves on the way into the buffer.
		 */
		MonoIrBuilder entry (entry_block, entry_block->begin ());
		llvm::Type *buffer_type = llvm::ArrayType::get (word, count);
		llvm::AllocaInst *buffer = entry.CreateAlloca (buffer_type);

		buffer->setAlignment (llvm::Align (TARGET_SIZEOF_VOID_P));

		for (size_t i = 0; i < count; ++i) {
			bool is_bound = count == 2 * rank && i % 2 == 0;
			size_t slot = count == 2 * rank ? (is_bound ? i / 2 : rank + i / 2)
			                                : i;

			/* Bounds are signed; lengths are not. */
			builder.CreateAlignedStore (
				builder.CreateIntCast (operands[i], word, is_bound),
				builder.CreateConstGEP2_32 (buffer_type, buffer, 0,
			                                    static_cast<unsigned> (slot)),
				llvm::Align (TARGET_SIZEOF_VOID_P));
		}

		llvm::Expected<llvm::Function *> allocate =
			icall_wrapper_decl (MONO_JIT_ICALL_mono_array_new_n_icall);

		if (!allocate)
			return allocate.takeError ();
		mark_gc_allocator (*allocate);

		result = emit_protected_call (
			builder, *allocate,
			adapt_to_callee (builder, *allocate,
		                         {method_symbol (ctor),
		                          builder.getInt32 (static_cast<uint32_t> (count)),
		                          buffer}));
	}

	pop_stack (count);
	push_stack (result, m_class_get_byval_arg (klass));
	return llvm::Error::success ();
}

} // namespace mono
