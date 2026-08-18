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

#include <llvm/Support/Error.h>

#include <atomic>
#include <cstddef>

#include <mono/llvm/jitlink-memory.hpp>

namespace mono {

/// What one thunk costs in code memory: the slot it jumps through, the unbox
/// prologue in front of it and the block itself.
extern const size_t thunk_group_size;

/// Bytes from a thunk's unbox entry through the end of its jump block - what
/// a jit-info record covering both has to span.
extern const size_t thunk_unbox_span;

/// An individual redirectable thunk.
class Thunk {
private:
	void *code_ = nullptr;
	std::atomic<void *> *slot_ = nullptr;

	friend llvm::Expected<Thunk> allocate_thunk (CodeArena *arena, void *key);

	Thunk (void *code, std::atomic<void *> *slot) : code_ (code), slot_ (slot) {}

public:
	Thunk () = default;

	/// Get the pointer that this thunk can be called with.
	void *code () const { return code_; }

	/// Where a call off a value type's vtable or IMT arrives: it steps the
	/// receiver past the object header, then enters the method.
	///
	/// Every thunk has one. Only a method publishes_unbox_entry () accepts can
	/// be entered here - any other receiver is stepped past bytes that are not
	/// there.
	void *unbox_entry () const;

	/// Redirect this thunk to point to a new target.
	void redirect (void *target) { slot_->store (target, std::memory_order_release); }

	/// Redirects to a permanent fatal trap, for a thunk that nothing may call
	/// through any more. Its bytes stay live until the arena they came from
	/// goes, the same as everything else in a CodeArena, so this is what makes
	/// a stray call into retired code fail loudly instead of running stale code.
	///
	/// A no-op on a default-constructed thunk.
	void quarantine ();

	explicit operator bool () const { return code_ != nullptr; }
};

/// Carves a fresh thunk out of arena, handing key to whatever it jumps to
/// when key is not null. Starts out quarantined; the caller redirects it to
/// something real.
///
/// Every call carves fresh code-arena bytes: there is no pool of retired
/// thunks to hand back out. A program that keeps minting dynamic methods -
/// Reflection.Emit, an expression-tree compiler, a Harmony-style patcher -
/// never gets a freed one's thunk memory back: each new method costs a fresh
/// carve, so a domain's low-2GB code pool only grows. Worth reconsidering if
/// that shows up as a real problem; nothing about this API forecloses adding
/// reuse back.
llvm::Expected<Thunk> allocate_thunk (CodeArena *arena, void *key);

} // namespace mono

#endif
