/**
 * \file
 */

#ifndef __MONO_INTERP_MINTOPS_H__
#define __MONO_INTERP_MINTOPS_H__

#include <glib.h>

#include <cstddef>
#include <cstdint>
#include <optional>

enum MintOpArgType : std::uint8_t {
	MintOpNoArgs,
	MintOpShortInt,
	MintOpUShortInt,
	MintOpInt,
	MintOpLongInt,
	MintOpFloat,
	MintOpDouble,
	MintOpBranch,
	MintOpShortBranch,
	MintOpSwitch,
	MintOpMethodToken,
	MintOpFieldToken,
	MintOpClassToken,
	MintOpTwoShorts,
	MintOpShortAndInt
};

#define OPDEF(a, b, c, d, e, f) a,
enum MintOpcode : std::uint16_t {
#include "mintops.def"
	MINT_LASTOP
};
#undef OPDEF

#if NO_UNALIGNED_ACCESS
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define READ32(x) (((guint16 *) (x))[0] | ((guint16 *) (x))[1] << 16)
#define READ64(x)                                                          \
	((guint64) ((guint16 *) (x))[0] | (guint64) ((guint16 *) (x))[1] << 16 \
	 | (guint64) ((guint16 *) (x))[2] << 32 | (guint64) ((guint16 *) (x))[3] << 48)
#else
#define READ32(x) (((guint16 *) (x))[0] << 16 | ((guint16 *) (x))[1])
#define READ64(x)                                                                \
	((guint64) ((guint16 *) (x))[0] << 48 | (guint64) ((guint16 *) (x))[1] << 32 \
	 | (guint64) ((guint16 *) (x))[2] << 16 | (guint64) ((guint16 *) (x))[3])
#endif
#else /* unaligned access OK */
#define READ32(x) (*(guint32 *) (x))
#define READ64(x) (*(guint64 *) (x))
#endif

#define MINT_SWITCH_LEN(n) (4 + (n) * 2)

#define MINT_IS_MOV(op) ((op) >= MINT_MOV_I1 && (op) <= MINT_MOV_VT)
#define MINT_IS_CONDITIONAL_BRANCH(op) ((op) >= MINT_BRFALSE_I4 && (op) <= MINT_BLT_UN_R8_S)
#define MINT_IS_UNOP_CONDITIONAL_BRANCH(op) ((op) >= MINT_BRFALSE_I4 && (op) <= MINT_BRTRUE_R8_S)
#define MINT_IS_BINOP_CONDITIONAL_BRANCH(op) ((op) >= MINT_BEQ_I4 && (op) <= MINT_BLT_UN_R8_S)
#define MINT_IS_CALL(op) ((op) >= MINT_CALL && (op) <= MINT_JIT_CALL)
#define MINT_IS_PATCHABLE_CALL(op) ((op) >= MINT_CALL && (op) <= MINT_VCALL)
#define MINT_IS_NEWOBJ(op) ((op) >= MINT_NEWOBJ && (op) <= MINT_NEWOBJ_MAGIC)
#define MINT_IS_LDC_I4(op) ((op) >= MINT_LDC_I4_M1 && (op) <= MINT_LDC_I4)
#define MINT_IS_UNOP(op) ((op) >= MINT_ADD1_I4 && (op) <= MINT_CEQ0_I4)
#define MINT_IS_BINOP(op) ((op) >= MINT_ADD_I4 && (op) <= MINT_CLT_UN_R8)
#define MINT_IS_LDFLD(op) ((op) >= MINT_LDFLD_I1 && (op) <= MINT_LDFLD_O)
#define MINT_IS_STFLD(op) ((op) >= MINT_STFLD_I1 && (op) <= MINT_STFLD_O)

#define MINT_CALL_ARGS 2

namespace mono::interp {

/// A table of every opcode's name, packed into one object so a lookup costs an
/// offset rather than a pointer and a relocation.
struct OpNames {
#define OPDEF(a, b, c, d, e, f) char a[sizeof (b)];
#include "mintops.def"
#undef OPDEF
};

inline constexpr OpNames opnames = {
#define OPDEF(a, b, c, d, e, f) b,
#include "mintops.def"
#undef OPDEF
};

/// The static shape of one opcode: its name, encoded length, operand type, and
/// register counts.
struct OpInfo {
	std::uint16_t name_offset;
	/// In guint16 units. Zero at MINT_SWITCH, whose length is its operand.
	std::uint8_t oplength;
	std::uint8_t num_sregs;
	/// Empty at the call opcodes, which write the call argument area rather
	/// than one register.
	std::optional<std::uint8_t> num_dregs;
	MintOpArgType optype;
};

inline constexpr OpInfo opinfos[MINT_LASTOP] = {
#define CallArgs std::nullopt
#define OPDEF(a, b, c, d, e, f)                  \
	OpInfo{.name_offset = offsetof (OpNames, a), \
	       .oplength = c,                        \
	       .num_sregs = e,                       \
	       .num_dregs = d,                       \
	       .optype = f},
#include "mintops.def"
#undef OPDEF
#undef CallArgs
};

inline const char *
opname (int op)
{
	return reinterpret_cast<const char *> (&opnames) + opinfos[op].name_offset;
}

inline int
oplen (int op)
{
	return opinfos[op].oplength;
}

/// Returns the destination register count, or MINT_CALL_ARGS for an opcode that
/// writes the call argument area.
inline int
num_dregs (int op)
{
	return opinfos[op].num_dregs.value_or (MINT_CALL_ARGS);
}

inline int
num_sregs (int op)
{
	return opinfos[op].num_sregs;
}

inline MintOpArgType
opargtype (int op)
{
	return opinfos[op].optype;
}

/// Where the instruction at ip ends.
inline const guint16 *
dis_mintop_len (const guint16 *ip)
{
	int len = oplen (*ip);

	if (len == 0) /* SWITCH */
		len = MINT_SWITCH_LEN (READ32 (ip + 2));

	return ip + len;
}

} // namespace mono::interp

#endif
