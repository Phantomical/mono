#include "mono/interp/interp.hpp"

namespace mono::interp {

MONO_INTERP_OP_IMPL (MINT_SWITCH)
{
	guint32 val = LOCAL_VAR (ip[1], guint32);
	guint32 n = READ32 (&ip[2]);

	ip += 4;

	if (val < n) {
		ip += 2 * val;
		int offset = READ32 (ip);
		ip += offset;
	} else {
		ip += 2 * n;
	}

	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
