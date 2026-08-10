/**
 * \file
 * \brief Which of the two engines this process compiles through.
 *
 * Temporary. The engine under runtime/ is being grown to replace the one in
 * runtime.cpp, and this exists so each step of that can be measured against the
 * engine it is replacing. It goes when runtime.cpp does, along with every branch
 * on it - there is no configuration here worth keeping.
 */

#ifndef MONO_LLVM_RUNTIME_ENGINE_HPP
#define MONO_LLVM_RUNTIME_ENGINE_HPP

namespace mono {

enum class EngineKind {
	/// runtime.cpp, which every compile goes through today.
	legacy,
	/// runtime/backend.cpp, which is being grown to replace it.
	backend,
};

/// Which engine MONO_LLVM_JIT_ENGINE asked for.
///
/// Read once, on the first call. A process compiles through one engine for its
/// whole life: they keep separate state for the same method, and mono's own
/// jit-info table would end up holding records from both.
EngineKind selected_engine ();

/// Record that an engine's singleton is being constructed, dying if the other
/// one already has been.
///
/// LLVM's command-line options are applied once, at the first MonoJit::create
/// (jit.cpp), so a second engine starting up would silently lose every
/// --llvm-opt the first one consumed. That is quiet enough to be worth an abort.
void claim_engine (EngineKind kind);

} // namespace mono

#endif
