/**
 * \file
 * \brief A slab-allocated implementation of ORC's redirectable symbols.
 *
 * ORC ships one of these (JITLinkRedirectableSymbolManager), but it builds and
 * links a LinkGraph per batch of stubs, and the runtime publishes stubs one
 * method at a time. Each graph gets its own allocation, whose segments are
 * page-aligned, so a lone stub costs one page of code plus one of data: 8K a
 * method, which is most of a gigabyte over a game's worth of them. Redirecting
 * likewise goes through a full symbol lookup.
 *
 * So we carve stubs out of a slab instead. A stub costs 24 bytes, publishing
 * one is a bump-allocate (or a pop off the free list) plus a few stores, and a
 * redirect is a single atomic store to the slot. The ORC interface is unchanged
 * bar the reclaim hook, which is what the promotion machinery is written
 * against.
 */

#include "stubs.hpp"

#include "arch/arch.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/Support/Memory.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <sys/mman.h>

using namespace llvm;
using namespace llvm::orc;

namespace mono {
namespace {

/*
 * How many stubs a slab holds. The two regions come out of one mapping so the
 * jump's rel32 displacement always reaches, and both are whole pages at this
 * count, which keeps the stub region 16-aligned without padding between them.
 */
constexpr size_t stubs_per_slab = 2048;
constexpr size_t slot_region_size = stubs_per_slab * sizeof (void *);
constexpr size_t stub_region_size = stubs_per_slab * arch::stub_block_size;

using Slot = std::atomic<void *>;

struct Stub {
	void *code;
	Slot *slot;
};

/// Allocator over mapped slabs of (slot region, stub region) pairs: bump within
/// a slab, with a free list of the stubs handed back.
class StubSlabs {
public:
	~StubSlabs ()
	{
		for (sys::MemoryBlock &slab : slabs_)
			sys::Memory::releaseMappedMemory (slab);
	}

	/// Carve one stub, jumping to TARGET.
	Expected<Stub> allocate (void *target)
	{
		if (!free_.empty ()) {
			Stub stub = free_.back ();

			free_.pop_back ();
			/*
			 * The jump reads this stub's own slot and always did, so
			 * there is nothing to rewrite but the destination.
			 */
			stub.slot->store (target, std::memory_order_release);
			return stub;
		}

		if (next_ == stubs_per_slab)
			if (Error err = add_slab ())
				return std::move (err);

		char *base = static_cast<char *> (slabs_.back ().base ());
		size_t i = next_++;

		Slot *slot = reinterpret_cast<Slot *> (base + i * sizeof (void *));
		char *code = base + slot_region_size + i * arch::stub_block_size;

		slot->store (target, std::memory_order_release);
		arch::write_jump_stub (code, slot);
		sys::Memory::InvalidateInstructionCache (code, arch::stub_block_size);

		return Stub { code, slot };
	}

	/// Take STUB back, for a later allocate () to hand out again.
	void release (Stub stub) { free_.push_back (stub); }

private:
	/*
	 * Stubs stay writable rather than being flipped to read-execute once
	 * written: they are carved one at a time out of a page other stubs are
	 * already running from, so there is no point at which the page is quiet
	 * enough to reprotect. A detour library mprotects the entry it patches
	 * anyway.
	 */
	Error add_slab ()
	{
		/*
		 * Low, like every other piece of published code (nearmem.hpp): a
		 * stub is exactly what mini's rel32 call patching targets, so it
		 * has to stay within reach of mini's own allocations.
		 */
		int flags = MAP_PRIVATE | MAP_ANONYMOUS;

#ifdef MAP_32BIT
		flags |= MAP_32BIT;
#endif

		void *base = mmap (nullptr, slot_region_size + stub_region_size,
		                   PROT_READ | PROT_WRITE, flags, -1, 0);

		if (base == MAP_FAILED)
			return make_error<StringError> ("could not map a stub slab",
			                                inconvertibleErrorCode ());

		sys::MemoryBlock slab (base, slot_region_size + stub_region_size);

		if (sys::Memory::protectMappedMemory (
		        sys::MemoryBlock (static_cast<char *> (slab.base ()) +
		                              slot_region_size,
		                          stub_region_size),
		        sys::Memory::MF_READ | sys::Memory::MF_WRITE |
		            sys::Memory::MF_EXEC)) {
			sys::Memory::releaseMappedMemory (slab);
			return make_error<StringError> ("could not make a stub slab "
			                                "executable",
			                                inconvertibleErrorCode ());
		}

		slabs_.push_back (slab);
		next_ = 0;
		return Error::success ();
	}

	std::vector<sys::MemoryBlock> slabs_;
	std::vector<Stub> free_;
	size_t next_ = stubs_per_slab;
};

class SlabStubManager : public StubManager {
public:
	void emitRedirectableSymbols (std::unique_ptr<MaterializationResponsibility> r,
	                              SymbolMap initial_dests) override
	{
		ExecutionSession &es = r->getExecutionSession ();
		JITDylib &jd = r->getTargetJITDylib ();

		SymbolMap resolved;
		{
			std::lock_guard<std::mutex> lock (mutex_);
			for (auto &[name, dest] : initial_dests) {
				Expected<Stub> stub =
					slabs_.allocate (dest.getAddress ().toPtr<void *> ());
				if (!stub) {
					es.reportError (stub.takeError ());
					return r->failMaterialization ();
				}

				stubs_[&jd][name] = *stub;
				resolved[name] = { ExecutorAddr::fromPtr (stub->code),
					               dest.getFlags () };
			}
		}

		if (Error err = r->notifyResolved (resolved)) {
			es.reportError (std::move (err));
			return r->failMaterialization ();
		}
		if (Error err = r->notifyEmitted ({})) {
			es.reportError (std::move (err));
			return r->failMaterialization ();
		}
	}

	Error redirect (JITDylib &jd, const SymbolMap &new_dests) override
	{
		std::lock_guard<std::mutex> lock (mutex_);

		auto jd_stubs = stubs_.find (&jd);
		for (auto &[name, dest] : new_dests) {
			Stub stub = jd_stubs == stubs_.end ()
			                ? Stub {}
			                : jd_stubs->second.lookup (name);
			if (stub.slot == nullptr)
				return make_error<StringError> (
					"no stub to redirect for " + *name + " in " + jd.getName (),
					inconvertibleErrorCode ());

			/* Callers may be running through this stub right now: the store has
			 * to land whole, and everything the new target reads has to be
			 * visible by the time it does. */
			stub.slot->store (dest.getAddress ().toPtr<void *> (),
			                  std::memory_order_release);
		}

		return Error::success ();
	}

	void discard (JITDylib &jd, const SymbolNameSet &names) override
	{
		std::lock_guard<std::mutex> lock (mutex_);

		auto jd_stubs = stubs_.find (&jd);

		if (jd_stubs == stubs_.end ())
			return;

		for (const SymbolStringPtr &name : names) {
			auto it = jd_stubs->second.find (name);

			if (it == jd_stubs->second.end ())
				continue;
			slabs_.release (it->second);
			jd_stubs->second.erase (it);
		}
	}

private:
	std::mutex mutex_;
	StubSlabs slabs_;

	/// Each published stub, by the name it was published under. Recorded here
	/// until discard () gives it back.
	DenseMap<JITDylib *, DenseMap<SymbolStringPtr, Stub>> stubs_;
};

} // namespace

Expected<std::unique_ptr<StubManager>>
make_stub_manager (ExecutionSession &es)
{
	const Triple &tt = es.getTargetTriple ();
	if (tt.getArch () != arch::target_arch)
		return make_error<StringError> (
			"redirectable stubs are not implemented for " + tt.str (),
			inconvertibleErrorCode ());

	return std::make_unique<SlabStubManager> ();
}

} // namespace mono
