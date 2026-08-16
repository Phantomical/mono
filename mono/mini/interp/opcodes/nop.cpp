#include "glib.h"
#include "mono/mini/interp/interp.hpp"

namespace mono::interp {

MONO_INTERP_OP_IMPL (MINT_NOP)
{
	g_assert_not_reached ();

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

// not implemented yet
MONO_INTERP_OP_IMPL (MINT_NIY)
{
	g_assert_not_reached ();

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
