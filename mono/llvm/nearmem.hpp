/**
 * \file
 * \brief Memory placement that keeps JIT'd code within reach of mini's.
 */

#ifndef MONO_LLVM_NEARMEM_HPP
#define MONO_LLVM_NEARMEM_HPP

#include <llvm/ExecutionEngine/Orc/MemoryMapper.h>

#include <memory>

namespace mono {

/// A MemoryMapper that reserves out of the MAP_32BIT window.
///
/// mini's code manager allocates there too and patches direct calls as rel32
/// with no thunk fallback, so anything it may be patched to call - a published
/// stub, an allocator body - has to sit within +-2GB of it. Everything in the
/// window reaches everything else in it. Behavior is otherwise
/// InProcessMemoryMapper's.
///
/// Do not read that +-2GB reach as the size of the pool: Linux hands MAP_32BIT
/// out of [0x40000000, 0x80000000), so there is one gigabyte to go around for
/// the whole process and nothing published here can grow past it.
class NearMemoryMapper final : public llvm::orc::MemoryMapper {
public:
	static llvm::Expected<std::unique_ptr<NearMemoryMapper>> Create ();

	explicit NearMemoryMapper (size_t page_size) : page_size_ (page_size) {}
	~NearMemoryMapper () override;

	unsigned int getPageSize () override { return page_size_; }

	void reserve (size_t bytes, OnReservedFunction on_reserved) override;
	char *prepare (llvm::jitlink::LinkGraph &g, llvm::orc::ExecutorAddr addr,
	               size_t content_size) override;
	void initialize (AllocInfo &ai, OnInitializedFunction on_initialized) override;
	void deinitialize (llvm::ArrayRef<llvm::orc::ExecutorAddr> allocations,
	                   OnDeinitializedFunction on_deinitialized) override;
	void release (llvm::ArrayRef<llvm::orc::ExecutorAddr> reservations,
	              OnReleasedFunction on_released) override;

private:
	struct Allocation {
		size_t size;
		std::vector<llvm::orc::shared::WrapperFunctionCall> deinit_actions;
	};

	std::mutex mutex_;
	llvm::DenseMap<void *, size_t> reservations_;
	llvm::DenseMap<llvm::orc::ExecutorAddr, Allocation> allocations_;
	size_t page_size_;
};

} // namespace mono

#endif /* MONO_LLVM_NEARMEM_HPP */
