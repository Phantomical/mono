/**
 * \file
 * \brief Memory placement that keeps JIT'd code within reach of mini's.
 *
 * A restatement of ORC's InProcessMemoryMapper whose reservations come from
 * mmap with MAP_32BIT: mini patches direct calls as rel32, so the code
 * published here has to stay within +-2GB of mini's own low allocations.
 * The rest - applying segment protections, running the allocation actions the
 * plugins schedule (eh-frame registration among them) - matches the original,
 * because JITLink's contract is exacting about it.
 */

#include "nearmem.hpp"

#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/Support/Process.h>

#include <sys/mman.h>

using namespace llvm;
using namespace llvm::orc;

namespace mono {

Expected<std::unique_ptr<NearMemoryMapper>>
NearMemoryMapper::Create ()
{
	auto page_size = sys::Process::getPageSize ();

	if (!page_size)
		return page_size.takeError ();
	return std::make_unique<NearMemoryMapper> (*page_size);
}

NearMemoryMapper::~NearMemoryMapper ()
{
	std::lock_guard<std::mutex> lock (mutex_);

	for (auto &[base, size] : reservations_)
		munmap (base, size);
}

void
NearMemoryMapper::reserve (size_t bytes, OnReservedFunction on_reserved)
{
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;

#ifdef MAP_32BIT
	flags |= MAP_32BIT;
#endif

	void *base = mmap (nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);

	if (base == MAP_FAILED)
		return on_reserved (createStringError (
			inconvertibleErrorCode (),
			"cannot reserve %zu bytes of low memory for JIT code", bytes));

	{
		std::lock_guard<std::mutex> lock (mutex_);
		reservations_[base] = bytes;
	}

	on_reserved (ExecutorAddrRange (ExecutorAddr::fromPtr (base), bytes));
}

char *
NearMemoryMapper::prepare (jitlink::LinkGraph &, ExecutorAddr addr, size_t)
{
	return addr.toPtr<char *> ();
}

void
NearMemoryMapper::initialize (AllocInfo &ai, OnInitializedFunction on_initialized)
{
	ExecutorAddr min_addr (~0ULL);
	ExecutorAddr max_addr (0);

	for (auto &segment : ai.Segments) {
		ExecutorAddr base = ai.MappingBase + segment.Offset;
		size_t size = segment.ContentSize + segment.ZeroFillSize;

		min_addr = std::min (min_addr, base);
		max_addr = std::max (max_addr, base + size);

		memset ((base + segment.ContentSize).toPtr<void *> (), 0,
		        segment.ZeroFillSize);

		if (auto ec = sys::Memory::protectMappedMemory (
			    { base.toPtr<void *> (), size },
			    toSysMemoryProtectionFlags (segment.AG.getMemProt ())))
			return on_initialized (errorCodeToError (ec));
		if ((segment.AG.getMemProt () & MemProt::Exec) == MemProt::Exec)
			sys::Memory::InvalidateInstructionCache (base.toPtr<void *> (),
			                                         size);
	}

	auto deinit = shared::runFinalizeActions (ai.Actions);
	if (!deinit)
		return on_initialized (deinit.takeError ());

	{
		std::lock_guard<std::mutex> lock (mutex_);
		Allocation &alloc = allocations_[min_addr];

		alloc.size = max_addr - min_addr;
		alloc.deinit_actions = std::move (*deinit);
	}

	on_initialized (min_addr);
}

void
NearMemoryMapper::deinitialize (ArrayRef<ExecutorAddr> allocations,
                                OnDeinitializedFunction on_deinitialized)
{
	Error all = Error::success ();

	{
		std::lock_guard<std::mutex> lock (mutex_);

		for (ExecutorAddr base : llvm::reverse (allocations)) {
			if (Error err = shared::runDeallocActions (
				    allocations_[base].deinit_actions))
				all = joinErrors (std::move (all), std::move (err));

			if (auto ec = sys::Memory::protectMappedMemory (
				    { base.toPtr<void *> (), allocations_[base].size },
				    sys::Memory::MF_READ | sys::Memory::MF_WRITE))
				all = joinErrors (std::move (all), errorCodeToError (ec));

			allocations_.erase (base);
		}
	}

	on_deinitialized (std::move (all));
}

void
NearMemoryMapper::release (ArrayRef<ExecutorAddr> reservations,
                           OnReleasedFunction on_released)
{
	Error all = Error::success ();

	{
		std::lock_guard<std::mutex> lock (mutex_);

		for (ExecutorAddr base : reservations) {
			auto it = reservations_.find (base.toPtr<void *> ());

			if (it == reservations_.end ()) {
				all = joinErrors (
					all ? std::move (all) : Error::success (),
					createStringError (inconvertibleErrorCode (),
				                           "releasing an unknown "
				                           "reservation"));
				continue;
			}

			munmap (it->first, it->second);
			reservations_.erase (it);
		}
	}

	on_released (std::move (all));
}

} // namespace mono
