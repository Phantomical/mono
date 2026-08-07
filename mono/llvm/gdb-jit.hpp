/**
 * \file
 * \brief Handing compiled objects to a debugger through gdb's JIT interface.
 *
 * gdb reads JIT-produced code out of a linked list rooted at
 * `__jit_debug_descriptor`, whose entries name in-memory ELF objects. This
 * backend already produces such objects, so all that is missing is the section
 * addresses the linker chose and a place on that list.
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

/// An object a debugger has been told about. Opaque: what a caller does with
/// one is hold it next to the code it describes and hand it back to retract ()
/// when that code goes away.
struct Registration;

/// OBJECT with every section header stamped with the address the linker put
/// that section at, which is how a debugger reading it finds the code.
///
/// SECTION_ADDRESS answers with the address a section was laid out at, or zero
/// for one that was not laid out at all. Empty when OBJECT is not an object
/// this can stamp.
std::vector<char>
debug_object (std::vector<char> object,
              llvm::function_ref<uint64_t (llvm::StringRef)> section_address);

/// Add OBJECT to the list a debugger reads JIT-produced code out of, and stop
/// at the rendezvous point so an attached one picks it up now.
///
/// The registration owns OBJECT, which has to stay mapped and unchanged for as
/// long as it is registered: a debugger reads the bytes whenever it is asked
/// about an address, not when it is told about the object. Null when OBJECT is
/// empty.
Registration *publish (std::vector<char> object);

/// Take REG back out of the debugger's list and free the object behind it.
void retract (Registration *reg);

} // namespace gdbjit
} // namespace mono

#endif
