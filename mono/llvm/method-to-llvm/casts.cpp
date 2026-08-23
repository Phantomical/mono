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
/**
 * Whether `mono_class_has_parent ()` is the whole answer for a cast to klass.
 *
 * `mono_class_is_assignable_from_general ()` (`mono/metadata/class.c`) ends at
 * that call, and each branch above it either agrees or governs a shape refused
 * here: an interface reads the interface bitmap, an array and a delegate have
 * variance, and a pointer, a nullable and a generic argument each have a rule
 * of their own. A marshal-by-ref target never reaches that function at all;
 * the runtime answers it with `mono_object_handle_isinst_mbyref ()` instead,
 * so this refuses it too.
 *
 * The caller refuses a context-dependent class as well, because the test
 * compares against the class and against its depth as constants, and a shared
 * body knows neither while it is translated.
 */
static bool
subtype_test_applies (MonoClass *klass)
{
	MonoType *self = m_class_get_byval_arg (klass);

	if (mono_class_is_interface (klass) || m_class_get_marshalbyref (klass)
	    || m_class_get_rank (klass) != 0 || m_class_is_valuetype (klass)
	    || mono_class_is_nullable (klass) || m_class_is_delegate (klass)
	    || m_class_get_class_kind (klass) == MONO_CLASS_POINTER
	    || self->type == MONO_TYPE_VAR || self->type == MONO_TYPE_MVAR)
		return false;

	// The depth reached below indexes the object's supertypes, and the
	// runtime writes this one once and never changes it.
	mono_class_setup_supertypes (klass);

	return m_class_get_idepth (klass) > 0;
}

/**
 * Whether the interface bitmap is the whole answer for a cast to klass.
 *
 * `mono_object_handle_isinst_mbyref_raw ()` (`mono/metadata/object.c`) reads
 * the same bitmap off the same vtable first, and the two branches below that
 * read only add a yes: an array special interface, and a variant generic
 * interface. A transparent proxy takes the bitmap first as well, and
 * `mono_upgrade_remote_class ()` is what puts the bit there.
 *
 * The caller refuses a context-dependent class as well, because the test
 * compares against the interface id as a constant, and a shared body knows
 * none while it is translated.
 */
static bool
interface_test_applies (MonoClass *klass)
{
#ifdef COMPRESSED_INTERFACE_BITMAP
	// A compressed bitmap holds runs of empty bytes rather than the bytes
	// themselves, so the constant index emit_interface_test () computes
	// reaches the wrong byte. mono_class_interface_match () walks it instead.
	return false;
#else
	// The id indexes the bitmap, and the runtime assigns one on demand.
	// Zero is never assigned, so an id of zero says the class has none yet.
	mono_class_setup_interface_id (klass);

	return m_class_get_interface_id (klass) != 0;
#endif
}

/**
 * Emits the inline half of a cast: branches to yes when the object's class has
 * klass among its supertypes, and to otherwise when this cannot tell.
 *
 * Leaves yes and otherwise unterminated. The caller fills in both.
 */
void
MethodLLVMEmitter::emit_subtype_test (MonoIrBuilder &builder, MonoClass *klass,
                                      llvm::Value *obj, llvm::Value *target,
                                      llvm::BasicBlock *yes,
                                      llvm::BasicBlock *otherwise)
{
	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	uint16_t depth = m_class_get_idepth (klass);

	llvm::Value *vtable = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), obj,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, vtable))),
		llvm::Align (TARGET_SIZEOF_VOID_P));
	llvm::Value *its_class = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), vtable,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, klass))),
		llvm::Align (TARGET_SIZEOF_VOID_P), "obj_class");

	/*
	 * The supertypes array holds one entry for each level down to the class
	 * itself, so a class shallower than klass cannot hold it and indexing at
	 * klass's depth would read past the end.
	 */
	llvm::Value *its_depth = builder.CreateAlignedLoad (
		builder.getInt16Ty (),
		builder.CreateGEP (builder.getInt8Ty (), its_class,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoClass, idepth))),
		llvm::Align (2), "obj_idepth");

	llvm::BasicBlock *deep_enough =
		llvm::BasicBlock::Create (context (), "cast_deep_enough", function);

	builder.CreateCondBr (
		builder.CreateICmpUGE (its_depth, builder.getInt16 (depth)),
		deep_enough, otherwise);

	builder.SetInsertPoint (deep_enough);

	llvm::Value *supertypes = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), its_class,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoClass, supertypes))),
		llvm::Align (TARGET_SIZEOF_VOID_P), "supertypes");
	llvm::Value *at_depth = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (ptr, supertypes, builder.getInt32 (depth - 1)),
		llvm::Align (TARGET_SIZEOF_VOID_P), "supertype");

	builder.CreateCondBr (builder.CreateICmpEQ (at_depth, target), yes, otherwise);
}

/**
 * Emits the inline half of a cast to an interface: branches to yes when the
 * object's vtable has the interface among the ones it implements, and to
 * otherwise when this cannot tell.
 *
 * Leaves yes and otherwise unterminated. The caller fills in both.
 *
 * This is `MONO_VTABLE_IMPLEMENTS_INTERFACE ()` (`class-internals.h`) as IR.
 * The vtable carries the bitmap and the bound both, so the object's vtable is
 * the only load in front of the test, and the target needs no operand.
 */
void
MethodLLVMEmitter::emit_interface_test (MonoIrBuilder &builder, MonoClass *klass,
                                        llvm::Value *obj, llvm::BasicBlock *yes,
                                        llvm::BasicBlock *otherwise)
{
	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	uint32_t iid = m_class_get_interface_id (klass);

	llvm::Value *vtable = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), obj,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, vtable))),
		llvm::Align (TARGET_SIZEOF_VOID_P), "obj_vtable");

	/*
	 * The bitmap holds one bit for each id up to the bound, so a bound below
	 * the target's id means the byte the test wants is past the end.
	 */
	llvm::Value *bound = builder.CreateAlignedLoad (
		builder.getInt32Ty (),
		builder.CreateGEP (builder.getInt8Ty (), vtable,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, max_interface_id))),
		llvm::Align (4), "max_interface_id");

	llvm::BasicBlock *in_range =
		llvm::BasicBlock::Create (context (), "cast_iface_in_range", function);

	builder.CreateCondBr (builder.CreateICmpUGE (bound, builder.getInt32 (iid)),
	                      in_range, otherwise);

	builder.SetInsertPoint (in_range);

	llvm::Value *bitmap = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), vtable,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, interface_bitmap))),
		llvm::Align (TARGET_SIZEOF_VOID_P), "interface_bitmap");
	llvm::Value *byte = builder.CreateAlignedLoad (
		builder.getInt8Ty (),
		builder.CreateGEP (builder.getInt8Ty (), bitmap, builder.getInt32 (iid >> 3)),
		llvm::Align (1), "interface_byte");
	llvm::Value *bit = builder.CreateAnd (byte, builder.getInt8 (1 << (iid & 7)));

	builder.CreateCondBr (builder.CreateIsNotNull (bit), yes, otherwise);
}

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

	/*
	 * In front of the cache, the test the runtime itself ends at. For a target
	 * class that is not an interface and not marshal-by-ref,
	 * mono_class_is_assignable_from_general () answers
	 * `mono_class_has_parent (object's class, target)`. For an interface,
	 * mono_object_handle_isinst_mbyref_raw () answers the bitmap on the
	 * object's vtable. Either one is a bounds check and one comparison, and
	 * either answers for every class rather than for the last one, so neither
	 * needs a slot and neither misses.
	 *
	 * Both are one-sided. Where one says yes the runtime says yes, and where
	 * one says no the answer can still be yes through a path they do not
	 * model, so a no falls through to the cache and then to the wrapper.
	 */
	bool to_interface = mono_class_is_interface (klass);
	llvm::BasicBlock *told_yes = nullptr;
	llvm::BasicBlock *first = probe;

	if (!depends_on_context (klass)
	    && (to_interface ? interface_test_applies (klass) : subtype_test_applies (klass))) {
		told_yes = llvm::BasicBlock::Create (context (), "cast_inline_yes", function);
		first = llvm::BasicBlock::Create (
			context (), to_interface ? "cast_interface" : "cast_subtype", function);

		builder.SetInsertPoint (first);

		if (to_interface) {
			emit_interface_test (builder, klass, obj.value, told_yes, probe);
		} else {
			llvm::Expected<llvm::Value *> target =
				class_operand (builder, klass, "mono_class_");

			if (!target)
				return target.takeError ();

			emit_subtype_test (builder, klass, obj.value, *target, told_yes, probe);
		}

		builder.SetInsertPoint (told_yes);
		builder.CreateBr (done);
	}

	// Both forms answer null for a null reference, and neither one reads the
	// vtable for it. The tests below do, so this one comes first.
	builder.SetInsertPoint (from_null);
	builder.CreateCondBr (builder.CreateIsNull (obj.value), done, first);

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

	llvm::PHINode *result = builder.CreatePHI (ptr, 4, "cast_result");

	result->addIncoming (null, from_null);
	result->addIncoming (answer, hit);
	result->addIncoming (slow, from_miss);

	if (told_yes != nullptr)
		result->addIncoming (obj.value, told_yes);

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
