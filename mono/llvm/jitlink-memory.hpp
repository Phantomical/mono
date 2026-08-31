#ifndef MONO_LLVM_JITLINK_MEMORY_HPP
#define MONO_LLVM_JITLINK_MEMORY_HPP

#include <llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/RecyclingAllocator.h>

#include <cstddef>
#include <mutex>
#include <vector>

typedef struct _MonoCodeManager MonoCodeManager;

namespace mono {

/// An arena allocator for executable memory.
///
/// Individual allocations are not freed until the whole arena is.
class CodeArena {
public:
	CodeArena ();

	/// Frees every reservation at once. Callers must guarantee the code is
	/// dead - nothing executing in it, nothing about to call into it.
	~CodeArena ();

	CodeArena (const CodeArena &) = delete;
	CodeArena &operator= (const CodeArena &) = delete;

	/// Reserves size bytes at align.
	///
	/// An align above 16 costs padding, because 16 is all the code manager
	/// promises. When code memory runs out - on amd64, when the low 2GB is
	/// full - this fails instead of handing back an address out of reach.
	llvm::Expected<char *> reserve (size_t size, size_t align);

	/// Gives back a reservation whose content was never completed.
	///
	/// Best effort, and silent when it does nothing. The bytes come back only
	/// while they are still the last thing reserved out of their chunk. Any
	/// other reservation in between, or padding an over-aligned request
	/// added, leaves them where they are.
	void unreserve (char *base, size_t size);

	/// Hands the OS the records of a linked object's function table, so an
	/// unwinder outside this runtime can cross the frames they describe.
	///
	/// \p table is the object's own table and \p size its length in bytes.
	/// It must be writable, and it and the code it describes must be
	/// reservations out of this arena.
	///
	/// Silent where the host publishes no such table. What that costs is
	/// unwinding through this code from outside the runtime, and mono's own
	/// unwinder reads `.mono_unwind` either way.
	void publish_function_table (void *table, size_t size);

private:
	std::mutex mutex_;
	MonoCodeManager *code_;
};

/// The JITLink memory manager that links objects into a CodeArena.
///
/// Stock JITLink memory managers round every segment of every object up to a
/// page (BasicLayout::getContiguousPageBasedLayoutSizes). For an object the size
/// of one method, that wastes most of what it maps. This one packs an object's
/// segments back to back into a single reservation, so a method costs its
/// content rather than a page per segment.
///
/// Protection does not enter into the packing. Code memory is read-write-execute
/// for its whole life, so a method's read-only data, its mutable globals and its
/// code all sit in one block.
class CodeMemoryManager final : public llvm::jitlink::JITLinkMemoryManager {
public:
	explicit CodeMemoryManager (CodeArena *arena) : arena_ (arena) {}

	void allocate (const llvm::jitlink::JITLinkDylib *jd,
	               llvm::jitlink::LinkGraph &g,
	               OnAllocatedFunction on_allocated) override;

	/// Runs the object's dealloc actions. The memory itself stays where it is
	/// until the arena goes.
	void deallocate (std::vector<FinalizedAlloc> allocs,
	                 OnDeallocatedFunction on_deallocated) override;

private:
	class InFlight;

	struct FinalizedInfo {
		char *base;
		size_t size;
		std::vector<llvm::orc::shared::WrapperFunctionCall> dealloc_actions;
	};

	FinalizedAlloc record (char *base, size_t size,
	                       std::vector<llvm::orc::shared::WrapperFunctionCall> actions);

	std::mutex mutex_;
	llvm::RecyclingAllocator<llvm::BumpPtrAllocator, FinalizedInfo> infos_;
	CodeArena *arena_;
};

} // namespace mono

#endif /* MONO_LLVM_JITLINK_MEMORY_HPP */
