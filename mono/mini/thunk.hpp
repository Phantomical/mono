/**
 * \file
 * \brief Redirectable call thunks - the only address the JIT ever publishes.
 *
 * Every method is published as a thunk that jumps through a writable slot, so
 * a later tier can be swapped in by writing the slot: callers keep their
 * direct call to the thunk and pick up the new code on their next call. That
 * is the mechanism promotion is built on, and it is also what makes runtime
 * detours (Harmony/MonoMod) work, which is where the unusual thunk geometry
 * comes from.
 */

#ifndef MONO_MINI_THUNK_HPP
#define MONO_MINI_THUNK_HPP

#include "mono/metadata/object-forward.h"
#include "mono/utils/mono-forward.h"
#include "mono/llvm/jitlink-memory.hpp"

#include <atomic>
#include <cstddef>
#include <string_view>

#include <llvm/Support/Error.h>

namespace mono {

/// An individual redirectable thunk.
class Thunk {
private:
	void *data_ = nullptr;

	friend llvm::Expected<Thunk> allocate_thunk (CodeArena *arena, void *key);

	explicit Thunk (void *data);

public:
	Thunk () = default;

	/// Allocate a new thunk pointing to a fatal error function.
	///
	/// Returns an error if allocation could not be performed.
	static llvm::Expected<Thunk> allocate (CodeArena *arena, void *key = nullptr);

	/// Get the function pointer for this thunk.
	void *code () const;

	/// Get a pointer to the unbox shim for this thunk.
	///
	/// This gets automatically added to all thunks, but is only valid to use
	/// for value types.
	void *unbox () const;

	/// Redirect this thunk to point to a new target.
	void redirect (void *target);

	/// Redirect this thunk to point to a fatal trap.
	void quarantine ();

	/// Create and register the appropriate MonoJitInfo for this thunk.
	MonoJitInfo *register_jinfo (std::string_view name, MonoDomain *domain, MonoMethod *method);

	explicit operator bool () const { return data_ != nullptr; }
};

} // namespace mono

#endif
