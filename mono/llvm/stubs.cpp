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
 * So we carve stubs out of a slab instead. A stub costs 40 bytes, publishing
 * one is a bump-allocate plus a few stores, and a redirect is a single atomic
 * store to the slot. The ORC interface is unchanged, which is what the
 * promotion machinery is written against.
 */

#include "stubs.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ExecutionEngine/JITLink/x86_64.h>
#include <llvm/Support/Memory.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

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
constexpr size_t stub_region_size = stubs_per_slab * stub_block_size;

using Slot = std::atomic<void *>;

struct Stub {
	void *code;
	Slot *slot;
};

/// Bump allocator over mapped slabs of (slot region, stub region) pairs.
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
		if (next_ == stubs_per_slab)
			if (Error err = add_slab ())
				return std::move (err);

		char *base = static_cast<char *> (slabs_.back ().base ());
		size_t i = next_++;

		Slot *slot = reinterpret_cast<Slot *> (base + i * sizeof (void *));
		char *code = base + slot_region_size + i * stub_block_size;

		slot->store (target, std::memory_order_release);
		write_stub (code, slot);
		sys::Memory::InvalidateInstructionCache (code, stub_block_size);

		return Stub { code, slot };
	}

private:
	/// `jmpq *slot(%rip)`, int3 to the end of the block.
	static void write_stub (char *code, const Slot *slot)
	{
		std::memcpy (code, jitlink::x86_64::PointerJumpStubContent,
		             sizeof (jitlink::x86_64::PointerJumpStubContent));
		std::memset (code + sizeof (jitlink::x86_64::PointerJumpStubContent),
		             0xcc,
		             stub_block_size -
		                 sizeof (jitlink::x86_64::PointerJumpStubContent));

		/* rip is the end of the instruction, and the displacement follows the
		 * two opcode bytes. */
		int32_t disp = static_cast<int32_t> (
			reinterpret_cast<const char *> (slot) -
			(code + sizeof (jitlink::x86_64::PointerJumpStubContent)));
		std::memcpy (code + 2, &disp, sizeof (disp));
	}

	/*
	 * Stubs stay writable rather than being flipped to read-execute once
	 * written: they are carved one at a time out of a page other stubs are
	 * already running from, so there is no point at which the page is quiet
	 * enough to reprotect. A detour library mprotects the entry it patches
	 * anyway.
	 */
	Error add_slab ()
	{
		std::error_code ec;
		sys::MemoryBlock slab = sys::Memory::allocateMappedMemory (
			slot_region_size + stub_region_size, nullptr,
			sys::Memory::MF_READ | sys::Memory::MF_WRITE, ec);
		if (ec)
			return make_error<StringError> ("could not map a stub slab", ec);

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
	size_t next_ = stubs_per_slab;
};

class SlabRedirectableSymbolManager : public RedirectableSymbolManager {
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

				slots_[&jd][name] = stub->slot;
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

		auto jd_slots = slots_.find (&jd);
		for (auto &[name, dest] : new_dests) {
			auto slot = jd_slots == slots_.end ()
			                ? nullptr
			                : jd_slots->second.lookup (name);
			if (!slot)
				return make_error<StringError> (
					"no stub to redirect for " + *name + " in " + jd.getName (),
					inconvertibleErrorCode ());

			/* Callers may be running through this stub right now: the store has
			 * to land whole, and everything the new target reads has to be
			 * visible by the time it does. */
			slot->store (dest.getAddress ().toPtr<void *> (),
			             std::memory_order_release);
		}

		return Error::success ();
	}

private:
	std::mutex mutex_;
	StubSlabs slabs_;

	/*
	 * The slot behind each published stub. Stubs are never reclaimed - a
	 * method keeps its entry for the life of the process - so these stay valid
	 * once recorded.
	 */
	DenseMap<JITDylib *, DenseMap<SymbolStringPtr, Slot *>> slots_;
};

} // namespace

Expected<std::unique_ptr<RedirectableSymbolManager>>
make_redirectable_symbol_manager (ExecutionSession &es)
{
	const Triple &tt = es.getTargetTriple ();
	if (tt.getArch () != Triple::x86_64)
		return make_error<StringError> (
			"redirectable stubs are only implemented for x86-64, not " +
				tt.str (),
			inconvertibleErrorCode ());

	return std::make_unique<SlabRedirectableSymbolManager> ();
}

} // namespace mono
