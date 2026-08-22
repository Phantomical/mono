/**
 * \file
 * \brief What the backend's dump points write through.
 *
 * `mono/mini/jit-dump.hpp` decides whether a dump happens and opens where it
 * goes. This turns that destination into the stream LLVM prints to. It also
 * carries the method's dump name on the function, so that codegen can name the
 * method a body came from.
 */

#ifndef MONO_LLVM_DUMP_HPP
#define MONO_LLVM_DUMP_HPP

#include <string>

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include "mono/mini/jit-dump.hpp"

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/**
 * Runs \p body with the stream this dump goes to, then closes it.
 *
 * Does nothing and reports success when the destination did not open. The
 * caller has already decided the dump happens: this does not test the point or
 * the filter again.
 */
llvm::Error with_dump_stream (DumpPoint point, llvm::StringRef name,
                              llvm::function_ref<llvm::Error (llvm::raw_pwrite_stream &)> body);

/// Records the method a function was translated from, under the name every
/// dump point matches the filter against.
///
/// Codegen runs long after the MonoMethod is out of reach, so a function that
/// wants naming there has to carry it.
void set_dump_name (llvm::Function &function, llvm::StringRef name);

/// The dump name \ref set_dump_name recorded, or the function's own symbol when
/// it carries none. A thrower, a dispatcher and an entry thunk carry none.
std::string dump_name_of (const llvm::Function &function);

/// Whether a function is one of the methods its module was built to publish,
/// rather than a body folded in beside them.
///
/// A folded copy has internal linkage, so it is not a method anything can enter
/// and it gets no dump of its own.
bool is_published_body (const llvm::Function &function);

} // namespace mono

#endif
