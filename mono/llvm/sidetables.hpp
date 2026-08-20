/**
 * \file
 * \brief The wire format of the side tables the compiler writes into a method's
 * object, and the reader that takes one back apart.
 *
 * Three sections, all target-neutral and code-relative:
 *
 * `.mono_lsda` is the clause table - the tiered backend's format, verbatim, so
 * its reader (mono_lsda.cpp) parses ours too. See mono_lsda.hpp
 * for the layout.
 *
 * `.mono_guards` is what the thread-abort guard needs about the finally handler
 * bodies: which PCs each occupies, so a stack walk can tell a frame is inside
 * one, and where in that frame the guard byte sits. Its own section rather than
 * more `.mono_lsda` entries because the partitions differ - one record per
 * surviving copy of a body against one clause entry per invoke range - and
 * because the clause table's format is shared with a backend that recovers the
 * same facts elsewhere.
 *
 *   Header (8 bytes, little-endian):
 *     u32 magic   = 0x4d475244 ('MGRD')
 *     u16 version = 1
 *     u16 count
 *   Record[count] (20 bytes each, little-endian):
 *     u32 clause_index     the IL clause the body belongs to
 *     u32 body_start       body covers [code+body_start, code+body_end)
 *     u32 body_end
 *     i32 exvar_offset     guard byte at exvar_base_reg + exvar_offset
 *     i32 exvar_dwarf_reg  DWARF number of the register that offset is from
 *
 * The register is carried as DWARF rather than as a mono hardware register so
 * the writer needs nothing from mono's target headers; the reader converts.
 *
 * `.mono_unwind` is the frame description: the CFI program LLVM tracked for the
 * function, recorded at the MC layer before any target encoding exists. One
 * block per function the object defines, concatenated - a module holds the
 * method's body, its filter bodies and its entry thunks, and each of those is a
 * frame something may be suspended in - so the block names the function it
 * describes rather than leaving attribution to position.
 *
 *   Header (20 bytes, little-endian):
 *     u32 magic   = 0x4d555744 ('MUWD')
 *     u16 version = 2
 *     u16 reserved
 *     u32 count
 *     u64 function    where the function this describes was linked
 *   Record[count] (17 bytes each, little-endian):
 *     u32 offset      code offset the rule takes effect at
 *     u8  operation   one of the MONO_UNWIND_OP_* codes below
 *     i32 reg         DWARF register number, or 0 where the op has none
 *     i64 value       the op's offset operand, or 0
 *
 * `.mono_lines` is the IL-offset map: which statement was in effect at each code
 * offset, which is what a managed stack trace prints and what the soft
 * debugger's sequence points are recovered from. The offsets are statement
 * starts rather than instruction starts, because a symbol reader given an
 * offset that is not a sequence point resolves it to the next one. Same
 * block-per-function shape as `.mono_unwind` and for the same reason.
 *
 *   Header (20 bytes, little-endian):
 *     u32 magic   = 0x4d4c4e45 ('MLNE')
 *     u16 version = 1
 *     u16 reserved
 *     u32 count
 *     u64 function    where the function this describes was linked
 *   Record[count] (8 bytes each, little-endian):
 *     u32 offset      code offset the row takes effect at
 *     u32 line        the translator's line number: an IL offset, or a
 *                     sequence-point marker (seq-point-marker.hpp)
 *
 * Rows arrive in code order and several may land on one offset, which is what a
 * run of IL instructions collapses to once the optimizer is done with it; the
 * reader keeps the last, so the map stays single-valued and says the most recent
 * point execution passed.
 *
 * The function address is the one field that is not code-relative, so it is the
 * one thing in these sections the linker has to relocate.
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
#include <cstring>
#include <vector>

namespace mono {

constexpr uint32_t guards_section_magic = 0x4d475244; /* 'MGRD' */
constexpr uint16_t guards_section_version = 1;
constexpr std::size_t guards_header_size = 8;
constexpr std::size_t guards_record_size = 20;

constexpr uint32_t unwind_section_magic = 0x4d555744; /* 'MUWD' */
constexpr uint16_t unwind_section_version = 2;
constexpr std::size_t unwind_header_size = 20;
constexpr std::size_t unwind_record_size = 17;

constexpr uint32_t lines_section_magic = 0x4d4c4e45; /* 'MLNE' */
constexpr uint16_t lines_section_version = 1;
constexpr std::size_t lines_header_size = 20;
constexpr std::size_t lines_record_size = 8;

/*
 * The id of the stackmap naming this frame's argument and local slots, in
 * `.llvm_stackmaps` - LLVM's own section, not one of ours. The finally markers
 * share that section, so the id is what tells the two apart; it is picked out of
 * the same high-half tag space (mono_lsda_format.hpp).
 */
constexpr uint64_t vars_stackmap_id = 0xF19A13ULL << 32;

/*
 * The id of the stackmap naming the receiver slot of a body shared between
 * reference instantiations, in the same section and picked out the same way.
 *
 * A shared body's jit info names the shared method, so a stack walk that wants
 * the instantiation a frame is running as has to read the receiver out of the
 * frame - which is what MonoGenericJitInfo describes and this marker pins.
 */
constexpr uint64_t rgctx_stackmap_id = 0xF19A14ULL << 32;

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

/// One `.mono_unwind` record in the form its readers work in.
struct UnwindRecord {
	uint32_t offset;
	uint8_t op;
	int32_t reg;
	int64_t value;
};

/// One field of a side table. The tables are packed and little-endian whatever
/// the host is, so an aligned load of a field is not available.
template <typename T>
T
read_le (const uint8_t *p)
{
	T value;

	memcpy (&value, p, sizeof (T));
	return value;
}

/// The records of the `.mono_unwind` block that describes the function linked
/// at code, or false when the section holds no such block.
inline bool
parse_unwind_records (const uint8_t *section, size_t size, const uint8_t *code,
                      std::vector<UnwindRecord> &out)
{
	if (section == nullptr)
		return false;

	const uint8_t *end = section + size;

	for (const uint8_t *block = section; (size_t) (end - block) >= unwind_header_size;) {
		if (read_le<uint32_t> (block) != unwind_section_magic)
			return false;
		if (read_le<uint16_t> (block + 4) != unwind_section_version)
			return false;

		uint32_t count = read_le<uint32_t> (block + 8);
		const uint8_t *function =
			(const uint8_t *) (uintptr_t) read_le<uint64_t> (block + 12);
		const uint8_t *p = block + unwind_header_size;
		size_t records = (size_t) count * unwind_record_size;

		if ((size_t) (end - p) < records)
			return false;
		if (function != code) {
			block = p + records;
			continue;
		}

		out.reserve (count);
		for (uint32_t i = 0; i < count; ++i, p += unwind_record_size) {
			UnwindRecord r;

			r.offset = read_le<uint32_t> (p);
			r.op = p[4];
			r.reg = read_le<int32_t> (p + 5);
			r.value = read_le<int64_t> (p + 9);
			out.push_back (r);
		}

		return true;
	}

	return false;
}

} // namespace mono

#endif /* MONO_LLVM_SIDETABLES_HPP */
