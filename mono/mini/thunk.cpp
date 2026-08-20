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

#include <atomic>
#include <mono/llvm/jitlink-memory.hpp>

#include <mono/arch/amd64/amd64-thunk.hpp>
#include "mini.h"
#include "mono/llvm/runtime/naming.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/object.h"

#include <llvm/Support/Memory.h>

namespace mono {

namespace {

// The arch thunk hardcodes the size of MonoObject. We validate that here.
static_assert (MONO_ABI_SIZEOF (MonoObject) == 0x10,
               "the baked unbox prologue assumes a different MonoObject size");

void
thunk_not_initialized ()
{
	llvm::report_fatal_error ("called a thunk that has not been initialized with a target");
}

/// The encoded unwind program every stub runs under.
///
/// A stub is a bare jump: it has pushed nothing, so at any instruction in it the
/// frame is still exactly the one the call left behind - which is what the arch's
/// CIE describes and nothing more. Without this a walk that catches a thread
/// mid-jump cannot step off the stub into its caller at all.
llvm::ArrayRef<uint8_t>
stub_unwind_info ()
{
	static const std::vector<uint8_t> encoded = [] {
		GSList *ops = mono_arch_get_cie_program ();
		guint32 len = 0;
		guint8 *bytes = mono_unwind_ops_encode (ops, &len);
		std::vector<uint8_t> program (bytes, bytes + len);

		g_free (bytes);
		mono_free_unwind_info (ops);
		return program;
	}();

	return encoded;
}

} // namespace

Thunk::Thunk (void *data) : data_ (data) {}

void *
Thunk::code () const
{
	return static_cast<char *> (data_) + arch::thunk_entry_offset;
}

void *
Thunk::unbox () const
{
	return static_cast<char *> (data_) + arch::thunk_unbox_offset;
}

void
Thunk::redirect (void *target)
{
	auto slot = static_cast<std::atomic<void *> *> (data_);
	slot->store (target, std::memory_order_release);
}

void
Thunk::quarantine ()
{
	if (!*this)
		return;

	redirect ((void *) thunk_not_initialized);
}

MonoJitInfo *
register_code_stub (void *code, size_t size, std::string_view name, MonoDomain *domain,
                    MonoMethod *method)
{
	auto unwind = stub_unwind_info ();
	guint8 *uw_info = const_cast<guint8 *> (unwind.data ());
	guint32 uw_info_len = (guint32) unwind.size ();
	std::string display = display_name (method, name);

	if (method->dynamic)
		return mono_tramp_info_register_reclaimable (domain, method, code, size,
		                                             display.c_str (), uw_info,
		                                             uw_info_len);

	MonoTrampInfo *tramp = g_new0 (MonoTrampInfo, 1);
	tramp->code = (guint8 *) code;
	tramp->code_size = (guint32) size;
	tramp->name = g_strdup (display.c_str ());
	tramp->method = method;
	tramp->uw_info = uw_info;
	tramp->uw_info_len = uw_info_len;
	mono_tramp_info_register (tramp, domain);
	return nullptr;
}

MonoJitInfo *
Thunk::register_jinfo (std::string_view name, MonoDomain *domain, MonoMethod *method)
{
	return register_code_stub (unbox (), arch::thunk_size - arch::thunk_unbox_offset,
	                           name, domain, method);
}

llvm::Expected<Thunk>
Thunk::allocate (CodeArena *arena, void *key)
{
	llvm::Expected<char *> data = arena->reserve (arch::thunk_size, arch::thunk_alignment);
	if (!data)
		return data.takeError ();

	arch::write_thunk (*data, key);
	Thunk thunk (*data);

	llvm::sys::Memory::InvalidateInstructionCache (*data, arch::thunk_size);

	return thunk;
}

} // namespace mono
