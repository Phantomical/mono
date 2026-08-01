/**
 * \file
 * \brief The wire format of the side tables the compiler writes into a method's
 * object, shared between the writer (compiler.cpp) and the reader (jinfo.cpp).
 *
 * Two sections, both target-neutral and code-relative:
 *
 * `.mono_lsda` is the clause table - the tiered backend's format, verbatim, so
 * its reader (mono/mini/llvm/mono_lsda.cpp) parses ours too. See mono_lsda.hpp
 * for the layout.
 *
 * `.mono_unwind` is the frame description: the CFI program LLVM tracked for the
 * function, recorded at the MC layer before any target encoding exists.
 *
 *   Header (12 bytes, little-endian):
 *     u32 magic   = 0x4d555744 ('MUWD')
 *     u16 version = 1
 *     u16 reserved
 *     u32 count
 *   Record[count] (17 bytes each, little-endian):
 *     u32 offset      code offset the rule takes effect at
 *     u8  operation   one of the MONO_UNWIND_OP_* codes below
 *     i32 reg         DWARF register number, or 0 where the op has none
 *     i64 value       the op's offset operand, or 0
 *
 * The operation codes are this format's own, not LLVM's OpType enumerators -
 * those are not a stable ABI across LLVM versions - and not DW_CFA opcodes,
 * which would suggest a byte-for-byte DWARF program when the records carry the
 * MC layer's semantic form. An operation the writer cannot express is recorded
 * as UNSUPPORTED rather than dropped, so the reader declines the method instead
 * of unwinding it wrongly.
 */

#ifndef MONO_LLVM_SIDETABLES_HPP
#define MONO_LLVM_SIDETABLES_HPP

#include <cstdint>

namespace mono {

constexpr uint32_t unwind_section_magic = 0x4d555744; /* 'MUWD' */
constexpr uint16_t unwind_section_version = 1;
constexpr std::size_t unwind_header_size = 12;
constexpr std::size_t unwind_record_size = 17;

enum MonoUnwindWireOp : uint8_t {
	MONO_UNWIND_OP_UNSUPPORTED = 0,
	MONO_UNWIND_OP_DEF_CFA = 1,        /* cfa = reg + value */
	MONO_UNWIND_OP_DEF_CFA_OFFSET = 2, /* cfa = same reg + value */
	MONO_UNWIND_OP_DEF_CFA_REGISTER = 3,
	MONO_UNWIND_OP_OFFSET = 4,         /* reg saved at cfa + value */
	MONO_UNWIND_OP_REMEMBER_STATE = 5,
	MONO_UNWIND_OP_RESTORE_STATE = 6,
	MONO_UNWIND_OP_RESTORE = 7,        /* reg reverts to its entry rule */
	MONO_UNWIND_OP_SAME_VALUE = 8,     /* reg is not saved after all */
};

} // namespace mono

#endif /* MONO_LLVM_SIDETABLES_HPP */
