#include "glib.h"
#include "mintops.h"
#include "mono/metadata/exception.h"
#include "mono/mini/interp/interp.hpp"
#include <cmath>
#include <limits>
#include <type_traits>

namespace mono::interp {

#define IMPL_TRIOP(opcode, type, expr)                                         \
	MONO_INTERP_OP_IMPL (opcode)                                               \
	{                                                                          \
		static_assert (InterpState::opinfos[opcode].num_sregs == 3,            \
		               "opcode " #opcode " does not have 3 source registers"); \
		auto a = LOCAL_VAR (ip[2], type);                                      \
		auto b = LOCAL_VAR (ip[3], type);                                      \
		auto c = LOCAL_VAR (ip[4], type);                                      \
		LOCAL_VAR (ip[1], type) = (expr);                                      \
                                                                               \
		MONO_INTERP_OP_ADVANCE ();                                             \
		MONO_INTERP_DISPATCH ();                                               \
	}

#define IMPL_BINOP(opcode, type, expr)                                         \
	MONO_INTERP_OP_IMPL (opcode)                                               \
	{                                                                          \
		static_assert (InterpState::opinfos[opcode].num_sregs == 2,            \
		               "opcode " #opcode " does not have 2 source registers"); \
		auto a = LOCAL_VAR (ip[2], type);                                      \
		auto b = LOCAL_VAR (ip[3], type);                                      \
		LOCAL_VAR (ip[1], type) = (expr);                                      \
                                                                               \
		MONO_INTERP_OP_ADVANCE ();                                             \
		MONO_INTERP_DISPATCH ();                                               \
	}
#define IMPL_BINOPS(opcode, expr)           \
	IMPL_BINOP (opcode##_I4, gint32, expr); \
	IMPL_BINOP (opcode##_I8, gint64, expr); \
	IMPL_BINOP (opcode##_R4, float, expr);  \
	IMPL_BINOP (opcode##_R8, double, expr)

#define IMPL_FLOAT_BINOPS(opcode, expr)    \
	IMPL_BINOP (opcode##_R4, float, expr); \
	IMPL_BINOP (opcode##_R8, double, expr)

#define IMPL_INT_BINOPS(opcode, expr)       \
	IMPL_BINOP (opcode##_I4, gint32, expr); \
	IMPL_BINOP (opcode##_I8, gint64, expr)

#define IMPL_UINT_BINOPS(opcode, expr)       \
	IMPL_BINOP (opcode##_I4, guint32, expr); \
	IMPL_BINOP (opcode##_I8, guint64, expr)

#define IMPL_UNOP(opcode, type, expr)                                         \
	MONO_INTERP_OP_IMPL (opcode)                                              \
	{                                                                         \
		static_assert (InterpState::opinfos[opcode].num_sregs == 1,           \
		               "opcode " #opcode " does not have 1 source register"); \
		auto x = LOCAL_VAR (ip[2], type);                                     \
		LOCAL_VAR (ip[1], type) = (expr);                                     \
                                                                              \
		MONO_INTERP_OP_ADVANCE ();                                            \
		MONO_INTERP_DISPATCH ();                                              \
	}

#define IMPL_UNOPS(opcode, expr)           \
	IMPL_UNOP (opcode##_I4, gint32, expr); \
	IMPL_UNOP (opcode##_I8, gint64, expr); \
	IMPL_UNOP (opcode##_R4, float, expr);  \
	IMPL_UNOP (opcode##_R8, double, expr)

#define IMPL_INT_UNOPS(opcode, expr)       \
	IMPL_UNOP (opcode##_I4, gint32, expr); \
	IMPL_UNOP (opcode##_I8, gint64, expr)

IMPL_BINOPS (MINT_ADD, a + b);
IMPL_UNOP (MINT_ADD1_I4, gint32, x + 1);
IMPL_UNOP (MINT_ADD1_I8, gint64, x + 1);
IMPL_BINOPS (MINT_SUB, a - b);
IMPL_UNOP (MINT_SUB1_I4, gint32, x - 1);
IMPL_UNOP (MINT_SUB1_I8, gint64, x - 1);
IMPL_BINOPS (MINT_MUL, a *b);
IMPL_FLOAT_BINOPS (MINT_DIV, a / b);
IMPL_FLOAT_BINOPS (MINT_REM, std::fmod (a, b));

MONO_INTERP_OP_IMPL (MINT_DIV_I4)
{
	auto a = LOCAL_VAR (ip[2], gint32);
	auto b = LOCAL_VAR (ip[3], gint32);
	if (G_UNLIKELY (b == 0))
		THROW_EX (mono_get_exception_divide_by_zero (), ip);
	if (G_UNLIKELY (b == -1 && a == G_MININT32))
		THROW_EX (mono_get_exception_overflow (), ip);
	LOCAL_VAR (ip[1], gint32) = a / b;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_DIV_I8)
{
	auto a = LOCAL_VAR (ip[2], gint64);
	auto b = LOCAL_VAR (ip[3], gint64);
	if (G_UNLIKELY (b == 0))
		THROW_EX (mono_get_exception_divide_by_zero (), ip);
	if (G_UNLIKELY (b == -1 && a == G_MININT64))
		THROW_EX (mono_get_exception_overflow (), ip);
	LOCAL_VAR (ip[1], gint64) = a / b;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_DIV_UN_I4)
{
	auto a = LOCAL_VAR (ip[2], guint32);
	auto b = LOCAL_VAR (ip[3], guint32);
	if (G_UNLIKELY (b == 0))
		THROW_EX (mono_get_exception_divide_by_zero (), ip);
	LOCAL_VAR (ip[1], guint32) = a / b;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_DIV_UN_I8)
{
	auto a = LOCAL_VAR (ip[2], guint64);
	auto b = LOCAL_VAR (ip[3], guint64);
	if (G_UNLIKELY (b == 0))
		THROW_EX (mono_get_exception_divide_by_zero (), ip);
	LOCAL_VAR (ip[1], guint64) = a / b;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_REM_I4)
{
	auto a = LOCAL_VAR (ip[2], gint32);
	auto b = LOCAL_VAR (ip[3], gint32);
	if (G_UNLIKELY (b == 0))
		THROW_EX (mono_get_exception_divide_by_zero (), ip);
	if (G_UNLIKELY (b == -1 && a == G_MININT32))
		THROW_EX (mono_get_exception_overflow (), ip);
	LOCAL_VAR (ip[1], gint32) = a % b;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_REM_I8)
{
	auto a = LOCAL_VAR (ip[2], gint64);
	auto b = LOCAL_VAR (ip[3], gint64);
	if (G_UNLIKELY (b == 0))
		THROW_EX (mono_get_exception_divide_by_zero (), ip);
	if (G_UNLIKELY (b == -1 && a == G_MININT64))
		THROW_EX (mono_get_exception_overflow (), ip);
	LOCAL_VAR (ip[1], gint64) = a % b;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_REM_UN_I4)
{
	auto a = LOCAL_VAR (ip[2], guint32);
	auto b = LOCAL_VAR (ip[3], guint32);
	if (G_UNLIKELY (b == 0))
		THROW_EX (mono_get_exception_divide_by_zero (), ip);
	LOCAL_VAR (ip[1], guint32) = a % b;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_REM_UN_I8)
{
	auto a = LOCAL_VAR (ip[2], guint64);
	auto b = LOCAL_VAR (ip[3], guint64);
	if (G_UNLIKELY (b == 0))
		THROW_EX (mono_get_exception_divide_by_zero (), ip);
	LOCAL_VAR (ip[1], guint64) = a % b;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

IMPL_INT_BINOPS (MINT_AND, a &b);
IMPL_INT_BINOPS (MINT_OR, a | b);
IMPL_INT_BINOPS (MINT_XOR, a ^ b);
IMPL_UNOPS (MINT_NEG, -x);
IMPL_INT_UNOPS (MINT_NOT, ~x);

#define IMPL_SHIFTOP(opcode, type, expr)    \
	MONO_INTERP_OP_IMPL (opcode)            \
	{                                       \
		auto a = LOCAL_VAR (ip[2], type);   \
		auto b = LOCAL_VAR (ip[3], gint32); \
		LOCAL_VAR (ip[1], type) = (expr);   \
                                            \
		MONO_INTERP_OP_ADVANCE ();          \
		MONO_INTERP_DISPATCH ();            \
	}

#define IMPL_SHIFTOPS(opcode, expr)           \
	IMPL_SHIFTOP (opcode##_I4, gint32, expr); \
	IMPL_SHIFTOP (opcode##_I8, gint64, expr)

#define IMPL_USHIFTOPS(opcode, expr)           \
	IMPL_SHIFTOP (opcode##_I4, guint32, expr); \
	IMPL_SHIFTOP (opcode##_I8, guint64, expr)

IMPL_SHIFTOPS (MINT_SHL, a << b);
IMPL_SHIFTOPS (MINT_SHR, a >> b);
IMPL_USHIFTOPS (MINT_SHR_UN, a >> b);

#define IMPL_INT_CMPOP(opcode, type, expr)  \
	MONO_INTERP_OP_IMPL (opcode)            \
	{                                       \
		auto a = LOCAL_VAR (ip[2], type);   \
		auto b = LOCAL_VAR (ip[3], type);   \
		LOCAL_VAR (ip[1], gint32) = (expr); \
                                            \
		MONO_INTERP_OP_ADVANCE ();          \
		MONO_INTERP_DISPATCH ();            \
	}

#define IMPL_INT_CMPOPS(opcode, expr)           \
	IMPL_INT_CMPOP (opcode##_I4, gint32, expr); \
	IMPL_INT_CMPOP (opcode##_I8, gint64, expr);

#define IMPL_UINT_CMPOPS(opcode, expr)              \
	IMPL_INT_CMPOP (opcode##_UN_I4, guint32, expr); \
	IMPL_INT_CMPOP (opcode##_UN_I8, guint64, expr);

#define IMPL_FLOAT_CMPOP(opcode, type, expr, no_order) \
	MONO_INTERP_OP_IMPL (opcode)                       \
	{                                                  \
		auto a = LOCAL_VAR (ip[2], type);              \
		auto b = LOCAL_VAR (ip[3], type);              \
		if (std::isunordered (a, b))                   \
			LOCAL_VAR (ip[1], gint32) = (no_order);    \
		else                                           \
			LOCAL_VAR (ip[1], gint32) = (expr);        \
                                                       \
		MONO_INTERP_OP_ADVANCE ();                     \
		MONO_INTERP_DISPATCH ();                       \
	}

#define IMPL_FLOAT_CMPOPS(opcode, expr, no_order)          \
	IMPL_FLOAT_CMPOP (opcode##_R4, float, expr, no_order); \
	IMPL_FLOAT_CMPOP (opcode##_R8, double, expr, no_order);

IMPL_UNOP (MINT_CEQ0_I4, gint32, x == 0);
IMPL_INT_CMPOPS (MINT_CEQ, a == b);
IMPL_INT_CMPOPS (MINT_CNE, a != b);
IMPL_INT_CMPOPS (MINT_CGT, a > b);
IMPL_UINT_CMPOPS (MINT_CGT, a > b);
IMPL_INT_CMPOPS (MINT_CGE, a >= b);
IMPL_UINT_CMPOPS (MINT_CGE, a >= b);
IMPL_INT_CMPOPS (MINT_CLT, a < b);
IMPL_UINT_CMPOPS (MINT_CLT, a < b);
IMPL_INT_CMPOPS (MINT_CLE, a <= b);
IMPL_UINT_CMPOPS (MINT_CLE, a <= b);

IMPL_FLOAT_CMPOPS (MINT_CEQ, a == b, 0);
IMPL_FLOAT_CMPOPS (MINT_CNE, a != b, 1);
IMPL_FLOAT_CMPOPS (MINT_CGT, a > b, 0);
IMPL_FLOAT_CMPOPS (MINT_CGT_UN, a > b, 1);
IMPL_FLOAT_CMPOPS (MINT_CGE, a >= b, 0);
IMPL_FLOAT_CMPOPS (MINT_CLT, a < b, 0);
IMPL_FLOAT_CMPOPS (MINT_CLT_UN, a < b, 1);
IMPL_FLOAT_CMPOPS (MINT_CLE, a <= b, 0);

#if defined(__has_builtin) && __has_builtin(__builtin_add_overflow) \
	&& __has_builtin(__builtin_sub_overflow) && __has_builtin(__builtin_mul_overflow)
#define MONO_INTERP_OVERFLOW_BUILTINS 1
#endif

namespace {

template<typename T>
bool
add_overflow (T a, T b, T &result)
{
#ifdef MONO_INTERP_OVERFLOW_BUILTINS
	return __builtin_add_overflow (a, b, &result);
#else
	if constexpr (std::is_signed_v<T>) {
		if (b >= 0 ? a > std::numeric_limits<T>::max () - b
		           : a < std::numeric_limits<T>::min () - b)
			return true;
	} else {
		if (a > std::numeric_limits<T>::max () - b)
			return true;
	}

	result = (T) (a + b);
	return false;
#endif
}

template<typename T>
bool
sub_overflow (T a, T b, T &result)
{
#ifdef MONO_INTERP_OVERFLOW_BUILTINS
	return __builtin_sub_overflow (a, b, &result);
#else
	if constexpr (std::is_signed_v<T>) {
		if (b < 0 ? a > std::numeric_limits<T>::max () + b : a < std::numeric_limits<T>::min () + b)
			return true;
	} else {
		if (a < b)
			return true;
	}

	result = (T) (a - b);
	return false;
#endif
}

template<typename T>
bool
mul_overflow (T a, T b, T &result)
{
#ifdef MONO_INTERP_OVERFLOW_BUILTINS
	return __builtin_mul_overflow (a, b, &result);
#else
	if (a == 0 || b == 0) {
		result = 0;
		return false;
	}

	if constexpr (std::is_signed_v<T>) {
		if (b == -1) {
			if (a == std::numeric_limits<T>::min ())
				return true;

			result = (T) -a;
			return false;
		}

		if (a > 0) {
			if (b > 0 ? a > std::numeric_limits<T>::max () / b
			          : a > std::numeric_limits<T>::min () / b)
				return true;
		} else {
			if (b > 0 ? a < std::numeric_limits<T>::min () / b
			          : a < std::numeric_limits<T>::max () / b)
				return true;
		}
	} else {
		if (b > std::numeric_limits<T>::max () / a)
			return true;
	}

	result = (T) (a * b);
	return false;
#endif
}

} // namespace

#define IMPL_OVFOP(opcode, type, check)                    \
	MONO_INTERP_OP_IMPL (opcode)                           \
	{                                                      \
		auto a = LOCAL_VAR (ip[2], type);                  \
		auto b = LOCAL_VAR (ip[3], type);                  \
                                                           \
		type result;                                       \
		if (G_UNLIKELY (check (a, b, result)))             \
			THROW_EX (mono_get_exception_overflow (), ip); \
		LOCAL_VAR (ip[1], type) = result;                  \
                                                           \
		MONO_INTERP_OP_ADVANCE ();                         \
		MONO_INTERP_DISPATCH ();                           \
	}

#define IMPL_OVFOPS(opcode, check)               \
	IMPL_OVFOP (opcode##_I4, gint32, check);     \
	IMPL_OVFOP (opcode##_I8, gint64, check);     \
	IMPL_OVFOP (opcode##_UN_I4, guint32, check); \
	IMPL_OVFOP (opcode##_UN_I8, guint64, check)

IMPL_OVFOPS (MINT_ADD_OVF, add_overflow);
IMPL_OVFOPS (MINT_SUB_OVF, sub_overflow);
IMPL_OVFOPS (MINT_MUL_OVF, mul_overflow);

IMPL_UNOP (MINT_ABS, double, std::abs (x));
IMPL_UNOP (MINT_ASIN, double, std::asin (x));
IMPL_UNOP (MINT_ASINH, double, std::asinh (x));
IMPL_UNOP (MINT_ACOS, double, std::acos (x));
IMPL_UNOP (MINT_ACOSH, double, std::acosh (x));
IMPL_UNOP (MINT_ATAN, double, std::atan (x));
IMPL_UNOP (MINT_ATANH, double, std::atanh (x));
IMPL_UNOP (MINT_CEILING, double, std::ceil (x));
IMPL_UNOP (MINT_COS, double, std::cos (x));
IMPL_UNOP (MINT_CBRT, double, std::cbrt (x));
IMPL_UNOP (MINT_COSH, double, std::cosh (x));
IMPL_UNOP (MINT_EXP, double, std::exp (x));
IMPL_UNOP (MINT_FLOOR, double, std::floor (x));
IMPL_UNOP (MINT_LOG, double, std::log (x));
IMPL_UNOP (MINT_LOG2, double, std::log2 (x));
IMPL_UNOP (MINT_LOG10, double, std::log10 (x));
IMPL_UNOP (MINT_SIN, double, std::sin (x));
IMPL_UNOP (MINT_SQRT, double, std::sqrt (x));
IMPL_UNOP (MINT_SINH, double, std::sinh (x));
IMPL_UNOP (MINT_TAN, double, std::tan (x));
IMPL_UNOP (MINT_TANH, double, std::tanh (x));
IMPL_BINOP (MINT_ATAN2, double, std::atan2 (a, b));
IMPL_BINOP (MINT_POW, double, std::pow (a, b));
IMPL_TRIOP (MINT_FMA, double, std::fma (a, b, c));

MONO_INTERP_OP_IMPL (MINT_SCALEB)
{
	auto a = LOCAL_VAR (ip[2], double);
	auto b = LOCAL_VAR (ip[3], gint32);
	LOCAL_VAR (ip[1], double) = std::scalbn (a, b);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_ILOGB)
{
	double x = LOCAL_VAR (ip[2], double);
	int result;
	if (FP_ILOGB0 != INT_MIN && x == 0.0)
		result = INT_MIN;
	else if (FP_ILOGBNAN != INT_MAX && std::isnan (x))
		result = INT_MAX;
	else
		result = std::ilogb (x);

	LOCAL_VAR (ip[1], gint32) = result;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

IMPL_UNOP (MINT_ABSF, float, std::abs (x));
IMPL_UNOP (MINT_ASINF, float, std::asin (x));
IMPL_UNOP (MINT_ASINHF, float, std::asinh (x));
IMPL_UNOP (MINT_ACOSF, float, std::acos (x));
IMPL_UNOP (MINT_ACOSHF, float, std::acosh (x));
IMPL_UNOP (MINT_ATANF, float, std::atan (x));
IMPL_UNOP (MINT_ATANHF, float, std::atanh(x));
IMPL_UNOP (MINT_CEILINGF, float, std::ceil (x));
IMPL_UNOP (MINT_COSF, float, std::cos (x));
IMPL_UNOP (MINT_CBRTF, float, std::cbrt (x));
IMPL_UNOP (MINT_COSHF, float, std::cosh (x));
IMPL_UNOP (MINT_EXPF, float, std::exp (x));
IMPL_UNOP (MINT_FLOORF, float, std::floor (x));
IMPL_UNOP (MINT_LOGF, float, std::log (x));
IMPL_UNOP (MINT_LOG2F, float, std::log2 (x));
IMPL_UNOP (MINT_LOG10F, float, std::log10 (x));
IMPL_UNOP (MINT_SINF, float, std::sin (x));
IMPL_UNOP (MINT_SQRTF, float, std::sqrt (x));
IMPL_UNOP (MINT_SINHF, float, std::sinh (x));
IMPL_UNOP (MINT_TANF, float, std::tan (x));
IMPL_UNOP (MINT_TANHF, float, std::tanh (x));
IMPL_BINOP (MINT_ATAN2F, float, std::atan2 (a, b));
IMPL_BINOP (MINT_POWF, float, std::pow (a, b));
IMPL_TRIOP (MINT_FMAF, float, std::fma (a, b, c));

MONO_INTERP_OP_IMPL (MINT_SCALEBF)
{
	auto a = LOCAL_VAR (ip[2], float);
	auto b = LOCAL_VAR (ip[3], gint32);
	LOCAL_VAR (ip[1], float) = std::scalbn (a, b);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_ILOGBF)
{
	double x = LOCAL_VAR (ip[2], float);
	int result;
	if (FP_ILOGB0 != INT_MIN && x == 0.0)
		result = INT_MIN;
	else if (FP_ILOGBNAN != INT_MAX && std::isnan (x))
		result = INT_MAX;
	else
		result = std::ilogb (x);

	LOCAL_VAR (ip[1], gint32) = result;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

#define IMPL_CKFINITE(opcode, type)                                \
	MONO_INTERP_OP_IMPL (opcode)                                   \
	{                                                              \
		type val = LOCAL_VAR (ip[2], type);                        \
                                                                   \
		if (!std::isfinite (val))                                  \
			THROW_EX (mono_get_exception_arithmetic (), ip);       \
		LOCAL_VAR (ip[1], type) = val;                             \
                                                                   \
		MONO_INTERP_OP_ADVANCE ();                                 \
		MONO_INTERP_DISPATCH ();                                   \
	}

IMPL_CKFINITE (MINT_CKFINITE, double);
IMPL_CKFINITE (MINT_CKFINITE_R4, float);

} // namespace mono::interp
