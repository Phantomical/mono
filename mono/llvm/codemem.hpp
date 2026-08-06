/**
 * \file
 * \brief Slab allocation for JIT'd code.
 */

#ifndef MONO_LLVM_CODEMEM_HPP
#define MONO_LLVM_CODEMEM_HPP

#include <llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h>
#include <llvm/Support/Error.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace mono {

/// Code memory bump-allocated out of large reservations.
///
/// A slab is one PROT_NONE reservation carrying two regions. Code and the
/// read-only data beside it bump up from the bottom and seal to read-execute as
/// they are written; everything that stays writable for its whole life - a
/// method's mutable globals, mono's published stubs - bumps down from the top
/// and is read-write-execute from the moment it is committed. Nothing is
/// committed until it is handed out, so a reservation costs address space and
/// nothing else, and a method costs its own bytes rather than a page per
/// segment.
///
/// Code pages have to be writable while an allocation is still being written
/// into them, so allocate () must be paired with finish () or abandon ().
/// Writable and executable at once is unavoidable rather than lazy: JITLink
/// copies an object's content into its final address, and under bump allocation
/// that page very likely already holds a finished method somebody is running.
///
/// **An object never spans two slabs**, and a slab is never larger than 2GB
/// with its top page held back from both regions, so any two bytes a slab hands
/// out are inside PCRel32 range of each other. That matters because JITLink
/// stubs a call it cannot reach but has no such fallback for a method reaching
/// its own data - PCRel32 to .rodata or to a mutable global, Delta32 from
/// .eh_frame back to .text - all of which hard-error past +-2GB. 2GB is exactly
/// that reach rather than comfortably inside it, so the guard page is what keeps
/// a fixup's own width and a negative addend from spilling past the edge. The
/// bound is what lets an object's code sit at the bottom of a slab and its
/// mutable data at the top.
///
/// A related hazard worth knowing about, though nothing reaches it today:
/// mini's MONO_PATCH_INFO_METHOD_JUMP patching (mini_patch_jump_sites) asserts
/// rather than thunks when a jump does not reach, and jump sites are only
/// registered from mono_codegen, which the mainline JIT no longer enters.
class CodeSlabs {
public:
	/// A range handed out by one of the allocate calls. SLAB indexes the
	/// reservation it came from; slabs are never dropped before the whole
	/// CodeSlabs is, so the index stays good.
	struct Alloc {
		char *base = nullptr;
		size_t size = 0;
		size_t slab = 0;
	};

	/// One linked object: its code and read-only data, and separately whatever
	/// of it stays writable. Both come from the same slab.
	struct Object {
		Alloc code;
		Alloc data;
	};

	static llvm::Expected<std::shared_ptr<CodeSlabs>> create ();

	~CodeSlabs ();

	CodeSlabs (const CodeSlabs &) = delete;
	CodeSlabs &operator= (const CodeSlabs &) = delete;

	/// Carve SIZE bytes at ALIGN from the code region, leaving every page it
	/// touches writable and executable.
	llvm::Expected<Alloc> allocate (size_t size, size_t align);

	/// Carve an object's code and its writable data out of one slab. Either
	/// size may be zero.
	llvm::Expected<Object> allocate_object (size_t code_size, size_t code_align,
	                                        size_t data_size, size_t data_align);

	/// Carve SIZE bytes at ALIGN from the writable region, which is readable,
	/// writable and executable for the life of the slab.
	llvm::Expected<Alloc> allocate_writable (size_t size, size_t align);

	/// Declare A fully written. Its pages seal back to read-execute once no
	/// other allocation is still writing to them.
	llvm::Error finish (const Alloc &a);
	llvm::Error finish (const Object &o) { return finish (o.code); }

	/// Like finish (), for an allocation whose content was never completed.
	llvm::Error abandon (const Alloc &a);
	llvm::Error abandon (const Object &o);

	/// Hand a finished allocation's bytes back for reuse.
	void release (const Alloc &a);
	void release_writable (const Alloc &a);
	void release (const Object &o);

	size_t page_size () const { return page_size_; }

	/// How many slabs are reserved.
	size_t slab_count ();

	/// Bytes handed out and not yet released.
	size_t live_bytes ();

	/// Bytes of the reservations that have been committed, which is what the
	/// process actually pays for.
	size_t committed_bytes ();

private:
	enum class PageProt : uint8_t { None, Rwx, Rx };

	struct Slab {
		char *base = nullptr;
		size_t size = 0;

		/// One past the highest code byte ever handed out.
		size_t bump = 0;
		/// Offset of the lowest byte the writable region has handed out.
		size_t writable_bump = 0;
		/// Lowest page index the writable region has committed, which is
		/// also the ceiling the code region bumps up against. It starts
		/// below the guard page, so neither region can reach into it.
		size_t writable_floor = 0;

		size_t live = 0;
		size_t committed_pages = 0;

		/// Per code page, grown as the bump pointer reaches them. The writable
		/// region needs neither, never being sealed.
		std::vector<uint16_t> writers;
		std::vector<PageProt> prot;

		/// Released ranges, offset -> length, coalesced on insert.
		std::map<size_t, size_t> free;
		std::map<size_t, size_t> writable_free;
	};

	explicit CodeSlabs (size_t page_size);

	llvm::Error add_slab ();
	bool carve (Slab &s, size_t index, size_t size, size_t align, Alloc &out);
	void uncarve (Slab &s, const Alloc &a);
	llvm::Expected<bool> carve_writable (Slab &s, size_t index, size_t size,
	                                     size_t align, Alloc &out);
	llvm::Error open (const Alloc &a);
	llvm::Error close (const Alloc &a);
	llvm::Error reprotect (Slab &s, size_t first, size_t last, bool seal);
	void put_free (std::map<size_t, size_t> &set, size_t offset, size_t length,
	               size_t *merged_offset = nullptr,
	               size_t *merged_length = nullptr);
	void drop_pages (Slab &s, size_t offset, size_t length);

	std::mutex mutex_;
	std::vector<std::unique_ptr<Slab>> slabs_;
	size_t page_size_;
	size_t slab_size_;
};

/// The JITLink memory manager that links objects into a CodeSlabs.
///
/// Stock JITLink memory managers round every segment of every object up to a
/// page (BasicLayout::getContiguousPageBasedLayoutSizes), which for objects the
/// size of one method wastes most of what it maps. This one packs an object's
/// segments back to back and asks for exactly the bytes they need, so a method
/// costs its content rather than a page per segment.
///
/// Segments are grouped by whether they stay writable rather than by their
/// exact protection, so a method's read-only data shares the code region and
/// ends up executable. That buys nothing to an attacker who already has the
/// read-write-execute window every code page passes through while it is being
/// written. What cannot be folded in is a segment the running program writes -
/// the per-call-site cast caches, among others - which is why those go to the
/// permanently writable region instead of being sealed with the code.
class SlabMemoryManager final : public llvm::jitlink::JITLinkMemoryManager {
public:
	explicit SlabMemoryManager (std::shared_ptr<CodeSlabs> slabs)
		: slabs_ (std::move (slabs))
	{
	}

	void allocate (const llvm::jitlink::JITLinkDylib *jd,
	               llvm::jitlink::LinkGraph &g,
	               OnAllocatedFunction on_allocated) override;
	void deallocate (std::vector<FinalizedAlloc> allocs,
	                 OnDeallocatedFunction on_deallocated) override;

private:
	class InFlight;

	/// What a FinalizedAlloc's address actually points at. The base address
	/// alone would not do: reclaiming needs the extents as well.
	struct FinalizedInfo {
		CodeSlabs::Object object;
		std::vector<llvm::orc::shared::WrapperFunctionCall> dealloc_actions;
	};

	FinalizedAlloc record (
		CodeSlabs::Object object,
		std::vector<llvm::orc::shared::WrapperFunctionCall> actions);

	std::mutex mutex_;
	llvm::RecyclingAllocator<llvm::BumpPtrAllocator, FinalizedInfo> infos_;
	std::shared_ptr<CodeSlabs> slabs_;
};

} // namespace mono

#endif /* MONO_LLVM_CODEMEM_HPP */
