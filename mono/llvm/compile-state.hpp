/**
 * \file
 * \brief What a pass can ask about the compile it is running inside.
 *
 * A pipeline is built once per compile thread and run for many compiles, so a
 * pass cannot be handed anything when it is built. What the running compile
 * owns goes here instead, for as long as the run, and a pass reads it back. Put
 * per-compile state a pass needs in this one place rather than adding a channel
 * of its own beside it.
 *
 * Nothing here is a fact about metadata. A pass that wants one of those calls
 * mono for it: what this carries is the compile, which mono cannot answer for.
 */

#ifndef MONO_LLVM_COMPILE_STATE_HPP
#define MONO_LLVM_COMPILE_STATE_HPP

#include <llvm/ADT/STLFunctionalExtras.h>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;

namespace llvm {
class Function;
}

namespace mono {

struct CompileState {
	/// The domain the code will run as - the owning linker's, never the
	/// thread's current one. Null outside a compile.
	MonoDomain *domain = nullptr;

	/// Publishes \p decl's method and resolves the symbol, so that a call to
	/// what comes back reaches that method's entry.
	///
	/// \p decl must carry the method as its marker and must have the shape a
	/// caller enters it with. What comes back is the declaration to call, which
	/// is decl itself or one the module already held for the method - the two
	/// are folded together, and the loser is erased.
	///
	/// Null means the method's own metadata will not load. decl is then not
	/// worth calling and the caller has to leave its site as it was.
	llvm::function_ref<llvm::Function *(llvm::Function &decl, MonoMethod *method)> publish;
};

/// This thread's compile. Empty outside one, which every reader has to take as
/// leaving the module alone.
CompileState &current_compile ();

/// Hands a compile's state to the passes for as long as the run, and takes it
/// back. The pipeline outlives the compile, so state left behind is state the
/// next run would read.
class CompileScope {
public:
	CompileScope (const CompileState &state) { current_compile () = state; }
	~CompileScope () { current_compile () = CompileState (); }

	CompileScope (const CompileScope &) = delete;
	CompileScope &operator= (const CompileScope &) = delete;
};

} // namespace mono

#endif
