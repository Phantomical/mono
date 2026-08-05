/**
 * \file
 * \brief The one instruction a redirectable stub is made of.
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

} // namespace mono::arch
