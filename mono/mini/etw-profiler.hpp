/**
 * \file
 * \brief The ETW event payloads a test can compute without an ETW session.
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

/// The CLR MethodFlags word for a code range that is not a method body: a
/// trampoline, a thunk or a stub.
///
/// TraceEvent 3.2.6 discards a method-load event carrying neither JitHelper
/// nor Jitted (ShouldTrackMethodLoad (), TraceLog.cs). Once JitHelper is set
/// it reads the MethodName field alone, so a stub event needs no namespace
/// and no signature.
uint32_t etw_stub_flags ();

/// Fills \p il_offsets and \p native_offsets, in step, from jinfo's own
/// per-body map (MonoJitInfo::il_offsets).
///
/// A run of rows sharing an IL offset collapses into the first of them.
/// Writes at most \p max_entries pairs, in the ascending native-offset order
/// il_offsets already keeps, and returns how many it wrote.
uint32_t etw_body_il_map (MonoJitInfo *jinfo, uint32_t *il_offsets,
                          uint32_t *native_offsets, uint32_t max_entries);

/// Returns the full name of \p method's declaring class - namespace,
/// nested-class chain and generic instantiation included - the way
/// CoreCLR's TypeString::AppendType fills the ETW MethodNamespace field
/// (MethodDesc::GetMethodInfoNoSig, vm/method.cpp).
///
/// Pair it with mono_method_get_name () for MethodName. TraceLog.cs
/// (TraceEvent) joins the two fields with "." on its own, so a class name
/// in both doubles the namespace.
///
/// Allocates. The caller frees the result with g_free ().
char *etw_method_namespace (MonoMethod *method);

/// Describes which ETW enumeration events to emit and which subjects to walk.
/// \c start and \c end select rundown-provider events; \c load selects the
/// runtime-provider event.
struct EtwRundownPass {
	bool start = false;
	bool end = false;
	bool load = false;
	bool images = false;
	bool methods = false;
};

/// Decodes an ETW control notification into an enumeration pass.
///
/// The provider and enumeration keyword select the event kind. The loader and
/// JIT keyword bits select whether to enumerate images, methods, or both.
EtwRundownPass etw_rundown_pass (uint32_t control_code, uint64_t match_any_keyword,
                                 bool is_rundown_provider);

} // namespace mono

#endif /* MONO_MINI_ETW_PROFILER_HPP */
