/**
 * \file
 * \brief How a module's symbols say which method and which piece of one they
 * stand for, and how the engine renames them to what it publishes.
 *
 * The translator cannot know what a method's stub is called: that is the
 * engine's to decide, and only the engine can publish one. So the translator
 * marks the declaration with the MonoMethod, and the engine renames it.
 * Neither side has to agree on a mangling, because only one side has one.
 */

#ifndef MONO_LLVM_METHOD_SYMBOLS_HPP
#define MONO_LLVM_METHOD_SYMBOLS_HPP

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include <string>
#include <string_view>

namespace llvm {
class GlobalValue;
class Module;
} // namespace llvm

typedef struct _MonoMethod MonoMethod;

namespace mono {

/// What a filter body's symbol adds to the symbol of the method it belongs to.
/// The clause index follows it, so the whole name is `<entry>$filter<index>`.
///
/// A filter runs as a function of its own and is compiled beside its method, so
/// this is what tells the two apart to anything walking a compiled object's
/// functions.
constexpr std::string_view filter_body_suffix = "$filter";

void mark_method_reference (llvm::GlobalValue &value, MonoMethod *method);

/// Returns the method \p value stands for, or null when it carries no marker.
MonoMethod *marked_method (const llvm::GlobalValue &value);

/// Renames every marked declaration in \p m to what \p name_of calls that
/// method, leaving the module referring only to symbols the engine publishes.
///
/// \p name_of must return the same symbol the engine will publish that
/// method under, or the renamed declaration will not resolve.
///
/// Declarations only. A definition already carries its own name and is left
/// alone even when it is marked too.
llvm::Error
bind_method_symbols (llvm::Module &m,
                     llvm::function_ref<llvm::Expected<std::string> (MonoMethod *)> name_of);

} // namespace mono

#endif
