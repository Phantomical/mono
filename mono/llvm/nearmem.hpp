/**
 * \file
 * \brief Where JIT'd code is mapped.
 */

#ifndef MONO_LLVM_NEARMEM_HPP
#define MONO_LLVM_NEARMEM_HPP

#include <llvm/ExecutionEngine/Orc/MemoryMapper.h>

#include <memory>

namespace mono {

/// A MemoryMapper that reserves wherever mmap cares to put it.
///
/// Placement is unconstrained because nothing patches a bare rel32 at us:
/// mini's callsite patcher declines outright for jinfos flagged from_llvm,
/// which is every jinfo this backend registers. Our own outbound calls leave
/// their object by symbol, so JITLink routes any that a direct branch cannot
/// reach through a jump stub. Behavior is otherwise InProcessMemoryMapper's.
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
