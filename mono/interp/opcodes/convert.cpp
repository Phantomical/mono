#include "glib.h"
#include "mintops.h"
#include "mono/metadata/exception.h"
#include "mono/interp/interp.hpp"
#include "mono/mini/jit-icalls.h"
#include "mono/utils/mono-math.h"
#include <cmath>

namespace mono::interp {

/*
 * The conversion opcodes. Each one reads one source variable and writes one
 * destination variable, and the pair of types in the name gives the direction:
 * MINT_CONV_I1_R8 makes an int8 out of a float64.
 *
 * A destination narrower than int32 still occupies an int32 on the interpreter
 * stack, because ECMA-335 Partition III has no narrower stack type. The value
 * goes back to int32 by sign extension or by zero extension, which the cast to
 * the narrow type and the assignment to the wide destination do together.
 */

#define IMPL_CONV(opcode, dsttype, srctype, expr) \
	MONO_INTERP_OP_IMPL (opcode)                  \
	{                                             \
		srctype val = LOCAL_VAR (ip[2], srctype); \
		LOCAL_VAR (ip[1], dsttype) = (expr);      \
                                                  \
		MONO_INTERP_OP_ADVANCE ();                \
		MONO_INTERP_DISPATCH ();                  \
	}

/*
 * The four widths that narrow to less than int32, over the four source types
 * the transform emits for them.
 *
 * A floating point source goes through via first. Without that cast the C
 * compiler is allowed to use undefined behaviour when the value is bigger than
 * the narrow type. See the conv.fpint section in the C standard:
 * > The conversion truncates; that is, the fractional  part
 * > is discarded.  The behavior is undefined if the truncated
 * > value cannot be represented in the destination type.
 */
#define IMPL_CONV_NARROW(opcode, narrow, via)                   \
	IMPL_CONV (opcode##_I4, gint32, gint32, (narrow) val);      \
	IMPL_CONV (opcode##_I8, gint32, gint64, (narrow) val);      \
	IMPL_CONV (opcode##_R4, gint32, float, (narrow) (via) val); \
	IMPL_CONV (opcode##_R8, gint32, double, (narrow) (via) val)

IMPL_CONV_NARROW (MINT_CONV_I1, gint8, gint32);
IMPL_CONV_NARROW (MINT_CONV_U1, guint8, guint32);
IMPL_CONV_NARROW (MINT_CONV_I2, gint16, gint32);
IMPL_CONV_NARROW (MINT_CONV_U2, guint16, guint32);

IMPL_CONV (MINT_CONV_I4_I8, gint32, gint64, (gint32) val);
IMPL_CONV (MINT_CONV_I4_R4, gint32, float, (gint32) val);
IMPL_CONV (MINT_CONV_I4_R8, gint32, double, (gint32) val);
IMPL_CONV (MINT_CONV_U4_I8, gint32, gint64, (gint32) val);

#ifdef MONO_ARCH_EMULATE_FCONV_TO_U4
IMPL_CONV (MINT_CONV_U4_R4, gint32, float, mono_rconv_u4 (val));
IMPL_CONV (MINT_CONV_U4_R8, gint32, double, mono_fconv_u4_2 (val));
#else
IMPL_CONV (MINT_CONV_U4_R4, gint32, float, (guint32) val);
IMPL_CONV (MINT_CONV_U4_R8, gint32, double, (guint32) val);
#endif

IMPL_CONV (MINT_CONV_I8_I4, gint64, gint32, val);
IMPL_CONV (MINT_CONV_I8_U4, gint64, gint32, (guint32) val);
IMPL_CONV (MINT_CONV_I8_R4, gint64, float, (gint64) val);
IMPL_CONV (MINT_CONV_I8_R8, gint64, double, (gint64) val);

#ifdef MONO_ARCH_EMULATE_FCONV_TO_U8
IMPL_CONV (MINT_CONV_U8_R4, gint64, float, mono_rconv_u8 (val));
IMPL_CONV (MINT_CONV_U8_R8, gint64, double, mono_fconv_u8_2 (val));
#else
IMPL_CONV (MINT_CONV_U8_R4, gint64, float, (guint64) val);
IMPL_CONV (MINT_CONV_U8_R8, gint64, double, (guint64) val);
#endif

IMPL_CONV (MINT_CONV_R4_I4, float, gint32, (float) val);
IMPL_CONV (MINT_CONV_R4_I8, float, gint64, (float) val);
IMPL_CONV (MINT_CONV_R4_R8, float, double, (float) val);

IMPL_CONV (MINT_CONV_R8_I4, double, gint32, (double) val);
IMPL_CONV (MINT_CONV_R8_I8, double, gint64, (double) val);
IMPL_CONV (MINT_CONV_R8_R4, double, float, (double) val);

// conv.r.un reads the source as unsigned. There is no float32 form: the
// transform gives conv.r.un a float64 result whatever the source width.
IMPL_CONV (MINT_CONV_R_UN_I4, double, guint32, (double) val);
IMPL_CONV (MINT_CONV_R_UN_I8, double, guint64, (double) val);

/*
 * The checked conversions. ovf is the condition that makes the value not fit,
 * and it names the source as val.
 */
#define IMPL_CONV_OVF(opcode, dsttype, srctype, ovf, expr) \
	MONO_INTERP_OP_IMPL (opcode)                           \
	{                                                      \
		srctype val = LOCAL_VAR (ip[2], srctype);          \
		if (G_UNLIKELY (ovf))                              \
			THROW_EX (mono_get_exception_overflow (), ip); \
		LOCAL_VAR (ip[1], dsttype) = (expr);               \
                                                           \
		MONO_INTERP_OP_ADVANCE ();                         \
		MONO_INTERP_DISPATCH ();                           \
	}

namespace {
template<typename T>
struct launder {
	using type = T;
};

template<typename T>
using launder_t = typename launder<T>::type;
} // namespace

template<typename T>
static inline bool
outside (T val, launder_t<T> lo, launder_t<T> hi)
{
	return !(val > lo && val < hi);
}

IMPL_CONV_OVF (MINT_CONV_OVF_I1_I4, gint32, gint32, (val < G_MININT8) || (val > G_MAXINT8), val);
IMPL_CONV_OVF (MINT_CONV_OVF_I1_U4, gint32, gint32, (val < 0) || (val > G_MAXINT8), val);
IMPL_CONV_OVF (MINT_CONV_OVF_I1_I8, gint32, gint64, (val < G_MININT8) || (val > G_MAXINT8),
               (gint8) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I1_U8, gint32, gint64, (val < 0) || (val > G_MAXINT8), (gint8) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I1_R4, gint32, float, outside (val, G_MININT8 - 1, G_MAXINT8 + 1),
               (gint8) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I1_R8, gint32, double, outside (val, G_MININT8 - 1, G_MAXINT8 + 1),
               (gint8) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I1_UN_R4, gint32, float,
               (val < 0) || (val > G_MAXINT8) || std::isnan (val), (gint8) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I1_UN_R8, gint32, double,
               (val < 0) || (val > G_MAXINT8) || std::isnan (val), (gint8) val);

IMPL_CONV_OVF (MINT_CONV_OVF_U1_I4, gint32, gint32, (val < 0) || (val > G_MAXUINT8), val);
IMPL_CONV_OVF (MINT_CONV_OVF_U1_I8, gint32, gint64, (val < 0) || (val > G_MAXUINT8), (guint8) val);
IMPL_CONV_OVF (MINT_CONV_OVF_U1_R4, gint32, float, outside (val, -1.0, G_MAXUINT8 + 1),
               (guint8) val);
IMPL_CONV_OVF (MINT_CONV_OVF_U1_R8, gint32, double, outside (val, -1.0, G_MAXUINT8 + 1),
               (guint8) val);

IMPL_CONV_OVF (MINT_CONV_OVF_I2_I4, gint32, gint32, (val < G_MININT16) || (val > G_MAXINT16),
               (gint16) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I2_U4, gint32, gint32, (val < 0) || (val > G_MAXINT16), (gint16) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I2_I8, gint32, gint64, (val < G_MININT16) || (val > G_MAXINT16),
               (gint16) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I2_U8, gint32, gint64, (val < 0) || (val > G_MAXINT16), (gint16) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I2_R4, gint32, float, outside (val, G_MININT16 - 1, G_MAXINT16 + 1),
               (gint16) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I2_R8, gint32, double, outside (val, G_MININT16 - 1, G_MAXINT16 + 1),
               (gint16) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I2_UN_R4, gint32, float,
               (val < 0) || (val > G_MAXINT16) || std::isnan (val), (gint16) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I2_UN_R8, gint32, double,
               (val < 0) || (val > G_MAXINT16) || std::isnan (val), (gint16) val);

IMPL_CONV_OVF (MINT_CONV_OVF_U2_I4, gint32, gint32, (val < 0) || (val > G_MAXUINT16), val);
IMPL_CONV_OVF (MINT_CONV_OVF_U2_I8, gint32, gint64, (val < 0) || (val > G_MAXUINT16),
               (guint16) val);
IMPL_CONV_OVF (MINT_CONV_OVF_U2_R4, gint32, float, outside (val, -1.0, G_MAXUINT16 + 1),
               (guint16) val);
IMPL_CONV_OVF (MINT_CONV_OVF_U2_R8, gint32, double, outside (val, -1.0, G_MAXUINT16 + 1),
               (guint16) val);

IMPL_CONV_OVF (MINT_CONV_OVF_I4_U4, gint32, gint32, val < 0, val);
IMPL_CONV_OVF (MINT_CONV_OVF_I4_I8, gint32, gint64, (val < G_MININT32) || (val > G_MAXINT32),
               (gint32) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I4_U8, gint32, guint64, val > G_MAXINT32, (gint32) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I4_R4, gint32, float, !(val >= -2147483648.0f && val < 2147483648.0f),
               (gint32) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I4_R8, gint32, double,
               outside (val, (double) G_MININT32 - 1, (double) G_MAXINT32 + 1), (gint32) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I4_UN_R8, gint32, double, (val < 0) || (val > G_MAXINT32),
               (gint32) val);

IMPL_CONV_OVF (MINT_CONV_OVF_U4_I4, gint32, gint32, val < 0, val);
IMPL_CONV_OVF (MINT_CONV_OVF_U4_I8, gint32, gint64, (val < 0) || (val > G_MAXUINT32),
               (guint32) val);
IMPL_CONV_OVF (MINT_CONV_OVF_U4_R4, gint32, float, outside (val, -1.0, (double) G_MAXUINT32 + 1),
               (guint32) val);
IMPL_CONV_OVF (MINT_CONV_OVF_U4_R8, gint32, double, outside (val, -1.0, (double) G_MAXUINT32 + 1),
               (guint32) val);

IMPL_CONV_OVF (MINT_CONV_OVF_I8_U8, gint64, guint64, val > G_MAXINT64, val);
IMPL_CONV_OVF (MINT_CONV_OVF_I8_UN_R4, gint64, float,
               (val < 0) || std::isnan (val) || std::trunc (val) != (gint64) val, (gint64) val);
IMPL_CONV_OVF (MINT_CONV_OVF_I8_UN_R8, gint64, double,
               (val < 0) || std::isnan (val) || std::trunc (val) != (gint64) val, (gint64) val);

IMPL_CONV_OVF (MINT_CONV_OVF_U8_I4, guint64, gint32, val < 0, val);
IMPL_CONV_OVF (MINT_CONV_OVF_U8_I8, guint64, gint64, val < 0, val);

// The 64 bit destinations from a floating point source go through the shared
// truncating helpers, which write the result themselves.
#define IMPL_CONV_OVF_TRUNC(opcode, dsttype, srctype, trunc)        \
	MONO_INTERP_OP_IMPL (opcode)                                    \
	{                                                               \
		srctype val = LOCAL_VAR (ip[2], srctype);                   \
		if (G_UNLIKELY (!trunc (val, &LOCAL_VAR (ip[1], dsttype)))) \
			THROW_EX (mono_get_exception_overflow (), ip);          \
                                                                    \
		MONO_INTERP_OP_ADVANCE ();                                  \
		MONO_INTERP_DISPATCH ();                                    \
	}

IMPL_CONV_OVF_TRUNC (MINT_CONV_OVF_I8_R4, gint64, float, mono_try_trunc_i64);
IMPL_CONV_OVF_TRUNC (MINT_CONV_OVF_I8_R8, gint64, double, mono_try_trunc_i64);
IMPL_CONV_OVF_TRUNC (MINT_CONV_OVF_U8_R4, guint64, float, mono_try_trunc_u64);
IMPL_CONV_OVF_TRUNC (MINT_CONV_OVF_U8_R8, guint64, double, mono_try_trunc_u64);

} // namespace mono::interp
