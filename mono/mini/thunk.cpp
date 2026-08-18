/**
 * \file
 * \brief Carving redirectable thunks directly out of a domain's CodeArena.
 *
 * A thunk costs thunk_group_size bytes, and publishing one is a single
 * CodeArena::reserve () plus a few stores; a redirect is a single atomic store
 * to the slot. There is no pool of retired thunks to reuse - see
 * allocate_thunk ()'s doc comment for what that costs.
 */

#include "thunk.hpp"

#include <mono/llvm/jitlink-memory.hpp>

#include <mono/arch/amd64/amd64-thunk.hpp>
#include "mono/metadata/abi-details.h"
#include "mono/metadata/object.h"

#include <llvm/Support/Memory.h>

using namespace llvm;

namespace mono {

namespace {

/*
 * The whole group - slot, padding, unbox prologue, jump block - is exactly
 * what amd64-thunk.hpp bakes as constant bytes. There is one description of
 * this layout, the baked one, rather than a second computed here and
 * reconciled against it.
 */
constexpr size_t group_size = arch::thunk_size;
constexpr size_t entry_offset = arch::thunk_entry_offset;
constexpr size_t unbox_offset = arch::thunk_unbox_offset;
constexpr size_t unbox_prologue_size = entry_offset - unbox_offset;

/* MonoObject's layout is what has to hold still for the baked unbox prologue
 * - a fixed `add $0x10, %rdi` - to keep stepping past exactly the header. */
static_assert (MONO_ABI_SIZEOF (MonoObject) == 0x10,
               "the baked unbox prologue assumes a different MonoObject size");

void
thunk_not_initialized ()
{
	llvm::report_fatal_error ("called a thunk that has not been initialized with a target");
}

} // namespace

const size_t thunk_group_size = group_size;
const size_t thunk_unbox_span = group_size - unbox_offset;

void *
Thunk::unbox_entry () const
{
	return static_cast<char *> (code_) - unbox_prologue_size;
}

void
Thunk::quarantine ()
{
	if (!*this)
		return;

	redirect ((void *) thunk_not_initialized);
}

llvm::Expected<Thunk>
allocate_thunk (CodeArena *arena, void *key)
{
	/*
	 * Unlike a compiled object (jitlink-memory.cpp), a thunk never gets a perf
	 * jitdump record of its own - there is nothing publishing one for it to
	 * name - so there is no reason to reserve perf::code_slack ()'s trailing
	 * room here the way every other code-arena allocation does.
	 */
	Expected<char *> group = arena->reserve (group_size, arch::thunk_alignment);

	if (!group)
		return group.takeError ();

	arch::write_thunk (*group, key);

	Thunk thunk (*group + entry_offset, reinterpret_cast<std::atomic<void *> *> (*group));
	thunk.quarantine ();

	sys::Memory::InvalidateInstructionCache (*group + unbox_offset, thunk_unbox_span);

	return thunk;
}

} // namespace mono
