/**
 * \file
 * \brief Compiling a `System.Math` or `System.MathF` icall into arithmetic.
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

/// One row of math_table: the method's name and arity, and what a call to it
/// becomes.
struct MathTableEntry {
	/// The method's name on System.Math and System.MathF.
	std::string_view name;
	/// How many floating-point arguments it takes.
	unsigned arity;
	MathIntrinsic::Emit emit;
	/// The intrinsic to call. Meaningful only for Emit::Intrinsic.
	llvm::Intrinsic::ID intrinsic;
	/// The libm function of the double form, and then of the float form.
	/// Meaningful only for Emit::Libm.
	const char *libm_double;
	const char *libm_float;
};

constexpr MathIntrinsic::Emit call_intrinsic = MathIntrinsic::Emit::Intrinsic;
constexpr llvm::Intrinsic::ID no_intrinsic = llvm::Intrinsic::not_intrinsic;

/*
 * The names this backend answers itself, and what it answers each one with.
 *
 * Every row is an icall, so an ordinary call site reaches it through the
 * managed-to-native wrapper the runtime builds. That wrapper saves five
 * callee-saved registers, pushes an LMF, calls the C function through a
 * pointer, and tests mono_thread_interruption_request_flag. It can also throw.
 * The optimizer therefore reads the site as writing memory and as able to
 * unwind. It cannot move the call out of a loop, hold two of them as one, or
 * keep a value in a register across it. Each row below computes the same value
 * with none of that.
 *
 * Two icalls are deliberately absent.
 *
 * Round stays on its wrapper. Its icall is mono_round_to_even ()
 * (mono/utils/mono-math.h), which computes floor (x + 0.5) and answers 1 for
 * the largest double under a half. llvm.roundeven answers 0, which is the
 * better answer and a different one. The interpreter has no intrinsic for
 * Round and calls the same icall. Taking llvm.roundeven would therefore make
 * Math.Round depend on which tier the caller runs at.
 *
 * ModF stays because it writes its integer part through an out parameter.
 * llvm.modf returns the two halves as a pair, so the site needs glue that no
 * other row needs.
 */
const MathTableEntry math_table[] = {
	{ "Abs", 1, call_intrinsic, llvm::Intrinsic::fabs, nullptr, nullptr },
	{ "Sqrt", 1, call_intrinsic, llvm::Intrinsic::sqrt, nullptr, nullptr },
	{ "Sin", 1, call_intrinsic, llvm::Intrinsic::sin, nullptr, nullptr },
	{ "Cos", 1, call_intrinsic, llvm::Intrinsic::cos, nullptr, nullptr },
	{ "Tan", 1, call_intrinsic, llvm::Intrinsic::tan, nullptr, nullptr },
	{ "Asin", 1, call_intrinsic, llvm::Intrinsic::asin, nullptr, nullptr },
	{ "Acos", 1, call_intrinsic, llvm::Intrinsic::acos, nullptr, nullptr },
	{ "Atan", 1, call_intrinsic, llvm::Intrinsic::atan, nullptr, nullptr },
	{ "Sinh", 1, call_intrinsic, llvm::Intrinsic::sinh, nullptr, nullptr },
	{ "Cosh", 1, call_intrinsic, llvm::Intrinsic::cosh, nullptr, nullptr },
	{ "Tanh", 1, call_intrinsic, llvm::Intrinsic::tanh, nullptr, nullptr },
	{ "Exp", 1, call_intrinsic, llvm::Intrinsic::exp, nullptr, nullptr },
	{ "Log", 1, call_intrinsic, llvm::Intrinsic::log, nullptr, nullptr },
	{ "Log10", 1, call_intrinsic, llvm::Intrinsic::log10, nullptr, nullptr },
	{ "Floor", 1, call_intrinsic, llvm::Intrinsic::floor, nullptr, nullptr },
	{ "Ceiling", 1, call_intrinsic, llvm::Intrinsic::ceil, nullptr, nullptr },
	{ "Atan2", 2, call_intrinsic, llvm::Intrinsic::atan2, nullptr, nullptr },
	{ "Pow", 2, call_intrinsic, llvm::Intrinsic::pow, nullptr, nullptr },

	// frem is fmod, which is what the icall calls.
	{ "FMod", 2, MathIntrinsic::Emit::Remainder, no_intrinsic, nullptr, nullptr },

	// LLVM has no intrinsic for a cube root. RuntimeLibcallsInfo names cbrt and
	// cbrtf on this triple, so get_libcall_builtins () (runtime/builtins.cpp)
	// registers both addresses already. Asinh, Acosh and Atanh, which the
	// interpreter also recognizes, have no such libcall: RuntimeLibcalls.td
	// declares ASINH, ACOSH and ATANH for vector types alone. Those three keep
	// their wrapper until the scalar symbols are registered.
	{ "Cbrt", 1, MathIntrinsic::Emit::Libm, no_intrinsic, "cbrt", "cbrtf" },
};

/// Whether t is `float` or `double` itself, rather than a reference or a
/// pointer to one.
bool
is_plain_float (MonoType *t)
{
	return !t->byref && (t->type == MONO_TYPE_R4 || t->type == MONO_TYPE_R8);
}

} // namespace

std::optional<MathIntrinsic>
math_intrinsic_for (MonoMethod *method, MonoMethodSignature *sig)
{
	// A vararg site brings a signature of its own, whose parameter list holds
	// a sentinel and whatever types the caller chose.
	if (sig == nullptr || sig->hasthis || sig->call_convention == MONO_CALL_VARARG)
		return std::nullopt;

	// A managed method of the same name carries no wrapper to remove, and its
	// own IL is what the call site is entitled to.
	if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) == 0)
		return std::nullopt;

	MonoClass *klass = method->klass;

	if (m_class_get_image (klass) != mono_get_corlib ())
		return std::nullopt;
	if (std::string_view (m_class_get_name_space (klass)) != "System")
		return std::nullopt;

	std::string_view class_name (m_class_get_name (klass));

	if (class_name != "Math" && class_name != "MathF")
		return std::nullopt;

	// The argument type decides the overload, not the class: System.Math
	// carries Abs(single) as well as Abs(double). One type across the arguments
	// and the return value is what lets the emitter give the intrinsic a single
	// overload. It also drops ModF, whose second parameter is a pointer, and
	// ILogB, which returns an int.
	if (!is_plain_float (sig->ret))
		return std::nullopt;

	for (int i = 0; i < sig->param_count; i++) {
		if (!is_plain_float (sig->params[i]) || sig->params[i]->type != sig->ret->type)
			return std::nullopt;
	}

	bool is_float = sig->ret->type == MONO_TYPE_R4;
	std::string_view name (method->name);

	for (const MathTableEntry &entry : math_table) {
		if (entry.name != name || entry.arity != (unsigned) sig->param_count)
			continue;

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
/// result is unused. So give this only a function that reads and writes no
/// memory, errno included.
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
	}

	pop_stack (sig->param_count);
	return push_produced (builder, result, sig->ret);
}

} // namespace mono
