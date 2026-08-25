/**
 * \file
 * \brief Compiling the RuntimeTypeHandle icalls that read an element type.
 */

#include "method-to-llvm.hpp"
#include "util/bitfield.hpp"

// class-internals.h brings in jit-icall-reg.h, which has no include guard.
#include "mono/metadata/class-internals.h"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/appdomain.h"
#include "mono/metadata/domain-internals.h"
#include "mono/metadata/metadata-internals.h"
#include "mono/metadata/object-internals.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>

#include <string_view>

namespace mono {

/*
 * Array.GetValue (int) and Array.SetValue (object, int) ask whether the element
 * type is a pointer, and a caller that walks an array asks once for each
 * element:
 *
 *     if (GetType ().GetElementType ().IsPointer)
 *         throw new NotSupportedException ("Type is not supported.");
 *
 * GetType () is already one load (emit_get_type ()). The two that stay are
 * RuntimeType.GetElementType () and Type.IsPointer, and each reaches the
 * runtime through RuntimeTypeHandle. Both answers sit in memory the compiled
 * code can read, so what the icall costs is the crossing rather than the work:
 * the wrapper's frame, and the handle mono_type_get_object_handle () opens.
 *
 * The two emitters below read that memory instead. They match the icall the IL
 * named, ahead of the wrapper swap, so every caller of IsPointer, IsArray,
 * IsPrimitive, IsByRef and GetElementType takes them, rather than Array alone.
 */

namespace {

/// Where MonoType keeps the tag that says which kind of type it is.
const BitfieldPlace &
type_tag ()
{
	return MONO_BITFIELD_PLACE (MonoType, type, MonoTypeEnum, 0xff);
}

/// Where MonoType keeps the flag that says the type is a managed pointer.
const BitfieldPlace &
type_byref ()
{
	return MONO_BITFIELD_PLACE (MonoType, byref, unsigned int, 1);
}

/// Where MonoImage keeps the flag that says Reflection.Emit built the module.
const BitfieldPlace &
image_dynamic ()
{
	return MONO_BITFIELD_PLACE (MonoImage, dynamic, guint8, 1);
}

/// Whether target is the named static RuntimeTypeHandle method that takes one
/// RuntimeType and nothing else.
bool
is_type_handle_icall (MonoMethod *target, MonoMethodSignature *sig, std::string_view what)
{
	if (sig == nullptr || sig->hasthis || sig->call_convention == MONO_CALL_VARARG)
		return false;
	if (sig->param_count != 1 || sig->params[0]->byref)
		return false;

	// The emitters read a MonoReflectionType off the argument, so the
	// signature has to promise one. RuntimeType is the class that carries the
	// field, and mono_defaults holds the pointer the runtime uses.
	if (mono_class_from_mono_type_internal (sig->params[0])
	    != mono_defaults.runtimetype_class)
		return false;

	MonoClass *klass = target->klass;

	if (m_class_get_image (klass) != mono_get_corlib ())
		return false;
	if (std::string_view (m_class_get_name_space (klass)) != "System")
		return false;
	if (std::string_view (m_class_get_name (klass)) != "RuntimeTypeHandle")
		return false;

	return std::string_view (target->name) == what;
}

} // namespace

bool
answers_cor_element_type (MonoMethod *target, MonoMethodSignature *sig)
{
	return is_type_handle_icall (target, sig, "GetCorElementType");
}

bool
answers_element_type (MonoMethod *target, MonoMethodSignature *sig)
{
	return is_type_handle_icall (target, sig, "GetElementType");
}

/// Loads the MonoType a System.Type object names, and the word that holds its
/// tag fields.
///
/// The object must already be null-checked.
MethodLLVMEmitter::TypeTagLoad
MethodLLVMEmitter::load_type_tag (MonoIrBuilder &builder, llvm::Value *reflection_type)
{
	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Value *type = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (
			builder.getInt8Ty (), reflection_type,
			builder.getInt32 (MONO_STRUCT_OFFSET (MonoReflectionType, type))),
		llvm::Align (TARGET_SIZEOF_VOID_P), "mono_type");
	llvm::Value *tag = builder.CreateAlignedLoad (
		builder.getInt32Ty (),
		builder.CreateGEP (builder.getInt8Ty (), type,
	                           builder.getInt32 (type_tag ().offset)),
		llvm::Align (4), "type_tag");

	return { type, tag };
}

/*
 * ves_icall_RuntimeTypeHandle_GetCorElementType () is the tag and nothing else:
 *
 *     if (type->byref)
 *         return MONO_TYPE_BYREF;
 *     else
 *         return (guint32)type->type;
 *
 * CorElementType is the managed spelling of the answer, and its members hold
 * the MonoTypeEnum values, so the tag travels as it is. The enum is narrower
 * than the guint32 the C function returns, which is why the answer goes back
 * through the return type rather than as an int32.
 */
llvm::Error
MethodLLVMEmitter::emit_cor_element_type (MonoIrBuilder &builder, MonoMethodSignature *sig)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue argument = get_stack (0);

	if (stack_type (argument.type) != ObjectRef)
		return invalid_il (llvm::Twine ("a RuntimeType was expected, not operand type ")
		                   + describe (argument.type, stack_type (argument.type)));

	llvm::Expected<llvm::Type *> answer = convert_type (sig->ret);

	if (!answer)
		return answer.takeError ();

	// The icall reads the field through a handle and faults on a null of its
	// own. An explicit check raises the same exception one load sooner.
	emit_null_check (builder, argument.value);

	TypeTagLoad loaded = load_type_tag (builder, argument.value);
	llvm::Value *tag = builder.CreateLShr (
		builder.CreateAnd (loaded.tag, builder.getInt32 (type_tag ().mask)),
		builder.getInt32 (type_tag ().shift));
	llvm::Value *byref = builder.CreateIsNotNull (
		builder.CreateAnd (loaded.tag, builder.getInt32 (type_byref ().mask)));
	llvm::Value *answered = builder.CreateSelect (
		byref, builder.getInt32 (MONO_TYPE_BYREF), tag, "cor_element_type");

	pop_stack (1);
	return push_produced (builder, builder.CreateZExtOrTrunc (answered, *answer),
	                      sig->ret);
}

/*
 * ves_icall_RuntimeTypeHandle_GetElementType () answers an szarray before it
 * does anything else, and that is the shape Array.GetValue () asks about:
 *
 *     if (!type->byref && type->type == MONO_TYPE_SZARRAY)
 *         return mono_type_get_object_handle (domain,
 *                                             m_class_get_byval_arg (type->data.klass),
 *                                             error);
 *
 * mono_type_get_object_checked () (mono/metadata/reflection.c) then reads the
 * answer off the element class's vtable, under conditions it states itself:
 *
 *     if (type == m_class_get_byval_arg (klass) && !image_is_dynamic (m_class_get_image (klass))) {
 *         MonoVTable *vtable = mono_class_try_get_vtable (domain, klass);
 *         if (vtable && vtable->type)
 *             return (MonoReflectionType *)vtable->type;
 *     }
 *
 * The first of those conditions holds by construction here, because the type
 * the icall hands it is the element class's own byval_arg. The rest are the
 * guards below, and the call the site named sits on the edge any of them
 * declines. So a rank of more than one, a byref, a pointer, a class with no
 * vtable in this domain and a module Reflection.Emit built all keep the icall.
 *
 * mono_class_try_get_vtable () is the walk from the class to the vtable:
 * runtime_info holds one vtable per domain, and domain_vtables is as long as
 * max_domain plus one. The domain is settled when this compiles, so the index
 * is a constant and only the bound is read.
 */
llvm::Error
MethodLLVMEmitter::emit_element_type (MonoIrBuilder &builder, MonoMethod *callee_method,
                                      MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);

	if (!args)
		return args.takeError ();

	llvm::Expected<llvm::Function *> slow_decl =
		create_method_decl (icall_wrapper_target (callee_method));

	if (!slow_decl)
		return slow_decl.takeError ();

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Align align (TARGET_SIZEOF_VOID_P);
	int32_t domain = mono_domain_get_id (cfg->domain);
	llvm::Value *object = (*args)[0];

	emit_null_check (builder, object);

	TypeTagLoad loaded = load_type_tag (builder, object);
	llvm::BasicBlock *walk =
		llvm::BasicBlock::Create (context (), "element_type_walk", function);
	llvm::BasicBlock *bound =
		llvm::BasicBlock::Create (context (), "element_type_bound", function);
	llvm::BasicBlock *found =
		llvm::BasicBlock::Create (context (), "element_type_found", function);
	llvm::BasicBlock *read =
		llvm::BasicBlock::Create (context (), "element_type_read", function);
	llvm::BasicBlock *declined =
		llvm::BasicBlock::Create (context (), "element_type_declined", function);
	llvm::BasicBlock *done =
		llvm::BasicBlock::Create (context (), "element_type_done", function);

	// One test for both fields: an szarray that is not byref leaves the tag
	// word holding MONO_TYPE_SZARRAY in the tag's bits and nothing in byref's.
	uint32_t shape = type_tag ().mask | type_byref ().mask;
	uint32_t szarray = static_cast<uint32_t> (MONO_TYPE_SZARRAY) << type_tag ().shift;

	builder.CreateCondBr (
		builder.CreateICmpEQ (builder.CreateAnd (loaded.tag, builder.getInt32 (shape)),
	                              builder.getInt32 (szarray)),
		walk, declined);

	// For an szarray, data holds the element class. data is the first member of
	// MonoType, so that load needs no displacement.
	builder.SetInsertPoint (walk);

	llvm::Value *element =
		builder.CreateAlignedLoad (ptr, loaded.type, align, "element_class");
	llvm::Value *image = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), element,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoClass, image))),
		align);
	llvm::Value *image_flags = builder.CreateAlignedLoad (
		builder.getInt32Ty (),
		builder.CreateGEP (builder.getInt8Ty (), image,
	                           builder.getInt32 (image_dynamic ().offset)),
		llvm::Align (4));
	llvm::Value *info = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (
			builder.getInt8Ty (), element,
			builder.getInt32 (MONO_STRUCT_OFFSET (MonoClass, runtime_info))),
		align, "runtime_info");

	builder.CreateCondBr (
		builder.CreateAnd (
			builder.CreateIsNull (builder.CreateAnd (
				image_flags, builder.getInt32 (image_dynamic ().mask))),
			builder.CreateIsNotNull (info)),
		bound, declined);

	builder.SetInsertPoint (bound);

	llvm::Value *reach = builder.CreateAlignedLoad (
		builder.getInt16Ty (),
		builder.CreateGEP (
			builder.getInt8Ty (), info,
			builder.getInt32 (MONO_STRUCT_OFFSET (MonoClassRuntimeInfo, max_domain))),
		llvm::Align (2), "max_domain");

	builder.CreateCondBr (
		builder.CreateICmpUGE (builder.CreateZExt (reach, builder.getInt32Ty ()),
	                               builder.getInt32 (domain)),
		found, declined);

	builder.SetInsertPoint (found);

	llvm::Value *vtable = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (
			builder.getInt8Ty (), info,
			builder.getInt32 (
				MONO_STRUCT_OFFSET (MonoClassRuntimeInfo, domain_vtables)
				+ domain * TARGET_SIZEOF_VOID_P)),
		align, "element_vtable");

	builder.CreateCondBr (builder.CreateIsNotNull (vtable), read, declined);

	// mono_class_create_runtime_vtable () fills in `type` before it publishes
	// the vtable, so a vtable reached through runtime_info has one. RuntimeType
	// is the exception: its own vtable takes `type` after the memory barrier,
	// which leaves a window where the field is null. mono_type_get_object_checked ()
	// tests the field for that reason and this follows it.
	builder.SetInsertPoint (read);

	llvm::Value *answered = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), vtable,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, type))),
		align, "element_type");

	builder.CreateCondBr (builder.CreateIsNotNull (answered), done, declined);

	builder.SetInsertPoint (declined);

	llvm::Value *raised =
		emit_protected_call (builder, *slow_decl, adapt_to_callee (builder, *slow_decl, *args));
	llvm::BasicBlock *raised_in = builder.GetInsertBlock ();

	builder.CreateBr (done);
	builder.SetInsertPoint (done);

	llvm::PHINode *result = builder.CreatePHI (ptr, 2, "element_type_answer");

	result->addIncoming (answered, read);
	result->addIncoming (raised, raised_in);

	pop_stack (sig->param_count);
	return push_produced (builder, result, sig->ret);
}

} // namespace mono
