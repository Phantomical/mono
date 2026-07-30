#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/ErrorHandling.h>

#include <string>
#include <vector>

namespace mono {

namespace {

llvm::Type *
pointer_type (llvm::LLVMContext &ctx)
{
	return llvm::PointerType::get (ctx, 0);
}

/// The integer type as wide as a pointer, for native int and native unsigned int.
llvm::Type *
int_ptr_type (llvm::LLVMContext &ctx)
{
	return llvm::Type::getIntNTy (ctx, TARGET_SIZEOF_VOID_P * 8);
}

llvm::Type *
primitive_type_to_llvm_type (llvm::LLVMContext &ctx, MonoTypeEnum type)
{
	switch (type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return llvm::Type::getInt8Ty (ctx);
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return llvm::Type::getInt16Ty (ctx);
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return llvm::Type::getInt32Ty (ctx);
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return llvm::Type::getInt64Ty (ctx);
	case MONO_TYPE_R4:
		return llvm::Type::getFloatTy (ctx);
	case MONO_TYPE_R8:
		return llvm::Type::getDoubleTy (ctx);
	case MONO_TYPE_I:
	case MONO_TYPE_U:
		return int_ptr_type (ctx);
	default:
		return nullptr;
	}
}

/// The vector type a SIMD class travels in, or null if this is not a shape we know
/// how to lower.
///
/// These have to be vectors rather than opaque bytes: the runtime's calling
/// convention hands them over in an SSE register, and a struct here would put them
/// somewhere else.
llvm::Type *
simd_class_to_llvm_type (llvm::LLVMContext &ctx, MonoClass *klass)
{
	/*
	 * Vector<T> and Vector64/128/256/512<T> all describe themselves: the element is
	 * the type argument, and how many of them there are is the class's own size.
	 */
	if (mono_class_is_ginst (klass)) {
		MonoType *etype =
			mono_class_get_generic_class (klass)->context.class_inst->type_argv[0];
		llvm::Type *element =
			primitive_type_to_llvm_type (ctx, static_cast<MonoTypeEnum> (etype->type));

		if (element == nullptr)
			return nullptr;

		unsigned esize = element->getScalarSizeInBits () / 8;
		unsigned size = mono_class_value_size (klass, NULL);

		if (esize == 0 || size % esize != 0)
			return nullptr;
		return llvm::FixedVectorType::get (element, size / esize);
	}

	std::string_view name = m_class_get_name (klass);

	if (name == "Vector2d")
		return llvm::FixedVectorType::get (llvm::Type::getDoubleTy (ctx), 2);
	if (name == "Vector2l" || name == "Vector2ul")
		return llvm::FixedVectorType::get (llvm::Type::getInt64Ty (ctx), 2);
	if (name == "Vector4i" || name == "Vector4ui")
		return llvm::FixedVectorType::get (llvm::Type::getInt32Ty (ctx), 4);
	if (name == "Vector8s" || name == "Vector8us")
		return llvm::FixedVectorType::get (llvm::Type::getInt16Ty (ctx), 8);
	if (name == "Vector16sb" || name == "Vector16b")
		return llvm::FixedVectorType::get (llvm::Type::getInt8Ty (ctx), 16);
	/* The short System.Numerics vectors still occupy a whole 4 x float register. */
	if (name == "Vector4f" || name == "Vector2" || name == "Vector3" || name == "Vector4")
		return llvm::FixedVectorType::get (llvm::Type::getFloatTy (ctx), 4);

	return nullptr;
}

/// How a narrow integer argument or return value is widened to fill its register.
///
/// The C ABI leaves the high bits undefined, so which way they get filled is part
/// of the signature rather than something the two ends can each decide.
llvm::Attribute::AttrKind
integer_extension (MonoType *t)
{
	if (t->byref)
		return llvm::Attribute::None;

	switch (mini_get_underlying_type (t)->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_I2:
		return llvm::Attribute::SExt;
	case MONO_TYPE_U1:
	case MONO_TYPE_U2:
		return llvm::Attribute::ZExt;
	default:
		return llvm::Attribute::None;
	}
}

} // namespace

llvm::Expected<llvm::Type *>
MethodLLVMEmitter::convert_type (MonoType *t)
{
	if (t->byref)
		return pointer_type (context ());

	t = mini_get_underlying_type (t);

	if (llvm::Type *primitive =
	            primitive_type_to_llvm_type (context (), static_cast<MonoTypeEnum> (t->type)))
		return primitive;

	switch (t->type) {
	case MONO_TYPE_VOID:
		return llvm::Type::getVoidTy (context ());
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	/* Generic sharing hands us these as references. */
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		return pointer_type (context ());
	case MONO_TYPE_GENERICINST:
		if (!mono_type_generic_inst_is_valuetype (t))
			return pointer_type (context ());
		/* Fall through */
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_TYPEDBYREF:
		return convert_vtype (t);
	default: {
		char *name = mono_type_full_name (t);
		llvm::Error error = conversion_error (llvm::Twine ("unsupported type ") + name);

		g_free (name);
		return std::move (error);
	}
	}
}

llvm::Expected<llvm::Type *>
MethodLLVMEmitter::convert_vtype (MonoType *t)
{
	MonoClass *klass = mono_class_from_mono_type_internal (t);

	if (MONO_CLASS_IS_SIMD (cfg, klass)) {
		llvm::Type *vector = simd_class_to_llvm_type (context (), klass);

		if (vector == nullptr)
			return conversion_error (llvm::Twine ("unsupported simd type ")
			                         + m_class_get_name (klass));
		return vector;
	}

	if (m_class_is_enumtype (klass))
		return convert_type (mono_class_enum_basetype_internal (klass));

	auto it = vtypes.find (klass);
	if (it != vtypes.end ())
		return it->second;

	/*
	 * The runtime is the authority on a vtype's layout, so LLVM only ever sees the
	 * right number of bytes under the type's name, never its fields.
	 */
	char *name = mono_type_full_name (m_class_get_byval_arg (klass));
	llvm::StructType *type = llvm::StructType::create (context (), name);
	g_free (name);

	std::vector<llvm::Type *> bytes (mono_class_value_size (klass, NULL),
	                                 llvm::Type::getInt8Ty (context ()));
	type->setBody (bytes);

	vtypes[klass] = type;
	return type;
}

/// The LLVM function type for SIG: the managed types written out as themselves,
/// leaving how each one travels to LLVM's own lowering.
///
/// So a vtype is a parameter of that struct type rather than a pointer plus an
/// attribute, and a returned one is returned - LLVM decides for itself which of
/// those fit in registers and which need a hidden return-address argument.
llvm::Expected<llvm::FunctionType *>
MethodLLVMEmitter::convert_method_signature (MonoMethodSignature *sig)
{
	llvm::Expected<llvm::Type *> ret_type = convert_type (sig->ret);
	if (!ret_type)
		return ret_type.takeError ();

	std::vector<llvm::Type *> params;

	if (sig->hasthis)
		params.push_back (pointer_type (context ()));

	for (int i = 0; i < sig->param_count; ++i) {
		llvm::Expected<llvm::Type *> type = convert_type (sig->params[i]);

		if (!type)
			return type.takeError ();
		params.push_back (*type);
	}

	return llvm::FunctionType::get (*ret_type, params, sig->call_convention == MONO_CALL_VARARG);
}

/// The declaration of METHOD in this module, created on first use and cached.
llvm::Expected<llvm::Function *>
MethodLLVMEmitter::create_method_decl (MonoMethod *method)
{
	auto it = declarations.find (method);
	if (it != declarations.end ())
		return it->second;

	ERROR_DECL (metadata_error);
	MonoMethodSignature *sig = mono_method_signature_checked (method, metadata_error);
	if (sig == nullptr) {
		llvm::Error error = conversion_error (mono_error_get_message (metadata_error));

		mono_error_cleanup (metadata_error);
		return std::move (error);
	}

	llvm::Expected<llvm::FunctionType *> type = convert_method_signature (sig);
	if (!type)
		return type.takeError ();

	char *full_name = mono_method_full_name (method, TRUE);
	llvm::Function *function = llvm::Function::Create (*type, llvm::GlobalValue::ExternalLinkage,
	                                                   full_name, module);
	g_free (full_name);

	if (llvm::Attribute::AttrKind ext = integer_extension (sig->ret); ext != llvm::Attribute::None)
		function->addRetAttr (ext);

	if (sig->hasthis)
		function->getArg (0)->setName ("this");

	std::vector<const char *> names (sig->param_count);
	if (sig->param_count > 0)
		mono_method_get_param_names (method, names.data ());

	for (int i = 0; i < sig->param_count; ++i) {
		unsigned pindex = i + sig->hasthis;

		if (names[i] != nullptr && names[i][0] != '\0')
			function->getArg (pindex)->setName (std::string ("arg_") + names[i]);
		else
			function->getArg (pindex)->setName ("arg_" + std::to_string (i));

		if (llvm::Attribute::AttrKind ext = integer_extension (sig->params[i]);
		    ext != llvm::Attribute::None)
			function->addParamAttr (pindex, ext);
	}

	declarations[method] = function;
	return function;
}

} // namespace mono
