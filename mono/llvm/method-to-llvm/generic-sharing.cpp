/**
 * \file
 * \brief What a body shared between reference instantiations reads out of its context.
 *
 * Every reference instantiation of a method compiles to the same instructions,
 * because a reference is one pointer whatever it points at: field offsets, array
 * element sizes and vtable slot indices all come out the same. What differs is
 * the metadata the body names - the MonoClass it casts to, the vtable it
 * allocates from, the statics block it reads. A shared body fetches each of
 * those from the runtime generic context of the instantiation that entered it.
 */

#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "sidetables.hpp"

#include "mini.h"
#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

char SharingRefusal::ID = 0;

void
MethodLLVMEmitter::cannot_share (const llvm::Twine &what)
{
	if (sharing_refusal.empty ())
		sharing_refusal = what.str ();
}

bool
MethodLLVMEmitter::depends_on_context (MonoClass *klass)
{
	/*
	 * A generic type definition carries its own type parameters, which are not
	 * this method's: typeof (List<>) is one object however the body it appears
	 * in was instantiated. mono_class_check_context_used () counts them all the
	 * same, and inflating one against this method's context would turn
	 * List<> into List<T>.
	 */
	if (mono_class_is_gtd (klass))
		return false;

	return sharing () && mono_class_check_context_used (klass) != 0;
}

bool
MethodLLVMEmitter::depends_on_context (MonoClassField *field)
{
	return depends_on_context (field->parent);
}

bool
MethodLLVMEmitter::depends_on_context (MonoMethod *target)
{
	MonoGenericContext *own = mini_method_get_context (target);

	if (own != nullptr && mono_generic_context_check_used (own) != 0)
		return sharing ();

	return depends_on_context (target->klass);
}

bool
MethodLLVMEmitter::calls_through_context (MonoMethod *target)
{
	if (!depends_on_context (target))
		return false;

	/*
	 * A wrapper is generated around one instantiation and carries no token the
	 * context could resolve, so mono's own entry for a method's code refuses
	 * one. The caller compiles concrete instead, and gets the wrapper it named.
	 */
	if (target->wrapper_type != MONO_WRAPPER_NONE) {
		cannot_share ("a call to a wrapper around an open method");
		return false;
	}

	return true;
}

void
MethodLLVMEmitter::open_sharing (MonoIrBuilder &builder)
{
	context_used = mono_method_check_context_used (method);

	if (!sharing ())
		return;

	if (args.empty () || !mono_method_signature_internal (method)->hasthis)
		return;

	/*
	 * The context comes out of the receiver: an object's vtable is its
	 * instantiation's, so a shared body that has one needs nothing from its
	 * caller. shared_form () refuses the methods with no receiver to read, so
	 * this is the only source a shared body has here.
	 *
	 * It is read once, in the prologue, so that one value dominates every fetch
	 * below it. A null receiver faults here rather than at whatever the body
	 * would have touched first. The two differ only for a body that reads the
	 * context without ever touching `this`, and a reference type's instance
	 * method is reached through callvirt, which has already checked.
	 */
	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Align align (TARGET_SIZEOF_VOID_P);
	llvm::Value *self = builder.CreateAlignedLoad (ptr, args[0].alloca, align);
	llvm::Value *slot = builder.CreateGEP (
		builder.getInt8Ty (), self,
		builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, vtable)));

	rgctx = builder.CreateAlignedLoad (ptr, slot, align);

	/*
	 * A stack walk that wants the instantiation this frame is running as reads
	 * the receiver out of the frame, because the jit info names the shared
	 * method. The marker both pins the slot - the intrinsic is neither a load
	 * nor a store, so mem2reg leaves it in memory - and gets codegen to resolve
	 * it against the laid-out frame, which is what jinfo.cpp reads back.
	 */
	builder.CreateIntrinsic (llvm::Intrinsic::experimental_stackmap, {},
	                         { builder.getInt64 (rgctx_stackmap_id),
	                           builder.getInt32 (0), args[0].alloca });
	pinned_receiver = true;
}

/// Which fill icall answers for a slot, and what the context it reads has to be.
///
/// A slot the class owns is filled from the class vtable, and one the method
/// owns from the MRGCTX its caller passed. MONO_RGCTX_SLOT_MAKE_MRGCTX marks
/// which of the two a slot index is.
static MonoJitICallId
fill_icall_for (uint32_t slot)
{
	return MONO_RGCTX_SLOT_IS_MRGCTX (slot) ? MONO_JIT_ICALL_mono_fill_method_rgctx
	                                        : MONO_JIT_ICALL_mono_fill_class_rgctx;
}

/// The patch mini_get_rgctx_entry_slot () reads \p data through, which is what
/// says whether the entry is about a class, a method or a field.
static MonoJumpInfoType
patch_kind_for (MonoRgctxInfoType info_type)
{
	switch (info_type) {
	case MONO_RGCTX_INFO_STATIC_DATA:
	case MONO_RGCTX_INFO_KLASS:
	case MONO_RGCTX_INFO_VTABLE:
	case MONO_RGCTX_INFO_CAST_CACHE:
		return MONO_PATCH_INFO_CLASS;
	case MONO_RGCTX_INFO_METHOD:
	case MONO_RGCTX_INFO_GENERIC_METHOD_CODE:
		return MONO_PATCH_INFO_METHODCONST;
	case MONO_RGCTX_INFO_CLASS_FIELD:
		return MONO_PATCH_INFO_FIELD;
	default:
		g_assert_not_reached ();
	}
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::rgctx_fetch (MonoIrBuilder &builder, MonoRgctxInfoType info_type,
                                void *data)
{
	if (rgctx == nullptr) {
		cannot_share ("a body with no receiver to read its generic context from");
		return llvm::ConstantPointerNull::get (llvm::PointerType::get (context (), 0));
	}

	MonoJumpInfo patch {};
	MonoJumpInfoRgctxEntry lookup {};

	patch.type = patch_kind_for (info_type);
	patch.data.target = data;

	lookup.d.klass = method->klass;
	lookup.in_mrgctx = FALSE;
	lookup.data = &patch;
	lookup.info_type = info_type;

	uint32_t slot = (uint32_t) mini_get_rgctx_entry_slot (&lookup);
	llvm::Expected<llvm::Function *> fill = icall_wrapper_decl (fill_icall_for (slot));

	if (!fill)
		return fill.takeError ();

	/*
	 * Filling a slot can create a vtable and run a class initializer, so this
	 * is a call rather than a load: it has to happen where the IL asked for the
	 * metadata, not where the body was entered.
	 */
	llvm::Value *info = emit_protected_call (
		builder, *fill,
		adapt_to_callee (builder, *fill,
	                         {rgctx, builder.getInt32 (MONO_RGCTX_SLOT_INDEX (slot))}));

	if (!info->getType ()->isPointerTy ())
		info = builder.CreateIntToPtr (info, llvm::PointerType::get (context (), 0));

	return info;
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::class_operand (MonoIrBuilder &builder, MonoClass *klass,
                                  const char *prefix)
{
	if (!depends_on_context (klass))
		return class_symbol (klass, prefix);

	llvm::StringRef which (prefix);
	MonoRgctxInfoType info_type = MONO_RGCTX_INFO_KLASS;

	if (which == "mono_vtable_")
		info_type = MONO_RGCTX_INFO_VTABLE;
	else if (which == "mono_statics_")
		info_type = MONO_RGCTX_INFO_STATIC_DATA;

	return rgctx_fetch (builder, info_type, klass);
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::field_operand (MonoIrBuilder &builder, MonoClassField *field)
{
	if (!depends_on_context (field))
		return field_symbol (field);

	return rgctx_fetch (builder, MONO_RGCTX_INFO_CLASS_FIELD, field);
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::method_operand (MonoIrBuilder &builder, MonoMethod *target)
{
	if (!depends_on_context (target))
		return method_symbol (target);

	return rgctx_fetch (builder, MONO_RGCTX_INFO_METHOD, target);
}

} // namespace mono
