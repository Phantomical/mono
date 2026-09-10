/**
 * \file
 * \brief The two ETW event payloads a test can compute without an ETW session.
 *
 * Declared outside etw-profiler.cpp's own HOST_WIN32 block, so a gtest that
 * links mini still finds them off Windows.
 */

#ifndef MONO_MINI_ETW_PROFILER_HPP
#define MONO_MINI_ETW_PROFILER_HPP

#include <cstdint>

typedef struct _MonoMethod MonoMethod;
typedef struct _MonoJitInfo MonoJitInfo;

namespace mono {

/// The CLR MethodFlags word (src/coreclr/inc/eventtracebase.h) for jinfo's
/// compile of method.
///
/// Jitted is always set. Generic comes from method, SharedGenericCode from
/// jinfo, and Dynamic from method. The tier bits at 7-9 are what this
/// backend maps jinfo's tier to.
uint32_t etw_method_flags (MonoMethod *method, MonoJitInfo *jinfo);

/// Fills \p il_offsets and \p native_offsets, in step, from jinfo's own
/// per-body map (MonoJitInfo::llvm_seq_points).
///
/// A run of rows sharing an IL offset collapses into the first of them.
/// Writes at most \p max_entries pairs, in the ascending native-offset order
/// llvm_seq_points already keeps, and returns how many it wrote.
///
/// Zero means jinfo has no per-body map of its own - see
/// etw-profiler.cpp's classic-tier-0 fallback for that case.
uint32_t etw_body_il_map (MonoJitInfo *jinfo, uint32_t *il_offsets,
                          uint32_t *native_offsets, uint32_t max_entries);

} // namespace mono

#endif /* MONO_MINI_ETW_PROFILER_HPP */
