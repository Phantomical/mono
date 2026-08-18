/**
 * \file
 * \brief The instructions a redirectable stub is made of.
 */

#include "arch/arch.hpp"

#include <llvm/ExecutionEngine/JITLink/x86_64.h>

#include <cstring>

using namespace llvm;

namespace mono::arch {

void
write_jump_stub (char *code, const void *slot)
{
	constexpr size_t jump_size = sizeof (jitlink::x86_64::PointerJumpStubContent);

	std::memcpy (code, jitlink::x86_64::PointerJumpStubContent, jump_size);
	std::memset (code + jump_size, 0xcc, stub_block_size - jump_size);

	/* rip is the end of the instruction, and the displacement follows the two
	 * opcode bytes. */
	int32_t disp = static_cast<int32_t> (static_cast<const char *> (slot) -
	                                     (code + jump_size));
	std::memcpy (code + 2, &disp, sizeof (disp));
}

void
write_keyed_jump_stub (char *code, const void *slot, const void *key)
{
	/* movabsq $key, %r10 - MONO_ARCH_IMT_REG, which is what `nest` pins to. */
	constexpr size_t load_size = 10;
	static const uint8_t load[2] = { 0x49, 0xba };
	constexpr size_t jump_size =
		sizeof (jitlink::x86_64::PointerJumpStubContent);

	static_assert (load_size + jump_size <= stub_block_size,
	               "a keyed stub does not fit in a stub block");

	std::memcpy (code, load, sizeof (load));
	std::memcpy (code + sizeof (load), &key, sizeof (key));

	char *jump = code + load_size;

	std::memcpy (jump, jitlink::x86_64::PointerJumpStubContent, jump_size);
	std::memset (jump + jump_size, 0xcc,
	             stub_block_size - load_size - jump_size);

	int32_t disp = static_cast<int32_t> (static_cast<const char *> (slot)
	                                     - (jump + jump_size));

	std::memcpy (jump + 2, &disp, sizeof (disp));
}

void
write_unbox_stub (char *code, const void *slot, unsigned adjust)
{
	/* addq $adjust, %rdi - the receiver, which is parameter 0 of this
	 * convention. A hidden return pointer never takes that place: it goes at
	 * index 1 whenever the prototype has more than one parameter, and an
	 * unboxing method always has the receiver as well. */
	constexpr size_t add_size = 7;
	static const uint8_t add[3] = { 0x48, 0x81, 0xc7 };
	constexpr size_t jump_size =
		sizeof (jitlink::x86_64::PointerJumpStubContent);

	static_assert (add_size + jump_size <= stub_block_size,
	               "an unboxing stub does not fit in a stub block");

	std::memcpy (code, add, sizeof (add));

	uint32_t imm = adjust;

	std::memcpy (code + sizeof (add), &imm, sizeof (imm));

	char *jump = code + add_size;

	std::memcpy (jump, jitlink::x86_64::PointerJumpStubContent, jump_size);
	std::memset (jump + jump_size, 0xcc,
	             stub_block_size - add_size - jump_size);

	int32_t disp = static_cast<int32_t> (static_cast<const char *> (slot)
	                                     - (jump + jump_size));

	std::memcpy (jump + 2, &disp, sizeof (disp));
}

} // namespace mono::arch
