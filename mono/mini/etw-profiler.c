// MIT License
//
// Copyright (c) 2021 Superluminal (www.superluminal.eu)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

/*
 * An ETW provider that speaks the CLR's own runtime and rundown manifests, so
 * PerfView, WPA and Superluminal attribute JIT'd code, IL offsets, module loads
 * and GC phases to a Unity player the way they do to a .NET process.
 *
 * Loaded like any other profiler: --profile=etw, or MONO_ENV_OPTIONS=--profile=etw
 * under an embedding host. mono_profiler_load () finds mono_profiler_init_etw ()
 * in the process's own modules, which is why it is exported.
 */

#include <config.h>

#if defined (HOST_WIN32)

#include "mini.h"

#include <glib.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/debug-internals.h>
#include <mono/metadata/debug-mono-ppdb.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/profiler.h>
#include <mono/metadata/unity-utils.h>
#include <mono/utils/mono-error-internals.h>
#include <mono/utils/mono-threads.h>

#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntcons.h>
#include <evntprov.h>
#include <evntrace.h>

/* The ManagedPdbSignature a module event carries when it has none. */
static const GUID null_guid = {0};

DECLSPEC_NOINLINE __inline VOID __stdcall Private_EventControlCallback (_In_ LPCGUID SourceId, _In_ ULONG ControlCode, _In_ UCHAR Level, _In_ ULONGLONG MatchAnyKeyword, _In_ ULONGLONG MatchAllKeyword, _In_opt_ PEVENT_FILTER_DESCRIPTOR FilterData, _Inout_opt_ PVOID CallbackContext);
#define MCGEN_PRIVATE_ENABLE_CALLBACK_V2 Private_EventControlCallback
#include "CLR-ETW-Generated.h"

/*
 * The generated header declares each provider's enable bits extern and
 * selectany, which is a definition only under C++. Compiled as C it is a
 * declaration alone, so this file, the header's only includer, defines them.
 */
DECLSPEC_CACHEALIGN ULONG Microsoft_Windows_DotNETRuntimeEnableBits[1] = {0};
DECLSPEC_CACHEALIGN ULONG Microsoft_Windows_DotNETRuntimeRundownEnableBits[1] = {0};

#define MAX_NUM_OFFSETS 7000
#define ENABLE_VERBOSE_LOGGING 0

static gboolean is_initialized = FALSE;

#if ENABLE_VERBOSE_LOGGING
	static void
	sLogInternal (const char* inFormat, ...)
	{
		va_list args;
		va_start (args, inFormat);

		char buffer[1024] = { 0 };
		vsprintf_s (buffer, 1024, inFormat, args);

		va_end (args);

		OutputDebugStringA (buffer);
	}

	#define ETW_PROFILER_LOG_ARGS(format, ...) sLogInternal (": "format"\n", __VA_ARGS__)
	#define ETW_PROFILER_LOG(str) sLogInternal ("ETW_PROFILER: %s\n", str)
#else
	#define ETW_PROFILER_LOG_ARGS(format, ...)
	#define ETW_PROFILER_LOG(str)
#endif

/* The CLR MethodFlags word (src/coreclr/inc/eventtracebase.h). */
#define METHOD_FLAGS_DYNAMIC 0x1
#define METHOD_FLAGS_GENERIC 0x2
#define METHOD_FLAGS_SHARED_GENERIC_CODE 0x4
#define METHOD_FLAGS_JITTED 0x8
#define METHOD_FLAGS_JIT_HELPER 0x10

/*
 * CoreCLR's PrepareCodeConfig::JitOptimizationTier (vm/method.hpp), which is
 * what bits 7-9 of MethodFlags hold. This runtime has one JIT and it compiles
 * every method at full optimization, which is Optimized (2). PerfView renders
 * it as such.
 */
#define METHOD_FLAGS_TIER_OPTIMIZED 2

/* Which flavour of an image/method event to emit. */
typedef enum {
	EVENT_KIND_LOAD,
	EVENT_KIND_UNLOAD,
	EVENT_KIND_DC_START,
	EVENT_KIND_DC_END
} EventKind;

/*
 * Which ETW enumeration events to emit and which subjects to walk. start and
 * end select rundown-provider events; load selects the runtime-provider one.
 */
typedef struct {
	gboolean start;
	gboolean end;
	gboolean load;
	gboolean images;
	gboolean methods;
} EtwRundownPass;

static uint32_t
etw_method_flags (MonoMethod *method, MonoJitInfo *jinfo)
{
	uint32_t flags = METHOD_FLAGS_JITTED;

	if (method->is_inflated)
		flags |= METHOD_FLAGS_GENERIC;

	if (jinfo->has_generic_jit_info
	    && mono_jit_info_get_generic_sharing_context (jinfo) != NULL)
		flags |= METHOD_FLAGS_SHARED_GENERIC_CODE;

	if (method->dynamic)
		flags |= METHOD_FLAGS_DYNAMIC;

	flags |= (METHOD_FLAGS_TIER_OPTIMIZED & 0x7) << 7;

	return flags;
}

/*
 * TraceEvent 3.2.6 discards a method-load event carrying neither JitHelper nor
 * Jitted (ShouldTrackMethodLoad (), TraceLog.cs). Once JitHelper is set it
 * reads the MethodName field alone, so a stub event needs no namespace and no
 * signature.
 */
static uint32_t
etw_stub_flags (void)
{
	return METHOD_FLAGS_JIT_HELPER;
}

/*
 * Fills il_offsets and native_offsets, in step, from the method-keyed debug
 * table. One IL instruction can lower to several native instructions, each
 * with its own native offset but the same IL offset; a run sharing an IL
 * offset collapses into the first of them, since a consumer only wants the
 * range each IL offset covers. Returns how many pairs it wrote.
 */
static uint32_t
etw_body_il_map (MonoDomain *domain, MonoMethod *method, uint32_t *il_offsets,
                 uint32_t *native_offsets, uint32_t max_entries)
{
	uint32_t count = 0;
	uint32_t last_il_offset = (uint32_t) -1;
	MonoDebugMethodJitInfo *dmji = mono_debug_find_method (method, domain);

	if (dmji == NULL)
		return 0;

	for (uint32_t i = 0; i < dmji->num_line_numbers && count < max_entries; ++i) {
		uint32_t il_offset = dmji->line_numbers [i].il_offset;

		if (il_offset == last_il_offset)
			continue;

		last_il_offset = il_offset;
		il_offsets [count] = il_offset;
		native_offsets [count] = dmji->line_numbers [i].native_offset;
		++count;
	}

	mono_debug_free_method_jit_info (dmji);

	return count;
}

/*
 * The full name of method's declaring class - namespace, nested-class chain
 * and generic instantiation included - the way CoreCLR fills the ETW
 * MethodNamespace field (MethodDesc::GetMethodInfoNoSig, vm/method.cpp).
 *
 * Paired with mono_method_get_name () for MethodName. TraceEvent joins the
 * two fields with "." on its own, so a class name in both doubles the
 * namespace. The caller frees the result with g_free ().
 */
static char *
etw_method_namespace (MonoMethod *method)
{
	return mono_type_get_full_name (mono_method_get_class (method));
}

/*
 * Decodes an ETW control notification into an enumeration pass. The provider
 * and enumeration keyword select the event kind. The loader and JIT keyword
 * bits select whether to enumerate images, methods, or both.
 */
static EtwRundownPass
etw_rundown_pass (ULONG control_code, ULONGLONG match_any_keyword, gboolean is_rundown_provider)
{
	EtwRundownPass pass;
	memset (&pass, 0, sizeof (pass));

	if (control_code != EVENT_CONTROL_CODE_ENABLE_PROVIDER
	    && control_code != EVENT_CONTROL_CODE_CAPTURE_STATE)
		return pass;

	if (is_rundown_provider) {
		pass.start = (match_any_keyword & CLR_RUNDOWNSTART_KEYWORD) != 0;
		pass.end = (match_any_keyword & CLR_RUNDOWNEND_KEYWORD) != 0;
	} else {
		/* Runtime-provider enumeration emits MethodLoad events. */
		pass.load = (match_any_keyword & CLR_STARTENUMERATION_KEYWORD) != 0;
	}

	if (!pass.start && !pass.end && !pass.load)
		return pass;

	/* The runtime and rundown providers share the loader and JIT keyword bits. */
	pass.images = (match_any_keyword & CLR_LOADER_KEYWORD) != 0;
	pass.methods = (match_any_keyword & CLR_JIT_KEYWORD) != 0;
	return pass;
}

/*
 * Check the generated per-event predicates before constructing payloads.
 * Provider IsEnabled alone does not account for level or keyword filters.
 */
static gboolean
method_event_enabled (EventKind kind)
{
	switch (kind) {
	case EVENT_KIND_DC_START:
		return EventEnabledMethodDCStartVerbose_V2 ();
	case EVENT_KIND_DC_END:
		return EventEnabledMethodDCEndVerbose_V2 ();
	default:
		return EventEnabledMethodLoadVerbose_V2 ();
	}
}

static gboolean
method_il_map_enabled (EventKind kind)
{
	switch (kind) {
	case EVENT_KIND_DC_START:
		return EventEnabledMethodDCStartILToNativeMap ();
	case EVENT_KIND_DC_END:
		return EventEnabledMethodDCEndILToNativeMap ();
	default:
		return EventEnabledMethodILToNativeMap ();
	}
}

static gboolean
image_event_enabled (EventKind kind)
{
	switch (kind) {
	case EVENT_KIND_DC_START:
		return EventEnabledModuleDCStart_V2 ();
	case EVENT_KIND_DC_END:
		return EventEnabledModuleDCEnd_V2 ();
	case EVENT_KIND_UNLOAD:
		return EventEnabledModuleUnload_V2 ();
	default:
		return EventEnabledModuleLoad_V2 ();
	}
}

static void
image_event (MonoImage *image, EventKind kind)
{
	if (!image_event_enabled (kind)) {
		ETW_PROFILER_LOG ("Module event not enabled, skipping image_event");
		return;
	}

	// Mono loads ppdb files as "images" marked with metadata-only. We can skip them as they
	// won't ever have executable code.
	if (image->metadata_only) {
		ETW_PROFILER_LOG ("Skipping image_event for metadata-only image");
		return;
	}

	const char *pdb_path = NULL;
	guint8 pe_guid [16] = {0};
	gint32 pe_age = 0;
	gint32 pe_timestamp = 0;

	// We only emit PDB info for loads *and* if the image is not a dynamic image (i.e. containing dynamic methods). This is becuase
	// dynamic images do not represent an actual on-disk image and so don't have any PDB info (calling mono_ppdb_get_signature will lead to a crash)
	if (kind != EVENT_KIND_UNLOAD && !mono_image_is_dynamic (image))
		mono_ppdb_get_signature (image, &pdb_path, pe_guid, &pe_age, &pe_timestamp);

	gunichar2 *image_path_utf16 = u8to16 (mono_image_get_filename (image));
	gunichar2 *pdb_path_utf16 = pdb_path != NULL ? u8to16 (pdb_path) : NULL;
	const wchar_t *pdb_path_arg = pdb_path_utf16 == NULL ? L"" : (const wchar_t *) pdb_path_utf16;

	MonoAssembly *assembly = mono_image_get_assembly (image);

	switch (kind) {
	case EVENT_KIND_DC_START:
		EventWriteModuleDCStart_V2 ((uint64_t) image, (uint64_t) assembly, 0, 0, (const wchar_t *) image_path_utf16, L"", 0, (GUID *) pe_guid, pe_age, pdb_path_arg, &null_guid, 0, L"");
		break;
	case EVENT_KIND_DC_END:
		EventWriteModuleDCEnd_V2 ((uint64_t) image, (uint64_t) assembly, 0, 0, (const wchar_t *) image_path_utf16, L"", 0, (GUID *) pe_guid, pe_age, pdb_path_arg, &null_guid, 0, L"");
		break;
	case EVENT_KIND_UNLOAD:
		EventWriteModuleUnload_V2 ((uint64_t) image, (uint64_t) assembly, 0, 0, (const wchar_t *) image_path_utf16, L"", 0, (GUID *) pe_guid, pe_age, pdb_path_arg, &null_guid, 0, L"");
		break;
	case EVENT_KIND_LOAD:
		EventWriteModuleLoad_V2 ((uint64_t) image, (uint64_t) assembly, 0, 0, (const wchar_t *) image_path_utf16, L"", 0, (GUID *) pe_guid, pe_age, pdb_path_arg, &null_guid, 0, L"");
		break;
	}

	g_free (image_path_utf16);
	g_free (pdb_path_utf16);
}

static void
image_loaded (MonoProfiler *prof, MonoImage *image)
{
	image_event (image, EVENT_KIND_LOAD);
}

static void
image_unloading (MonoProfiler *prof, MonoImage *image)
{
	image_event (image, EVENT_KIND_UNLOAD);
}

/*
 * Reports a trampoline, a thunk or a stub the way CoreCLR's
 * ETW::MethodLog::SendHelperEvent () (vm/eventtrace.cpp) reports a JIT helper.
 * The MethodID is the code start, there being no MonoMethod to name, and the
 * token is zero. The ModuleID is zero because TraceEvent answers a JitHelper
 * event with a synthetic generatedruntimehelpers module of its own
 * (GetOrCreateMethodModuleFile (), TraceLog.cs).
 *
 * No MethodILToNativeMap goes with it, because a stub has no IL.
 */
static void
stub_event (gpointer code, uint64_t size, const char *name, EventKind kind)
{
	if (!method_event_enabled (kind)) {
		ETW_PROFILER_LOG ("Method event not enabled, skipping stub_event");
		return;
	}

	if (code == NULL || size == 0)
		return;

	uint64_t start = (uint64_t) code;
	uint32_t flags = etw_stub_flags ();
	uint32_t code_size = (uint32_t) size;
	gunichar2 *name_utf16 = u8to16 (name != NULL ? name : "stub");

	switch (kind) {
	case EVENT_KIND_LOAD:
		EventWriteMethodLoadVerbose_V2 (start, 0, start, code_size, 0, flags, L"", (const wchar_t *) name_utf16, L"", 0, 0);
		break;
	case EVENT_KIND_DC_START:
		EventWriteMethodDCStartVerbose_V2 (start, 0, start, code_size, 0, flags, L"", (const wchar_t *) name_utf16, L"", 0, 0);
		break;
	case EVENT_KIND_DC_END:
		EventWriteMethodDCEndVerbose_V2 (start, 0, start, code_size, 0, flags, L"", (const wchar_t *) name_utf16, L"", 0, 0);
		break;
	case EVENT_KIND_UNLOAD:
		g_assert_not_reached ();
	}

	g_free (name_utf16);
}

/*
 * A code buffer's type is what names it in a trace, except a specific
 * trampoline, which carries its own name in data.
 */
static const char *
code_buffer_name (MonoProfilerCodeBufferType type, const void *data)
{
	switch (type) {
	case MONO_PROFILER_CODE_BUFFER_METHOD_TRAMPOLINE:
		return "method_trampoline";
	case MONO_PROFILER_CODE_BUFFER_UNBOX_TRAMPOLINE:
		return "unbox_trampoline";
	case MONO_PROFILER_CODE_BUFFER_IMT_TRAMPOLINE:
		return "imt_trampoline";
	case MONO_PROFILER_CODE_BUFFER_GENERICS_TRAMPOLINE:
		return "generics_trampoline";
	case MONO_PROFILER_CODE_BUFFER_SPECIFIC_TRAMPOLINE:
		return data != NULL ? (const char *) data : "specific_trampoline";
	case MONO_PROFILER_CODE_BUFFER_HELPER:
		return "helper";
	case MONO_PROFILER_CODE_BUFFER_MONITOR:
		return "monitor";
	case MONO_PROFILER_CODE_BUFFER_DELEGATE_INVOKE:
		return "delegate_invoke";
	case MONO_PROFILER_CODE_BUFFER_EXCEPTION_HANDLING:
		return "exception_handling";
	default:
		return "code_buffer";
	}
}

static void
code_buffer_new (MonoProfiler *prof, const mono_byte *buffer, uint64_t size, MonoProfilerCodeBufferType type, const void *data)
{
	/* A method's own body is reported by jit_done, with its MonoMethod. */
	if (type == MONO_PROFILER_CODE_BUFFER_METHOD)
		return;

	stub_event ((gpointer) buffer, size, code_buffer_name (type, data), EVENT_KIND_LOAD);
}

// These values are part of the CLR ETW schema and must not be renumbered.
#define GC_TYPE_NON_CONCURRENT 0

// The profiler callback does not identify what triggered the collection.
#define GC_REASON_ALLOC_SMALL 0

// The profiler callback only reports suspensions initiated by the GC.
#define GC_SUSPEND_REASON_FOR_GC 1

static void
handle_gc_event (MonoProfiler *prof, MonoProfilerGCEvent evt, uint32_t generation)
{
	// SGen reports generation 0 or 1, while Boehm always reports generation 0.
	// Collections of the same generation do not overlap.
	static uint32_t gc_count [2];

	if (generation > 1)
		generation = 1;

	/*
	 * This profiler API says nothing about whether a collection ran on the
	 * mutator's threads or concurrently with them, so every collection is
	 * reported as the stop-the-world kind.
	 */
	switch (evt) {
	case MONO_GC_EVENT_PRE_STOP_WORLD:
		EventWriteGCSuspendEEBegin_V1 (GC_SUSPEND_REASON_FOR_GC, gc_count [generation] + 1, 0);
		break;
	case MONO_GC_EVENT_POST_STOP_WORLD:
		EventWriteGCSuspendEEEnd_V1 (0);
		break;
	case MONO_GC_EVENT_START:
		++gc_count [generation];
		EventWriteGCStart_V2 (gc_count [generation], generation, GC_REASON_ALLOC_SMALL, GC_TYPE_NON_CONCURRENT, 0, 0);
		break;
	case MONO_GC_EVENT_END:
		EventWriteGCEnd_V1 (gc_count [generation], generation, 0);
		break;
	case MONO_GC_EVENT_PRE_START_WORLD:
		EventWriteGCRestartEEBegin_V1 (0);
		break;
	case MONO_GC_EVENT_POST_START_WORLD:
		EventWriteGCRestartEEEnd_V1 (0);
		break;
	default:
		// The CLR schema does not distinguish the locked and unlocked portions
		// of the suspend and restart sequences.
		break;
	}
}

static void
method_load (MonoDomain *domain, MonoMethod *method, MonoJitInfo *jinfo, EventKind kind)
{
	static __declspec(thread) unsigned int il_offsets [MAX_NUM_OFFSETS] = {0};
	static __declspec(thread) unsigned int native_offsets [MAX_NUM_OFFSETS] = {0};

	if (!method_event_enabled (kind)) {
		ETW_PROFILER_LOG ("Method event not enabled, skipping method_load");
		return;
	}

	/*
	 * A trampoline has no MonoMethod, so none of the fields below exist for
	 * one: no token, signature, declaring class or IL map. MonoJitInfo::d
	 * holds the MonoTrampInfo once is_trampoline is set.
	 */
	if (jinfo->is_trampoline) {
		MonoTrampInfo *tramp_info = (MonoTrampInfo *) jinfo->d.tramp_info;
		stub_event (mono_jit_info_get_code_start (jinfo),
		            (uint64_t) mono_jit_info_get_code_size (jinfo),
		            tramp_info != NULL ? tramp_info->name : "trampoline", kind);
		return;
	}

	if (method == NULL)
		return;

	int compressed_num_lines = method_il_map_enabled (kind)
		? (int) etw_body_il_map (domain, method, il_offsets, native_offsets, MAX_NUM_OFFSETS)
		: 0;

	MonoClass *klass = mono_method_get_class (method);
	char *signature = mono_signature_get_desc (mono_method_signature (method), TRUE);
	char *class_full_name = etw_method_namespace (method);
	const char *method_name = mono_method_get_name (method);
	gpointer code_start = mono_jit_info_get_code_start (jinfo);
	int code_size = mono_jit_info_get_code_size (jinfo);
	MonoImage *image = mono_class_get_image (klass);
	uint32_t method_token = mono_unity_method_get_token (method);
	uint32_t method_flags = etw_method_flags (method, jinfo);

	gunichar2 *namespace_utf16 = u8to16 (class_full_name);
	gunichar2 *method_name_utf16 = u8to16 (method_name);
	gunichar2 *signature_utf16 = u8to16 (signature);

	// An empty map is noise no consumer can use.
	switch (kind) {
	case EVENT_KIND_DC_START:
		EventWriteMethodDCStartVerbose_V2 ((uint64_t) method, (uint64_t) image, (uint64_t) code_start, code_size, method_token, method_flags, (const wchar_t *) namespace_utf16, (const wchar_t *) method_name_utf16, (const wchar_t *) signature_utf16, 0, 0);
		if (compressed_num_lines > 0)
			EventWriteMethodDCStartILToNativeMap ((uint64_t) method, 0, 0, compressed_num_lines, il_offsets, native_offsets, 0);
		break;
	case EVENT_KIND_DC_END:
		EventWriteMethodDCEndVerbose_V2 ((uint64_t) method, (uint64_t) image, (uint64_t) code_start, code_size, method_token, method_flags, (const wchar_t *) namespace_utf16, (const wchar_t *) method_name_utf16, (const wchar_t *) signature_utf16, 0, 0);
		if (compressed_num_lines > 0)
			EventWriteMethodDCEndILToNativeMap ((uint64_t) method, 0, 0, compressed_num_lines, il_offsets, native_offsets, 0);
		break;
	default:
		// Always EVENT_KIND_LOAD: method_load () is never called with ::unload.
		EventWriteMethodLoadVerbose_V2 ((uint64_t) method, (uint64_t) image, (uint64_t) code_start, code_size, method_token, method_flags, (const wchar_t *) namespace_utf16, (const wchar_t *) method_name_utf16, (const wchar_t *) signature_utf16, 0, 0);
		if (compressed_num_lines > 0)
			EventWriteMethodILToNativeMap ((uint64_t) method, 0, 0, compressed_num_lines, il_offsets, native_offsets, 0);
		break;
	}

	g_free (signature_utf16);
	g_free (method_name_utf16);
	g_free (namespace_utf16);
	g_free (class_full_name);
	g_free (signature);
}

typedef struct {
	int mNumDomains;
	int mNumAssemblies;
	int mNumMethods;
	EtwRundownPass pass;
} JITEnumerationData;

/*
 * MethodJittingStarted and MethodLoadVerbose use the same MethodID, allowing
 * ETW consumers to measure the time between compilation and publication. A
 * failed compilation has no corresponding method-load event.
 */
static void
jit_begin (MonoProfiler *prof, MonoMethod *method)
{
	if (!EventEnabledMethodJittingStarted ())
		return;

	MonoError error;
	error_init (&error);
	MonoMethodHeader *header = mono_method_get_header_checked (method, &error);
	uint32_t il_size = 0;

	if (header != NULL) {
		il_size = header->code_size;
		mono_metadata_free_mh (header);
	} else {
		mono_error_cleanup (&error);
	}

	MonoClass *klass = mono_method_get_class (method);
	MonoImage *image = mono_class_get_image (klass);
	uint32_t method_token = mono_unity_method_get_token (method);
	char *signature = mono_signature_get_desc (mono_method_signature (method), TRUE);
	char *class_full_name = etw_method_namespace (method);
	const char *method_name = mono_method_get_name (method);

	gunichar2 *namespace_utf16 = u8to16 (class_full_name);
	gunichar2 *method_name_utf16 = u8to16 (method_name);
	gunichar2 *signature_utf16 = u8to16 (signature);

	EventWriteMethodJittingStarted ((uint64_t) method, (uint64_t) image, method_token, il_size,
	                                (const wchar_t *) namespace_utf16, (const wchar_t *) method_name_utf16, (const wchar_t *) signature_utf16);

	g_free (signature_utf16);
	g_free (method_name_utf16);
	g_free (namespace_utf16);
	g_free (class_full_name);
	g_free (signature);
}

static void
method_jit_done (MonoProfiler *prof, MonoMethod *method, MonoJitInfo *jinfo)
{
	/*
	 * A shared generic body is compiled once and jit_done names the
	 * instantiation that asked for it. The jinfo names the body itself, which
	 * is also what a rundown walk hands out and what the debug table keys the
	 * IL map by, so the same MethodID reaches the trace either way.
	 */
	MonoMethod *jitted = mono_jit_info_get_method (jinfo);

	if (jitted != NULL)
		method = jitted;

	method_load (mono_domain_get (), method, jinfo, EVENT_KIND_LOAD);
}

static void
on_enumerate_assembly (MonoAssembly *assembly, void *user_data)
{
	JITEnumerationData *enumerationData = (JITEnumerationData *) user_data;
	enumerationData->mNumAssemblies++;

	MonoImage *image = mono_assembly_get_image (assembly);
	if (enumerationData->pass.start)
		image_event (image, EVENT_KIND_DC_START);
	if (enumerationData->pass.end)
		image_event (image, EVENT_KIND_DC_END);
	if (enumerationData->pass.load)
		image_event (image, EVENT_KIND_LOAD);
}

static void
on_enumerate_jit_method (MonoDomain *domain, MonoMethod *method, MonoJitInfo *jinfo, void *user_data)
{
	JITEnumerationData *enumerationData = (JITEnumerationData *) user_data;
	enumerationData->mNumMethods++;

	if (enumerationData->pass.start)
		method_load (domain, method, jinfo, EVENT_KIND_DC_START);
	if (enumerationData->pass.end)
		method_load (domain, method, jinfo, EVENT_KIND_DC_END);
	if (enumerationData->pass.load)
		method_load (domain, method, jinfo, EVENT_KIND_LOAD);
}

static void
on_enumerate_domain (MonoDomain *domain, void *user_data)
{
	JITEnumerationData *enumerationData = (JITEnumerationData *) user_data;
	enumerationData->mNumDomains++;

	if (enumerationData->pass.images)
		mono_domain_assembly_foreach (domain, on_enumerate_assembly, enumerationData);
	if (enumerationData->pass.methods)
		mono_domain_jit_foreach (domain, on_enumerate_jit_method, enumerationData);
}

static void
on_attach (EtwRundownPass pass)
{
	ETW_PROFILER_LOG ("Enumerating JIT data...");

	JITEnumerationData enumerationData;
	memset (&enumerationData, 0, sizeof (enumerationData));
	enumerationData.pass = pass;

	if (pass.start)
		EventWriteDCStartInit_V1 (0);
	if (pass.end)
		EventWriteDCEndInit_V1 (0);

	/*
	 * Synchronous, like CoreCLR's own rundown, even though Microsoft's
	 * PENABLECALLBACK contract says not to block on a lock here.
	 *
	 * The JIT table walk holds the domain lock for the whole walk, so what
	 * it excludes is every JIT compile and class load in that domain until
	 * it returns. A player keeps one domain for its whole life and a rundown
	 * happens once per trace session, so that stall is paid once, at attach.
	 */
	mono_domain_foreach (on_enumerate_domain, &enumerationData);

	if (pass.start)
		EventWriteDCStartComplete_V1 (0);
	if (pass.end)
		EventWriteDCEndComplete_V1 (0);

	ETW_PROFILER_LOG_ARGS ("Finished enumerating JIT data. Found %d domains, %d assemblies, %d methods", enumerationData.mNumDomains, enumerationData.mNumAssemblies, enumerationData.mNumMethods);
}

// This callback is called by the ETW system when tracing is started / stopped. We use it to enumerate & output information about JIT compilation that happened *before* tracing started.
DECLSPEC_NOINLINE __inline VOID __stdcall Private_EventControlCallback (_In_ LPCGUID SourceId, _In_ ULONG ControlCode, _In_ UCHAR Level, _In_ ULONGLONG MatchAnyKeyword, _In_ ULONGLONG MatchAllKeyword, _In_opt_ PEVENT_FILTER_DESCRIPTOR FilterData, _Inout_opt_ PVOID CallbackContext)
{
	ETW_PROFILER_LOG_ARGS ("EventControlCallback (%d)", ControlCode);

	// Windows can call this synchronously from inside EventRegister* (),
	// for a session that already has the provider enabled, before
	// is_initialized is set. There is nothing to walk yet.
	if (!is_initialized)
		return;

	gboolean isRundown = (CallbackContext == &MICROSOFT_WINDOWS_DOTNETRUNTIME_RUNDOWN_PROVIDER_Context);
	EtwRundownPass pass = etw_rundown_pass (ControlCode, MatchAnyKeyword, isRundown);

	ETW_PROFILER_LOG_ARGS ("EventControlCallback -- IsRundown: %s, StartPass: %s, EndPass: %s, LoadPass: %s, Images: %s, Methods: %s",
	                       isRundown ? "true" : "false", pass.start ? "true" : "false",
	                       pass.end ? "true" : "false", pass.load ? "true" : "false",
	                       pass.images ? "true" : "false", pass.methods ? "true" : "false");

	if (pass.images || pass.methods) {
		/*
		 * ETW invokes this on a worker thread of its own. The walk reads
		 * runtime structures that expect an attached thread, so attach it
		 * for the walk and detach after, unless it was attached already.
		 */
		gboolean attached_here = mono_thread_info_current_unchecked () == NULL;

		if (attached_here)
			mono_thread_info_attach ();

		on_attach (pass);

		if (attached_here)
			mono_thread_info_detach ();
	}
}

static void
mono_profiler_cleanup_etw (MonoProfiler *prof)
{
	EventUnregisterMicrosoft_Windows_DotNETRuntimeRundown ();
	EventUnregisterMicrosoft_Windows_DotNETRuntime ();
}

/* the entry point */
MONO_API void
mono_profiler_init_etw (const char *desc)
{
	ETW_PROFILER_LOG ("Initializing Plugin");

	/*
	 * The IL map is read out of the debug table, which exists only once
	 * debugging is on. A player without the debugger turns it on here so
	 * every method compiled from now on gets a line table.
	 */
	if (!mono_debug_enabled ())
		mono_debug_init (MONO_DEBUG_FORMAT_MONO);

	EventRegisterMicrosoft_Windows_DotNETRuntime ();
	EventRegisterMicrosoft_Windows_DotNETRuntimeRundown ();

	MonoProfilerHandle handle = mono_profiler_create (NULL);
	mono_profiler_set_image_loaded_callback (handle, image_loaded);
	mono_profiler_set_image_unloading_callback (handle, image_unloading);
	mono_profiler_set_jit_begin_callback (handle, jit_begin);
	mono_profiler_set_jit_done_callback (handle, method_jit_done);
	mono_profiler_set_jit_code_buffer_callback (handle, code_buffer_new);
	mono_profiler_set_gc_event_callback (handle, handle_gc_event);
	mono_profiler_set_cleanup_callback (handle, mono_profiler_cleanup_etw);

	is_initialized = TRUE;

	ETW_PROFILER_LOG ("Plugin Initialized");
}

#endif /* HOST_WIN32 */
