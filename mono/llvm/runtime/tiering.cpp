#include "tiering.hpp"

#include "method-to-llvm.hpp"
#include "verification.hpp"

namespace mono {

/*
 * A compile on the worker has to be purely generative, because the worker must
 * never run managed code: it is a thread the program has no idea exists, and a
 * static constructor on it can block on anything and take any lock.
 *
 * Everything a compile does is generative except two branches. A method not
 * implemented in IL is handed to mono_jit_compile_method (), whose cache-hit
 * path takes the class's vtable and calls mono_runtime_class_init_full () -
 * which is a cctor. And a body still owed verification reaches the IL verifier,
 * which asks questions the translator never does: whether one class is
 * assignable to another is answered by calling into managed code when either
 * of them is a TypeBuilder that has not been created yet. A method only ever
 * gets here after a compile on the requesting thread, which is where both of
 * those happen, so declining them costs the worker nothing. Translation itself
 * resolves classes and reserves callee stubs and compiles nothing
 * transitively.
 */
bool
compilable_off_thread (MonoMethod *method)
{
	return !implemented_outside_il (method) && !needs_verification (method);
}

} // namespace mono
