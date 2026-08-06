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

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/RedirectionManager.h>
#include <llvm/Support/Error.h>

#include <memory>

namespace mono {

class CodeSlabs;

/// The manager the JIT publishes stubs through: ORC's redirectable-symbol
/// interface, plus a way to take a stub back that the interface does not have.
class StubManager : public llvm::orc::RedirectableSymbolManager {
public:
	/// Reclaim the stubs published for NAMES in JD, so that a later
	/// createRedirectableSymbols () can hand out the same blocks again. A name
	/// that never got a stub is ignored.
	///
	/// The caller has already undefined the names and proved nothing can reach
	/// the stubs.
	virtual void discard (llvm::orc::JITDylib &jd,
	                      const llvm::orc::SymbolNameSet &names) = 0;
};

/// Build the stub manager the JIT publishes stubs through, carving stubs out of
/// SLABS. Fails on architectures we do not emit stubs for.
llvm::Expected<std::unique_ptr<StubManager>>
make_stub_manager (llvm::orc::ExecutionSession &es, CodeSlabs &slabs);

} // namespace mono

#endif
