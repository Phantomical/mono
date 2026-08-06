#include "method-to-llvm.hpp"
#include "hidden-return.hpp"
#include "layout.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/icall-internals.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MathExtras.h>

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

/// Whether marshalling hands KLASS across a native boundary byte for byte, so
/// that its native layout is the managed one convert_vtype already builds.
///
/// The same three cases marshal-ilgen skips conversion for
/// (emit_marshal_vtype_ilgen): everything else is copied field by field into a
/// buffer that can be a different size, with the fields somewhere else in it.
bool
marshals_unchanged (MonoClass *klass)
{
	return mono_class_is_explicit_layout (klass) || m_class_is_blittable (klass)
	       || m_class_is_enumtype (klass);
}

/// Whether METHOD is entered with SIG's value types already marshalled.
///
/// A pinvoke signature describes native code, but that is not enough on its own
/// to say that a caller holds marshalled values. A [DllImport] method carries
/// one too, and a managed call to it enters at the marshalling wrapper the
/// runtime built - the marshalled layout only begins inside that wrapper, past
/// where its arguments were converted. What does speak it is a method whose own
/// body is native-facing: the wrapper a delegate is handed out to native code
/// as, whose signature is the native one because it is the native entry.
bool
speaks_marshalled_layout (MonoMethod *method, MonoMethodSignature *sig)
{
	return sig->pinvoke != 0 && method->wrapper_type != MONO_WRAPPER_NONE;
}

/// One field of a value type, ready to be packed into a struct body.
struct LayoutField {
	int offset;
	int size;
	llvm::Type *type;
};

/*
 * The bytes from AT to SIZE, appended to BODY as the continuation of LAST.
 *
 * A gap at the end of a value type is not padding. A C# `fixed` buffer is a
 * single field of the element type inside a class sized for the whole array,
 * so everything past the first element has no metadata field to be found
 * under; a .pack directive leaves the same shape. Those bytes are live data,
 * and the classification only ever sees fields, so the last field has to be
 * carried out to the end of the type - which is what mini does too, in
 * collect_field_info_nested (mini-amd64.c, "This can happen with .pack
 * directives eg. 'fixed' arrays").
 *
 * Repeating a primitive is what keeps the register file right: the tail of a
 * `fixed float` buffer rides the SSE file, which bytes would not say. Anything
 * else - a pointer, an inlined array, a marshalled string - classifies as
 * integer whatever width it is given, which is what plain bytes say as well,
 * and is what mini widens rather than replicates for the same reason.
 */
void
fill_tail (llvm::LLVMContext &ctx, std::vector<llvm::Type *> &body,
           const LayoutField *last, int at, unsigned size)
{
	int gap = static_cast<int> (size) - at;

	/* A type with no field at all is all padding, and stays that way. */
	if (last == nullptr) {
		body.push_back (padding_type (ctx, gap));
		return;
	}

	if (last->size > 0
	    && (last->type->isIntegerTy () || last->type->isFloatingPointTy ())) {
		int count = gap / last->size;

		if (count == 1)
			body.push_back (last->type);
		else if (count > 1)
			body.push_back (llvm::ArrayType::get (last->type, count));
		gap -= count * last->size;
	}

	if (gap > 0)
		body.push_back (llvm::ArrayType::get (llvm::Type::getInt8Ty (ctx), gap));
}

/*
 * FIELDS laid into TYPE's body as a packed struct spelling out the real layout:
 * each field at the offset it was laid out at, with the gaps between them
 * filled in by padding_type (). Real layout so LLVM can reason about the
 * fields; packed so the offsets are exactly the runtime's rather than whatever
 * the DataLayout would infer; padding spelled as a shape no field ever takes,
 * which is what lets LegacyAbiPass tell data from padding when it classifies
 * (a float sharing an eightbyte with padding is still a float to the C ABI).
 */
void
set_packed_body (llvm::LLVMContext &ctx, llvm::StructType *type, unsigned size,
                 std::vector<LayoutField> &fields)
{
	std::sort (fields.begin (), fields.end (),
	           [] (const LayoutField &a, const LayoutField &b) {
		           return a.offset < b.offset;
	           });

	/*
	 * An explicit layout can overlap fields, which a struct cannot express;
	 * whichever comes first keeps its slot and the rest of the union becomes
	 * padding. What that loses is only the overlapped fields' say in the
	 * native classification - the bytes are all still there.
	 */
	std::vector<llvm::Type *> body;
	const LayoutField *last = nullptr;
	int at = 0;
	bool expressible = true;

	for (const LayoutField &field : fields) {
		if (field.offset < at)
			continue;
		if (field.offset > at)
			body.push_back (padding_type (ctx, field.offset - at));
		if (field.offset + field.size > static_cast<int> (size)) {
			expressible = false;
			break;
		}
		body.push_back (field.type);
		at = field.offset + field.size;
		last = &field;
	}

	if (expressible) {
		if (at < static_cast<int> (size))
			fill_tail (ctx, body, last, at, size);
		type->setBody (body, /*isPacked=*/true);
		return;
	}

	/* A layout the walk cannot restate keeps the right size, opaquely. */
	type->setBody (llvm::ArrayType::get (llvm::Type::getInt8Ty (ctx), size),
	               /*isPacked=*/true);
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

arch::LegacyFlavor
legacy_call_flavor (MonoMethodSignature *sig)
{
	if (sig->pinvoke)
		return arch::LegacyFlavor::Pinvoke;
	return arch::managed_call_flavor (sig);
}

arch::LegacyFlavor
legacy_entry_flavor (MonoMethod *method, MonoMethodSignature *sig)
{
	/*
	 * The address the runtime publishes for a no-wrapper icall is the
	 * registered C function itself (mono_jit_compile_method_inner), so that
	 * one entry really is C. Everything else implemented outside IL is
	 * reached through a wrapper, and a wrapper is a managed method whose own
	 * signature has the pinvoke flag cleared - the convention to speak to it
	 * is mini's, not the C ABI its declaration reads like.
	 */
	if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) != 0) {
		guint32 flags = 0;

		mono_lookup_internal_call_full_with_flags (method, FALSE, &flags);
		if ((flags & MONO_ICALL_FLAGS_NO_WRAPPER) != 0)
			return arch::LegacyFlavor::Pinvoke;
	}

	return arch::managed_call_flavor (sig);
}

void
MethodLLVMEmitter::mark_legacy_call (llvm::CallBase *call, MonoMethodSignature *sig)
{
	call->addFnAttr (llvm::Attribute::get (
		call->getContext (), arch::legacy_cc_attribute,
		arch::legacy_flavor_value (legacy_call_flavor (sig))));
}

void
MethodLLVMEmitter::mark_legacy_entry_call (llvm::CallBase *call, MonoMethod *method,
                                           MonoMethodSignature *sig)
{
	call->addFnAttr (llvm::Attribute::get (
		call->getContext (), arch::legacy_cc_attribute,
		arch::legacy_flavor_value (legacy_entry_flavor (method, sig))));
}

llvm::Expected<llvm::Type *>
MethodLLVMEmitter::convert_type (MonoType *t, bool native)
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
		return convert_vtype (t, native);
	default: {
		char *name = mono_type_full_name (t);
		llvm::Error error = conversion_error (llvm::Twine ("unsupported type ") + name);

		g_free (name);
		return std::move (error);
	}
	}
}

/*
 * Whether a value of T rides the evaluation stack as the address of a frame slot
 * holding it, rather than as an SSA value.
 *
 * Exactly the types convert_type () gives a struct: a value class laid out field
 * by field. LLVM does not keep a struct whole - SROA and InstCombine take every
 * load of one apart into its fields and put it back together at every store - so
 * a front end that moves them around as SSA values spends the pipeline's time
 * undoing its own work. A memcpy between slots says the same thing in one
 * instruction, and mem2reg still promotes the slot where the fields turn out to
 * be all that is wanted.
 *
 * A SIMD class and an enum are not here. convert_type gives them a vector and a
 * scalar, which genuinely do belong in a register.
 */
bool
MethodLLVMEmitter::held_in_memory (MonoType *t)
{
	/*
	 * Asked of convert_type rather than worked out again from the metadata: the
	 * two have to agree about SIMD and enums, and the only way to be sure of
	 * that is to let it answer. A type it cannot convert has no representation
	 * to pick either - whichever caller needed one is about to fail on it.
	 */
	llvm::Expected<llvm::Type *> type = convert_type (t);

	if (!type) {
		llvm::consumeError (type.takeError ());
		return false;
	}

	return (*type)->isStructTy ();
}

unsigned
MethodLLVMEmitter::vtype_size (MonoType *t, bool native)
{
	MonoClass *klass = mono_class_from_mono_type_internal (mini_get_underlying_type (t));

	if (native && !marshals_unchanged (klass))
		return mono_class_native_size (klass, NULL);
	return mono_class_value_size (klass, NULL);
}

/*
 * A value type converts to a packed struct spelling out its real layout - see
 * set_packed_body (), which is where that shape is described.
 *
 * NATIVE asks for the layout marshalling gives the class instead, which is a
 * different struct whenever it moves a field or changes its width. Only a
 * pinvoke signature is in those terms, and only for the classes marshalling
 * actually rewrites: for the rest the two layouts are the same bytes, and
 * sharing one type keeps a value crossing between the two worlds from needing
 * a conversion that would be the identity.
 */
llvm::Expected<llvm::Type *>
MethodLLVMEmitter::convert_vtype (MonoType *t, bool native)
{
	MonoClass *klass = mono_class_from_mono_type_internal (t);

	/*
	 * Laying the class out is what discovers a bad layout - an unaligned
	 * reference field, say - and a class that cannot be laid out has no
	 * layout to convert. Surface its own failure rather than reading the
	 * offsets it never got: the caller turns that into the TypeLoadException
	 * the program is owed. This settles metadata only, and is not the class
	 * initializer, which must never run here.
	 */
	ERROR_DECL (metadata_error);

	if (!mono_class_init_checked (klass, metadata_error))
		return runtime_error (metadata_error);

	if (MONO_CLASS_IS_SIMD (cfg, klass)) {
		llvm::Type *vector = simd_class_to_llvm_type (context (), klass);

		if (vector == nullptr)
			return conversion_error (llvm::Twine ("unsupported simd type ")
			                         + m_class_get_name (klass));
		return vector;
	}

	if (m_class_is_enumtype (klass))
		return convert_type (mono_class_enum_basetype_internal (klass));

	if (native && !marshals_unchanged (klass))
		return convert_native_vtype (klass);

	auto it = vtypes.find (klass);
	if (it != vtypes.end ())
		return it->second;

	char *printed = mono_type_full_name (m_class_get_byval_arg (klass));
	llvm::StructType *type = llvm::StructType::create (context (), printed);

	g_free (printed);

	unsigned size = mono_class_value_size (klass, NULL);
	std::vector<LayoutField> fields;

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

	set_packed_body (context (), type, size, fields);
	vtypes[klass] = type;
	return type;
}

/// The LLVM type for one field of a native layout, of SIZE bytes.
///
/// Only two things about a native field reach LegacyAbiPass: how many bytes it
/// covers, and whether those bytes ride in an SSE register. A field marshalling
/// passes through keeps its own type so the classifier still sees the float or
/// recurses into the nested struct; everything marshalling rewrites - a bool
/// widened to a Win32 BOOL, a string turned into a pointer, an array inlined -
/// becomes opaque data of the right width, which classifies as integer whatever
/// it started as.
llvm::Expected<llvm::Type *>
MethodLLVMEmitter::native_field_type (MonoType *t, MonoMarshalSpec *mspec, int size)
{
	if (mspec == nullptr && !t->byref) {
		MonoType *underlying = mini_get_underlying_type (t);

		switch (underlying->type) {
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			return convert_type (underlying);
		case MONO_TYPE_VALUETYPE:
		case MONO_TYPE_GENERICINST:
			if (m_class_is_valuetype (
			            mono_class_from_mono_type_internal (underlying)))
				return convert_type (underlying, /*native=*/true);
			break;
		default:
			break;
		}
	}

	if (size == 1 || size == 2 || size == 4 || size == 8)
		return llvm::Type::getIntNTy (context (), size * 8);
	return llvm::ArrayType::get (llvm::Type::getInt8Ty (context ()), size);
}

/// KLASS in the layout marshalling copies it into: the offsets and widths
/// mono_marshal_load_type_info () worked out, which is what the C on the other
/// side of the boundary was compiled against.
llvm::Expected<llvm::Type *>
MethodLLVMEmitter::convert_native_vtype (MonoClass *klass)
{
	auto it = native_vtypes.find (klass);
	if (it != native_vtypes.end ())
		return it->second;

	MonoMarshalType *info = mono_marshal_load_type_info (klass);

	if (info == nullptr)
		return conversion_error (llvm::Twine ("no native layout for ")
		                         + m_class_get_name (klass));

	char *printed = mono_type_full_name (m_class_get_byval_arg (klass));
	std::string name = std::string (printed) + "$native";
	llvm::StructType *type = llvm::StructType::create (context (), name);

	g_free (printed);

	unsigned size = mono_class_native_size (klass, NULL);
	bool unicode = m_class_is_unicode (klass);
	std::vector<LayoutField> fields;

	for (guint32 i = 0; i < info->num_fields; ++i) {
		MonoMarshalField &field = info->fields[i];

		if (field.field->type->attrs & FIELD_ATTRIBUTE_STATIC)
			continue;

		guint32 align;
		int width = mono_marshal_type_size (field.field->type, field.mspec, &align,
		                                    /*as_field=*/TRUE, unicode);
		llvm::Expected<llvm::Type *> converted =
			native_field_type (field.field->type, field.mspec, width);

		if (!converted)
			return converted.takeError ();

		fields.push_back ({ static_cast<int> (field.offset), width, *converted });
	}

	set_packed_body (context (), type, size, fields);
	native_vtypes[klass] = type;
	return type;
}

bool
MethodLLVMEmitter::native_signature () const
{
	return speaks_marshalled_layout (method, mono_method_signature_internal (method));
}

/// The alignment an instance of T needs in memory.
///
/// The struct convert_vtype builds is packed, which the data layout reads as
/// 1-aligned, so every alloca of one has to be told what the runtime decided
/// instead.
llvm::Align
MethodLLVMEmitter::type_alignment (MonoType *t, bool native)
{
	if (t->byref)
		return llvm::Align (TARGET_SIZEOF_VOID_P);

	MonoClass *klass = mono_class_from_mono_type_internal (mini_get_underlying_type (t));
	unsigned align = mono_class_min_align (klass);

	/*
	 * A marshalled layout widens fields the managed one packs tightly, so it
	 * is the marshalling code's own alignment that a buffer of it needs.
	 */
	if (native && m_class_is_valuetype (klass) && !marshals_unchanged (klass)) {
		guint32 native_align = 0;

		mono_class_native_size (klass, &native_align);
		if (native_align != 0)
			align = native_align;
	}

	/*
	 * A vector's natural alignment is its size, but nothing the runtime hands
	 * out promises more than 8 - the GC allocates on words, so a vector inside
	 * an array or an object is only word-aligned. Claiming 8 keeps every
	 * vector access an unaligned instruction; LLVM raises it back where it can
	 * prove more, which is exactly the allocas.
	 */
	if (MONO_CLASS_IS_SIMD (cfg, klass))
		align = std::min (8, mono_class_value_size (klass, NULL));

	/*
	 * A packed layout can ask for an alignment that is not a power of two, and
	 * a class whose metadata failed to load has no alignment at all.
	 */
	return llvm::Align (llvm::PowerOf2Ceil (std::max (align, 1u)));
}

/// The LLVM function type for SIG, in this backend's own convention: every
/// value in its natural type, value types by value as their struct, aggregate
/// returns returned as aggregates. Only LegacyAbiPass ever lowers any of it.
///
/// NATIVE says the operands are in the layout marshalling produced rather than
/// the managed one, which is what a signature the C side was compiled against
/// describes. Whether a given pinvoke signature is being used that way is the
/// caller's to know: an indirect call through one really does reach native
/// code, while a [DllImport] method's own signature is a description of the
/// native function and not of the wrapper every managed caller enters.
llvm::Expected<llvm::FunctionType *>
MethodLLVMEmitter::convert_method_signature (MonoMethodSignature *sig, bool native)
{
	/*
	 * A vararg signature that is also a native one is C varargs, a different
	 * convention entirely from the runtime's cookie one, and nothing here
	 * speaks it.
	 */
	if (sig->call_convention == MONO_CALL_VARARG && native)
		return conversion_error ("a native vararg signature is C varargs");

	llvm::Expected<llvm::Type *> ret = convert_type (sig->ret, native);

	if (!ret)
		return ret.takeError ();

	std::vector<llvm::Type *> params;

	if (sig->hasthis)
		params.push_back (pointer_type (context ()));

	for (int i = 0; i < vararg_fixed_params (sig); ++i) {
		llvm::Expected<llvm::Type *> converted = convert_type (sig->params[i], native);

		if (!converted)
			return converted.takeError ();
		params.push_back (*converted);
	}

	/* The cookie buffer the variable part travels in - see build_sig_cookie (). */
	if (sig->call_convention == MONO_CALL_VARARG)
		params.push_back (pointer_type (context ()));

	return llvm::FunctionType::get (*ret, params, false);
}

/// The number of SIG's parameters that are ordinary ones, which for a vararg
/// signature means the fixed part ahead of the sentinel.
///
/// A vararg method's own signature carries its sentinel past the last
/// parameter, so a declaration and every call site that names it agree on this
/// count - which is what lets both convert to one function type.
int
vararg_fixed_params (MonoMethodSignature *sig)
{
	if (sig->call_convention != MONO_CALL_VARARG || sig->sentinelpos < 0)
		return sig->param_count;

	return std::min (static_cast<int> (sig->sentinelpos),
	                 static_cast<int> (sig->param_count));
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
	/* ARGS are the signature's arguments, which start past any hidden return pointer. */
	unsigned leading = hidden_return_type (callee) != nullptr ? 1 : 0;

	for (unsigned i = 0; i < adapted.size () && i + leading < type->getNumParams (); ++i) {
		llvm::Type *want = type->getParamType (i + leading);
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

	/* The checkpoint icall is that check, so wrapping it in one would recurse. */
	bool check = id != MONO_JIT_ICALL_mono_thread_interruption_checkpoint;

	return create_method_decl (mono_marshal_get_icall_wrapper (info, check));
}

/// The declaration of METHOD in this module, created on first use and cached.
///
/// A method this backend compiles is declared fastcc against its `$fast`
/// symbol, which the engine resolves to the fastcc body's stub. One whose code
/// mini produces instead - an icall, a pinvoke, a runtime-implemented method -
/// is declared against the plain symbol in the legacy convention, and every
/// call to it lowers in LegacyAbiPass.
///
/// A fastcc declaration whose return will not fit in the return registers
/// carries the hidden pointer it comes back through as its leading parameter -
/// see hidden-return.hpp. The legacy convention has a hidden pointer of its
/// own, in a place the runtime's trampolines fixed, so a legacy declaration is
/// left in the signature's own terms for LegacyAbiPass to lower.
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

	llvm::Expected<llvm::FunctionType *> type =
		convert_method_signature (sig, speaks_marshalled_layout (method, sig));
	if (!type)
		return type.takeError ();

	bool legacy = implemented_outside_il (method);
	llvm::Type *hidden = nullptr;

	if (!legacy && returns_by_hidden_pointer ((*type)->getReturnType ())) {
		hidden = (*type)->getReturnType ();
		*type = hidden_return_prototype (*type, hidden);
	}

	char *printed = mono_method_full_name (method, TRUE);

	/*
	 * identity_symbol () for the same reason it is used everywhere else, with one
	 * more of its own: conversion operators overload on their return type, which
	 * no printed signature carries, and runtime-minted wrappers print alike. This
	 * name is how a caller's reference finds the method's stub, so
	 * symbol_for_code () (runtime.cpp) must agree with it.
	 */
	std::string full_name = identity_symbol (printed, method);

	g_free (printed);

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
			context (), arch::legacy_cc_attribute,
			arch::legacy_flavor_value (legacy_entry_flavor (method, sig))));
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

	/*
	 * Every parameter index below is an IL argument number shifted past the
	 * hidden return pointer, which is a parameter of this convention's own and
	 * belongs to none of them.
	 */
	unsigned leading = hidden != nullptr ? 1 : 0;

	if (hidden != nullptr) {
		function->addParamAttrs (0, llvm::AttrBuilder (
						    context (),
						    hidden_return_attributes (context (), hidden)));
		function->getArg (0)->setName ("ret");
	}

	if (sig->hasthis)
		function->getArg (leading)->setName ("this");

	std::vector<const char *> names (sig->param_count);
	if (sig->param_count > 0)
		mono_method_get_param_names (method, names.data ());

	for (int i = 0; i < sig->param_count; ++i) {
		unsigned pindex = i + sig->hasthis + leading;

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
