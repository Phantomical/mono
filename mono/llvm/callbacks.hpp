/**
 * \file
 * \brief The compiler re-entry trampolines a lazy stub starts out pointing at.
 */

#ifndef MONO_LLVM_CALLBACKS_HPP
#define MONO_LLVM_CALLBACKS_HPP

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/FunctionExtras.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/Support/Error.h>

#include <memory>
#include <mutex>
#include <string>

namespace llvm {
namespace orc {
class TrampolinePool;
}
} // namespace llvm

namespace mono {

/// What a lazy stub runs the first time it is called: produces the address to
/// continue into. It cannot fail - whoever handed it over has already decided
/// where a failed compile lands.
using LazyCompile = llvm::unique_function<void *()>;

/// The re-entry trampolines lazy stubs point at, and the compile behind each.
///
/// ORC has this as well (JITCompileCallbackManager), but a callback it hands
/// out can never be given back: the trampoline, an interned symbol and the
/// materialization unit holding the closure all live as long as the JIT does,
/// and a program that mints and drops methods pays that for every one of them.
/// Here the compile lives in a map release () erases and the trampoline goes
/// back on the pool for the next lazy stub - and reaching one is a hash lookup
/// rather than a symbol lookup through the session.
///
/// A trampoline's address is its identity. Whoever reserved it is holding it
/// anyway - it is what the stub in front of it was pointed at - so there is
/// nothing for a name to buy here.
class LazyCallbacks {
public:
	/// Build the pool over the host's re-entry ABI. A trampoline that fires
	/// with no compile behind it lands at ON_ERROR, which cannot return.
	static llvm::Expected<std::unique_ptr<LazyCallbacks>> create (void *on_error);

	~LazyCallbacks ();

	LazyCallbacks (const LazyCallbacks &) = delete;
	LazyCallbacks &operator= (const LazyCallbacks &) = delete;

	/// Reserve a trampoline that runs COMPILE the first time it is called and
	/// continues into the address it returns, and hand back its address.
	/// Threads arriving together compile once and all land on the same address.
	llvm::Expected<void *> reserve (LazyCompile compile);

	/// Give TRAMPOLINE back, for a later reserve () to hand out again, and drop
	/// the compile behind it. An address this never handed out is ignored.
	///
	/// The caller has proved nothing can reach whatever pointed at it: the next
	/// stub to be published may be given the very same trampoline.
	void release (void *trampoline);

private:
	LazyCallbacks (void *on_error) : on_error_ (on_error) {}

	/// One reserved trampoline's compile, and where it landed.
	struct Callback {
		std::mutex mutex;
		LazyCompile compile;
		void *landing = nullptr;
	};

	/// Run the compile TRAMPOLINE stands for. Called on whichever thread
	/// entered the stub, from the resolver.
	void *fire (llvm::orc::ExecutorAddr trampoline);

	void *on_error_;

	std::mutex mutex_;
	llvm::DenseMap<llvm::orc::ExecutorAddr, std::shared_ptr<Callback>> callbacks_;

	/*
	 * Declared last: the pool's resolver closure holds this object, so it has
	 * to be torn down before the maps it reads.
	 */
	std::unique_ptr<llvm::orc::TrampolinePool> pool_;
};

} // namespace mono

#endif
