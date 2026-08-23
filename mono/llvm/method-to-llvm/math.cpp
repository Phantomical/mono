/**
 * \file
 * \brief Compiling a `System.Math` or `System.MathF` method into arithmetic.
 */

#include "method-to-llvm.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/ModRef.h>

#include <optional>
#include <string_view>

namespace mono {

namespace {

/// What stands behind the method a row names.
///
/// A row has to match its method, so a name whose implementation changes stops
/// being recognized rather than being answered from the wrong premise.
enum class Body {
	/// An internal call. The row replaces a call to its marshalling wrapper.
	Icall,
	/// Managed IL. The row replaces that body, which the interpreter runs, so
	/// it has to compute what the IL computes on every input.
	Managed,
};

/// One row of math_table: the method's name and arity, and what a call to it
/// becomes.
struct MathTableEntry {
	/// The method's name on System.Math and System.MathF.
	std::string_view name;
	/// How many floating-point arguments it takes.
	unsigned arity;
	Body body;
	MathIntrinsic::Emit emit;
	/// The intrinsic to call. Meaningful only for Emit::Intrinsic.
	llvm::Intrinsic::ID intrinsic;
	/// The libm function of the double form, and then of the float form.
	/// Meaningful only for Emit::Libm.
	const char *libm_double;
	const char *libm_float;
};

constexpr Body icall = Body::Icall;
constexpr MathIntrinsic::Emit call_intrinsic = MathIntrinsic::Emit::Intrinsic;
constexpr llvm::Intrinsic::ID no_intrinsic = llvm::Intrinsic::not_intrinsic;

/*
 * The names this backend answers itself, and what it answers each one with.
 *
 * Most rows are icalls, so an ordinary call site reaches one through the
 * managed-to-native wrapper the runtime builds. That wrapper saves five
 * callee-saved registers, pushes an LMF, calls the C function through a
 * pointer, and tests mono_thread_interruption_request_flag. It can also throw.
 * The optimizer therefore reads the site as writing memory and as able to
 * unwind. It cannot move the call out of a loop, hold two of them as one, or
 * keep a value in a register across it. Each row below computes the same value
 * with none of that.
 *
 * Truncate is the one managed row, and it is held to a stricter test. The
 * interpreter runs its IL, so the intrinsic has to answer as that IL answers on
 * every input rather than merely as the documentation reads. Math.Truncate is
 * `ModF (d, &d); return d`, and modf writes the integral part through the
 * pointer. That is llvm.trunc. Both give -0.0 for -0.5 and for -0.0, and both
 * pass an infinity and a NaN through.
 *
 * ModF is the row that writes memory. llvm.modf hands back the fractional and
 * the integral half as a pair. Storing the integral half through the call's
 * second argument is what the icall's out parameter was. Truncate no longer
 * calls it, but the Round overloads that take a digit count still do.
 *
 * Three names are deliberately absent.
 *
 * Round stays. The double form is the mono_round_to_even () icall
 * (mono/utils/mono-math.h). MathF.Round is managed IL that computes the same
 * thing, floor (x + 0.5) with a half-to-even correction. Both answer 1 for the
 * largest value under a half, 0.49999999999999994 and 0.49999997f, where
 * llvm.roundeven answers 0. That is the better answer and a different one. The
 * interpreter runs the icall and the IL, so taking the intrinsic would make
 * Round depend on which tier the caller runs at.
 *
 * Max and Min stay, and the reason is signed zero rather than NaN. The body of
 * Math.Max (double, double) returns the first operand when it is greater and
 * when it is a NaN, and the second operand otherwise. Two operands that compare
 * equal therefore give back the second: Max (+0.0, -0.0) is -0.0 and
 * Max (-0.0, +0.0) is +0.0. llvm.maximum answers +0.0 to both, and llvm.maxnum
 * answers the wrong operand for a NaN as well. unity-main takes these two only
 * under mono_use_fast_math, which is the same conclusion.
 *
 * IEEERemainder stays. Its managed body preserves the NaN payload of whichever
 * operand was a NaN, and it calls Math.Round on the quotient. libm remainder is
 * therefore a different computation, not the same one written out.
 *
 * CopySign, Log2, ILogB, ScaleB and FusedMultiplyAdd are not here because this
 * corlib does not have them. They are .NET Core API, and Math.CoreCLR.cs stops
 * at the .NET Framework surface.
 */
const MathTableEntry math_table[] = {
	{ "Abs", 1, icall, call_intrinsic, llvm::Intrinsic::fabs, nullptr, nullptr },
	{ "Sqrt", 1, icall, call_intrinsic, llvm::Intrinsic::sqrt, nullptr, nullptr },
	{ "Sin", 1, icall, call_intrinsic, llvm::Intrinsic::sin, nullptr, nullptr },
	{ "Cos", 1, icall, call_intrinsic, llvm::Intrinsic::cos, nullptr, nullptr },
	{ "Tan", 1, icall, call_intrinsic, llvm::Intrinsic::tan, nullptr, nullptr },
	{ "Asin", 1, icall, call_intrinsic, llvm::Intrinsic::asin, nullptr, nullptr },
	{ "Acos", 1, icall, call_intrinsic, llvm::Intrinsic::acos, nullptr, nullptr },
	{ "Atan", 1, icall, call_intrinsic, llvm::Intrinsic::atan, nullptr, nullptr },
	{ "Sinh", 1, icall, call_intrinsic, llvm::Intrinsic::sinh, nullptr, nullptr },
	{ "Cosh", 1, icall, call_intrinsic, llvm::Intrinsic::cosh, nullptr, nullptr },
	{ "Tanh", 1, icall, call_intrinsic, llvm::Intrinsic::tanh, nullptr, nullptr },
	{ "Exp", 1, icall, call_intrinsic, llvm::Intrinsic::exp, nullptr, nullptr },
	{ "Log", 1, icall, call_intrinsic, llvm::Intrinsic::log, nullptr, nullptr },
	{ "Log10", 1, icall, call_intrinsic, llvm::Intrinsic::log10, nullptr, nullptr },
	{ "Floor", 1, icall, call_intrinsic, llvm::Intrinsic::floor, nullptr, nullptr },
	{ "Ceiling", 1, icall, call_intrinsic, llvm::Intrinsic::ceil, nullptr, nullptr },
	{ "Atan2", 2, icall, call_intrinsic, llvm::Intrinsic::atan2, nullptr, nullptr },
	{ "Pow", 2, icall, call_intrinsic, llvm::Intrinsic::pow, nullptr, nullptr },

	// frem is fmod, which is what the icall calls.
	{ "FMod", 2, icall, MathIntrinsic::Emit::Remainder, no_intrinsic, nullptr, nullptr },

	// LLVM has no intrinsic for these four. cbrt and cbrtf are runtime
	// libcalls, so get_libcall_builtins () registers them on its own. The six
	// inverse hyperbolic names are not, and runtime/builtins.cpp registers
	// those by hand.
	{ "Cbrt", 1, icall, MathIntrinsic::Emit::Libm, no_intrinsic, "cbrt", "cbrtf" },
	{ "Asinh", 1, icall, MathIntrinsic::Emit::Libm, no_intrinsic, "asinh", "asinhf" },
	{ "Acosh", 1, icall, MathIntrinsic::Emit::Libm, no_intrinsic, "acosh", "acoshf" },
	{ "Atanh", 1, icall, MathIntrinsic::Emit::Libm, no_intrinsic, "atanh", "atanhf" },

	{ "ModF", 2, icall, MathIntrinsic::Emit::Modf, llvm::Intrinsic::modf,
	  nullptr, nullptr },

	{ "Truncate", 1, Body::Managed, call_intrinsic, llvm::Intrinsic::trunc,
	  nullptr, nullptr },
};

/// Whether t is `float` or `double` itself, rather than a reference or a
/// pointer to one.
bool
is_plain_float (MonoType *t)
{
	return !t->byref && (t->type == MONO_TYPE_R4 || t->type == MONO_TYPE_R8);
}

/// Whether sig has the shape entry is written for: one primitive float type
/// across the return value and every argument.
///
/// The argument type decides the overload, not the class, because System.Math
/// carries Abs(single) as well as Abs(double). One type throughout is also what
/// lets the emitter give an intrinsic a single overload.
bool
matches_shape (const MathTableEntry &entry, MonoMethodSignature *sig)
{
	if (!is_plain_float (sig->ret))
		return false;

	int values = sig->param_count;

	// ModF is the one row whose last argument is not a value. It is where the
	// integral half goes, so it is a pointer to the same type.
	if (entry.emit == MathIntrinsic::Emit::Modf) {
		MonoType *out = sig->params[--values];
		MonoType *target = out->byref || out->type != MONO_TYPE_PTR
		                           ? nullptr
		                           : mono_type_get_ptr_type (out);

		if (target == nullptr || target->byref || target->type != sig->ret->type)
			return false;
	}

	for (int i = 0; i < values; i++) {
		if (!is_plain_float (sig->params[i]) || sig->params[i]->type != sig->ret->type)
			return false;
	}

	return true;
}

} // namespace

std::optional<MathIntrinsic>
math_intrinsic_for (MonoMethod *method, MonoMethodSignature *sig)
{
	// A vararg site brings a signature of its own, whose parameter list holds
	// a sentinel and whatever types the caller chose.
	if (sig == nullptr || sig->hasthis || sig->call_convention == MONO_CALL_VARARG)
		return std::nullopt;

	Body body = (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) != 0
	                    ? Body::Icall
	                    : Body::Managed;
	MonoClass *klass = method->klass;

	if (m_class_get_image (klass) != mono_get_corlib ())
		return std::nullopt;
	if (std::string_view (m_class_get_name_space (klass)) != "System")
		return std::nullopt;

	std::string_view class_name (m_class_get_name (klass));

	if (class_name != "Math" && class_name != "MathF")
		return std::nullopt;

	std::string_view name (method->name);

	for (const MathTableEntry &entry : math_table) {
		if (entry.name != name || entry.arity != (unsigned) sig->param_count
		    || entry.body != body)
			continue;
		if (!matches_shape (entry, sig))
			return std::nullopt;

		bool is_float = sig->ret->type == MONO_TYPE_R4;

		return MathIntrinsic { entry.emit, entry.intrinsic,
			               is_float ? entry.libm_float : entry.libm_double };
	}

	return std::nullopt;
}

/// Declares the libm function name, over arity arguments of type and returning
/// one of the same.
///
/// The declaration promises a leaf computation on its arguments. That is what
/// lets the optimizer move the call, hold two of them as one, or drop one whose
/// result is unused. A domain error that libm reports through errno is
/// therefore not modelled. LLVM's own math intrinsics, which the rest of the
/// table emits, make the same choice. So give this only a function whose whole
/// result is its return value.
llvm::FunctionCallee
MethodLLVMEmitter::libm_decl (const char *name, llvm::Type *type, size_t arity)
{
	std::vector<llvm::Type *> params (arity, type);
	llvm::FunctionCallee callee =
		module->getOrInsertFunction (name, llvm::FunctionType::get (type, params, false));

	if (llvm::Function *decl = llvm::dyn_cast<llvm::Function> (callee.getCallee ())) {
		decl->setDoesNotThrow ();
		decl->setWillReturn ();
		decl->setMemoryEffects (llvm::MemoryEffects::none ());
	}

	return callee;
}

/// Compiles a call opcode as the arithmetic in math, in place of the call.
/// The arguments come off the evaluation stack, and the result goes on.
llvm::Error
MethodLLVMEmitter::emit_math_call (MonoIrBuilder &builder, const MathIntrinsic &math,
                                   MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::Type *type = (*args)[0]->getType ();
	llvm::Value *result = nullptr;

	switch (math.emit) {
	case MathIntrinsic::Emit::Intrinsic:
		result = builder.CreateIntrinsic (math.intrinsic, { type }, *args);
		break;
	case MathIntrinsic::Emit::Remainder:
		result = builder.CreateFRem ((*args)[0], (*args)[1]);
		break;
	case MathIntrinsic::Emit::Libm:
		result = builder.CreateCall (libm_decl (math.libm, type, args->size ()), *args);
		break;
	case MathIntrinsic::Emit::Modf: {
		// The pair is the fractional half and then the integral one. The
		// caller owns the address the integral half goes to. A store through a
		// bad one faults, which is what the icall's own store did.
		llvm::Value *halves =
			builder.CreateIntrinsic (math.intrinsic, { type }, { (*args)[0] });

		builder.CreateAlignedStore (builder.CreateExtractValue (halves, 1),
		                            (*args)[1], type_alignment (sig->ret));
		result = builder.CreateExtractValue (halves, 0);
		break;
	}
	}

	pop_stack (sig->param_count);
	return push_produced (builder, result, sig->ret);
}

} // namespace mono
