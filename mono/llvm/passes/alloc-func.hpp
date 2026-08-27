/**
 * \file
 * \brief The declaration an allocation is written as, and the allocator it
 * lowers to.
 *
 * An allocation written as the collector's own allocator puts the collector
 * into the IR. SGen names a managed allocator and Boehm names an icall, and the
 * two carry different attributes. Written as a call to one of these
 * declarations, an allocation has one shape under either collector, and the
 * allocator is an operand.
 */

#ifndef MONO_LLVM_PASSES_ALLOC_FUNC_HPP
#define MONO_LLVM_PASSES_ALLOC_FUNC_HPP

#include <llvm/ADT/StringRef.h>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/*
 * Each declaration takes the same three operands:
 *
 *   ptr @mono.alloc.object (ptr vtable, iN size, ptr allocator)
 *
 * vtable is the vtable of the class the site allocates. It is a marked global
 * for a class the compile can name, and the value an rgctx fetch answered for
 * one it cannot.
 *
 * size is the instance size for an object and the element count for a vector.
 * A zero instance size names a class whose layout the compile does not know. A
 * zero element count is an ordinary empty vector.
 *
 * allocator is the function the lowering calls. It takes the vtable and the
 * second operand in that order, and an allocator declaring one parameter gets
 * the vtable alone. The attributes describing the collector's own entry point
 * go on that declaration, because only the translator knows which collector
 * answered.
 *
 * The `.kept` name means the same allocation as the name beside it, and LLVM
 * must keep it. The name carries that rather than the site, because an alloc
 * kind is an attribute of the declaration.
 *
 * None of the declarations is nounwind. Both allocators raise
 * OutOfMemoryException, so a site inside a clause is an invoke and the lowering
 * keeps that edge.
 */

/// Allocates an object of the class the vtable names.
constexpr llvm::StringRef alloc_object_name = "mono.alloc.object";

/// Allocates a vector of that class, of the element count the second operand
/// gives.
constexpr llvm::StringRef alloc_vector_name = "mono.alloc.vector";

/// Allocates an object whose allocation the program can observe.
constexpr llvm::StringRef alloc_object_kept_name = "mono.alloc.object.kept";

/// Allocates a vector whose allocation the program can observe.
constexpr llvm::StringRef alloc_vector_kept_name = "mono.alloc.vector.kept";

/// The kind of object an allocation site makes.
enum class AllocShape {
	object,
	vector,
};

/// The declaration in \p m for one of the four forms, created on first use.
///
/// \p erasable marks the form LLVM can erase once nothing reads the object.
llvm::Function *alloc_func_decl (llvm::Module &m, AllocShape shape, bool erasable);

/// Rewrites every allocation call into a call of the allocator it carries,
/// erases the declarations, and says whether it changed anything.
bool lower_allocations (llvm::Module &m);

} // namespace mono

#endif
