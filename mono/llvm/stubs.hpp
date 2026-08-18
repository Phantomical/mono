/**
 * \file
 * \brief Redirectable call stubs - the only address the JIT ever publishes.
 *
 * Every method is published as a stub that jumps through a writable slot, so a
 * later tier can be swapped in by writing the slot: callers keep their direct
 * call to the stub and pick up the new code on their next call. That is the
 * mechanism promotion is built on, and it is also what makes runtime detours
 * (Harmony/MonoMod) work, which is where the unusual stub geometry comes from.
 */

#ifndef MONO_LLVM_STUBS_HPP
#define MONO_LLVM_STUBS_HPP

#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jitlink-memory.hpp"

namespace mono {

class StubExistsError : public llvm::ErrorInfo<StubExistsError> {
public:
	static char ID;

	StubExistsError (llvm::StringRef name);

	void log (llvm::raw_ostream &OS) const override;
	std::error_code convertToErrorCode () const override;

private:
	std::string name;
};

/// What one stub costs in code memory: the slot it jumps through, the unbox
/// prologue in front of it and the block itself.
extern const size_t stub_group_size;

/// An individual redirectable stub.
class Stub {
private:
	void *code_ = nullptr;
	std::atomic<void *> *slot_ = nullptr;

	friend class StubSlabs;

	Stub (void *code, std::atomic<void *> *slot) : code_ (code), slot_ (slot) {}

public:
	Stub () = default;

	/// Get the pointer that this stub can be called with.
	void *code () const { return code_; }

	/// Where a call off a value type's vtable or IMT arrives: it steps the
	/// receiver past the object header, then enters the method.
	///
	/// Every stub has one. Only a method publishes_unbox_entry () accepts can be
	/// entered here - any other receiver is stepped past bytes that are not
	/// there.
	void *unbox_entry () const;

	/// Redirect this stub to point to a new target.
	void redirect (void *target) { slot_->store (target, std::memory_order_release); }

	explicit operator bool () const { return code_ != nullptr; }
};

/// The blocks stubs are carved out of, and the free list they go back on.
///
/// Not thread safe - StubTable is what serialises access to one of these.
class StubSlabs {
public:
	explicit StubSlabs (CodeArena *arena);

	StubSlabs (const StubSlabs &) = delete;
	StubSlabs &operator= (const StubSlabs &) = delete;

	/// Carve a stub that jumps through its own slot, handing KEY to whatever it
	/// jumps to when KEY is not null. The stub starts out pointing at a fatal
	/// error; the caller redirects it to something real.
	llvm::Expected<Stub> allocate (void *key);

	void release (Stub stub);

private:
	llvm::Expected<Stub> acquire ();
	llvm::Error add_slab ();

	CodeArena *arena_;
	/// The batch stubs come out of. Earlier batches are full, and nothing needs
	/// to name them again, because code memory is never given back.
	char *batch_ = nullptr;
	std::vector<Stub> free_;
	size_t next_;
	/// How far apart two groups sit, which is a group and whatever a perf dump
	/// needs behind it.
	size_t stride_;
};

/// The stub table.
///
/// This tracks which stubs have been created by name.
class StubTable {
public:
	explicit StubTable (CodeArena *arena) : slabs_ (arena) {}

	/// Find a pre-existing stub, if one exists with the requested name.
	std::optional<Stub> find (llvm::StringRef name);

	/// Create a stub for the requested name, failing with StubExistsError if one
	/// already exists.
	llvm::Expected<Stub> create (llvm::StringRef name);

	/// Create a stub that hands the requested key to whatever it jumps to.
	/// Fails the same way if the name already has one.
	llvm::Expected<Stub> create (llvm::StringRef name, void *key);

	/// Remove a single named stub.
	bool remove (llvm::StringRef name);

	/// Remove the all the named stubs.
	template<typename C>
	void remove_all (const C &names)
	{
		std::lock_guard<std::mutex> lock (mutex_);

		for (llvm::StringRef name : names)
			remove_locked (name, lock);
	}

private:
	bool remove_locked (llvm::StringRef name, std::lock_guard<std::mutex> &guard);

	std::mutex mutex_;
	StubSlabs slabs_;
	llvm::StringMap<Stub> stubs_;
};

} // namespace mono

#endif
