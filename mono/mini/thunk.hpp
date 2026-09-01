/**
 * \file
 * \brief Redirectable call thunks - the indirection every method is
 * published through.
 *
 * Every method is published as a thunk that jumps through a writable slot. A
 * later tier is swapped in with one store to that slot. Callers keep their
 * direct call to the thunk, and pick up the new code on their next call.
 * Promotion is built on this redirect, and so is a runtime detour (Harmony,
 * MonoMod) - which is why the thunk carries its unusual geometry.
 */

#ifndef MONO_MINI_THUNK_HPP
#define MONO_MINI_THUNK_HPP

#include "mono/metadata/object-forward.h"
#include "mono/utils/mono-forward.h"
#include "mono/llvm/jitlink-memory.hpp"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string_view>

#include <llvm/Support/Error.h>

namespace mono {

/// A CodeArena reservation shared by a run of thunks, and the state of
/// filling it - see Thunk::allocate ().
///
/// Owned by whoever owns the CodeArena it draws from, so the two share a
/// lifetime.
struct ThunkPool {
	std::mutex mutex;
	char *base = nullptr;
	size_t filled = 0;
};

/// A redirectable thunk.
class Thunk {
private:
	void *data_ = nullptr;

	friend llvm::Expected<Thunk> allocate_thunk (CodeArena *arena, void *key);

	explicit Thunk (void *data);

public:
	Thunk () = default;

	/// Allocates a new thunk with no target set.
	///
	/// The slot starts null, so calling through it before the first
	/// redirect () faults. pool must be arena's own - see ThunkPool.
	///
	/// Returns an error if allocation fails.
	static llvm::Expected<Thunk> allocate (CodeArena *arena, ThunkPool &pool, void *key = nullptr);

	/// Returns the function pointer for this thunk.
	void *code () const;

	/// Returns a pointer to this thunk's unbox shim.
	///
	/// Every thunk carries one, but only a value type's instance methods
	/// can enter through it.
	void *unbox () const;

	/// Redirects this thunk to a new target.
	void redirect (void *target);

	/// Redirects this thunk to a fatal trap.
	///
	/// A no-op on a default-constructed thunk.
	void quarantine ();

	/// Registers this thunk's MonoJitInfo record.
	///
	/// Returns the record for a dynamic method's stub, and null otherwise. A
	/// returned record goes to mono_jit_info_table_remove () before the code
	/// is freed.
	MonoJitInfo *register_jinfo (std::string_view name, MonoDomain *domain, MonoMethod *method);

	explicit operator bool () const { return data_ != nullptr; }
};

/// Registers a bare jump stub with the runtime, so a stack walk can cross it.
///
/// Returns the record for a dynamic method's stub, and null otherwise. A
/// returned record goes to mono_jit_info_table_remove () before the code is
/// freed, or a walk can still resolve against a stub that has gone.
///
/// perf_dump_deferred skips this stub's own perf-dump record, for a caller
/// that publishes it batched with others instead.
MonoJitInfo *register_code_stub (void *code, size_t size, std::string_view name,
                                 MonoDomain *domain, MonoMethod *method,
                                 bool perf_dump_deferred = false);

} // namespace mono

#endif
