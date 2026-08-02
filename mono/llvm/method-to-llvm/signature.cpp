#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/ErrorHandling.h>

#include <algorithm>
#include <string>
#include <vector>

namespace mono {

namespace {

/// An error describing something the converter could not express in LLVM IR.
///
/// An ExecutionEngineException, like the refusals in invalid-il.cpp: a signature
/// or a type this engine cannot express is a limit of the engine, not of the
/// program, and nothing else is going to compile the method instead.
inline llvm::Error
conversion_error (const llvm::Twine &reason)
{
	ERROR_DECL (error);

	mono_error_set_execution_engine (error, "%s", reason.str ().c_str ());
	return runtime_error (error);
}

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
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return llvm::Type::getInt8Ty (ctx);
	case MONO_TYPE_CHAR:
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
/// These have to be vectors rather than a struct of their elements: the calling
/// conventions hand them over in an SSE register, and a struct here would put them
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

} // namespace

/// The C ABI leaves a narrow integer's high bits undefined, so which way they get
/// filled is part of the signature rather than something the two ends can each
/// decide.
llvm::Attribute::AttrKind
integer_extension (MonoType *t)
{
	if (t->byref)
		return llvm::Attribute::None;

	switch (mini_get_underlying_type (t)->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_I2:
		return llvm::Attribute::SExt;
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U1:
	case MONO_TYPE_U2:
		return llvm::Attribute::ZExt;
	default:
		return llvm::Attribute::None;
	}
}

bool
implemented_outside_il (MonoMethod *method)
{
	/*
	 * A wrapper is translated however the method it wraps is marked: the
	 * wrapper the runtime builds around a pinvoke keeps the flags of the
	 * method it wraps, and the wrapper is exactly the part that is IL.
	 */
	if (method->wrapper_type != MONO_WRAPPER_NONE)
		return false;

	return (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) != 0 ||
	       (method->iflags & METHOD_IMPL_ATTRIBUTE_RUNTIME) != 0 ||
	       (method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) != 0;
}

/// The hidden return pointer sits behind the first argument whenever the
/// runtime keeps a receiver there: the trampolines that recover a receiver
/// from a call site always look in the first register. The same applies when
/// the first declared parameter is a reference type, because delegate-invoke
/// wrappers make virtual calls through calli signatures with hasthis unset
/// (mini-amd64.c, get_call_info).
LegacyFlavor
legacy_call_flavor (MonoMethodSignature *sig)
{
	if (sig->pinvoke)
		return LegacyFlavor::Pinvoke;
	if (sig->hasthis)
		return LegacyFlavor::ManagedVret1;
	if (sig->param_count > 0
	    && MONO_TYPE_IS_REFERENCE (mini_get_underlying_type (sig->params[0])))
		return LegacyFlavor::ManagedVret1;
	return LegacyFlavor::Managed;
}

void
MethodLLVMEmitter::mark_legacy_call (llvm::CallBase *call, MonoMethodSignature *sig)
{
	call->addFnAttr (llvm::Attribute::get (
		call->getContext (), legacy_cc_attribute,
		legacy_flavor_value (legacy_call_flavor (sig))));
}

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
	case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY:
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

/*
 * A value type converts to a packed struct spelling out its real layout: each
 * field at the offset the runtime laid it out at, with the gaps filled in as
 * [n x i1]. Real layout so LLVM can reason about the fields; packed so the
 * offsets are exactly the runtime's rather than whatever the DataLayout would
 * infer; padding as i1 arrays because no real field is ever one, which is what
 * lets LegacyAbiPass tell data from padding when it classifies (a float
 * sharing an eightbyte with padding is still a float to the C ABI).
 */
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

	char *printed = mono_type_full_name (m_class_get_byval_arg (klass));
	llvm::StructType *type = llvm::StructType::create (context (), printed);

	g_free (printed);

	unsigned size = mono_class_value_size (klass, NULL);

	struct Field {
		int offset;
		int size;
		llvm::Type *type;
	};
	std::vector<Field> fields;

	gpointer iter = NULL;

	while (MonoClassField *field = mono_class_get_fields_internal (klass, &iter)) {
		if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
			continue;

		llvm::Expected<llvm::Type *> converted = convert_type (field->type);

		if (!converted)
			return converted.takeError ();

		int align;

		fields.push_back ({
			field->offset - static_cast<int> (MONO_ABI_SIZEOF (MonoObject)),
			mono_type_size (field->type, &align),
			*converted,
		});
	}

	std::sort (fields.begin (), fields.end (),
	           [] (const Field &a, const Field &b) { return a.offset < b.offset; });

	/*
	 * An explicit layout can overlap fields, which a struct cannot express;
	 * whichever comes first keeps its slot and the rest of the union becomes
	 * padding. What that loses is only the overlapped fields' say in the
	 * native classification - the bytes are all still there.
	 */
	llvm::Type *pad = llvm::Type::getInt1Ty (context ());
	std::vector<llvm::Type *> body;
	int at = 0;
	bool expressible = true;

	for (const Field &field : fields) {
		if (field.offset < at)
			continue;
		if (field.offset > at)
			body.push_back (llvm::ArrayType::get (pad, field.offset - at));
		if (field.offset + field.size > static_cast<int> (size)) {
			expressible = false;
			break;
		}
		body.push_back (field.type);
		at = field.offset + field.size;
	}

	if (expressible) {
		if (at < static_cast<int> (size))
			body.push_back (llvm::ArrayType::get (pad, size - at));
		type->setBody (body, /*isPacked=*/true);
	} else {
		/* A layout the walk cannot restate keeps the right size, opaquely. */
		type->setBody (llvm::ArrayType::get (llvm::Type::getInt8Ty (context ()),
		                                     size),
		               /*isPacked=*/true);
	}

	vtypes[klass] = type;
	return type;
}

/// The alignment an instance of T needs in memory.
///
/// The struct convert_vtype builds is packed, which the data layout reads as
/// 1-aligned, so every alloca of one has to be told what the runtime decided
/// instead.
llvm::Align
MethodLLVMEmitter::type_alignment (MonoType *t)
{
	if (t->byref)
		return llvm::Align (TARGET_SIZEOF_VOID_P);

	MonoClass *klass = mono_class_from_mono_type_internal (mini_get_underlying_type (t));
	unsigned align = mono_class_min_align (klass);

	/*
	 * A vector's natural alignment is its size, but nothing the runtime hands
	 * out promises more than 8 - the GC allocates on words, so a vector inside
	 * an array or an object is only word-aligned. Claiming 8 keeps every
	 * vector access an unaligned instruction; LLVM raises it back where it can
	 * prove more, which is exactly the allocas.
	 */
	if (MONO_CLASS_IS_SIMD (cfg, klass))
		align = std::min (8, mono_class_value_size (klass, NULL));

	/* A packed layout can ask for an alignment that is not a power of two. */
	while (mono_is_power_of_two (align) == -1)
		align++;

	return llvm::Align (align);
}

/// The LLVM function type for SIG, in this backend's own convention: every
/// value in its natural type, value types by value as their struct, aggregate
/// returns returned as aggregates. Only LegacyAbiPass ever lowers any of it.
llvm::Expected<llvm::FunctionType *>
MethodLLVMEmitter::convert_method_signature (MonoMethodSignature *sig)
{
	/*
	 * The runtime's vararg convention passes a signature cookie in a stack slot
	 * between the fixed and the variadic arguments, and ArgIterator walks the rest
	 * from it. Neither end of that is expressible as LLVM's C-style varargs, so
	 * these signatures are declined whole rather than compiled to an ABI nothing
	 * else speaks.
	 */
	if (sig->call_convention == MONO_CALL_VARARG)
		return conversion_error ("a vararg signature uses the runtime's cookie "
		                         "convention");

	llvm::Expected<llvm::Type *> ret = convert_type (sig->ret);

	if (!ret)
		return ret.takeError ();

	std::vector<llvm::Type *> params;

	if (sig->hasthis)
		params.push_back (pointer_type (context ()));

	for (int i = 0; i < sig->param_count; ++i) {
		llvm::Expected<llvm::Type *> converted = convert_type (sig->params[i]);

		if (!converted)
			return converted.takeError ();
		params.push_back (*converted);
	}

	return llvm::FunctionType::get (*ret, params, false);
}

/// ARGS shaped to CALLEE's declared parameter types.
///
/// The icall signatures spell runtime addresses as native int while the
/// translator holds them as pointers, and a call has to say exactly what the
/// declaration says.
std::vector<llvm::Value *>
MethodLLVMEmitter::adapt_to_callee (MonoIrBuilder &builder, llvm::Function *callee,
                                    llvm::ArrayRef<llvm::Value *> args)
{
	llvm::FunctionType *type = callee->getFunctionType ();
	std::vector<llvm::Value *> adapted (args.begin (), args.end ());

	for (unsigned i = 0; i < adapted.size () && i < type->getNumParams (); ++i) {
		llvm::Type *want = type->getParamType (i);
		llvm::Value *have = adapted[i];

		if (have->getType () == want)
			continue;
		if (want->isPointerTy () && !have->getType ()->isPointerTy ())
			adapted[i] = builder.CreateIntToPtr (have, want);
		else
			adapted[i] = coerce (builder, have, want);
	}

	return adapted;
}

/// The declaration of the managed wrapper around jit icall ID.
///
/// The runtime's own entry points report failure by leaving a pending
/// exception, which nothing would ever look at again if the raw C function
/// were called directly; the wrapper follows the call with the check that
/// turns it into a throw. Any entry point that can fail has to be called
/// this way.
llvm::Expected<llvm::Function *>
MethodLLVMEmitter::icall_wrapper_decl (MonoJitICallId id)
{
	MonoJitICallInfo *info = mono_find_jit_icall_info (id);

	return create_method_decl (mono_marshal_get_icall_wrapper (info, TRUE));
}

/// The declaration of METHOD in this module, created on first use and cached.
///
/// A method this backend compiles is declared fastcc against its `$fast`
/// symbol, which the engine resolves to the fastcc body's stub. One whose code
/// mini produces instead - an icall, a pinvoke, a runtime-implemented method -
/// is declared against the plain symbol in the legacy convention, and every
/// call to it lowers in LegacyAbiPass.
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

	/*
	 * A string constructor compiles against a signature returning the string it
	 * creates - there is no preallocated this to fill in - and every caller has
	 * to see that shape.
	 */
	if (method->string_ctor)
		sig = mono_marshal_get_string_ctor_signature (method);

	llvm::Expected<llvm::FunctionType *> type = convert_method_signature (sig);
	if (!type)
		return type.takeError ();

	bool legacy = implemented_outside_il (method);
	char *printed = mono_method_full_name (method, TRUE);
	std::string full_name = printed;
	char suffix[32];

	g_free (printed);

	/*
	 * The printed name is for reading; the pointer is the identity. No name
	 * scheme is unique on its own - conversion operators overload on their
	 * return type, which no printed signature carries, and runtime-minted
	 * wrappers print alike - and this name is how a caller's reference finds
	 * the method's stub. symbol_for_code () (runtime.cpp) must agree.
	 */
	snprintf (suffix, sizeof (suffix), "@%p", (void *) method);
	full_name += suffix;
	if (!legacy)
		full_name += "$fast";

	/*
	 * The emitter's cache is per instance, but filter bodies share the
	 * method's module across instances - a name already declared there must
	 * be reused or LLVM quietly uniques it into a symbol nothing resolves.
	 */
	if (llvm::Function *existing = module->getFunction (full_name)) {
		declarations[method] = existing;
		return existing;
	}

	llvm::Function *function = llvm::Function::Create (
		*type, llvm::GlobalValue::ExternalLinkage, full_name, module);

	record_external (full_name, ExternalSymbol::Kind::Code, method);

	if (legacy)
		function->addFnAttr (llvm::Attribute::get (
			context (), legacy_cc_attribute,
			legacy_flavor_value (legacy_call_flavor (sig))));
	else
		function->setCallingConv (llvm::CallingConv::Fast);

	if (llvm::Attribute::AttrKind ext = integer_extension (sig->ret);
	    ext != llvm::Attribute::None)
		function->addRetAttr (ext);

	/*
	 * The creator's string is fresh, aliasing nothing older than the call. Nothing
	 * stronger: the body is arbitrary managed code, so the allocator attributes'
	 * zeroed and elidable claims are not made for it.
	 */
	if (method->string_ctor)
		function->addRetAttr (llvm::Attribute::NoAlias);

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
