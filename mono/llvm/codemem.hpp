/**
 * \file
 * \brief Slab allocation for JIT'd code.
 */

#ifndef MONO_LLVM_CODEMEM_HPP
#define MONO_LLVM_CODEMEM_HPP

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
/// A slab is one PROT_NONE reservation with method bodies growing up from the
/// bottom and mono's published stubs down from the top. Nothing is committed
/// until it is handed out, so the reservation costs address space and nothing
/// else, and a method costs its own bytes rather than the page it landed in.
///
/// Code pages are read-write-execute while an allocation is still being
/// written into them and read-execute once every writer is done, so allocate ()
/// must be paired with finish () or abandon (). Writable and executable at the
/// same time is unavoidable rather than lazy: JITLink copies an object's
/// content into its final address, and under bump allocation that page very
/// likely already holds a finished method somebody is running. The stub region
/// stays writable for the life of the slab, because stubs are carved one at a
/// time out of a page other stubs are already running from.
///
/// **One allocation never spans two slabs**, and a slab is never larger than
/// 2GB. JITLink stubs an out-of-range call, but it has no such fallback for the
/// PCRel32 edge that reaches a method's own .rodata, nor for the Delta32 edges
/// EHFrameEdgeFixer plants from .eh_frame back to .text - both hard-error past
/// +-2GB. Those are all intra-object, so keeping an object whole inside one
/// bounded slab keeps the distances down in the tens of bytes.
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

	static llvm::Expected<std::shared_ptr<CodeSlabs>> create ();

	~CodeSlabs ();

	CodeSlabs (const CodeSlabs &) = delete;
	CodeSlabs &operator= (const CodeSlabs &) = delete;

	/// Carve SIZE bytes at ALIGN from the code region, leaving every page it
	/// touches writable and executable.
	llvm::Expected<Alloc> allocate (size_t size, size_t align);

	/// Declare A fully written. Its pages seal back to read-execute once no
	/// other allocation is still writing to them.
	llvm::Error finish (const Alloc &a);

	/// Like finish (), for an allocation whose content was never completed.
	llvm::Error abandon (const Alloc &a);

	/// Hand a finished allocation's bytes back for reuse.
	void release (const Alloc &a);

	/// Carve SIZE bytes at ALIGN from the top-of-slab stub region, which is
	/// writable and executable for the life of the slab.
	llvm::Expected<Alloc> allocate_stubs (size_t size, size_t align);

	size_t page_size () const { return page_size_; }

	/// How many slabs are reserved.
	size_t slab_count ();

	/// Bytes handed out by allocate () and not yet released.
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
		/// Offset of the lowest published stub.
		size_t stub_bump = 0;
		/// Lowest page index the stub region has committed.
		size_t stub_floor = 0;

		size_t live = 0;
		size_t committed_pages = 0;

		/// Per code page, grown as the bump pointer reaches them. The stub
		/// region needs neither, never being sealed.
		std::vector<uint16_t> writers;
		std::vector<PageProt> prot;

		/// Released ranges, offset -> length, coalesced on insert.
		std::map<size_t, size_t> free;
	};

	explicit CodeSlabs (size_t page_size);

	llvm::Error add_slab ();
	bool carve (Slab &s, size_t index, size_t size, size_t align, Alloc &out);
	llvm::Error open (const Alloc &a);
	llvm::Error close (const Alloc &a);
	llvm::Error reprotect (Slab &s, size_t first, size_t last, bool seal);
	void put_free (Slab &s, size_t offset, size_t length);

	std::mutex mutex_;
	std::vector<std::unique_ptr<Slab>> slabs_;
	size_t page_size_;
	size_t slab_size_;
};

} // namespace mono

#endif /* MONO_LLVM_CODEMEM_HPP */
