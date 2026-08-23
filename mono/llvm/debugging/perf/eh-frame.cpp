/**
 * \file
 * \brief Writing `.eh_frame` and `.eh_frame_hdr` for a jitdump unwinding record.
 *
 * `perf inject --jit` does not load the record as it stands. It builds one ELF
 * image per record and places the pieces itself. So what goes in has to be
 * addressed for the image that comes out:
 *
 *     +---------------+ <- .text, at a fixed offset T the image chooses
 *     |     code      |
 *     +---------------+
 *     |    padding    |    up to the next 8
 *     +---------------+ <- .eh_frame, at T + align8(image size)
 *     |      CIE      |
 *     |     FDE(s)    |    one per function in the code
 *     |  terminator   |
 *     +---------------+ <- .eh_frame_hdr
 *     |  the pointers |    a lookup row per FDE, in address order
 *     +---------------+
 *
 * Every pointer written here is therefore a displacement, never an address. Two
 * of them have to reach the code: the FDE's own record of where its function is,
 * and the header's lookup row. Both count back to the start of `.text`, so T
 * cancels out of the subtraction and this writer never has to know it.
 */

#include "debugging/perf/eh-frame.hpp"

#include "arch/arch.hpp"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Support/LEB128.h>

#include <algorithm>

using namespace llvm::dwarf;

namespace mono::perf {

namespace {

/// version, three encoding bytes, the pointer to `.eh_frame` and the row count.
/// Each row is another eight bytes on top.
constexpr size_t eh_frame_hdr_size = 4 + 4 + 4;

/// An entry's total length, the length field included, has to be a multiple of
/// the address size.
constexpr size_t entry_alignment = 8;

size_t
align_up (size_t value, size_t to)
{
	return (value + to - 1) & ~(to - 1);
}

struct Writer {
	std::vector<uint8_t> bytes;

	size_t here () const { return bytes.size (); }

	void u8 (uint8_t v) { bytes.push_back (v); }

	void u32 (uint32_t v)
	{
		for (int i = 0; i < 4; ++i)
			bytes.push_back ((uint8_t) (v >> (8 * i)));
	}

	void i32 (int32_t v) { u32 ((uint32_t) v); }

	void patch_i32 (size_t at, int32_t v)
	{
		for (int i = 0; i < 4; ++i)
			bytes[at + i] = (uint8_t) ((uint32_t) v >> (8 * i));
	}

	void uleb (uint64_t v)
	{
		uint8_t buf[16];

		bytes.insert (bytes.end (), buf, buf + llvm::encodeULEB128 (v, buf));
	}

	void sleb (int64_t v)
	{
		uint8_t buf[16];

		bytes.insert (bytes.end (), buf, buf + llvm::encodeSLEB128 (v, buf));
	}

	/// Pad the entry that starts at from out to entry_alignment. DW_CFA_nop is
	/// the instruction that does nothing, which is what the padding wants.
	void pad_entry (size_t from)
	{
		while ((here () - from) % entry_alignment != 0)
			u8 (DW_CFA_nop);
	}
};

/// One `.mono_unwind` rule as the DWARF instruction that says the same thing,
/// or false where DWARF cannot say it in this encoding.
bool
write_rule (Writer &w, const UnwindRecord &r)
{
	switch (r.op) {
	case MONO_UNWIND_OP_DEF_CFA:
		if (r.value < 0)
			return false;
		w.u8 (DW_CFA_def_cfa);
		w.uleb ((uint32_t) r.reg);
		w.uleb ((uint64_t) r.value);
		return true;

	case MONO_UNWIND_OP_DEF_CFA_OFFSET:
		if (r.value < 0)
			return false;
		w.u8 (DW_CFA_def_cfa_offset);
		w.uleb ((uint64_t) r.value);
		return true;

	case MONO_UNWIND_OP_DEF_CFA_REGISTER:
		w.u8 (DW_CFA_def_cfa_register);
		w.uleb ((uint32_t) r.reg);
		return true;

	case MONO_UNWIND_OP_OFFSET: {
		/* Where the register sits below the CFA, in units of the CIE's data
		 * alignment factor. A slot that is not a whole number of them cannot
		 * be said at all. */
		if (r.value % arch::dwarf_data_alignment_factor != 0)
			return false;

		int64_t factored = r.value / arch::dwarf_data_alignment_factor;

		if (factored >= 0 && r.reg < 0x40) {
			w.u8 ((uint8_t) (DW_CFA_offset | r.reg));
			w.uleb ((uint64_t) factored);
		} else {
			w.u8 (DW_CFA_offset_extended_sf);
			w.uleb ((uint32_t) r.reg);
			w.sleb (factored);
		}
		return true;
	}

	case MONO_UNWIND_OP_REMEMBER_STATE:
		w.u8 (DW_CFA_remember_state);
		return true;

	case MONO_UNWIND_OP_RESTORE_STATE:
		w.u8 (DW_CFA_restore_state);
		return true;

	case MONO_UNWIND_OP_RESTORE:
		if (r.reg < 0x40)
			w.u8 ((uint8_t) (DW_CFA_restore | r.reg));
		else {
			w.u8 (DW_CFA_restore_extended);
			w.uleb ((uint32_t) r.reg);
		}
		return true;

	case MONO_UNWIND_OP_SAME_VALUE:
		w.u8 (DW_CFA_same_value);
		w.uleb ((uint32_t) r.reg);
		return true;

	default:
		return false;
	}
}

void
write_advance (Writer &w, uint32_t from, uint32_t to)
{
	uint32_t delta = (to - from) / arch::dwarf_code_alignment_factor;

	if (delta == 0)
		return;

	if (delta < 0x40) {
		w.u8 ((uint8_t) (DW_CFA_advance_loc | delta));
	} else if (delta <= 0xff) {
		w.u8 (DW_CFA_advance_loc1);
		w.u8 ((uint8_t) delta);
	} else if (delta <= 0xffff) {
		w.u8 (DW_CFA_advance_loc2);
		w.u8 ((uint8_t) delta);
		w.u8 ((uint8_t) (delta >> 8));
	} else {
		w.u8 (DW_CFA_advance_loc4);
		w.u32 (delta);
	}
}

/// One function of a record, with its CFI program already encoded.
struct Encoded {
	size_t offset;
	size_t size;
	std::vector<uint8_t> program;
};

/// The rules the target's ABI leaves in force at a function's first
/// instruction. The call pushed the return address, so the CFA is one slot
/// above the stack pointer, and the address sits in that slot.
///
/// This is the CIE's whole program, and it is what makes DW_CFA_restore mean
/// "back to the state the function was entered in". A function whose own
/// program opens with the same rules restates them in its FDE, which changes
/// nothing.
std::vector<uint8_t>
entry_state ()
{
	Writer w;
	size_t slot = (size_t) -arch::dwarf_data_alignment_factor;

	w.u8 (DW_CFA_def_cfa);
	w.uleb (arch::dwarf_stack_pointer_reg);
	w.uleb (slot);
	w.u8 ((uint8_t) (DW_CFA_offset | arch::dwarf_return_address_reg));
	w.uleb (1);
	return w.bytes;
}

/// Assemble a CIE and one FDE per function.
///
/// The functions have to be in ascending order of offset: a reader binary
/// searches the table in `.eh_frame_hdr`. image_size is what the code load
/// declares, and it is what the image is laid out from.
EhFrame
assemble (const std::vector<Encoded> &functions, size_t image_size)
{
	Writer w;
	size_t cie_off = w.here ();
	std::vector<uint8_t> initial = entry_state ();

	w.u32 (0); /* length, patched below */
	w.u32 (0); /* the CIE id, which is what tells it from an FDE */
	w.u8 (1);  /* version */
	w.u8 ('z');
	w.u8 ('R'); /* augmentation: a data block, holding the FDE encoding */
	w.u8 (0);
	w.uleb (arch::dwarf_code_alignment_factor);
	w.sleb (arch::dwarf_data_alignment_factor);
	w.uleb (arch::dwarf_return_address_reg);
	w.uleb (1); /* augmentation data length */
	w.u8 (DW_EH_PE_pcrel | DW_EH_PE_sdata4);
	w.bytes.insert (w.bytes.end (), initial.begin (), initial.end ());
	w.pad_entry (cie_off);
	w.patch_i32 (cie_off, (int32_t) (w.here () - cie_off - 4));

	std::vector<size_t> fde_offs, location_offs;

	for (const Encoded &fn : functions) {
		size_t fde_off = w.here ();

		w.u32 (0);                                  /* length, patched below */
		w.u32 ((uint32_t) (fde_off + 4 - cie_off)); /* back to the CIE */
		location_offs.push_back (w.here ());
		w.i32 (0); /* where the function is, patched below */
		w.u32 ((uint32_t) fn.size);
		w.uleb (0); /* augmentation data length */
		w.bytes.insert (w.bytes.end (), fn.program.begin (), fn.program.end ());
		w.pad_entry (fde_off);
		w.patch_i32 (fde_off, (int32_t) (w.here () - fde_off - 4));
		fde_offs.push_back (fde_off);
	}

	w.u32 (0); /* the terminator, an entry of length zero */

	size_t eh_frame_size = w.here ();
	size_t code_end = align_up (image_size, 8);

	/*
	 * Back from the field to the function. The image puts `.eh_frame` at
	 * T + align8(image size), and the field this many bytes into that, and the
	 * function at T + its offset. The two Ts cancel, so what is left is the same
	 * wherever the image puts the code.
	 */
	for (size_t i = 0; i < functions.size (); ++i)
		w.patch_i32 (location_offs[i],
		             (int32_t) functions[i].offset
		                     - (int32_t) (code_end + location_offs[i]));

	/* The `.eh_frame_hdr`, whose displacements are all against its own start. */
	w.u8 (1); /* version */
	w.u8 (DW_EH_PE_pcrel | DW_EH_PE_sdata4);   /* the pointer below */
	w.u8 (DW_EH_PE_udata4);                    /* the row count */
	w.u8 (DW_EH_PE_datarel | DW_EH_PE_sdata4); /* the rows */
	w.i32 (-(int32_t) (eh_frame_size + 4));
	w.u32 ((uint32_t) functions.size ());

	for (size_t i = 0; i < functions.size (); ++i) {
		w.i32 ((int32_t) functions[i].offset
		       - (int32_t) (code_end + eh_frame_size));
		w.i32 ((int32_t) fde_offs[i] - (int32_t) eh_frame_size);
	}

	EhFrame result;

	result.bytes = std::move (w.bytes);
	result.header_size = eh_frame_hdr_size + 8 * functions.size ();
	return result;
}

} // namespace

EhFrame
build_eh_frame (std::vector<FrameFunction> functions, size_t image_size)
{
	std::sort (functions.begin (), functions.end (),
	           [] (const FrameFunction &a, const FrameFunction &b) {
		           return a.offset < b.offset;
	           });

	std::vector<Encoded> encoded;

	for (const FrameFunction &fn : functions) {
		Writer program;
		uint32_t at = 0;
		bool sayable = true;

		if (fn.size == 0)
			continue;

		for (const UnwindRecord &r : fn.records) {
			write_advance (program, at, r.offset);
			at = r.offset;

			if (!write_rule (program, r)) {
				sayable = false;
				break;
			}
		}

		if (sayable)
			encoded.push_back ({fn.offset, fn.size, std::move (program.bytes)});
	}

	if (encoded.empty ())
		return {};

	return assemble (encoded, image_size);
}

EhFrame
build_eh_frame (const uint8_t *cfi, size_t cfi_size, size_t code_size,
                size_t image_size)
{
	if (cfi == nullptr || cfi_size == 0)
		return {};

	return assemble ({{0, code_size, std::vector<uint8_t> (cfi, cfi + cfi_size)}},
	                 image_size);
}

} // namespace mono::perf
