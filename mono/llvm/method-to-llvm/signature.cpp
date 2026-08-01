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

/// The alignment an instance of T needs in memory.
///
/// LLVM cannot work this out for itself: a vtype reaches it as a run of bytes,
/// which the data layout reads as 1-aligned, so every alloca of one has to be told
/// what the runtime decided instead.
llvm::Align
MethodLLVMEmitter::type_alignment (MonoType *t)
{
	if (t->byref)
		return llvm::Align (TARGET_SIZEOF_VOID_P);

	MonoClass *klass = mono_class_from_mono_type_internal (mini_get_underlying_type (t));
	unsigned align = MONO_CLASS_IS_SIMD (cfg, klass) ? mono_class_value_size (klass, NULL)
	                                                 : mono_class_min_align (klass);

	/* A packed layout can ask for an alignment that is not a power of two. */
	while (mono_is_power_of_two (align) == -1)
		align++;

	return llvm::Align (align);
}

/*
 * How a value type travels through a call.
 *
 * LLVM's own lowering of a first-class struct argument passes each IR element
 * separately, so the {N x i8} shape convert_vtype builds would cross a call as
 * N byte-sized arguments - nothing like what code the runtime compiled expects
 * on the other side. The convention is mini's (get_call_info/add_valuetype,
 * mini-amd64.c), so it is restated here as IR types LLVM has no discretion
 * over:
 *
 *   - a managed value type of up to 16 bytes travels as one or two integer
 *     register words (never float registers, unlike the C ABI);
 *   - one whose fields straddle an 8-byte boundary, or that is bigger than 16
 *     bytes, or that arrives after the integer argument registers have run
 *     out, is copied onto the stack instead - a byval pointer here;
 *   - a managed return of up to 8 bytes comes back in RAX; anything bigger
 *     travels through a pointer the caller passes (vret_arg_index), because a
 *     hidden argument LLVM inserted on its own would sit in front of `this`,
 *     which the runtime's trampolines insist on finding in the first register;
 *   - a native (pinvoke) signature classifies each word as integer or float
 *     the way the C ABI does, since the other side is C.
 *
 * TODO: this whole convention exists to interoperate with code mini compiled.
 * Once mini's code generator is dropped, drop the explicit vret pointer and
 * the coercions here and let LLVM's own sret and aggregate lowering define the
 * convention.
 */

namespace {

/// One eightbyte of a value type, classified as the register file it rides in.
enum class QuadClass { None, Integer, Sse };

/// How a value type crosses a call, before register availability is known.
struct VtypeShape {
	bool memory = false;  ///< always a stack copy, no matter the registers
	bool nothing = false; ///< no data travels at all (a fieldless native type)
	unsigned nquads = 0;
	QuadClass cls[2] = { QuadClass::None, QuadClass::None };
	unsigned qsize[2] = { 0, 0 };
};

struct VtypeField {
	MonoType *type;
	int size;
	int offset;
};

/// Every leaf field of KLASS with its flattened offset, nested structs walked
/// into - mini's collect_field_info_nested.
void
collect_vtype_fields (MonoClass *klass, std::vector<VtypeField> &fields, int offset,
                      bool pinvoke, bool unicode)
{
	if (pinvoke) {
		MonoMarshalType *info = mono_marshal_load_type_info (klass);

		for (int i = 0; i < info->num_fields; ++i) {
			MonoType *ftype = info->fields[i].field->type;

			if (MONO_TYPE_ISSTRUCT (ftype)) {
				collect_vtype_fields (mono_class_from_mono_type_internal (ftype),
				                      fields, offset + info->fields[i].offset,
				                      pinvoke, unicode);
				continue;
			}

			guint32 align;
			VtypeField f = {
				ftype,
				static_cast<int> (mono_marshal_type_size (
					ftype, info->fields[i].mspec, &align, TRUE, unicode)),
				offset + static_cast<int> (info->fields[i].offset),
			};

			/*
			 * A .pack directive (a 'fixed' array, say) can leave the last
			 * field short of the native size; the classification needs the
			 * tail described, so the field repeats until it covers it.
			 */
			if (i == info->num_fields - 1
			    && f.size + f.offset < offset + static_cast<int> (info->native_size)) {
				if (MONO_TYPE_IS_PRIMITIVE (f.type)) {
					fields.push_back (f);
					while (f.size + f.offset
					       < offset + static_cast<int> (info->native_size)) {
						f.offset += f.size;
						fields.push_back (f);
					}
				} else {
					f.size = offset + info->native_size - f.offset;
					fields.push_back (f);
				}
			} else {
				fields.push_back (f);
			}
		}
		return;
	}

	gpointer iter = NULL;

	while (MonoClassField *field = mono_class_get_fields_internal (klass, &iter)) {
		if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
			continue;

		if (MONO_TYPE_ISSTRUCT (field->type)) {
			collect_vtype_fields (
				mono_class_from_mono_type_internal (field->type), fields,
				offset + field->offset - MONO_ABI_SIZEOF (MonoObject),
				pinvoke, unicode);
			continue;
		}

		int align;

		fields.push_back ({
			field->type,
			mono_type_size (field->type, &align),
			offset + field->offset - static_cast<int> (MONO_ABI_SIZEOF (MonoObject)),
		});
	}
}

/// The register file T rides in under the C ABI: floats ride the SSE file,
/// everything else the integer one.
QuadClass
scalar_class (MonoType *t)
{
	switch (mini_get_underlying_type (t)->type) {
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
		return QuadClass::Sse;
	default:
		return QuadClass::Integer;
	}
}

/// How SIG's value type T crosses a call - mini's add_valuetype, short of the
/// register accounting, which is the caller's to do.
llvm::Expected<VtypeShape>
classify_vtype (MonoMethodSignature *sig, MonoType *t, bool is_return)
{
	MonoClass *klass = mono_class_from_mono_type_internal (t);
	VtypeShape shape;

	unsigned size = mini_type_stack_size_full (m_class_get_byval_arg (klass), NULL,
	                                           sig->pinvoke);
	unsigned struct_size = sig->pinvoke
	                               ? mono_marshal_load_type_info (klass)->native_size
	                               : mono_class_value_size (klass, NULL);

	std::vector<VtypeField> fields;
	collect_vtype_fields (klass, fields, 0, sig->pinvoke, m_class_is_unicode (klass));

	bool straddles = false;
	for (const VtypeField &f : fields)
		if (f.offset < 8 && f.offset + f.size > 8)
			straddles = true;

	if (straddles && sig->pinvoke)
		return conversion_error (llvm::Twine ("a field of ")
		                         + m_class_get_name (klass)
		                         + " straddles an eightbyte, which the runtime "
		                           "does not marshal");

	bool in_registers = sig->pinvoke
	                            ? size != 0 && size <= 16 && struct_size <= 16
	                            : (is_return ? size == 8 : size <= 16);

	if (sig->pinvoke && fields.empty () && in_registers) {
		shape.nothing = true;
		return shape;
	}

	if (!in_registers || straddles) {
		shape.memory = true;
		return shape;
	}

	shape.nquads = size > 8 ? 2 : 1;

	if (!sig->pinvoke) {
		/* Managed data always rides the integer file, floats included. */
		unsigned n = struct_size;

		shape.cls[0] = QuadClass::Integer;
		shape.qsize[0] = n >= 8 ? 8 : n;
		if (shape.nquads == 2) {
			shape.cls[1] = QuadClass::Integer;
			shape.qsize[1] = 8;
		}
	} else {
		for (unsigned quad = 0; quad < shape.nquads; ++quad) {
			for (const VtypeField &f : fields) {
				if (quad == 0 && f.offset >= 8)
					continue;
				if (quad == 1 && f.offset < 8)
					continue;

				shape.qsize[quad] = f.offset + f.size - quad * 8;

				QuadClass fc = scalar_class (f.type);
				if (shape.cls[quad] == QuadClass::None)
					shape.cls[quad] = fc;
				else if (shape.cls[quad] != fc)
					shape.cls[quad] = QuadClass::Integer;
			}
		}
	}

	/* Register loads come in power-of-two widths. */
	for (unsigned quad = 0; quad < shape.nquads; ++quad) {
		g_assert (shape.qsize[quad] <= 8);
		while (shape.qsize[quad] != 0 && shape.qsize[quad] != 1
		       && shape.qsize[quad] != 2 && shape.qsize[quad] != 4
		       && shape.qsize[quad] != 8)
			shape.qsize[quad]++;
	}

	return shape;
}

/// The IR type SHAPE's register words travel as.
llvm::Type *
vtype_travel_type (llvm::LLVMContext &ctx, const VtypeShape &shape)
{
	llvm::SmallVector<llvm::Type *, 2> words;

	for (unsigned quad = 0; quad < shape.nquads; ++quad) {
		switch (shape.cls[quad]) {
		case QuadClass::None:
			break;
		case QuadClass::Integer:
			words.push_back (llvm::Type::getIntNTy (ctx, shape.qsize[quad] * 8));
			break;
		case QuadClass::Sse:
			words.push_back (shape.qsize[quad] <= 4
			                         ? llvm::Type::getFloatTy (ctx)
			                         : llvm::Type::getDoubleTy (ctx));
			break;
		}
	}

	if (words.empty ())
		return llvm::StructType::get (ctx);
	if (words.size () == 1)
		return words[0];
	return llvm::StructType::get (ctx, { words[0], words[1] });
}

} // namespace

/// Whether SIG's return value travels through a pointer the caller passes:
/// a managed value type bigger than a register. Smaller ones come back in RAX,
/// and native signatures keep the C ABI, which LLVM speaks for itself.
bool
MethodLLVMEmitter::returns_by_address (MonoMethodSignature *sig)
{
	if (sig->pinvoke)
		return false;

	MonoType *ret = mini_get_underlying_type (sig->ret);

	if (ret->byref || !mini_type_is_vtype (ret))
		return false;

	return mono_class_value_size (mono_class_from_mono_type_internal (ret), NULL) > 8;
}

/// Where the hidden return pointer sits in SIG's argument list.
///
/// The runtime keeps `this` in the first argument register no matter what, so
/// the trampolines that recover a receiver from a call site always know where
/// to look; the return pointer comes second. The same applies when the first
/// declared parameter is a reference type, because delegate-invoke wrappers
/// make virtual calls through calli signatures with hasthis unset
/// (mini-amd64.c, get_call_info).
unsigned
MethodLLVMEmitter::vret_arg_index (MonoMethodSignature *sig)
{
	if (sig->hasthis)
		return 1;
	if (sig->param_count > 0
	    && MONO_TYPE_IS_REFERENCE (mini_get_underlying_type (sig->params[0])))
		return 1;
	return 0;
}

/// How SIG's values travel, restated as IR types - see the classification
/// comment above. Computed once per signature and cached; the pointer stays
/// valid for the emitter's lifetime.
llvm::Expected<const MethodLLVMEmitter::SignatureABI *>
MethodLLVMEmitter::lower_signature (MonoMethodSignature *sig)
{
	auto it = signature_abis.find (sig);
	if (it != signature_abis.end ())
		return it->second.get ();

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

	constexpr unsigned param_gregs = 6, param_fregs = 8;
	unsigned gr = 0, fr = 0;

	auto abi = std::make_unique<SignatureABI> ();

	llvm::Expected<llvm::Type *> ret_type = convert_type (sig->ret);
	if (!ret_type)
		return ret_type.takeError ();

	llvm::Type *ret = *ret_type;
	MonoType *ret_mono = mini_get_underlying_type (sig->ret);

	if (!ret_mono->byref && mini_type_is_vtype (ret_mono)) {
		if (returns_by_address (sig)) {
			abi->ret_by_address = true;
			abi->vret_index = vret_arg_index (sig);
			ret = llvm::Type::getVoidTy (context ());
		} else {
			llvm::Expected<VtypeShape> shape =
				classify_vtype (sig, ret_mono, true);

			if (!shape)
				return shape.takeError ();

			/*
			 * A native return the C ABI keeps in memory stays the raw
			 * aggregate: LLVM demotes it to the C hidden-pointer shape on
			 * its own, and native is the one place its own rule is right.
			 */
			if (!shape->memory && !shape->nothing) {
				abi->ret_coerced =
					vtype_travel_type (context (), *shape);
				ret = abi->ret_coerced;
			}
		}
	}

	/* this, then the return slot: each rides an integer register. */
	if (sig->hasthis) {
		abi->args.push_back ({ ArgABI::Direct, pointer_type (context ()) });
		if (gr < param_gregs)
			gr++;
	}
	if (abi->ret_by_address && gr < param_gregs)
		gr++;

	for (int i = 0; i < sig->param_count; ++i) {
		llvm::Expected<llvm::Type *> converted = convert_type (sig->params[i]);
		if (!converted)
			return converted.takeError ();

		MonoType *ptype = mini_get_underlying_type (sig->params[i]);

		if (ptype->byref || !mini_type_is_vtype (ptype)) {
			abi->args.push_back ({ ArgABI::Direct, *converted });

			if (!ptype->byref
			    && (ptype->type == MONO_TYPE_R4 || ptype->type == MONO_TYPE_R8)) {
				if (fr < param_fregs)
					fr++;
			} else if (gr < param_gregs) {
				gr++;
			}
			continue;
		}

		llvm::Expected<VtypeShape> shape = classify_vtype (sig, ptype, false);
		if (!shape)
			return shape.takeError ();

		if (shape->nothing) {
			abi->args.push_back ({ ArgABI::Coerced,
			                       llvm::StructType::get (context ()) });
			continue;
		}

		unsigned need_gr = 0, need_fr = 0;
		for (unsigned quad = 0; quad < shape->nquads; ++quad) {
			if (shape->cls[quad] == QuadClass::Integer)
				need_gr++;
			else if (shape->cls[quad] == QuadClass::Sse)
				need_fr++;
		}

		/*
		 * A value type that no longer fits leaves the registers it would
		 * have taken free for later arguments, exactly as mini rewinds its
		 * counters.
		 */
		if (shape->memory || gr + need_gr > param_gregs || fr + need_fr > param_fregs) {
			abi->args.push_back (
				{ ArgABI::Memory, pointer_type (context ()), *converted });
			continue;
		}

		gr += need_gr;
		fr += need_fr;
		abi->args.push_back (
			{ ArgABI::Coerced, vtype_travel_type (context (), *shape) });
	}

	std::vector<llvm::Type *> params;

	for (const ArgABI &arg : abi->args)
		params.push_back (arg.travel);
	if (abi->ret_by_address)
		params.insert (params.begin () + abi->vret_index,
		               pointer_type (context ()));

	abi->type = llvm::FunctionType::get (ret, params, false);

	const SignatureABI *result = abi.get ();
	signature_abis[sig] = std::move (abi);
	return result;
}

/// The LLVM function type for SIG, in the runtime's calling convention.
llvm::Expected<llvm::FunctionType *>
MethodLLVMEmitter::convert_method_signature (MonoMethodSignature *sig)
{
	llvm::Expected<const SignatureABI *> abi = lower_signature (sig);

	if (!abi)
		return abi.takeError ();
	return (*abi)->type;
}

/// Mark CALL's stack-copied value types byval, past LEADING extra arguments
/// (a nest key) in front of the regular ones.
///
/// byval is what makes LLVM place the pointee itself in the outgoing argument
/// area; without it the pointer would ride a register and the callee would
/// read the wrong memory. Alignment 8 matches mini's argument slots.
void
MethodLLVMEmitter::apply_arg_abi (llvm::CallBase *call, const SignatureABI &abi,
                                  unsigned leading)
{
	for (unsigned i = 0; i < abi.args.size (); ++i) {
		const ArgABI &arg = abi.args[i];

		if (arg.kind != ArgABI::Memory)
			continue;

		unsigned at = leading + abi.param_index (i);

		call->addParamAttr (at, llvm::Attribute::getWithByValType (
		                                call->getContext (), arg.memory));
		call->addParamAttr (at, llvm::Attribute::getWithAlignment (
		                                call->getContext (), llvm::Align (8)));
	}
}

/// The declaration-side half of the above: the callee reads a byval argument
/// out of its incoming argument area only if its own prototype says so too.
void
MethodLLVMEmitter::apply_arg_abi (llvm::Function *fn, const SignatureABI &abi)
{
	for (unsigned i = 0; i < abi.args.size (); ++i) {
		const ArgABI &arg = abi.args[i];

		if (arg.kind != ArgABI::Memory)
			continue;

		unsigned at = abi.param_index (i);

		fn->addParamAttr (at, llvm::Attribute::getWithByValType (
		                              fn->getContext (), arg.memory));
		fn->addParamAttr (at, llvm::Attribute::getWithAlignment (
		                              fn->getContext (), llvm::Align (8)));
	}
}

/// VALUE, a value type on the evaluation stack, converted to what ABI says it
/// travels as. The register words load from a spill of the value: the words
/// can be wider than the value itself (a 12-byte type travels as two full
/// ones), so the spill is sized for the travel type, and the bytes past the
/// value are as undefined as the register bits mini leaves unwritten.
llvm::Value *
MethodLLVMEmitter::coerce_vtype_arg (MonoIrBuilder &builder, llvm::Value *value,
                                     MonoType *mtype, const ArgABI &abi)
{
	if (abi.kind == ArgABI::Direct)
		return value;

	llvm::Type *spilled = abi.kind == ArgABI::Memory ? abi.memory : abi.travel;
	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *temp = entry.CreateAlloca (spilled);

	temp->setAlignment (std::max (type_alignment (mtype), llvm::Align (8)));
	builder.CreateAlignedStore (value, temp, temp->getAlign ());

	if (abi.kind == ArgABI::Memory)
		return temp;
	return builder.CreateAlignedLoad (abi.travel, temp, temp->getAlign ());
}

/// RESULT, as a call to a signature lowered as ABI left it, converted back to
/// the value type the stack carries - the inverse of coerce_vtype_arg.
llvm::Value *
MethodLLVMEmitter::decoerce_vtype_return (MonoIrBuilder &builder, llvm::Value *result,
                                          MonoType *ret, const SignatureABI &abi)
{
	if (abi.ret_coerced == nullptr)
		return result;

	llvm::Expected<llvm::Type *> type = convert_type (ret);

	/* The signature already lowered, so its return type already converted. */
	if (!type)
		llvm::report_fatal_error ("a lowered return type failed to convert again");

	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *temp = entry.CreateAlloca (abi.ret_coerced);

	temp->setAlignment (std::max (type_alignment (ret), llvm::Align (8)));
	builder.CreateAlignedStore (result, temp, temp->getAlign ());
	return builder.CreateAlignedLoad (*type, temp, temp->getAlign ());
}

/// Give ARGS ([this?, params...]) the hidden return slot SIG's convention asks
/// for, if any: a temporary is allocated, its address inserted where the callee
/// expects it, and returned so the caller can read the result back out.
llvm::Expected<llvm::AllocaInst *>
MethodLLVMEmitter::insert_vret_arg (MonoMethodSignature *sig,
                                    std::vector<llvm::Value *> &args)
{
	if (!returns_by_address (sig))
		return nullptr;

	llvm::Expected<llvm::Type *> type = convert_type (sig->ret);
	if (!type)
		return type.takeError ();

	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *slot = entry.CreateAlloca (*type, nullptr, "vret");

	slot->setAlignment (type_alignment (sig->ret));
	args.insert (args.begin () + vret_arg_index (sig), slot);
	return slot;
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

	llvm::Expected<const SignatureABI *> abi = lower_signature (sig);
	if (!abi)
		return abi.takeError ();

	llvm::FunctionType *type = (*abi)->type;
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

	llvm::Function *function = llvm::Function::Create (
		type, llvm::GlobalValue::ExternalLinkage, full_name, module);

	record_external (full_name, ExternalSymbol::Kind::Code, method);
	apply_arg_abi (function, **abi);

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
		unsigned pindex = (*abi)->param_index (i + sig->hasthis);

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
