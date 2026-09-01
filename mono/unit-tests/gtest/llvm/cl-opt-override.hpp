/**
 * \file
 * \brief An RAII override for a registered `llvm::cl::opt<bool>`.
 */

#ifndef MONO_LLVM_TESTS_CL_OPT_OVERRIDE_HPP
#define MONO_LLVM_TESTS_CL_OPT_OVERRIDE_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>

#include <cassert>

namespace mono {
namespace test {

/// An override of a named `llvm::cl::opt<bool>` for the current scope,
/// restoring the value it found on destruction.
struct BoolOptionOverride {
	llvm::cl::opt<bool> *option;
	bool was_set;

	BoolOptionOverride (llvm::StringRef name, bool value)
	{
		llvm::cl::Option *found = llvm::cl::getRegisteredOptions ().lookup (name);

		assert (found != nullptr);
		option = static_cast<llvm::cl::opt<bool> *> (found);
		was_set = option->getValue ();
		option->setValue (value);
	}

	~BoolOptionOverride () { option->setValue (was_set); }
};

} // namespace test
} // namespace mono

#endif
