#ifndef MONO_LLVM_UTIL_NEVER_DESTROYED_HPP
#define MONO_LLVM_UTIL_NEVER_DESTROYED_HPP

#include <type_traits>
#include <utility>

namespace mono {

/// Moves \p value to storage that is retained until process exit.
///
/// Use this for compiler tables that can be accessed while exit waits for
/// compilation workers to finish.
template<typename T>
const std::decay_t<T> &
never_destroyed (T &&value)
{
	return *new std::decay_t<T> (std::forward<T> (value));
}

} // namespace mono

#endif
