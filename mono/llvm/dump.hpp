/**
 * \file
 * \brief What the backend's dump points write through.
 *
 * `mono/mini/jit-dump.hpp` decides whether a dump happens and writes it. This
 * gives LLVM a stream for the dump and carries the method name on the function
 * so codegen can name the body it came from.
 */

#ifndef MONO_LLVM_DUMP_HPP
#define MONO_LLVM_DUMP_HPP

#include <memory>
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
 * Runs \p body on a buffer, then hands the buffer to write_dump ().
 *
 * The caller has already decided the dump happens: this does not test the
 * point or the filter again.
 */
llvm::Error with_dump_stream (DumpPoint point, llvm::StringRef name,
                              llvm::function_ref<llvm::Error (llvm::raw_pwrite_stream &)> body);

/**
 * Clones \p module, retaining only \p entry and the bodies inlined into it.
 * Where \p entry is a filter, also retains the parent it recovers its frame
 * from.
 *
 * Rewrites references to removed definitions so the clone remains valid IR.
 * Returns null when \p entry does not name a function in \p module.
 */
std::unique_ptr<llvm::Module> clone_body_module (const llvm::Module &module,
                                                 llvm::StringRef entry);

/**
 * Prints the module one body needs, so that the dump parses on its own.
 *
 * Does nothing when \p entry names no function. The caller has already decided
 * that the dump happens; this does not test the point or filter again.
 */
llvm::Error dump_body_module (DumpPoint point, const llvm::Module &module,
                              llvm::StringRef entry, llvm::StringRef name);

/// Records the method a function was translated from, under the name every
/// dump point matches the filter against.
///
/// Codegen runs long after the MonoMethod is out of reach, so a function that
/// wants naming there has to carry it.
void set_dump_name (llvm::Function &function, llvm::StringRef name);

/// The dump name \ref set_dump_name recorded, or the function's own symbol when
/// it carries none. A dispatcher carries none.
std::string dump_name_of (const llvm::Function &function);

/// Whether a function is one of the methods its module was built to publish,
/// rather than a body inlined in beside them.
///
/// An inlined copy has internal linkage, so it is not a method anything can
/// enter and it gets no dump of its own.
bool is_published_body (const llvm::Function &function);

} // namespace mono

#endif
