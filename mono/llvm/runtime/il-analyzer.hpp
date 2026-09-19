/**
 * \file
 * \brief Counts the IL reachable for a specific generic instantiation.
 *
 * The analysis propagates known constants, references and types through the
 * control-flow graph. Known branch conditions select one successor; unknown
 * conditions preserve every successor, and conflicting facts become unknown
 * when paths merge.
 *
 * The result is used only as an inlining cost estimate. It does not change the
 * IL translated for the method.
 */

#ifndef MONO_LLVM_RUNTIME_IL_ANALYZER_HPP
#define MONO_LLVM_RUNTIME_IL_ANALYZER_HPP

#include <cstdint>
#include <optional>

typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodHeader MonoMethodHeader;

namespace mono {

struct ILReachability {
	/// Bytes in reachable basic blocks.
	uint32_t live_bytes;

	/// Conditional terminators resolved to one successor.
	uint32_t decided_branches;
};

/// Analyzes method in its generic context. Returns nullopt when the IL is
/// malformed or uses a construct the analyzer cannot model safely.
std::optional<ILReachability> analyze_il_reachability (MonoMethod *method,
                                                       MonoMethodHeader *header);

} // namespace mono

#endif
