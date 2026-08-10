/**
 * \file
 * \brief The instructions a call-counting thunk is made of.
 */

#include "arch/arch.hpp"

#include <llvm/Support/ErrorHandling.h>

#include <cassert>
#include <cstring>
#include <initializer_list>

namespace mono::arch {

namespace {

/* Where the head keeps what the code reads. */
constexpr size_t target_offset = 8;
constexpr size_t promote_offset = 16;
constexpr size_t code_offset = 32;

static_assert (code_offset + counter_thunk_code_size == counter_thunk_size,
               "a counter thunk does not fill its block");

/// Emits into a block, tracking how far it has got so that a rip-relative
/// displacement can be worked out from the address of the next instruction.
class Emitter {
public:
	explicit Emitter (char *code) : code_ (code) {}

	void bytes (std::initializer_list<uint8_t> opcodes)
	{
		for (uint8_t byte : opcodes)
			code_[at_++] = (char) byte;
	}

	void imm32 (uint32_t value)
	{
		std::memcpy (code_ + at_, &value, sizeof (value));
		at_ += sizeof (value);
	}

	/// The displacement reaching ADDRESS from the end of an instruction that
	/// ends WIDTH bytes from here - which is where rip points while it runs.
	void rip32 (const void *address, size_t width)
	{
		const char *rip = code_ + at_ + width;
		int64_t disp = (const char *) address - rip;

		/*
		 * Everything a thunk reads is carved from the same slab as its own
		 * code, and a slab is bounded so that any two bytes in one are within
		 * this reach of each other. Worth a loud death rather than an assert:
		 * a build with -DNDEBUG would otherwise write a jump to nowhere.
		 */
		if (disp < INT32_MIN || disp > INT32_MAX)
			llvm::report_fatal_error (
				"a counter thunk cannot reach what it reads", false);
		imm32 ((uint32_t) (int32_t) disp);
	}

	size_t at () const { return at_; }

private:
	char *code_;
	size_t at_ = 0;
};

} // namespace

void *
write_counter_thunk (char *block, uint32_t *counter, uint32_t threshold,
                     const void *target, const void *promote)
{
	/* A threshold of zero has no call to fire on: the counter is only ever
	 * read after it has been incremented at least once. */
	if (threshold < 1)
		llvm::report_fatal_error ("a counter thunk needs a threshold of at "
		                          "least one",
		                          false);

	std::memcpy (block + target_offset, &target, sizeof (target));
	std::memcpy (block + promote_offset, &promote, sizeof (promote));

	char *code = block + code_offset;
	Emitter e (code);

	/*
	 * cmpl $threshold, counter(%rip)
	 * ja   carry_on
	 *
	 * The guard, and the reason the counter cannot overflow: once it is past
	 * the threshold nothing below ever runs again. Threads that pass the guard
	 * together do all increment, so the counter can end up a few over - as many
	 * as there are threads in here at once - and that is fine, because which
	 * call fires is settled by the exchange below rather than by this compare.
	 */
	e.bytes ({ 0x81, 0x3d });
	e.rip32 (counter, sizeof (uint32_t) + sizeof (uint32_t));
	e.imm32 (threshold);
	e.bytes ({ 0x77, 0x1e });

	/*
	 * movl       $1, %r11d
	 * lock xaddl %r11d, counter(%rip)
	 *
	 * %r11 is the one register free here: it is call-clobbered and carries no
	 * argument, and %r10 has to reach whatever this jumps on to untouched - the
	 * stub in front loaded the method's key into it. So the counting costs the
	 * thunk's callers a compare and a branch and disturbs nothing else - no
	 * stack, no saves - which is also why a stack walk that catches a thread in
	 * here can use the same unwind program as any other stub.
	 *
	 * Relaxed is all this needs. The counter synchronizes nothing; what
	 * publishes a promoted body is the release store to the stub's slot.
	 */
	e.bytes ({ 0x41, 0xbb });
	e.imm32 (1);
	e.bytes ({ 0xf0, 0x44, 0x0f, 0xc1, 0x1d });
	e.rip32 (counter, sizeof (uint32_t));

	/*
	 * cmpl $threshold - 1, %r11d
	 * jne  carry_on
	 *
	 * xadd hands back the value the counter held before this call, so a call
	 * that finds THRESHOLD - 1 there is the THRESHOLD'th: **the trigger fires
	 * on call N, not on call N + 1**. Exactly one call can see that value,
	 * which is what makes == right and >= wrong - the latter would fire on
	 * every call from the Nth until the guard shut it off.
	 */
	e.bytes ({ 0x41, 0x81, 0xfb });
	e.imm32 (threshold - 1);
	e.bytes ({ 0x75, 0x06 });

	/*
	 * jmpq *promote(%rip)
	 *
	 * Reached once in the life of the method. Whatever is behind it may decline
	 * - a full queue, a domain being torn down - and nothing here retries: the
	 * method then stays at the tier it is at for good, which is the direction
	 * that cannot go wrong.
	 */
	e.bytes ({ 0xff, 0x25 });
	e.rip32 (block + promote_offset, sizeof (uint32_t));

	/* carry_on: jmpq *target(%rip) */
	e.bytes ({ 0xff, 0x25 });
	e.rip32 (block + target_offset, sizeof (uint32_t));

	assert (e.at () == counter_thunk_code_size);
	return code;
}

} // namespace mono::arch
