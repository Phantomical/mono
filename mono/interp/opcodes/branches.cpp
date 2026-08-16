#include "config.h"

#include "glib.h"
#include "mintops.h"
#include "mono/interp/interp.hpp"
#include <cmath>

namespace mono::interp {

/*
 * The branch opcodes, most of them pasted together from a base name and a type
 * suffix: MINT_BEQ_I4 and MINT_BEQ_I4_S come from IMPL_INT_CONDBR (MINT_BEQ, ...),
 * so grepping for the full name of one finds nothing. The _S form is the same
 * branch with a short displacement.
 */

#define IMPL_BRANCH_CORE(opcode, cond)                                         \
	MONO_INTERP_OP_IMPL (opcode)                                               \
	{                                                                          \
		if (cond) {                                                            \
			size_t broffset = InterpState::opinfos[opcode].num_sregs;          \
			if (InterpState::opinfos[opcode].optype == MintOpBranch)           \
				this->ip += (gint32) READ32 (&ip[broffset + 1]);               \
			else if (InterpState::opinfos[opcode].optype == MintOpShortBranch) \
				this->ip += (gint16) ip[broffset + 1];                         \
			else                                                               \
				g_assert_not_reached ();                                       \
		} else {                                                               \
			MONO_INTERP_OP_ADVANCE ();                                         \
		}                                                                      \
		MONO_INTERP_DISPATCH ();                                               \
	}

// IMPL_BRANCH_CORE can handle both _S and non-_S versions of the opcode transparently,
// so we implement both at the same time here.
#define IMPL_BRANCH(opcode, cond)   \
	IMPL_BRANCH_CORE (opcode, cond) \
	IMPL_BRANCH_CORE (opcode##_S, cond)

IMPL_BRANCH(MINT_BR, true);

#define IMPL_BRZERO(opcode, type, op) IMPL_BRANCH (opcode, LOCAL_VAR (ip[1], type) op 0)

IMPL_BRZERO (MINT_BRFALSE_I4, gint32, ==);
IMPL_BRZERO (MINT_BRFALSE_I8, gint64, ==);
IMPL_BRZERO (MINT_BRFALSE_R4, float, ==);
IMPL_BRZERO (MINT_BRFALSE_R8, double, ==);
IMPL_BRZERO (MINT_BRTRUE_I4, gint32, !=);
IMPL_BRZERO (MINT_BRTRUE_I8, gint64, !=);
IMPL_BRZERO (MINT_BRTRUE_R4, float, !=);
IMPL_BRZERO (MINT_BRTRUE_R8, double, !=);

#define IMPL_CONDBR(opcode, type, op) \
	IMPL_BRANCH (opcode, LOCAL_VAR (ip[1], type) op LOCAL_VAR (ip[2], type))
#define IMPL_INT_CONDBR(opcode, btype, op)    \
	IMPL_CONDBR (opcode##_I4, btype##32, op); \
	IMPL_CONDBR (opcode##_I8, btype##64, op)

IMPL_INT_CONDBR (MINT_BEQ, gint, ==);
IMPL_INT_CONDBR (MINT_BGE, gint, >=);
IMPL_INT_CONDBR (MINT_BGT, gint, >);
IMPL_INT_CONDBR (MINT_BLT, gint, <);
IMPL_INT_CONDBR (MINT_BLE, gint, <=);
IMPL_INT_CONDBR (MINT_BNE_UN, guint, !=);
IMPL_INT_CONDBR (MINT_BGE_UN, guint, >=);
IMPL_INT_CONDBR (MINT_BGT_UN, guint, >);
IMPL_INT_CONDBR (MINT_BLT_UN, guint, <);
IMPL_INT_CONDBR (MINT_BLE_UN, guint, <=);

#define IMPL_FLT_CONDBR_ORD(opcode, type, op)                                                 \
	IMPL_BRANCH (opcode, !std::isunordered (LOCAL_VAR (ip[1], type), LOCAL_VAR (ip[2], type)) \
	                         && (LOCAL_VAR (ip[1], type) op LOCAL_VAR (ip[2], type)))

#define IMPL_FLT_CONDBR_UN(opcode, type, op)                                                 \
	IMPL_BRANCH (opcode, std::isunordered (LOCAL_VAR (ip[1], type), LOCAL_VAR (ip[2], type)) \
	                         || (LOCAL_VAR (ip[1], type) op LOCAL_VAR (ip[2], type)))

#define IMPL_FLT_CONDBR(opcode, op)                 \
	IMPL_FLT_CONDBR_ORD (opcode##_R4, float, op);   \
	IMPL_FLT_CONDBR_ORD (opcode##_R8, double, op);  \
	IMPL_FLT_CONDBR_UN (opcode##_UN_R4, float, op); \
	IMPL_FLT_CONDBR_UN (opcode##_UN_R8, double, op)

IMPL_FLT_CONDBR_ORD (MINT_BEQ_R4, float, ==);
IMPL_FLT_CONDBR_ORD (MINT_BEQ_R8, double, ==);
IMPL_FLT_CONDBR_UN (MINT_BNE_UN_R4, float, !=)
IMPL_FLT_CONDBR_UN (MINT_BNE_UN_R8, double, !=)
IMPL_FLT_CONDBR (MINT_BGE, >=);
IMPL_FLT_CONDBR (MINT_BGT, >);
IMPL_FLT_CONDBR (MINT_BLT, <);
IMPL_FLT_CONDBR (MINT_BLE, <=);

} // namespace mono::interp
