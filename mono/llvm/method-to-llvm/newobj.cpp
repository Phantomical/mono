#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/class.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/remoting.h"
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace mono {

/// Fails with a TypeLoadException when the delegate class has no Invoke method,
/// or when Invoke's signature will not resolve, which is what a missing type in
/// that signature looks like.
///
/// The runtime builds a delegate's invoke trampoline from Invoke's signature and
/// crashes rather than reporting an error on either of those, so the check has
/// to happen before the call runs. klass must be a delegate class; nothing here
/// tests that.
llvm::Error
MethodLLVMEmitter::check_delegate_invoke (MonoClass *klass)
{
	ERROR_DECL (invoke_error);
	MonoMethod *invoke = mono_get_delegate_invoke_internal (klass);

	if (invoke == nullptr) {
		char *name = mono_type_get_full_name (klass);

		mono_error_set_type_load_class (invoke_error, klass,
		                                "Delegate %s has no Invoke method", name);
		g_free (name);
		return runtime_error (invoke_error);
	}

	if (mono_method_signature_checked (invoke, invoke_error) == nullptr)
		return runtime_error (invoke_error);

	return llvm::Error::success ();
}

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

	if (checks_accessibility () && !mono_method_can_access_method (method, *target)) {
		if (llvm::Error error = emit_method_access_failure (builder, *target))
			return error;
	}

	MonoMethodSignature *sig = mono_method_signature_internal (*target);

	if (sig == nullptr)
		return invalid_il ("the constructor has no signature");
	if (!sig->hasthis || strcmp ((*target)->name, ".ctor") != 0)
		return invalid_il ("newobj needs an instance constructor");
	if (sig->call_convention == MONO_CALL_VARARG)
		return unsupported_il ("newobj on a vararg constructor");

	MonoClass *klass = (*target)->klass;

	if (m_class_get_parent (klass) == mono_defaults.multicastdelegate_class)
		if (llvm::Error error = check_delegate_invoke (klass))
			return error;

	if (m_class_get_rank (klass) != 0)
		return emit_array_newobj (builder, *target, sig);

	MonoMethod *ctor = *target;

#ifndef DISABLE_REMOTING
	/*
	 * Allocating a MarshalByRefObject-derived class can return a transparent
	 * proxy instead of a plain instance. The construction must go through
	 * that proxy, because that step activates the object and gives the proxy
	 * its identity. Calling the constructor directly on the proxy scribbles
	 * on it instead of activating it. The with-check wrapper remotes exactly
	 * that proxy case. For every other MarshalByRefObject-derived class,
	 * allocation returns the real object, and the wrapper falls through to
	 * the plain constructor call.
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

	// A synchronized constructor holds its lock over the body, not the allocation.
	ctor = synchronized_target (ctor);

	/*
	 * The runtime answers some constructors with a marshalling wrapper: every
	 * string constructor, and a handful of other internal-call ones. This
	 * backend enters the constructor at that wrapper and compiles it like any
	 * other method. The string-constructor path below asks for the same
	 * retarget on its own.
	 */
	ctor = icall_wrapper_target (ctor);

	bool ctor_by_context = calls_through_context (ctor);
	llvm::Expected<llvm::Function *> declaration =
		create_method_decl (ctor, ctor_by_context);
	if (!declaration)
		return declaration.takeError ();

	size_t count = sig->param_count;

	if (stack.size () < count)
		return unbalanced_stack (count);

	// The constructor sees the fresh instance as argument 0. The stack's
	// operands unwind into positions 1 through count.
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
	 * A string's length is not known before its constructor runs, so nothing
	 * can allocate it in advance. Its constructor builds and returns the
	 * string itself, so this code allocates nothing here.
	 */
	if ((*target)->string_ctor) {
		llvm::Expected<llvm::Value *> created =
			emit_string_constructor (builder, ctor, llvm::ArrayRef (args).drop_front ());

		if (!created)
			return created.takeError ();

		pop_stack (count);
		push_stack (*created, pushed);
		return llvm::Error::success ();
	}

	if (m_class_is_valuetype (klass)) {
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
		// An abstract class has no instances. Neither the allocator nor the
		// constructor it runs refuses one, so the refusal must happen here.
		if (mono_class_get_flags (klass) & TYPE_ATTRIBUTE_ABSTRACT) {
			char *name = mono_type_get_full_name (klass);
			ERROR_DECL (error);

			mono_error_set_member_access (error, "Cannot create an abstract class: %s",
			                              name);
			g_free (name);
			return runtime_error (error);
		}

		/*
		 * The constructor runs the type initializer at its own entry, but
		 * allocation happens first. For a finalizable class that is too late:
		 * the allocator registers the object for finalization before the cctor
		 * gets a chance to throw. The constructor then unwinds, and nothing
		 * ever holds the instance. At shutdown, mono_gc_run_finalize reruns the
		 * class-init check on the finalizer thread. That check rethrows the
		 * cached TypeInitializationException, and the finalizer thread has
		 * nobody to catch it. Running class init before allocation keeps that
		 * orphan from existing at all.
		 */
		if (mono_class_needs_cctor_run (klass, method))
			if (llvm::Error error = emit_class_init (builder, klass))
				return error;

		llvm::Expected<llvm::Value *> allocated =
			emit_object_alloc (builder, klass, false);

		if (!allocated)
			return allocated.takeError ();

		created = *allocated;
		args[0] = created;
	}

	llvm::FunctionCallee run = *declaration;
	bool keyed = pass_context_to (*declaration, args);

	// The instantiation compiles its own constructor, so the entry to call
	// comes out of the context like any other piece of metadata.
	if (ctor_by_context) {
		llvm::Expected<llvm::Value *> code =
			rgctx_fetch (builder, MONO_RGCTX_INFO_GENERIC_METHOD_CODE, ctor);

		if (!code)
			return code.takeError ();

		run = llvm::FunctionCallee ((*declaration)->getFunctionType (), *code);
	}

	emit_protected_call (builder, run, args, [&] (llvm::CallBase *site) {
		if (keyed)
			site->addParamAttr (site->arg_size () - 1, llvm::Attribute::Nest);
		carry_parameter_extensions (site, *declaration);
	});
	pop_stack (count);

	if (temp == nullptr) {
		// The object has the class this opcode named, so a virtual call on it
		// while it is still this value needs no dispatch.
		allocated_here.insert (created);
		push_stack (created, pushed);
	} else if (held_in_memory (pushed)) {
		push_stack (temp, pushed);
	} else {
		push_stack (builder.CreateAlignedLoad (slot, temp, align), pushed);
	}

	return llvm::Error::success ();
}

namespace {

llvm::FunctionCallee
mark_gc_allocator (llvm::FunctionCallee callee)
{
	if (auto *function = llvm::dyn_cast<llvm::Function> (callee.getCallee ()))
		function->addRetAttr (llvm::Attribute::NoAlias);

	return callee;
}

/// Where operand i of an array constructor goes in mono_array_new_n_icall's
/// buffer, and whether that operand is a lower bound.
///
/// The icall wants every lower bound first and every length after them. A
/// constructor that takes both interleaves them instead, as a (bound, length)
/// pair for each dimension, and that is the shape count == 2 * rank names. A
/// constructor that takes lengths alone passes them straight through.
std::pair<size_t, bool>
bound_or_length_slot (size_t i, size_t count, size_t rank)
{
	if (count != 2 * rank)
		return { i, false };

	bool is_bound = i % 2 == 0;

	return { is_bound ? i / 2 : rank + i / 2, is_bound };
}

} // namespace

/// Handles every array shape that constructs through newobj: rank above one,
/// explicit lower bounds, and the vector that newarr also writes. The metadata
/// constructor has no body. A vector allocates the way newarr does. Every other
/// shape goes to one of the runtime's array-new icalls, keyed by the
/// constructor's method so the icall can recover the array class.
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

	// A szarray constructor with one length builds the same object newarr does,
	// so it takes the same allocator. Its negative-length answer is
	// OverflowException either way.
	if (count == 1 && rank == 1 && int32_lengths
	    && m_class_get_byval_arg (klass)->type == MONO_TYPE_SZARRAY) {
		llvm::Expected<llvm::Value *> created =
			emit_vector_alloc (builder, klass, operands[0]);

		if (!created)
			return created.takeError ();
		result = *created;
	} else if (count == rank && count <= 4 && int32_lengths) {
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
		MonoIrBuilder entry (entry_block, entry_block->begin ());
		llvm::Type *buffer_type = llvm::ArrayType::get (word, count);
		llvm::AllocaInst *buffer = entry.CreateAlloca (buffer_type);

		buffer->setAlignment (llvm::Align (TARGET_SIZEOF_VOID_P));

		for (size_t i = 0; i < count; ++i) {
			auto [slot, is_bound] = bound_or_length_slot (i, count, rank);

			// Bounds are signed. Lengths are not.
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

	// The array holds the element class this constructor named, so a store
	// into it while it is still this value needs no covariance test.
	allocated_here.insert (result);
	push_stack (result, m_class_get_byval_arg (klass));
	return llvm::Error::success ();
}

} // namespace mono
