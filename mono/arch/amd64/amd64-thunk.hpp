/**
 * \file
 * \brief The instructions that make up a redirectable thunk.
 */

#ifndef MONO_ARCH_AMD64_THUNK_HPP
#define MONO_ARCH_AMD64_THUNK_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mono::arch {

// clang-format off
static constexpr std::uint8_t thunk_code[32] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // +0  slot: null until Stub::redirect () sets it
	0xCC, 0xCC, 0xCC, 0xCC,                         // +8  int3 padding

	// +12 unbox: add rdi, sizeof(MonoObject)
	0x48, 0x83, 0xc7, 0x10,

	// +16 entry: jmp qword ptr [rip-0x16]  (-> +0, the slot)
	0xFF, 0x25, 0xEA, 0xFF, 0xFF, 0xFF,

	0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // int3 padding
};

static constexpr std::uint8_t thunk_keyed_code[32] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // +0  slot: null until Stub::redirect () sets it
	0xCC, 0xCC, 0xCC, 0xCC,                         // +8  int3 padding

	// +12 unbox: add rdi, sizeof(MonoObject)
	0x48, 0x83, 0xc7, 0x10,

	// +16 entry: movabs r10, <key>  (MONO_ARCH_IMT_REG, the `nest` register)
	0x49, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

	// +26 jmp qword ptr [rip-0x20]  (-> +0, the slot)
	0xFF, 0x25, 0xE0, 0xFF, 0xFF, 0xFF,
};
// clang-format on

static constexpr size_t thunk_unbox_offset = 12;
static constexpr size_t thunk_entry_offset = 16;
static constexpr size_t thunk_key_offset = 18;
static constexpr size_t thunk_size = 32;

/// Write a thunk at group, thunk_size bytes: the slot, the unbox prologue
/// and the block. key, when given, is patched into the block's key
/// register.
inline void
write_thunk (char *group, const void *key = nullptr)
{
	if (key != nullptr) {
		std::memcpy (group, thunk_keyed_code, thunk_size);
		std::memcpy (group + thunk_key_offset, &key, sizeof (key));
	} else {
		std::memcpy (group, thunk_code, thunk_size);
	}
}

} // namespace mono::arch

#endif // MONO_ARCH_AMD64_THUNK_HPP
