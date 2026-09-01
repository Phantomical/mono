/**
 * \file
 * \brief Carving redirectable thunks directly out of a domain's CodeArena.
 *
 * A thunk costs arch::thunk_size bytes, reserved from the domain's
 * CodeArena. A redirect is a single atomic store to the slot. There is no
 * pool of retired thunks to reuse.
 */

#include "thunk.hpp"

#include <atomic>
#include <mono/llvm/jitlink-memory.hpp>

#include <mono/arch/amd64/amd64-thunk.hpp>
#include "mini.h"
#include "mono/llvm/debugging/perf/jitdump.hpp"
#include "mono/llvm/runtime/naming.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/object.h"

#include <llvm/Support/Memory.h>

#include <mutex>

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

/// How many thunks share one CodeArena reservation - and its perf-dump
/// record.
///
/// Each reservation holds code_slack () past its last thunk, so its record
/// is never trimmed. Paying that once per group instead of once per thunk is
/// what a bigger group amortizes further.
constexpr size_t thunk_batch_size = 32;

/// Publishes pool's whole reservation as one perf-dump record, covering
/// thunks not yet allocated along with the ones already there.
///
/// Every thunk in a pool runs the same stub_unwind_info () program, so one
/// FrameFunction spanning the reservation describes them all. That is why
/// this writes once, not once per thunk.
///
/// perf never reads a record's code bytes to unwind. It reads the address
/// range and the eh_frame this call hands it, both already known at the
/// reservation. Describing the range this early keeps every record here
/// disjoint - one write, one range, no later record for the same bytes.
/// That is the partition check_jitdump.py holds every jitdump record to.
void
publish_thunk_pool (const ThunkPool &pool)
{
	size_t extent = thunk_batch_size * arch::thunk_size;
	size_t room = extent + perf::code_slack ();

	perf::publish ("redirect thunks", {(const uint8_t *) pool.base, extent, room},
	              {perf::FrameFunction{0, extent, {}}});
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
                    MonoMethod *method, bool perf_dump_deferred)
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
	tramp->perf_dump_deferred = perf_dump_deferred;
	mono_tramp_info_register (tramp, domain);
	return nullptr;
}

MonoJitInfo *
Thunk::register_jinfo (std::string_view name, MonoDomain *domain, MonoMethod *method)
{
	// The pool this thunk lands in already published a perf-dump record for
	// the whole group - see publish_thunk_pool ().
	return register_code_stub (unbox (), arch::thunk_size - arch::thunk_unbox_offset,
	                           name, domain, method, perf::enabled ());
}

llvm::Expected<Thunk>
Thunk::allocate (CodeArena *arena, ThunkPool &pool, void *key)
{
	if (!perf::enabled ()) {
		llvm::Expected<char *> data = arena->reserve (arch::thunk_size, arch::thunk_alignment);
		if (!data)
			return data.takeError ();

		arch::write_thunk (*data, key);
		Thunk thunk (*data);

		llvm::sys::Memory::InvalidateInstructionCache (*data, arch::thunk_size);

		return thunk;
	}

	std::lock_guard<std::mutex> lock (pool.mutex);

	if (pool.base == nullptr) {
		llvm::Expected<char *> reservation = arena->reserve (
			thunk_batch_size * arch::thunk_size + perf::code_slack (),
			arch::thunk_alignment);
		if (!reservation)
			return reservation.takeError ();
		pool.base = *reservation;
		pool.filled = 0;
		publish_thunk_pool (pool);
	}

	char *data = pool.base + pool.filled * arch::thunk_size;
	arch::write_thunk (data, key);
	Thunk thunk (data);

	llvm::sys::Memory::InvalidateInstructionCache (data, arch::thunk_size);

	++pool.filled;
	if (pool.filled == thunk_batch_size) {
		pool.base = nullptr;
		pool.filled = 0;
	}

	return thunk;
}

} // namespace mono
