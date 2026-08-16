#include "config.h"

#include "mintops.hpp"
#include "mono/interp/interp.hpp"

namespace mono::interp {

#define IMPL_LDC(opcode, type, value)      \
	MONO_INTERP_OP_IMPL (opcode)           \
	{                                      \
		LOCAL_VAR (ip[1], type) = (value); \
                                           \
		MONO_INTERP_OP_ADVANCE ();         \
		MONO_INTERP_DISPATCH ();           \
	}

IMPL_LDC (MINT_LDNULL, gpointer, nullptr);
IMPL_LDC (MINT_LDC_I4_M1, gint32, -1);
IMPL_LDC (MINT_LDC_I4_0, gint32, 0);
IMPL_LDC (MINT_LDC_I4_1, gint32, 1);
IMPL_LDC (MINT_LDC_I4_2, gint32, 2);
IMPL_LDC (MINT_LDC_I4_3, gint32, 3);
IMPL_LDC (MINT_LDC_I4_4, gint32, 4);
IMPL_LDC (MINT_LDC_I4_5, gint32, 5);
IMPL_LDC (MINT_LDC_I4_6, gint32, 6);
IMPL_LDC (MINT_LDC_I4_7, gint32, 7);
IMPL_LDC (MINT_LDC_I4_8, gint32, 8);
IMPL_LDC (MINT_LDC_I4_S, gint32, (short) ip[2]);
IMPL_LDC (MINT_LDC_I4, gint32, READ32 (&ip[2]));
IMPL_LDC (MINT_LDC_I8, gint64, READ64 (&ip[2]));
IMPL_LDC (MINT_LDC_I8_S, gint64, (short) ip[2]);
IMPL_LDC (MINT_LDC_R4, gint32, READ32 (&ip[2]));
IMPL_LDC (MINT_LDC_R8, gint64, READ64 (&ip[2]));

} // namespace mono::interp
