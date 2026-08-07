/**
 * \file
 * \brief The gdb JIT interface: `__jit_debug_descriptor` and what goes on it.
 */

#include "gdb-jit.hpp"

#include <llvm/BinaryFormat/ELF.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h>
#include <llvm/Object/ELF.h>

#include <cstdlib>
#include <mutex>

using namespace llvm;

/*
 * The rendezvous a debugger watches. Both come out of LLVM's own JIT loader,
 * which is already linked here, so everything in this process that speaks the
 * protocol shares one list - which is the point of the descriptor being
 * process-global.
 */
extern "C" {
extern struct jit_descriptor __jit_debug_descriptor;
void __jit_debug_register_code ();
}

namespace mono {
namespace gdbjit {

namespace {

/// Serializes both the list and the rendezvous, so two domains compiling at
/// once cannot interleave halfway through handing the debugger an entry.
std::mutex &
descriptor_lock ()
{
	static std::mutex lock;
	return lock;
}

using ELFT = object::ELF64LE;

} // namespace

/// A list entry and the object it names, allocated together so the address the
/// debugger holds stays put for as long as the bytes behind it do.
struct Registration {
	jit_code_entry entry {};
	std::vector<char> object;
};

bool
enabled ()
{
	static bool on = [] {
		const char *setting = std::getenv ("MONO_LLVM_JIT_GDB");

		return setting != nullptr && StringRef (setting) != "0";
	}();

	return on;
}

std::vector<char>
debug_object (std::vector<char> bytes,
              function_ref<uint64_t (StringRef)> section_address)
{
	auto [elf_class, endianness] =
		object::getElfArchType (StringRef (bytes.data (), bytes.size ()));

	if (elf_class != ELF::ELFCLASS64 || endianness != ELF::ELFDATA2LSB)
		return {};

	Expected<object::ELFFile<ELFT>> elf =
		object::ELFFile<ELFT>::create (StringRef (bytes.data (), bytes.size ()));

	if (!elf) {
		consumeError (elf.takeError ());
		return {};
	}

	Expected<ArrayRef<ELFT::Shdr>> sections = elf->sections ();
	if (!sections) {
		consumeError (sections.takeError ());
		return {};
	}

	/*
	 * Filling in the section addresses is the whole of what turns a copy of
	 * the object into a description of running code. Everything in it that
	 * names an address - the symbol values, the frame descriptions in
	 * `.eh_frame` - is section-relative and left for a linker to resolve, and
	 * the reader resolves it against exactly these.
	 */
	for (const ELFT::Shdr &section : *sections) {
		Expected<StringRef> name = elf->getSectionName (section);

		if (!name) {
			consumeError (name.takeError ());
			continue;
		}
		if (uint64_t addr = section_address (*name))
			const_cast<ELFT::Shdr &> (section).sh_addr = addr;
	}

	return bytes;
}

Registration *
publish (std::vector<char> object)
{
	if (object.empty ())
		return nullptr;

	Registration *reg = new Registration { {}, std::move (object) };

	reg->entry.symfile_addr = reg->object.data ();
	reg->entry.symfile_size = reg->object.size ();

	std::lock_guard<std::mutex> lock (descriptor_lock ());

	reg->entry.prev_entry = nullptr;
	reg->entry.next_entry = __jit_debug_descriptor.first_entry;
	if (reg->entry.next_entry != nullptr)
		reg->entry.next_entry->prev_entry = &reg->entry;

	__jit_debug_descriptor.first_entry = &reg->entry;
	__jit_debug_descriptor.relevant_entry = &reg->entry;
	__jit_debug_descriptor.action_flag = JIT_REGISTER_FN;
	__jit_debug_register_code ();
	return reg;
}

void
retract (Registration *reg)
{
	if (reg == nullptr)
		return;

	{
		std::lock_guard<std::mutex> lock (descriptor_lock ());

		if (reg->entry.prev_entry != nullptr)
			reg->entry.prev_entry->next_entry = reg->entry.next_entry;
		else
			__jit_debug_descriptor.first_entry = reg->entry.next_entry;
		if (reg->entry.next_entry != nullptr)
			reg->entry.next_entry->prev_entry = reg->entry.prev_entry;

		__jit_debug_descriptor.relevant_entry = &reg->entry;
		__jit_debug_descriptor.action_flag = JIT_UNREGISTER_FN;
		__jit_debug_register_code ();
	}

	delete reg;
}

} // namespace gdbjit
} // namespace mono
