#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Type.h>

namespace mono {

/*
 * III.4.3  castclass - cast an object to a class
 *
 *   Format     Assembly Format     Description
 *   74 <T>     castclass typeTok   Cast obj to typeTok.
 *
 * Stack Transition:
 *
 *   ..., obj -> ..., obj2
 *
 * Description:
 *
 *   typeTok is a metadata token (a typeref, typedef or typespec), indicating the
 *   desired class. If typeTok is a non-nullable value type or a generic parameter
 *   type it is interpreted as "boxed" typeTok. If typeTok is a nullable type,
 *   Nullable<T>, it is interpreted as "boxed" T.
 *
 *   The castclass instruction determines if obj (of type O) is an instance of the
 *   type typeTok, termed "casting".
 *
 *   If the actual type (not the verifier tracked type) of obj is
 *   verifier-assignable-to the type typeTok the cast succeeds and obj (as obj2) is
 *   returned unchanged while verification tracks its type as typeTok.
 *
 *   Unlike coercions (§III.1.6) and conversions (§III.3.27), a cast never changes the
 *   actual type of an object and preserves object identity (see Partition I).
 *
 *   If the cast fails then an InvalidCastException is thrown.
 *
 *   If obj is null, castclass succeeds and returns null. This behavior differs
 *   semantically from isinst where if obj is null, isinst fails and returns null.
 *
 * Exceptions:
 *
 *   System.InvalidCastException is thrown if obj cannot be cast to typeTok.
 *
 *   System.TypeLoadException is thrown if typeTok cannot be found. This is typically
 *   detected when CIL is converted to native code rather than at runtime.
 *
 * Correctness:
 *
 *   Correct CIL ensures that typeTok is a valid typeRef, typeDef or typeSpec token,
 *   and that obj is always either null or an object reference.
 *
 * Verifiability:
 *
 *   Verification tracks the type of obj2 as typeTok.
 *
 *
 * III.4.6  isinst - test if an object is an instance of a class or interface
 *
 *   Format     Assembly Format   Description
 *   75 <T>     isinst typeTok    Test if obj is an instance of typeTok, returning
 *                                null or an instance of that class or interface.
 *
 * Stack Transition:
 *
 *   ..., obj -> ..., result
 *
 * Description:
 *
 *   typeTok is a metadata token (a typeref, typedef or typespec), indicating the
 *   desired class. If typeTok is a non-nullable value type or a generic parameter
 *   type it is interpreted as "boxed" typeTok. If typeTok is a nullable type,
 *   Nullable<T>, it is interpreted as "boxed" T.
 *
 *   The isinst instruction tests whether obj (type O) is an instance of the type
 *   typeTok.
 *
 *   If the actual type (not the verifier tracked type) of obj is
 *   verifier-assignable-to the type typeTok then isinst succeeds and obj (as result)
 *   is returned unchanged while verification tracks its type as typeTok. Unlike
 *   coercions (§III.1.6) and conversions (§III.3.27), isinst never changes the actual
 *   type of an object and preserves object identity (see Partition I).
 *
 *   If obj is null, or obj is not verifier-assignable-to the type typeTok, isinst
 *   fails and returns null.
 *
 * Exceptions:
 *
 *   System.TypeLoadException is thrown if typeTok cannot be found. This is typically
 *   detected when CIL is converted to native code rather than at runtime.
 *
 * Correctness:
 *
 *   Correct CIL ensures that typeTok is a valid typeref or typedef or typespec token,
 *   and that obj is always either null or an object reference.
 *
 * Verifiability:
 *
 *   Verification tracks the type of result as typeTok.
 */
llvm::Error
MethodLLVMEmitter::emit_cast (MonoIrBuilder &builder, uint32_t token, bool throw_on_fail)
{
	llvm::Expected<MonoType *> type = element_type_from_token (token);
	if (!type)
		return type.takeError ();

	MonoClass *klass = mono_class_from_mono_type_internal (*type);

	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue obj = get_stack (0);
	StackType obj_type = stack_type (obj.type);

	if (obj_type != ObjectRef)
		return invalid_il (llvm::Twine ("a cast is not defined for operand type ")
		                   + describe (obj.type, obj_type));

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Type *word = builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);
	llvm::Constant *null = llvm::ConstantPointerNull::get (
		llvm::cast<llvm::PointerType> (ptr));

	llvm::Expected<llvm::Function *> test = icall_wrapper_decl (
		throw_on_fail ? MONO_JIT_ICALL_mono_object_castclass_with_cache
	                      : MONO_JIT_ICALL_mono_object_isinst_with_cache);

	if (!test)
		return test.takeError ();

	/*
	 * Each site owns a cache slot that holds the vtable which last answered
	 * for it, and the answer is read here rather than in the wrapper. A hit
	 * is two loads and a comparison. A miss pays the call.
	 *
	 * The two forms encode the slot differently, so they cannot share one
	 * test. mono_object_castclass_with_cache () keeps the bare vtable of an
	 * object that passed. mono_object_isinst_with_cache () keeps the vtable
	 * with bit 0 set when the answer was no, which is how it caches a
	 * refusal. Both are in mono/mini/jit-icalls.c.
	 *
	 * A read that races with the wrapper's write costs a miss and no more.
	 * The wrapper writes a vtable only after the test answered for this
	 * class, so a hit stays correct however old the value is. It never
	 * writes the vtable of a transparent proxy, whose answer can change.
	 */
	llvm::BasicBlock *from_null = builder.GetInsertBlock ();
	llvm::BasicBlock *probe = llvm::BasicBlock::Create (context (), "cast_probe", function);
	llvm::BasicBlock *hit = llvm::BasicBlock::Create (context (), "cast_hit", function);
	llvm::BasicBlock *miss = llvm::BasicBlock::Create (context (), "cast_miss", function);
	llvm::BasicBlock *done = llvm::BasicBlock::Create (context (), "cast_done", function);

	// Both forms answer null for a null reference and neither one reads the
	// cache for it. The probe loads the vtable, so this test comes first.
	builder.CreateCondBr (builder.CreateIsNull (obj.value), done, probe);

	builder.SetInsertPoint (probe);

	llvm::Value *cache = nullptr;

	if (depends_on_context (klass)) {
		// A cached vtable only answers for the class the site tests against,
		// so one instantiation must not read another's cache. The context
		// holds a slot for each of them.
		llvm::Expected<llvm::Value *> own =
			rgctx_fetch (builder, MONO_RGCTX_INFO_CAST_CACHE, klass);

		if (!own)
			return own.takeError ();

		cache = *own;
	} else {
		cache = new llvm::GlobalVariable (
			*module, ptr, false, llvm::GlobalValue::InternalLinkage, null,
			"cast_cache");
	}

	llvm::Value *cached = builder.CreateAlignedLoad (
		ptr, cache, llvm::Align (TARGET_SIZEOF_VOID_P), "cached_vtable");
	llvm::Value *vtable = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), obj.value,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, vtable))),
		llvm::Align (TARGET_SIZEOF_VOID_P), "obj_vtable");

	llvm::Value *cached_word = builder.CreatePtrToInt (cached, word);
	llvm::Value *key = throw_on_fail
		? cached_word
		: builder.CreateAnd (cached_word,
	                             llvm::ConstantInt::get (word, ~(uint64_t) 1));

	builder.CreateCondBr (
		builder.CreateICmpEQ (key, builder.CreatePtrToInt (vtable, word)), hit, miss);

	builder.SetInsertPoint (hit);

	llvm::Value *answer = obj.value;

	if (!throw_on_fail)
		// Bit 0 records that this vtable failed the test here before.
		answer = builder.CreateSelect (
			builder.CreateTrunc (cached_word, builder.getInt1Ty ()), null,
			obj.value);

	builder.CreateBr (done);

	builder.SetInsertPoint (miss);

	llvm::Expected<llvm::Value *> tested = class_operand (builder, klass, "mono_class_");

	if (!tested)
		return tested.takeError ();

	// Castclass reports a failed cast as a pending InvalidCastException. Only
	// the wrapper's check after the call turns that into a throw.
	llvm::Value *slow = emit_protected_call (
		builder, *test,
		adapt_to_callee (builder, *test, {obj.value, *tested, cache}));

	// A protected call inside a clause is an invoke, and translation goes on
	// in the block its normal edge reaches. That block is the one the phi
	// takes this value from, which is not always the block the call sits in.
	llvm::BasicBlock *from_miss = builder.GetInsertBlock ();

	builder.CreateBr (done);
	builder.SetInsertPoint (done);

	llvm::PHINode *result = builder.CreatePHI (ptr, 3, "cast_result");

	result->addIncoming (null, from_null);
	result->addIncoming (answer, hit);
	result->addIncoming (slow, from_miss);

	// A value-type token means the boxed form. What comes back is still an
	// object reference, not the token's own type.
	pop_stack (1);
	push_stack (result, m_class_is_valuetype (klass) ? mono_get_object_type ()
	                                                 : m_class_get_byval_arg (klass));
	return llvm::Error::success ();
}

llvm::Error
MethodLLVMEmitter::emit_castclass (MonoIrBuilder &builder, uint32_t token)
{
	return emit_cast (builder, token, true);
}

llvm::Error
MethodLLVMEmitter::emit_isinst (MonoIrBuilder &builder, uint32_t token)
{
	return emit_cast (builder, token, false);
}

} // namespace mono
