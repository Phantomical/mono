/**
 * \file
 * \brief The instructions a redirectable stub is made of.
 */

#include "arch/arch.hpp"

#include <llvm/ExecutionEngine/JITLink/x86_64.h>

#include <cassert>
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
write_unbox_prologue (char *code, unsigned adjust)
{
	/* addq $adjust, %rdi - the receiver, which is parameter 0 of this
	 * convention. A hidden return pointer never takes that place: it goes at
	 * index 1 whenever the prototype has more than one parameter, and an
	 * unboxing method always has the receiver as well. */
	static const uint8_t add[3] = { 0x48, 0x83, 0xc7 };

	static_assert (sizeof (add) + 1 == unbox_prologue_size,
	               "the unbox prologue is not the size the layout reserves");

	assert (adjust <= 0x7f && "the receiver adjustment does not fit an imm8");

	uint8_t imm = static_cast<uint8_t> (adjust);

	std::memcpy (code, add, sizeof (add));
	std::memcpy (code + sizeof (add), &imm, sizeof (imm));
}

} // namespace mono::arch
