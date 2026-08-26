/**
 * \file
 * \brief The compile a thread is running, for the passes to read.
 */

#include "compile-state.hpp"

namespace mono {

CompileState &
current_compile ()
{
	static thread_local CompileState state;

	return state;
}

} // namespace mono
