#include "method-to-llvm.hpp"
#include "hidden-return.hpp"
#include "layout.hpp"
#include "mini-runtime.h"
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

/// An error for a signature or a type this converter cannot express in LLVM IR.
///
/// This becomes an ExecutionEngineException, the same as the refusals in
/// invalid-il.cpp. A type this engine cannot express is a limit of the
/// engine, not of the program, and no other engine compiles the method
/// instead.
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

/// The vector type a SIMD class travels in, or null if this is not a shape
/// this converter can lower.
///
/// The type must be a vector, not a struct of its elements. The calling
/// convention places a SIMD value in an SSE register, and a struct does not
/// get that placement.
llvm::Type *
simd_class_to_llvm_type (llvm::LLVMContext &ctx, MonoClass *klass)
{
	/*
	 * Vector<T> describes itself: the element is the type argument, and how
	 * many of them there are is the class's own size.
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
	/*
	 * Vector4f is a Mono.Simd type. Vector2, Vector3 and Vector4 are
	 * System.Numerics types. All four fit in one 128-bit, four-float register.
	 */
	if (name == "Vector4f" || name == "Vector2" || name == "Vector3" || name == "Vector4")
		return llvm::FixedVectorType::get (llvm::Type::getFloatTy (ctx), 4);

	return nullptr;
}

/// Whether marshalling hands klass across a native boundary byte for byte, so
/// its native layout is the managed one that convert_vtype already builds.
///
/// These are the same three cases marshal-ilgen skips conversion for, in
/// emit_marshal_vtype_ilgen. Marshalling copies every other value type field
/// by field into a buffer that can be a different size, with fields at
/// different offsets.
bool
marshals_unchanged (MonoClass *klass)
{
	return mono_class_is_explicit_layout (klass) || m_class_is_blittable (klass)
	       || m_class_is_enumtype (klass);
}

/// Whether method is entered with sig's value types already marshalled.
///
/// A pinvoke signature by itself does not prove that a caller holds
/// marshalled values. A [DllImport] method carries a pinvoke signature too,
/// but a managed caller enters it through the wrapper the runtime built. The
/// marshalled layout begins only inside that wrapper, after it converts its
/// arguments.
///
/// A wrapper whose own body faces native code is different. The wrapper a
/// delegate hands to native code as its entry point uses the native
/// signature, because it is that native entry point.
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
 * The bytes from at to size, appended to body as the continuation of last.
 *
 * A gap at the end of a value type is not padding. A C# `fixed` buffer is a
 * single field of the element type, inside a class sized for the whole
 * array. Metadata has no field for anything past the first element, and a
 * .pack directive leaves the same shape. Those bytes are live data, but the
 * classification only ever sees fields, so the last field must extend to
 * the end of the type. mini does the same thing in collect_field_info_nested
 * (arch-amd64.c): "This can happen with .pack directives eg. 'fixed'
 * arrays".
 *
 * Repeating a primitive keeps the register file correct. The tail of a
 * `fixed float` buffer rides the SSE file, and plain bytes do not say that.
 * Anything else - a pointer, an inlined array, a marshalled string - becomes
 * integer data at whatever width it is given. Plain bytes say the same
 * thing, and mini widens the field's size instead of repeating it, for the
 * same reason.
 */
void
fill_tail (llvm::LLVMContext &ctx, std::vector<llvm::Type *> &body,
           const LayoutField *last, int at, unsigned size)
{
	int gap = static_cast<int> (size) - at;

	// A type with no field at all is all padding, and stays that way.
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
 * Lays fields into type's body as a packed struct that spells out the real
 * layout. Each field sits at the offset it was laid out at, and
 * padding_type () fills the gaps between them.
 *
 * The layout is real, so LLVM can reason about the fields. The struct is
 * packed, so the offsets match the runtime's layout instead of whatever
 * DataLayout infers on its own. Padding takes a shape no real field ever
 * takes, which lets MonoAbiPass tell data from padding when it classifies
 * a value. A float that shares an eightbyte with padding is still a float
 * to the C ABI.
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
	 * An explicit layout can overlap fields, and a struct cannot express an
	 * overlap. Whichever field comes first keeps its slot, and the rest of
	 * the union becomes padding. This only loses the overlapped fields' say
	 * in the native classification - the bytes are all still there.
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

	// A layout the walk cannot restate keeps the right size, opaquely.
	type->setBody (llvm::ArrayType::get (llvm::Type::getInt8Ty (ctx), size),
	               /*isPacked=*/true);
}

} // namespace

/// The C ABI leaves a narrow integer's high bits undefined. The signature
/// must say which way to fill them, because the caller and the callee
/// cannot each decide on their own.
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
is_intrinsic (MonoMethod *method)
{
	return m_class_get_image (method->klass) == mono_get_corlib ()
	       && std::string_view (m_class_get_name_space (method->klass)) == "System"
	       && std::string_view (m_class_get_name (method->klass)) == "ByReference`1";
}

bool
implemented_outside_il (MonoMethod *method)
{
	/*
	 * A wrapper carries its own IL body, whatever native code or icall it
	 * wraps. Checking wrapper_type first stops a marshalling wrapper from
	 * counting as implemented outside IL.
	 */
	if (method->wrapper_type != MONO_WRAPPER_NONE)
		return false;

	return (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) != 0 ||
	       (method->iflags & METHOD_IMPL_ATTRIBUTE_RUNTIME) != 0 ||
	       (method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) != 0;
}

MonoMethod *
icall_wrapper_target (MonoMethod *method)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (method->wrapper_type != MONO_WRAPPER_NONE)
		return method;
	if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) == 0)
		return method;
	// Array Get/Set/Address: no icall stands behind them at all.
	if ((method->iflags & METHOD_IMPL_ATTRIBUTE_NATIVE) != 0)
		return method;
	/*
	 * A COM class's wrapper is the interop one, and that code is compiled
	 * out of some builds. MONO_CLASS_IS_IMPORT is the flag
	 * mono_marshal_get_native_wrapper () branches on before it asserts.
	 */
	if (MONO_CLASS_IS_IMPORT (method->klass))
		return method;
	if (sig == nullptr || sig->pinvoke == 0)
		return method;

	guint32 flags = 0;

	mono_lookup_internal_call_full_with_flags (method, FALSE, &flags);
	if ((flags & MONO_ICALL_FLAGS_NO_WRAPPER) != 0)
		return method;

	return mono_marshal_get_native_wrapper (method, TRUE, mono_aot_only);
}

bool
entered_in_c (MonoMethod *method)
{
	/*
	 * The address the runtime publishes for a no-wrapper icall is the
	 * registered C function itself, so that one entry really is C.
	 * Everything else implemented outside IL is reached through a wrapper,
	 * and a wrapper is a method this backend compiles and publishes like any
	 * other. A call to it is an ordinary call, whatever the declaration
	 * reads like.
	 */
	if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) == 0)
		return false;

	guint32 flags = 0;

	mono_lookup_internal_call_full_with_flags (method, FALSE, &flags);
	return (flags & MONO_ICALL_FLAGS_NO_WRAPPER) != 0;
}

void
MethodLLVMEmitter::mark_mono_call (llvm::CallBase *call)
{
	call->addFnAttr (llvm::Attribute::get (call->getContext (), arch::mono_cc_attribute));
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
	// Generic sharing hands these over as references.
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		return pointer_type (context ());
	case MONO_TYPE_GENERICINST:
		if (!mono_type_generic_inst_is_valuetype (t))
			return pointer_type (context ());
		// Fall through
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
 * Whether a value of type t rides the evaluation stack as the address of a
 * frame slot holding it, instead of as an SSA value.
 *
 * This is true exactly for the types convert_type () turns into a struct: a
 * value class laid out field by field. LLVM does not keep a struct whole.
 * SROA and InstCombine take every load of one apart into its fields, and
 * put it back together at every store. A front end that moves a struct
 * around as an SSA value spends the pipeline's time undoing its own work. A
 * memcpy between slots says the same thing in one instruction. mem2reg
 * still promotes the slot when the fields turn out to be all that is
 * wanted.
 *
 * A SIMD class and an enum are not held in memory. convert_type turns them
 * into a vector and a scalar, and both belong in a register.
 */
bool
MethodLLVMEmitter::held_in_memory (MonoType *t)
{
	/*
	 * This asks convert_type instead of working it out again from the
	 * metadata. The two must agree about SIMD types and enums, and letting
	 * convert_type answer is the only way to guarantee that. A type
	 * convert_type cannot convert has no representation to pick either -
	 * whichever caller wanted one is about to fail on it.
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
 * A value type converts to a packed struct that spells out its real layout
 * - see set_packed_body (), which describes that shape.
 *
 * native asks for the layout marshalling gives the class instead. That
 * layout is a different struct whenever marshalling moves a field or
 * changes its width. Only a pinvoke signature speaks in those terms, and
 * only for classes marshalling actually rewrites. For every other class, the
 * two layouts are the same bytes. Sharing one type lets a value cross
 * between the two worlds without a conversion that changes nothing.
 */
llvm::Expected<llvm::Type *>
MethodLLVMEmitter::convert_vtype (MonoType *t, bool native)
{
	MonoClass *klass = mono_class_from_mono_type_internal (t);

	/*
	 * A class's layout step is what finds a bad layout, for example an
	 * unaligned reference field. A class that fails layout has no layout to
	 * convert.
	 *
	 * If the class fails to lay out, this function reports that failure
	 * directly, instead of reading offsets a failed class never received.
	 * The caller turns the error into the TypeLoadException the program is
	 * owed.
	 *
	 * mono_class_init_checked only settles metadata here. It is not the
	 * class initializer, and the initializer must never run on this path.
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

	auto it = types.vtypes.find (klass);
	if (it != types.vtypes.end ())
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
	types.vtypes[klass] = type;
	return type;
}

/// The LLVM type for one field of a native layout, of size bytes.
///
/// Only two facts about a native field reach MonoAbiPass: how many bytes
/// it covers, and whether those bytes ride in an SSE register. A field that
/// marshalling passes through unchanged keeps its own type, so the
/// classifier still sees the float, or recurses into the nested struct.
/// Everything marshalling rewrites becomes opaque data of the right width.
/// This covers a bool widened to a Win32 BOOL, a string turned into a
/// pointer, and an array inlined. That data classifies as integer, whatever
/// type it started as.
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

/// klass in the layout marshalling copies it into.
///
/// mono_marshal_load_type_info () works out the offsets and widths of that
/// layout. That is the layout the C code on the other side of the boundary
/// was compiled against.
llvm::Expected<llvm::Type *>
MethodLLVMEmitter::convert_native_vtype (MonoClass *klass)
{
	auto it = types.native_vtypes.find (klass);
	if (it != types.native_vtypes.end ())
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
	types.native_vtypes[klass] = type;
	return type;
}

bool
MethodLLVMEmitter::native_signature () const
{
	return speaks_marshalled_layout (method, mono_method_signature_internal (method));
}

/// The alignment a location needs to hold a value of type t.
///
/// convert_vtype builds a packed struct, and the data layout reads a packed
/// struct as 1-aligned. Every alloca of one must be told the alignment the
/// runtime decided on instead.
llvm::Align
MethodLLVMEmitter::type_alignment (MonoType *t, bool native)
{
	if (t->byref)
		return llvm::Align (TARGET_SIZEOF_VOID_P);

	/*
	 * A reference is one pointer wide, and the runtime puts every slot that
	 * holds one on a pointer boundary. mono_class_layout_fields force-aligns
	 * a reference-bearing field to TARGET_SIZEOF_VOID_P, whatever Pack asked
	 * for. An explicit layout that puts one off that boundary is a type load
	 * failure. The collector marks in words and cannot find the reference
	 * otherwise.
	 *
	 * Resolving to a class below gives the alignment of the object pointed
	 * at, and that says nothing about the slot itself. An access that names
	 * an unaligned slot still carries the `unaligned.` prefix.
	 */
	if (mini_type_is_reference (t))
		return llvm::Align (TARGET_SIZEOF_VOID_P);

	MonoClass *klass = mono_class_from_mono_type_internal (mini_get_underlying_type (t));
	unsigned align = mono_class_min_align (klass);

	/*
	 * A marshalled layout widens fields that the managed layout packs
	 * tightly. A buffer of it needs the marshalling code's own alignment.
	 */
	if (native && m_class_is_valuetype (klass) && !marshals_unchanged (klass)) {
		guint32 native_align = 0;

		mono_class_native_size (klass, &native_align);
		if (native_align != 0)
			align = native_align;
	}

	/*
	 * A vector's natural alignment equals its size, but nothing the runtime
	 * hands out promises more than 8 bytes. The GC allocates on word
	 * boundaries, so a vector inside an array or an object is only
	 * word-aligned. Claiming 8 keeps every vector access an unaligned
	 * instruction. LLVM raises the alignment back up where it can prove
	 * more, which happens exactly for allocas.
	 */
	if (MONO_CLASS_IS_SIMD (cfg, klass))
		align = std::min (8, mono_class_value_size (klass, NULL));

	/*
	 * A packed layout can ask for an alignment that is not a power of two.
	 * A class whose metadata failed to load has no alignment at all.
	 */
	return llvm::Align (llvm::PowerOf2Ceil (std::max (align, 1u)));
}

/// The LLVM function type for sig, built in this backend's own convention.
/// Every value keeps its natural type. A value type travels by value, as
/// its struct, and an aggregate return comes back as an aggregate. Only
/// MonoAbiPass lowers any of this.
///
/// native means the operands use the layout marshalling produced, instead
/// of the managed layout. That is the layout a signature describes when the
/// C side was compiled against it. The caller must know whether a given
/// pinvoke signature is used that way. An indirect call through a pinvoke
/// signature does reach native code. A [DllImport] method's own signature
/// describes the native function, not the wrapper every managed caller
/// enters.
llvm::Expected<llvm::FunctionType *>
MethodLLVMEmitter::convert_method_signature (MonoMethodSignature *sig, bool native)
{
	/*
	 * A vararg signature that is also a native signature is C varargs, a
	 * different convention from the runtime's cookie convention. Nothing
	 * here expresses it.
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

	// The variable part of a vararg call travels in the cookie buffer that
	// build_sig_cookie () builds.
	if (sig->call_convention == MONO_CALL_VARARG)
		params.push_back (pointer_type (context ()));

	return llvm::FunctionType::get (*ret, params, false);
}

/// The number of sig's ordinary parameters. For a vararg signature, this is
/// the fixed part ahead of the sentinel.
///
/// A vararg method's own signature carries its sentinel past the last fixed
/// parameter. A declaration and every call site that names the method agree
/// on this count, which lets both convert to one function type.
int
vararg_fixed_params (MonoMethodSignature *sig)
{
	if (sig->call_convention != MONO_CALL_VARARG || sig->sentinelpos < 0)
		return sig->param_count;

	return std::min (static_cast<int> (sig->sentinelpos),
	                 static_cast<int> (sig->param_count));
}

/// args, reshaped to match callee's declared parameter types.
///
/// An icall signature spells a runtime address as native int, while the
/// translator holds the same value as a pointer. A call must match exactly
/// what the declaration says.
std::vector<llvm::Value *>
MethodLLVMEmitter::adapt_to_callee (MonoIrBuilder &builder, llvm::Function *callee,
                                    llvm::ArrayRef<llvm::Value *> args)
{
	llvm::FunctionType *type = callee->getFunctionType ();
	std::vector<llvm::Value *> adapted (args.begin (), args.end ());

	// args are the signature's arguments. The hidden return pointer is not one of them.
	for (unsigned i = 0; i < adapted.size (); ++i) {
		unsigned at = natural_parameter_index (i, callee);

		if (at >= type->getNumParams ())
			break;

		llvm::Type *want = type->getParamType (at);
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

/// The declaration of the managed wrapper around jit icall id.
///
/// The runtime's own entry points report failure by leaving a pending
/// exception. Calling the raw C function directly leaves that exception
/// unchecked, so the wrapper follows the call with the check that turns it
/// into a throw. Any entry point that can fail must be called this way.
llvm::Expected<llvm::Function *>
MethodLLVMEmitter::icall_wrapper_decl (MonoJitICallId id)
{
	MonoJitICallInfo *info = mono_find_jit_icall_info (id);

	// The checkpoint icall is that check, so wrapping it in one recurses.
	bool check = id != MONO_JIT_ICALL_mono_thread_interruption_checkpoint;

	return create_method_decl (mono_marshal_get_icall_wrapper (info, check));
}

/// The declaration of method in this module, created on first use and cached.
///
/// A method this backend translates is declared in this backend's own
/// convention. The engine resolves that declaration to the stub the
/// method's body is published under. A method implemented outside IL - an
/// icall, a pinvoke, a runtime-implemented method - is declared in the C
/// convention instead. It is marked with the `monocc` function attribute,
/// and every call to it lowers in MonoAbiPass.
///
/// A natural declaration whose return does not fit in the return registers
/// carries the hidden return pointer as a parameter of its own - see
/// hidden-return.hpp. A C declaration stays in the signature's own terms,
/// and MonoAbiPass lowers it.
llvm::Expected<llvm::Function *>
MethodLLVMEmitter::create_method_decl (MonoMethod *method, bool by_context)
{
	/*
	 * An open callee has one entry per instantiation, and this declaration
	 * resolves to the shared method's own, which no caller can enter: it would
	 * arrive with the receiver of some other instantiation, or none. A call
	 * site reads the entry it wants out of the context and says so with
	 * by_context. Every other site refuses, and the method is compiled against
	 * the instantiation that was asked for.
	 */
	if (!by_context && method != this->method && depends_on_context (method))
		cannot_share ("a direct reference to an open method's entry");

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
	 * A string constructor compiles against a signature that returns the
	 * string it creates. There is no preallocated this to fill in, and every
	 * caller must see that shape.
	 */
	if (method->string_ctor)
		sig = mono_marshal_get_string_ctor_signature (method);

	llvm::Expected<llvm::FunctionType *> type =
		convert_method_signature (sig, speaks_marshalled_layout (method, sig));
	if (!type)
		return type.takeError ();

	bool in_c = entered_in_c (method);
	llvm::Type *hidden = nullptr;

	if (!in_c && returns_by_hidden_pointer ((*type)->getReturnType ())) {
		hidden = (*type)->getReturnType ();
		*type = hidden_return_prototype (*type, hidden);
	}

	/*
	 * A shared body with no receiver is entered with its context in a register,
	 * which `nest` is how unmodified LLVM asks for. It trails, and after the
	 * hidden return pointer has been placed: `nest` takes no argument register,
	 * so it must not shift the sequence the pointer's position is counted in.
	 */
	bool keyed = method == this->method && takes_context_argument ();

	if (keyed) {
		std::vector<llvm::Type *> params ((*type)->param_begin (),
		                                  (*type)->param_end ());

		params.push_back (llvm::PointerType::get (context (), 0));
		*type = llvm::FunctionType::get ((*type)->getReturnType (), params,
		                                 (*type)->isVarArg ());
	}

	/*
	 * A placeholder name. The engine reads the marker below and renames the
	 * declaration to whatever it publishes the entry under. Nothing needs
	 * to agree with it in advance. It only needs to be unique, which
	 * identity_symbol () gives it, and legible, because a dump reads the
	 * untranslated IR directly.
	 *
	 * The name skips the signature, the expensive half of printing a
	 * method, because the signature buys nothing here. The pointer inside
	 * identity_symbol () already makes the name unique, and a dump shows
	 * the declaration's own type right beside it.
	 */
	char *printed = mono_method_full_name (method, FALSE);
	std::string full_name = identity_symbol (printed, method);

	g_free (printed);

	// A batched module defines several methods, and a call to one of the others
	// must reach its published entry rather than the body next door: a direct
	// call is off the thunk, so a later detour or promotion of that method never
	// reaches it. A name of its own is what keeps the declaration from finding
	// the definition, the way code_address_symbol () keeps ldftn off it.
	if (method != this->method && llvm::is_contained (siblings, method))
		full_name += "$thunk";

	// A copy folded into a caller is a second body for the method, and the
	// module can already hold the method's own under the plain name.
	if (method == this->method)
		full_name += body_suffix;

	/*
	 * The emitter's own cache is per instance, but filter bodies share their
	 * method's module across instances. A name already declared there must
	 * be reused, or LLVM quietly uniques it into a symbol nothing resolves.
	 */
	if (llvm::Function *existing = module->getFunction (full_name)) {
		declarations[method] = existing;
		return existing;
	}

	llvm::Function *function = llvm::Function::Create (
		*type, llvm::GlobalValue::ExternalLinkage, full_name, module);

	/*
	 * Before anything below counts parameters: placed_parameter_count () leaves
	 * out a trailing `nest`, and the hidden return pointer's position is
	 * counted with it left out.
	 */
	if (keyed) {
		unsigned at = function->arg_size () - 1;

		function->addParamAttr (at, llvm::Attribute::Nest);
		function->getArg (at)->setName ("rgctx");
	}

	record_external (full_name, ExternalSymbol::Kind::Code, method);
	mark_method_reference (*function, method);

	if (in_c)
		function->addFnAttr (
			llvm::Attribute::get (context (), arch::mono_cc_attribute));

	if (llvm::Attribute::AttrKind ext = integer_extension (sig->ret);
	    ext != llvm::Attribute::None)
		function->addRetAttr (ext);

	// A string constructor returns the string it creates, and that string is new.
	if (method->string_ctor)
		function->addRetAttr (llvm::Attribute::NoAlias);

	/*
	 * Every parameter index below is an IL argument number, shifted around
	 * the hidden return pointer. The pointer belongs to this convention
	 * alone, not to any IL argument. "Around" and not "past": the pointer
	 * sits behind the first argument, so argument 0 still comes before it.
	 */
	if (hidden != nullptr) {
		unsigned at = hidden_return_index (placed_parameter_count (function));

		function->addParamAttrs (at, llvm::AttrBuilder (
						     context (),
						     hidden_return_attributes (context (), hidden)));
		function->getArg (at)->setName ("ret");
	}

	if (sig->hasthis)
		function->getArg (natural_parameter_index (0, function))->setName ("this");

	std::vector<const char *> names (sig->param_count);
	if (sig->param_count > 0)
		mono_method_get_param_names (method, names.data ());

	for (int i = 0; i < sig->param_count; ++i) {
		unsigned pindex = natural_parameter_index (i + sig->hasthis, function);

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
