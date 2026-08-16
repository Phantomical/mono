#include "config.h"

/**
 * \file
 * \brief Moving values between the interpreter's locals, and taking their address.
 */

#include "mintops.h"
#include "mono/interp/interp-internals.h"
#include "mono/interp/interp.hpp"

#include <cstring>

namespace mono::interp {

#define IMPL_MOV(opcode, dsttype, srctype)                     \
	MONO_INTERP_OP_IMPL (opcode)                               \
	{                                                          \
		LOCAL_VAR (ip[1], dsttype) = LOCAL_VAR (ip[2], srctype); \
                                                               \
		MONO_INTERP_OP_ADVANCE ();                             \
		MONO_INTERP_DISPATCH ();                               \
	}

// Loading from a local may need a sign or zero extension to 4 bytes, which is the
// smallest thing the interpreter holds a value in. Only a local whose address is
// taken needs one, since nothing can propagate that local away.
IMPL_MOV (MINT_MOV_I1, guint32, gint8);
IMPL_MOV (MINT_MOV_U1, guint32, guint8);
IMPL_MOV (MINT_MOV_I2, guint32, gint16);
IMPL_MOV (MINT_MOV_U2, guint32, guint16);

IMPL_MOV (MINT_MOV_4, guint32, guint32);
IMPL_MOV (MINT_MOV_8, guint64, guint64);

// Source and destination are both interpreter locals, so they can overlap.
MONO_INTERP_OP_IMPL (MINT_MOV_VT)
{
	std::memmove (locals + ip[1], locals + ip[2], ip[3]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDLOCA_S)
{
	LOCAL_VAR (ip[1], gpointer) = locals + ip[2];

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
