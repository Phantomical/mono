/**
 * \file
 * \brief The machine-level recovery of mono's EH clauses from the final
 * landing-pad set.
 */

#ifndef MONO_LLVM_PASSES_EH_GATHER_HPP
#define MONO_LLVM_PASSES_EH_GATHER_HPP

/*
 * LLVM uses `PIC` as an identifier (PassInstrumentationCallbacks). Mono's
 * build defines it as a macro.
 */
#ifdef PIC
#undef PIC
#endif

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/CodeGen/MachineFunctionPass.h>

#include <cstdint>
#include <optional>

namespace llvm {
class DISubprogram;
class Module;
} // namespace llvm

namespace mono {

struct MonoEHSideChannel;

/**
 * Find the clause chain reached from a faulting operation's handler block.
 * Follow only unique-successor trampolines; stop at branches and cycles.
 */
template <typename Block, typename ChainMap>
std::optional<unsigned>
faulting_handler_chain (const Block *handler, const ChainMap &chain_for_block)
{
	llvm::SmallPtrSet<const Block *, 8> visited;

	while (handler != nullptr && visited.insert (handler).second) {
		auto found = chain_for_block.find (handler);
		if (found != chain_for_block.end ())
			return found->second;

		if (handler->succ_size () != 1)
			return std::nullopt;

		handler = *handler->succ_begin ();
	}

	return std::nullopt;
}

/**
 * Gathers the info needed to build mono's exception handling tables for the
 * function being compiled.
 */
class MonoEHGatherPass : public llvm::MachineFunctionPass {
public:
	static char ID;

	/* sc must outlive the pipeline this pass is added to. */
	explicit MonoEHGatherPass (MonoEHSideChannel *sc)
	    : llvm::MachineFunctionPass (ID), sc_ (sc)
	{
	}

	llvm::StringRef getPassName () const override
	{
		return "Mono EH clause gather";
	}

	/* Builds ids_ once per module, rather than once per function - the same
	 * cost IlLineHandler::beginModule () (compiler.cpp) already pays for the
	 * same map. */
	bool doInitialization (llvm::Module &m) override;

	bool runOnMachineFunction (llvm::MachineFunction &mf) override;

private:
	MonoEHSideChannel *sc_;
	llvm::DenseMap<const llvm::DISubprogram *, std::uint64_t> ids_;
};

} // namespace mono

#endif /* MONO_LLVM_PASSES_EH_GATHER_HPP */
