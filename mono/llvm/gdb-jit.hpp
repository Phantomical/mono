/**
 * \file
 * \brief Handing compiled objects to a debugger through gdb's JIT interface.
 *
 * gdb reads JIT-produced code out of a linked list rooted at
 * `__jit_debug_descriptor`, whose entries name in-memory ELF objects. This
 * backend already produces such objects, so all that is missing is the section
 * addresses the linker chose and a place on that list.
 *
 * LLVM's own ELFDebugObjectPlugin does the same job and does it correctly right
 * up to the point where code goes away: it registers through an alloc action
 * with no matching deallocation action, and its notifyRemovingResources frees
 * the debug object's own backing memory without telling the debugger - a
 * separate allocation from the compiled code, which this backend never frees
 * per method. An entry left standing past that point is a dangling pointer.
 * Hence retract () below.
 */

#ifndef MONO_LLVM_GDB_JIT_HPP
#define MONO_LLVM_GDB_JIT_HPP

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>

#include <cstdint>
#include <vector>

namespace mono {
namespace gdbjit {

/// Whether compiled objects are handed to a debugger. Off unless
/// MONO_LLVM_JIT_GDB is set to something other than `0`.
bool enabled ();

/// An object a debugger has been told about. Opaque: a caller holds it next to
/// the code it describes, and hands it back to retract () when that code goes
/// away.
struct Registration;

/// Returns object with every section header stamped with the address the
/// linker put that section at, which is how a debugger reading it finds the
/// code.
///
/// section_address answers with the address a section was laid out at, or zero
/// for one that was not laid out at all. Empty when object is not one this can
/// stamp.
std::vector<char>
debug_object (std::vector<char> object,
              llvm::function_ref<uint64_t (llvm::StringRef)> section_address);

/// Adds object to the list a debugger reads JIT-produced code out of, and stops
/// at the rendezvous point so an attached one picks it up now.
///
/// The registration owns object, which has to stay mapped and unchanged for as
/// long as it is registered: a debugger reads the bytes whenever it is asked
/// about an address, not when it is told about the object. Null when object is
/// empty.
Registration *publish (std::vector<char> object);

/// Takes reg back out of the debugger's list and frees the object behind it.
void retract (Registration *reg);

} // namespace gdbjit
} // namespace mono

#endif
