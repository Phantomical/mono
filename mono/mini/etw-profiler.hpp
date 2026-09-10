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

/// The full name of \p method's declaring class - namespace, nested-class
/// chain and generic instantiation included - for the ETW MethodNamespace
/// field, the way CoreCLR's TypeString::AppendType fills it
/// (MethodDesc::GetMethodInfoNoSig, vm/method.cpp). Pair it with
/// mono_method_get_name () for MethodName: TraceLog.cs (TraceEvent) joins
/// the two fields with "." on its own, so a class name in both doubles the
/// namespace.
///
/// Allocates. The caller frees the result with g_free ().
char *etw_method_namespace (MonoMethod *method);

/// The rundown pass(es) to run: start, end, or both.
struct EtwRundownPass {
	bool start = false;
	bool end = false;
};

/// Which rundown pass(es) \p match_any_keyword asks for, per
/// CLR-ETW-Generated.h: start for CLR_RUNDOWNSTART_KEYWORD (0x40), end for
/// CLR_RUNDOWNEND_KEYWORD (0x100), either or both.
///
/// \p control_code has to be ENABLE_PROVIDER or CAPTURE_STATE, and
/// \p is_rundown_provider has to be true - both carry the keyword the same
/// way. Anything else answers with neither pass set.
EtwRundownPass etw_rundown_pass (uint32_t control_code, uint64_t match_any_keyword,
                                 bool is_rundown_provider);

} // namespace mono

#endif /* MONO_MINI_ETW_PROFILER_HPP */
