/**
 * \file
 * Runtime code for the JIT
 *
 * Authors:
 *   Paolo Molaro (lupus@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * Copyright 2002-2003 Ximian, Inc.
 * Copyright 2003-2010 Novell, Inc.
 * Copyright 2011-2015 Xamarin, Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include <config.h>
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <math.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <signal.h>

#include <mono/utils/memcheck.h>

#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/class.h>
#include <mono/metadata/object.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/environment.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/gc-internals.h>
#include <mono/metadata/threads-types.h>
#include <mono/metadata/mempool-internals.h>
#include <mono/metadata/attach.h>
#include <mono/metadata/runtime.h>
#include <mono/metadata/reflection-internals.h>
#include <mono/metadata/monitor.h>
#include <mono/metadata/icall-internals.h>
#include <mono/metadata/loader-internals.h>
#define MONO_MATH_DECLARE_ALL 1
#include <mono/utils/mono-math.h>
#include <mono/utils/mono-compiler.h>
#include <mono/utils/mono-counters.h>
#include <mono/utils/mono-error-internals.h>
#include <mono/utils/mono-logger-internals.h>
#include <mono/utils/mono-mmap.h>
#include <mono/utils/mono-path.h>
#include <mono/utils/mono-tls.h>
#include <mono/utils/mono-hwcap.h>
#include <mono/utils/dtrace.h>
#include <mono/utils/mono-signal-handler.h>
#include <mono/utils/mono-threads.h>
#include <mono/utils/mono-threads-coop.h>
#include <mono/utils/checked-build.h>
#include <mono/utils/mono-compiler.h>
#include <mono/utils/mono-proclib.h>
#include <mono/utils/mono-state.h>
#include <mono/utils/mono-time.h>
#include <mono/metadata/w32handle.h>
#include <mono/metadata/threadpool.h>

#ifdef ENABLE_PERFTRACING
#include <eventpipe/ep.h>
#include <eventpipe/ds-server.h>
#endif

#include "mini.h"
#include "seq-points.h"
#include "tasklets.h"
#include <string.h>
#include <ctype.h>
#include "trace.h"
#ifndef ENABLE_NETCORE
#include "version.h"
#endif
#include "aot-runtime.h"
#include "llvmonly-runtime.h"

#include "jit-icalls.h"

#include "mini-gc.h"
#include "debugger-agent.h"
#include "debugger-engine.h"
#include "lldb.h"
#include "mini-runtime.h"
#include "mono/interp/interp.h"
#include "mixed_callstack_plugin.h"

#if defined(ENABLE_LLVM) && defined(HAVE_UNWIND_H)
#include <unwind.h>
/* Registered as a JIT icall below; defined in mini-exceptions.c. */
G_EXTERN_C _Unwind_Reason_Code mono_debug_personality (int a, _Unwind_Action b,
	uint64_t c, struct _Unwind_Exception *d, struct _Unwind_Context *e);
#endif
#include "../llvm/runtime.h"
#include "domain-method.h"
#include "method-override.h"
#include "../llvm/debugging/perf/perf.h"
#include "mono/metadata/icall-signatures.h"
#include "mono/utils/mono-tls-inline.h"

static guint32 default_opt = 0;
static gboolean default_opt_set = FALSE;
MonoMethodDesc *mono_stats_method_desc;

gboolean mono_compile_aot = FALSE;
/* If this is set, no code is generated dynamically, everything is taken from AOT files */
gboolean mono_aot_only = FALSE;
/* Same as mono_aot_only, but only LLVM compiled code is used, no trampolines */
gboolean mono_llvm_only = FALSE;
/* By default, don't require AOT but attempt to probe */
MonoAotMode mono_aot_mode = MONO_AOT_MODE_NORMAL;
MonoEEFeatures mono_ee_features;

const char *mono_build_date;
gboolean mono_do_signal_chaining;
gboolean mono_do_crash_chaining;
int mini_verbose = 0;

/*
 * This flag controls whenever the runtime uses LLVM for JIT compilation, and whenever
 * it can load AOT code compiled by LLVM.
 *
 * The value here is only the pre-startup one; mini_init () resolves the real
 * default (on for JIT runs, off for the AOT compiler) unless an embedder already
 * picked a side through mono_set_use_llvm ().
 */
gboolean mono_use_llvm = FALSE;

/*
 * TRUE once somebody named LLVM explicitly rather than taking whatever mini_init ()
 * defaults to. Two things want to tell those apart: the default must not clobber a
 * choice that was already made, and only a deliberate mono_set_use_llvm (TRUE) means
 * "prefer LLVM code over anything else", which is what the AOT image usability check
 * keys off.
 */
static gboolean use_llvm_explicit;

gboolean mono_use_fast_math = FALSE;

// Lists of allowlisted and blocklisted CPU features 
MonoCPUFeatures mono_cpu_features_enabled = (MonoCPUFeatures)0;

#ifdef DISABLE_SIMD
MonoCPUFeatures mono_cpu_features_disabled = MONO_CPU_X86_FULL_SSEAVX_COMBINED;
#else
MonoCPUFeatures mono_cpu_features_disabled = (MonoCPUFeatures)0;
#endif

gboolean mono_use_interpreter = FALSE;
const char *mono_interp_opts_string = NULL;

#define mono_jit_lock() mono_os_mutex_lock (&jit_mutex)
#define mono_jit_unlock() mono_os_mutex_unlock (&jit_mutex)
static mono_mutex_t jit_mutex;

static MonoCodeManager *global_codeman;

MonoDebugOptions mini_debug_options;
char *sdb_options;

#ifdef VALGRIND_JIT_REGISTER_MAP
int valgrind_register;
#endif

static GPtrArray *profile_options;

static GSList *tramp_infos;
GSList *mono_interp_only_classes;

static void register_icalls (void);
static void runtime_cleanup (MonoDomain *domain, gpointer user_data);
#ifdef ENABLE_METADATA_UPDATE
static void mini_metadata_update_init (MonoError *error);
static void mini_invalidate_transformed_interp_methods (MonoDomain *domain, MonoAssemblyLoadContext *alc, uint32_t generation);
#endif


gboolean
mono_running_on_valgrind (void)
{
#ifndef HOST_WIN32
	if (RUNNING_ON_VALGRIND){
#ifdef VALGRIND_JIT_REGISTER_MAP
		valgrind_register = TRUE;
#endif
		return TRUE;
	} else
#endif
		return FALSE;
}

void
mono_set_use_llvm (mono_bool use_llvm)
{
	mini_set_use_llvm ((gboolean)use_llvm);
}

/*
 * The runtime-internal half of mono_set_use_llvm (), which is embedder API and
 * deprecated for our own use.
 */
void
mini_set_use_llvm (gboolean use_llvm)
{
	mono_use_llvm = use_llvm;
	use_llvm_explicit = TRUE;
}

gboolean
mini_use_llvm_explicitly_set (void)
{
	return use_llvm_explicit;
}


typedef struct {
	void *ip;
	MonoMethod *method;
} FindTrampUserData;

static void
find_tramp (gpointer key, gpointer value, gpointer user_data)
{
	FindTrampUserData *ud = (FindTrampUserData*)user_data;

	if (value == ud->ip)
		ud->method = (MonoMethod*)key;
}

/* debug function */
char*
mono_get_method_from_ip (void *ip)
{
	MonoJitInfo *ji;
	MonoMethod *method;
	char *method_name;
	char *res;
	MonoDomain *domain = mono_domain_get ();
	MonoDebugSourceLocation *location;
	FindTrampUserData user_data;

	if (!domain)
		domain = mono_get_root_domain ();

	ji = mono_jit_info_table_find_internal (domain, ip, TRUE, TRUE);
	if (!ji) {
		user_data.ip = ip;
		user_data.method = NULL;
		mono_domain_lock (domain);
		g_hash_table_foreach (domain_jit_info (domain)->jit_trampoline_hash, find_tramp, &user_data);
		mono_domain_unlock (domain);
		if (user_data.method) {
			char *mname = mono_method_full_name (user_data.method, TRUE);
			res = g_strdup_printf ("<%p - JIT trampoline for %s>", ip, mname);
			g_free (mname);
			return res;
		}
		else
			return NULL;
	} else if (ji->is_trampoline) {
		res = g_strdup_printf ("<%p - %s trampoline>", ip, ji->d.tramp_info->name);
		return res;
	}

	method = jinfo_get_method (ji);
	method_name = mono_method_get_name_full (method, TRUE, FALSE, MONO_TYPE_NAME_FORMAT_IL);
	location = mono_jinfo_lookup_source_location (domain, ji, (guint32)((guint8*)ip - (guint8*)ji->code_start));

	char *file_loc = NULL;
	if (location)
		file_loc = g_strdup_printf ("[%s :: %du]", location->source_file, location->row);

	const char *in_interp = ji->is_interp ? " interp" : "";

	res = g_strdup_printf (" %s [{%p} + 0x%x%s] %s (%p %p) [%p - %s]", method_name, method, (int)((char*)ip - (char*)ji->code_start), in_interp, file_loc ? file_loc : "", ji->code_start, (char*)ji->code_start + ji->code_size, domain, domain->friendly_name);

	mono_debug_free_source_location (location);
	g_free (method_name);
	g_free (file_loc);

	return res;
}

/**
 * mono_pmip:
 * \param ip an instruction pointer address
 *
 * This method is used from a debugger to get the name of the
 * method at address \p ip.   This routine is typically invoked from
 * a debugger like this:
 *
 * (gdb) print mono_pmip ($pc)
 *
 * \returns the name of the method at address \p ip.
 */
G_GNUC_UNUSED char *
mono_pmip (void *ip)
{
	return mono_get_method_from_ip (ip);
}

/**
 * mono_print_method_from_ip:
 * \param ip an instruction pointer address
 *
 * This method is used from a debugger to get the name of the
 * method at address \p ip.
 *
 * This prints the name of the method at address \p ip in the standard
 * output.  Unlike \c mono_pmip which returns a string, this routine
 * prints the value on the standard output.
 */
MONO_ATTR_USED void
mono_print_method_from_ip (void *ip)
{
	MonoJitInfo *ji;
	char *method;
	MonoDebugSourceLocation *source;
	MonoDomain *domain = mono_domain_get ();
	MonoDomain *target_domain = mono_domain_get ();
	FindTrampUserData user_data;
	MonoGenericSharingContext*gsctx;
	const char *shared_type;

	if (!domain)
		domain = mono_get_root_domain ();
	ji = mini_jit_info_table_find_ext (domain, (char *)ip, TRUE, &target_domain);
	if (ji && ji->is_trampoline) {
		MonoTrampInfo *tinfo = ji->d.tramp_info;

		printf ("IP %p is at offset 0x%x of trampoline '%s'.\n", ip, (int)((guint8*)ip - tinfo->code), tinfo->name);
		return;
	}

	if (!ji) {
		user_data.ip = ip;
		user_data.method = NULL;
		mono_domain_lock (domain);
		g_hash_table_foreach (domain_jit_info (domain)->jit_trampoline_hash, find_tramp, &user_data);
		mono_domain_unlock (domain);

		if (user_data.method) {
			char *mname = mono_method_full_name (user_data.method, TRUE);
			printf ("IP %p is a JIT trampoline for %s\n", ip, mname);
			g_free (mname);
			return;
		}

		g_print ("No method at %p\n", ip);
		fflush (stdout);
		return;
	}
	method = mono_method_full_name (jinfo_get_method (ji), TRUE);
	source = mono_jinfo_lookup_source_location (target_domain, ji, (guint32)((guint8*)ip - (guint8*)ji->code_start));

	gsctx = mono_jit_info_get_generic_sharing_context (ji);
	shared_type = "";
	if (gsctx) {
		if (gsctx->is_gsharedvt)
			shared_type = "gsharedvt ";
		else
			shared_type = "gshared ";
	}

	g_print ("IP %p at offset 0x%x of %smethod %s (%p %p)[domain %p - %s]\n", ip, (int)((char*)ip - (char*)ji->code_start), shared_type, method, ji->code_start, (char*)ji->code_start + ji->code_size, target_domain, target_domain->friendly_name);

	if (source)
		g_print ("%s:%d\n", source->source_file, source->row);
	fflush (stdout);

	mono_debug_free_source_location (source);
	g_free (method);
}

/*
 * mono_method_same_domain:
 *
 * Determine whenever two compiled methods are in the same domain, thus
 * the address of the callee can be embedded in the caller.
 */
gboolean mono_method_same_domain (MonoJitInfo *caller, MonoJitInfo *callee)
{
	if (!caller || caller->is_trampoline || !callee || callee->is_trampoline)
		return FALSE;

	/*
	 * If the call was made from domain-neutral to domain-specific
	 * code, we can't patch the call site.
	 */
	if (caller->domain_neutral && !callee->domain_neutral)
		return FALSE;

#ifndef ENABLE_NETCORE
	MonoMethod *cmethod;

	cmethod = jinfo_get_method (caller);
	if ((cmethod->klass == mono_defaults.appdomain_class) &&
		(strstr (cmethod->name, "InvokeInDomain"))) {
		 /* The InvokeInDomain methods change the current appdomain */
		return FALSE;
	}
#endif
	return TRUE;
}

/*
 * mono_global_codeman_reserve:
 *
 *  Allocate code memory from the global code manager.
 */
void *(mono_global_codeman_reserve) (int size)
{
	void *ptr;

	if (mono_aot_only)
		g_error ("Attempting to allocate from the global code manager while running in aot-only mode.\n");

	/* Nothing reads these bytes. They keep the next trampoline out of the range
	 * a perf dump gives this one's frame description. */
	size += mono_llvm_perf_code_slack ();

	if (!global_codeman) {
		/* This can happen during startup */
		global_codeman = mono_code_manager_new ();
		return mono_code_manager_reserve (global_codeman, size);
	}
	else {
		mono_jit_lock ();
		ptr = mono_code_manager_reserve (global_codeman, size);
		mono_jit_unlock ();
		return ptr;
	}
}

/* The callback shouldn't take any locks */
void
mono_global_codeman_foreach (MonoCodeManagerFunc func, void *user_data)
{
	mono_jit_lock ();
	mono_code_manager_foreach (global_codeman, func, user_data);
	mono_jit_unlock ();
}

/**
 * mono_create_unwind_op:
 *
 *   Create an unwind op with the given parameters.
 */
MonoUnwindOp*
mono_create_unwind_op (int when, int tag, int reg, int val)
{
	MonoUnwindOp *op = g_new0 (MonoUnwindOp, 1);

	op->op = tag;
	op->reg = reg;
	op->val = val;
	op->when = when;

	return op;
}

MonoJumpInfoToken *
mono_jump_info_token_new2 (MonoMemPool *mp, MonoImage *image, guint32 token, MonoGenericContext *context)
{
	MonoJumpInfoToken *res = (MonoJumpInfoToken *)mono_mempool_alloc0 (mp, sizeof (MonoJumpInfoToken));
	res->image = image;
	res->token = token;
	res->has_context = context != NULL;
	if (context)
		memcpy (&res->context, context, sizeof (MonoGenericContext));

	return res;
}

MonoJumpInfoToken *
mono_jump_info_token_new (MonoMemPool *mp, MonoImage *image, guint32 token)
{
	return mono_jump_info_token_new2 (mp, image, token, NULL);
}

/*
 * mono_tramp_info_create:
 *
 *   Create a MonoTrampInfo structure from the arguments. This function assumes ownership
 * of JI, and UNWIND_OPS.
 */
MonoTrampInfo*
mono_tramp_info_create (const char *name, guint8 *code, guint32 code_size, MonoJumpInfo *ji, GSList *unwind_ops)
{
	MonoTrampInfo *info = g_new0 (MonoTrampInfo, 1);

	info->name = g_strdup (name);
	info->code = code;
	info->code_size = code_size;
	info->ji = ji;
	info->unwind_ops = unwind_ops;

	return info;
}

void
mono_tramp_info_free (MonoTrampInfo *info)
{
	g_free (info->name);

	// FIXME: ji
	mono_free_unwind_info (info->unwind_ops);
	if (info->owns_uw_info)
		g_free (info->uw_info);
	g_free (info);
}

static void
init_trampoline_jit_info (MonoJitInfo *ji, MonoTrampInfo *info)
{
	mono_jit_info_init (ji, NULL, (guint8*)MINI_FTNPTR_TO_ADDR (info->code), info->code_size, (MonoJitInfoFlags)0, 0, 0);
	ji->d.tramp_info = info;
	ji->is_trampoline = TRUE;

	ji->unwind_info = mono_cache_unwind_info (info->uw_info, info->uw_info_len);
}

static void
register_trampoline_jit_info (MonoDomain *domain, MonoTrampInfo *info)
{
	MonoJitInfo *ji;

	ji = (MonoJitInfo *)mono_domain_alloc0 (domain, mono_jit_info_size ((MonoJitInfoFlags)0, 0, 0));
	init_trampoline_jit_info (ji, info);

	mono_jit_info_table_add (domain, ji);
}

/*
 * mono_tramp_info_register_reclaimable:
 *
 *   Register a trampoline jit-info record for METHOD covering CODE_SIZE bytes at
 * CODE, and hand it back so the caller can unregister it again. UW_INFO is that
 * code's already-encoded unwind program; the caller keeps owning it and it has
 * to outlive the record.
 *
 * The record, the MonoTrampInfo behind it and NAME are one allocation, from the
 * allocator mono_jit_info_table_remove () frees with - so removing the record
 * takes all three, and takes them under the same hazard-pointer delay that makes
 * removal safe against a lookup already in flight.
 */
MonoJitInfo *
mono_tramp_info_register_reclaimable (MonoDomain *domain, MonoMethod *method, gpointer code,
									  guint32 code_size, const char *name,
									  guint8 *uw_info, guint32 uw_info_len)
{
	MonoJitInfo *ji;
	MonoTrampInfo *info;
	char *block;
	size_t info_offset = ALIGN_TO (mono_jit_info_size ((MonoJitInfoFlags)0, 0, 0), sizeof (gpointer));
	size_t name_offset = info_offset + sizeof (MonoTrampInfo);
	size_t name_size = name ? strlen (name) + 1 : 0;

	block = (char *)g_malloc0 (name_offset + name_size);
	ji = (MonoJitInfo *)block;
	info = (MonoTrampInfo *)(block + info_offset);

	info->code = (guint8*)code;
	info->code_size = code_size;
	info->method = method;
	info->uw_info = uw_info;
	info->uw_info_len = uw_info_len;
	if (name) {
		memcpy (block + name_offset, name, name_size);
		info->name = block + name_offset;
	}

	init_trampoline_jit_info (ji, info);

	mono_lldb_save_trampoline_info (info);
	mixed_callstack_plugin_save_trampoline_info (info, domain);
	if (mono_jit_map_is_enabled ())
		mono_emit_jit_tramp (info->code, info->code_size, info->name);

	mono_jit_info_table_add (domain, ji);

	return ji;
}

/*
 * mono_tramp_info_register:
 *
 * Remember INFO for use by mono_print_method_from_ip (), jit maps, etc.
 * INFO can be NULL.
 * Frees INFO.
 */
static void
mono_tramp_info_register_internal (MonoTrampInfo *info, MonoDomain *domain, gboolean aot)
{
	MonoTrampInfo *copy;

	if (!info)
		return;

	if (!domain)
		domain = mono_get_root_domain ();

	if (domain)
		copy = mono_domain_alloc0 (domain, sizeof (MonoTrampInfo));
	else
		copy = g_new0 (MonoTrampInfo, 1);

	copy->code = info->code;
	copy->code_size = info->code_size;
	copy->name = g_strdup (info->name);
	copy->method = info->method;
	copy->perf_dump_deferred = info->perf_dump_deferred;

	if (info->unwind_ops) {
		copy->uw_info = mono_unwind_ops_encode (info->unwind_ops, &copy->uw_info_len);
		copy->owns_uw_info = TRUE;
		if (domain) {
			/* Move unwind info into the domain's memory pool so that it is removed once the domain is released. */
			guint8 *temp = copy->uw_info;
			copy->uw_info = mono_domain_alloc (domain, copy->uw_info_len);
			memcpy (copy->uw_info, temp, copy->uw_info_len);
			g_free (temp);
		}
	} else {
		/* Trampolines from aot have the unwind ops already encoded */
		copy->uw_info = info->uw_info;
		copy->uw_info_len = info->uw_info_len;
	}

	/*
	 * Every stub and thunk the runtime plants comes through here, and each sits
	 * on the call path of the code around it. So a profile that cannot name them
	 * has unattributed gaps in exactly the hot places, and one that cannot unwind
	 * out of them loses every managed frame above - except a redirect thunk,
	 * whose group already published its own record (perf_dump_deferred,
	 * mono/mini/thunk.cpp). Code in an AOT image is already named by the
	 * image's own symbol table.
	 */
	if (!aot && copy->code && copy->code_size && !copy->perf_dump_deferred)
		mono_llvm_perf_dump_stub (copy->name ? copy->name : "trampoline",
					  copy->code, copy->code_size, copy->uw_info,
					  copy->uw_info_len);

	mono_lldb_save_trampoline_info (info);
	mixed_callstack_plugin_save_trampoline_info (info, domain);

#ifdef MONO_ARCH_HAVE_UNWIND_TABLE
	if (!aot)
		mono_arch_unwindinfo_install_tramp_unwind_info (info->unwind_ops, info->code, info->code_size);
#endif

	if (!domain) {
		/* If no root domain has been created yet, postpone the registration. */
		mono_jit_lock ();
		tramp_infos = g_slist_prepend (tramp_infos, copy);
		mono_jit_unlock ();
	} else if (copy->uw_info || info->method) {
		/* Only register trampolines that have unwind info */
		register_trampoline_jit_info (domain, copy);
	}

	if (mono_jit_map_is_enabled ())
		mono_emit_jit_tramp (info->code, info->code_size, info->name);

	mono_tramp_info_free (info);
}

void
mono_tramp_info_register (MonoTrampInfo *info, MonoDomain *domain)
{
	mono_tramp_info_register_internal (info, domain, FALSE);
}

void
mono_aot_tramp_info_register (MonoTrampInfo *info, MonoDomain *domain)
{
	mono_tramp_info_register_internal (info, domain, TRUE);
}

static void
mono_tramp_info_cleanup (void)
{
	GSList *l;

	for (l = tramp_infos; l; l = l->next) {
		MonoTrampInfo *info = (MonoTrampInfo *)l->data;

		mono_tramp_info_free (info);
	}
	g_slist_free (tramp_infos);
}

/* Register trampolines created before the root domain was created in the jit info tables */
static void
register_trampolines (MonoDomain *domain)
{
	GSList *l;

	for (l = tramp_infos; l; l = l->next) {
		MonoTrampInfo *info = (MonoTrampInfo *)l->data;

		register_trampoline_jit_info (domain, info);
	}
}

G_GNUC_UNUSED static void
break_count (void)
{
}

/*
 * Runtime debugging tool, use if (debug_count ()) <x> else <y> to do <x> the first COUNT times, then do <y> afterwards.
 * Set a breakpoint in break_count () to break the last time <x> is done.
 */
G_GNUC_UNUSED gboolean
mono_debug_count (void)
{
	static int count = 0, int_val = 0;
	static gboolean inited, has_value = FALSE;

	count ++;

	if (!inited) {
		char *value = g_getenv ("COUNT");
		if (value) {
			int_val = atoi (value);
			g_free (value);
			has_value = TRUE;
		}
		inited = TRUE;
	}

	if (!has_value)
		return TRUE;

	if (count == int_val)
		break_count ();

	if (count > int_val)
		return FALSE;

	return TRUE;
}

MonoMethod*
mono_icall_get_wrapper_method (MonoJitICallInfo* callinfo)
{
	/* This icall is used to check for exceptions, so don't check in the wrapper */
	gboolean check_exc = (callinfo != &mono_get_jit_icall_info ()->mono_thread_interruption_checkpoint);

	return mono_marshal_get_icall_wrapper (callinfo, check_exc);
}

gconstpointer
mono_icall_get_wrapper_full (MonoJitICallInfo* callinfo, gboolean do_compile)
{
	ERROR_DECL (error);
	MonoMethod *wrapper;
	gconstpointer addr, trampoline;
	MonoDomain *domain = mono_get_root_domain ();

	if (callinfo->wrapper)
		return callinfo->wrapper;

	wrapper = mono_icall_get_wrapper_method (callinfo);

	if (do_compile) {
		addr = mono_compile_method_checked (wrapper, error);
		mono_error_assert_ok (error);
		mono_memory_barrier ();
		callinfo->wrapper = addr;
		return addr;
	} else {
		if (callinfo->trampoline)
			return callinfo->trampoline;
		trampoline = mono_create_jit_trampoline (domain, wrapper, error);
		mono_error_assert_ok (error);
		trampoline = mono_create_ftnptr (domain, (gpointer)trampoline);

		mono_loader_lock ();
		if (!callinfo->trampoline) {
			callinfo->trampoline = trampoline;
		}
		mono_loader_unlock ();

		return callinfo->trampoline;
	}
}

gconstpointer
mono_icall_get_wrapper (MonoJitICallInfo* callinfo)
{
	return mono_icall_get_wrapper_full (callinfo, FALSE);
}

/*
 * For JIT icalls implemented in C.
 * NAME should be the same as the name of the C function whose address is FUNC.
 * If @avoid_wrapper is TRUE, no wrapper is generated. This is for perf critical icalls which
 * can't throw exceptions.
 *
 * func is an identifier, that names a function, and is also in jit-icall-reg.h,
 * and therefore a field in mono_jit_icall_info and can be token pasted into an enum value.
 *
 * The name of func must be linkable for AOT, for example g_free does not work (monoeg_g_free instead),
 * nor does the C++ overload fmod (mono_fmod instead). These functions therefore
 * must be extern "C".
 */
#define register_icall(func, sig, avoid_wrapper) \
	(mono_register_jit_icall_info (&mono_get_jit_icall_info ()->func, func, #func, (sig), (avoid_wrapper), #func))

#define register_icall_no_wrapper(func, sig) register_icall (func, sig, TRUE)
#define register_icall_with_wrapper(func, sig) register_icall (func, sig, FALSE)

/*
 * Register an icall where FUNC is dynamically generated or otherwise not
 * possible to link to it using NAME during AOT.
 *
 * func is an expression, such a local variable or a function call to get a function pointer.
 * name is an identifier
 *
 * Providing func and name separately is what distinguishes "dyn" from regular.
 *
 * This also passes last parameter c_symbol=NULL since there is not a directly linkable symbol.
 */
#define register_dyn_icall(func, name, sig, save) \
	(mono_register_jit_icall_info (&mono_get_jit_icall_info ()->name, (func), #name, (sig), (save), NULL))

MonoLMF *
mono_get_lmf (void)
{
	MonoJitTlsData *jit_tls;

	if ((jit_tls = mono_tls_get_jit_tls ()))
		return jit_tls->lmf;
	/*
	 * We do not assert here because this function can be called from
	 * mini-gc.c on a thread that has not executed any managed code, yet
	 * (the thread object allocation can trigger a collection).
	 */
	return NULL;
}

void
mono_set_lmf (MonoLMF *lmf)
{
	(*mono_get_lmf_addr ()) = lmf;
}

static void
mono_set_jit_tls (MonoJitTlsData *jit_tls)
{
	MonoThreadInfo *info;

	mono_tls_set_jit_tls (jit_tls);

	/* Save it into MonoThreadInfo so it can be accessed by mono_thread_state_init_from_handle () */
	info = mono_thread_info_current ();
	if (info)
		mono_thread_info_tls_set (info, TLS_KEY_JIT_TLS, jit_tls);
}

static void
mono_set_lmf_addr (MonoLMF **lmf_addr)
{
	MonoThreadInfo *info;

	mono_tls_set_lmf_addr (lmf_addr);

	/* Save it into MonoThreadInfo so it can be accessed by mono_thread_state_init_from_handle () */
	info = mono_thread_info_current ();
	if (info)
		mono_thread_info_tls_set (info, TLS_KEY_LMF_ADDR, lmf_addr);
}

/*
 * mono_push_lmf:
 *
 *   Push an MonoLMFExt frame on the LMF stack.
 */
void
mono_push_lmf (MonoLMFExt *ext)
{
	MonoLMF **lmf_addr;

	lmf_addr = mono_get_lmf_addr ();

	ext->lmf.previous_lmf = *lmf_addr;
	/* Mark that this is a MonoLMFExt */
	ext->lmf.previous_lmf = (gpointer)(((gssize)ext->lmf.previous_lmf) | 2);

	mono_set_lmf ((MonoLMF*)ext);
}

/*
 * mono_pop_lmf:
 *
 *   Pop the last frame from the LMF stack.
 */
void
mono_pop_lmf (MonoLMF *lmf)
{
	/* pthread_exit () unwinds the stack, and mono_thread_exit () detaches the
	 * thread before it calls that, so a cleanup in a frame the unwinder passes
	 * runs with no jit_tls left to pop into. There is no LMF stack any more,
	 * so there is nothing to do. */
	if (!mono_get_lmf_addr ())
		return;

	mono_set_lmf ((MonoLMF *)(((gssize)lmf->previous_lmf) & ~3));
}

/*
 * mono_jit_thread_attach:
 *
 * Called by Xamarin.Mac and other products. Attach thread to runtime if
 * needed and switch to @domain.
 *
 * This function is external only and @deprecated don't use it.  Use mono_threads_attach_coop ().
 *
 * If the thread is newly-attached, put into GC Safe mode.
 *
 * @return the original domain which needs to be restored, or NULL.
 */
MonoDomain*
mono_jit_thread_attach (MonoDomain *domain)
{
	MonoDomain *orig;
	gboolean attached;

	if (!domain) {
		/* Happens when called from AOTed code which is only used in the root domain. */
		domain = mono_get_root_domain ();
	}

	g_assert (domain);

	attached = mono_tls_get_jit_tls () != NULL;

	if (!attached) {
		// #678164
		gboolean background = TRUE;
		mono_thread_attach_external_native_thread (mono_get_root_domain (), background);

		/* mono_jit_thread_attach is external-only and not called by
		 * the runtime on any of our own threads.  So if we get here,
		 * the thread is running native code - leave it in GC Safe mode
		 * and leave it to the n2m invoke wrappers or MONO_API entry
		 * points to switch to GC Unsafe.
		 */
		MONO_STACKDATA (stackdata);
		mono_threads_enter_gc_safe_region_unbalanced_internal (&stackdata);
	}

	orig = mono_domain_get ();
	if (orig != domain) {
		mono_thread_push_appdomain_ref (domain);
		mono_domain_set_fast (domain, TRUE);
	}

	return orig != domain ? orig : NULL;
}

/**
 * mono_thread_abort:
 * \param obj exception object
 * Abort the thread, print exception information and stack trace
 */
static void
mono_thread_abort (MonoObject *obj)
{
	/* MonoJitTlsData *jit_tls = mono_tls_get_jit_tls (); */

	/* handle_remove should be eventually called for this thread, too
	g_free (jit_tls);*/

	if ((mono_runtime_unhandled_exception_policy_get () == MONO_UNHANDLED_POLICY_LEGACY) ||
			(obj->vtable->klass == mono_defaults.threadabortexception_class) ||
			((obj->vtable->klass) == mono_class_try_get_appdomain_unloaded_exception_class () &&
			mono_thread_info_current ()->runtime_thread)) {
		mono_thread_exit ();
	} else {
		mono_invoke_unhandled_exception_hook (obj);
	}
}

static MonoJitTlsData*
setup_jit_tls_data (gpointer stack_start, MonoAbortFunction abort_func)
{
	MonoJitTlsData *jit_tls;
	MonoLMF *lmf;

	jit_tls = mono_tls_get_jit_tls ();
	if (jit_tls)
		return jit_tls;

	jit_tls = g_new0 (MonoJitTlsData, 1);

	jit_tls->abort_func = abort_func;
	jit_tls->end_of_stack = stack_start;

	mono_set_jit_tls (jit_tls);

	lmf = g_new0 (MonoLMF, 1);
	MONO_ARCH_INIT_TOP_LMF_ENTRY (lmf);

	jit_tls->first_lmf = lmf;

	mono_set_lmf_addr (&jit_tls->lmf);

	jit_tls->lmf = lmf;

	mono_setup_resume_states (jit_tls);

#ifdef MONO_ARCH_HAVE_TLS_INIT
	mono_arch_tls_init ();
#endif

	mono_setup_altstack (jit_tls);

	return jit_tls;
}

static void
free_jit_tls_data (MonoJitTlsData *jit_tls)
{
	//This happens during AOT cuz the thread is never attached
	if (!jit_tls)
		return;
	mono_free_altstack (jit_tls);

	if (jit_tls->interp_context)
		mini_get_interp_callbacks ()->free_context (jit_tls->interp_context);

	mono_free_resume_states (jit_tls);

	g_free (jit_tls->first_lmf);
	g_free (jit_tls);
}

static void
mono_thread_start_cb (intptr_t tid, gpointer stack_start, gpointer func)
{
	MonoThreadInfo *thread;
	MonoJitTlsData *jit_tls = setup_jit_tls_data (stack_start, mono_thread_abort);
	thread = mono_thread_info_current_unchecked ();
	if (thread)
		thread->jit_data = jit_tls;

	mono_arch_cpu_init ();
}

void (*mono_thread_attach_aborted_cb ) (MonoObject *obj) = NULL;

static void
mono_thread_abort_dummy (MonoObject *obj)
{
  if (mono_thread_attach_aborted_cb)
    mono_thread_attach_aborted_cb (obj);
  else
    mono_thread_abort (obj);
}

static void
mono_thread_attach_cb (intptr_t tid, gpointer stack_start)
{
	MonoThreadInfo *thread;
	MonoJitTlsData *jit_tls = setup_jit_tls_data (stack_start, mono_thread_abort_dummy);
	thread = mono_thread_info_current_unchecked ();
	if (thread)
		thread->jit_data = jit_tls;

	mono_arch_cpu_init ();
}

static void
mini_thread_cleanup (MonoNativeThreadId tid)
{
	MonoJitTlsData *jit_tls = NULL;
	MonoThreadInfo *info;

	info = mono_thread_info_current_unchecked ();

	/* We can't clean up tls information if we are on another thread, it will clean up the wrong stuff
	 * It would be nice to issue a warning when this happens outside of the shutdown sequence. but it's
	 * not a trivial thing.
	 *
	 * The current offender is mono_thread_manage which cleanup threads from the outside.
	 */
	if (info && mono_thread_info_get_tid (info) == tid) {
		jit_tls = info->jit_data;
		info->jit_data = NULL;

		/* The LMF address points into jit_tls, which free_jit_tls_data ()
		 * releases below, so clear it while it is still there to clear. Test
		 * the address, not the LMF: a thread that attached and never called
		 * into managed land has neither, and mono_get_lmf () reads through the
		 * jit_tls this used to drop first, so it answered NULL either way. */
		if (mono_get_lmf_addr ()) {
			mono_set_lmf (NULL);
			mono_set_lmf_addr (NULL);
		}

		mono_set_jit_tls (NULL);
	} else {
		info = mono_thread_info_lookup (tid);
		if (info) {
			jit_tls = info->jit_data;
			info->jit_data = NULL;
		}
		mono_hazard_pointer_clear (mono_hazard_pointer_get (), 1);
	}

	if (jit_tls)
		free_jit_tls_data (jit_tls);
}

MonoJumpInfo *
mono_patch_info_list_prepend (MonoJumpInfo *list, int ip, MonoJumpInfoType type, gconstpointer target)
{
	MonoJumpInfo *ji = g_new0 (MonoJumpInfo, 1);

	ji->ip.i = ip;
	ji->type = type;
	ji->data.target = target;
	ji->next = list;

	return ji;
}

#if !defined(DISABLE_LOGGING) && !defined(DISABLE_JIT)

static const char* const patch_info_str[] = {
#define PATCH_INFO(a,b) "" #a,
#include "patch-info.h"
#undef PATCH_INFO
};

const char*
mono_ji_type_to_string (MonoJumpInfoType type)
{
	return patch_info_str [type];
}

/*
 * mono_ji_to_string:
 *
 *   Return a newly allocated human-readable description of the patch target JI,
 * e.g. "JIT_ICALL mono_amd64_throw_corlib_exception", "method int Foo:Bar (int)",
 * "class Ns.Cls". The caller frees the result with g_free. This is the single
 * source of truth for target names shared by mono_print_ji and any native
 * disassembly annotation.
 */
char*
mono_ji_to_string (const MonoJumpInfo *ji)
{
	const char *type = patch_info_str [ji->type];
	switch (ji->type) {
	case MONO_PATCH_INFO_RGCTX_FETCH:
	case MONO_PATCH_INFO_RGCTX_SLOT_INDEX: {
		MonoJumpInfoRgctxEntry *entry = ji->data.rgctx_entry;
		char *inner = mono_ji_to_string (entry->data);
		char *res = g_strdup_printf ("%s [%s] -> %s", type, inner, mono_rgctx_info_type_to_str (entry->info_type));
		g_free (inner);
		return res;
	}
	case MONO_PATCH_INFO_METHOD:
	case MONO_PATCH_INFO_METHODCONST:
	case MONO_PATCH_INFO_METHOD_FTNDESC: {
		char *s = mono_method_get_full_name (ji->data.method);
		char *res = g_strdup_printf ("%s %s", type, s);
		g_free (s);
		return res;
	}
	case MONO_PATCH_INFO_JIT_ICALL_ID:
		return g_strdup_printf ("JIT_ICALL %s", mono_find_jit_icall_info (ji->data.jit_icall_id)->name);
	case MONO_PATCH_INFO_CLASS:
	case MONO_PATCH_INFO_VTABLE: {
		char *name = mono_class_full_name (ji->data.klass);
		char *res = g_strdup_printf ("%s %s", type, name);
		g_free (name);
		return res;
	}
	default:
		return g_strdup (type);
	}
}

void
mono_print_ji (const MonoJumpInfo *ji)
{
	char *s = mono_ji_to_string (ji);
	printf ("[%s]", s);
	g_free (s);
}

#else

const char*
mono_ji_type_to_string (MonoJumpInfoType type)
{
	return "";
}

char*
mono_ji_to_string (const MonoJumpInfo *ji)
{
	return g_strdup ("");
}

void
mono_print_ji (const MonoJumpInfo *ji)
{
}

#endif

/**
 * mono_patch_info_dup_mp:
 *
 * Make a copy of PATCH_INFO, allocating memory from the mempool MP.
 */
MonoJumpInfo*
mono_patch_info_dup_mp (MonoMemPool *mp, MonoJumpInfo *patch_info)
{
	MonoJumpInfo *res = (MonoJumpInfo *)mono_mempool_alloc (mp, sizeof (MonoJumpInfo));
	memcpy (res, patch_info, sizeof (MonoJumpInfo));

	switch (patch_info->type) {
	case MONO_PATCH_INFO_RVA:
	case MONO_PATCH_INFO_LDSTR:
	case MONO_PATCH_INFO_TYPE_FROM_HANDLE:
	case MONO_PATCH_INFO_LDTOKEN:
	case MONO_PATCH_INFO_DECLSEC:
		res->data.token = (MonoJumpInfoToken *)mono_mempool_alloc (mp, sizeof (MonoJumpInfoToken));
		memcpy (res->data.token, patch_info->data.token, sizeof (MonoJumpInfoToken));
		break;
	case MONO_PATCH_INFO_SWITCH:
		res->data.table = (MonoJumpInfoBBTable *)mono_mempool_alloc (mp, sizeof (MonoJumpInfoBBTable));
		memcpy (res->data.table, patch_info->data.table, sizeof (MonoJumpInfoBBTable));
		res->data.table->table = (MonoBasicBlock **)mono_mempool_alloc (mp, sizeof (MonoBasicBlock*) * patch_info->data.table->table_size);
		memcpy (res->data.table->table, patch_info->data.table->table, sizeof (MonoBasicBlock*) * patch_info->data.table->table_size);
		break;
	case MONO_PATCH_INFO_RGCTX_FETCH:
	case MONO_PATCH_INFO_RGCTX_SLOT_INDEX:
		res->data.rgctx_entry = (MonoJumpInfoRgctxEntry *)mono_mempool_alloc (mp, sizeof (MonoJumpInfoRgctxEntry));
		memcpy (res->data.rgctx_entry, patch_info->data.rgctx_entry, sizeof (MonoJumpInfoRgctxEntry));
		res->data.rgctx_entry->data = mono_patch_info_dup_mp (mp, res->data.rgctx_entry->data);
		break;
	case MONO_PATCH_INFO_DELEGATE_TRAMPOLINE:
		res->data.del_tramp = (MonoDelegateClassMethodPair *)mono_mempool_alloc0 (mp, sizeof (MonoDelegateClassMethodPair));
		memcpy (res->data.del_tramp, patch_info->data.del_tramp, sizeof (MonoDelegateClassMethodPair));
		break;
	case MONO_PATCH_INFO_GSHAREDVT_CALL:
		res->data.gsharedvt = (MonoJumpInfoGSharedVtCall *)mono_mempool_alloc (mp, sizeof (MonoJumpInfoGSharedVtCall));
		memcpy (res->data.gsharedvt, patch_info->data.gsharedvt, sizeof (MonoJumpInfoGSharedVtCall));
		break;
	case MONO_PATCH_INFO_GSHAREDVT_METHOD: {
		MonoGSharedVtMethodInfo *info;
		MonoGSharedVtMethodInfo *oinfo;
		int i;

		oinfo = patch_info->data.gsharedvt_method;
		info = (MonoGSharedVtMethodInfo *)mono_mempool_alloc (mp, sizeof (MonoGSharedVtMethodInfo));
		res->data.gsharedvt_method = info;
		memcpy (info, oinfo, sizeof (MonoGSharedVtMethodInfo));
		info->entries = (MonoRuntimeGenericContextInfoTemplate *)mono_mempool_alloc (mp, sizeof (MonoRuntimeGenericContextInfoTemplate) * info->count_entries);
		for (i = 0; i < oinfo->num_entries; ++i) {
			MonoRuntimeGenericContextInfoTemplate *otemplate = &oinfo->entries [i];
			MonoRuntimeGenericContextInfoTemplate *template_ = &info->entries [i];

			memcpy (template_, otemplate, sizeof (MonoRuntimeGenericContextInfoTemplate));
		}
		//info->locals_types = mono_mempool_alloc0 (mp, info->nlocals * sizeof (MonoType*));
		//memcpy (info->locals_types, oinfo->locals_types, info->nlocals * sizeof (MonoType*));
		break;
	}
	case MONO_PATCH_INFO_VIRT_METHOD: {
		MonoJumpInfoVirtMethod *info;
		MonoJumpInfoVirtMethod *oinfo;

		oinfo = patch_info->data.virt_method;
		info = (MonoJumpInfoVirtMethod *)mono_mempool_alloc0 (mp, sizeof (MonoJumpInfoVirtMethod));
		res->data.virt_method = info;
		memcpy (info, oinfo, sizeof (MonoJumpInfoVirtMethod));
		break;
	}
	default:
		break;
	}

	return res;
}

guint
mono_patch_info_hash (gconstpointer data)
{
	const MonoJumpInfo *ji = (MonoJumpInfo*)data;
	const MonoJumpInfoType type = ji->type;
	guint hash = type << 8;

	switch (type) {
	case MONO_PATCH_INFO_RVA:
	case MONO_PATCH_INFO_LDSTR:
	case MONO_PATCH_INFO_LDTOKEN:
	case MONO_PATCH_INFO_DECLSEC:
		return hash | ji->data.token->token;
	case MONO_PATCH_INFO_TYPE_FROM_HANDLE:
		return hash | ji->data.token->token | (ji->data.token->has_context ? (gsize)ji->data.token->context.class_inst : 0);
	case MONO_PATCH_INFO_OBJC_SELECTOR_REF: // Hash on the selector name
	case MONO_PATCH_INFO_LDSTR_LIT:
		return g_str_hash (ji->data.name);
	case MONO_PATCH_INFO_VTABLE:
	case MONO_PATCH_INFO_CLASS:
	case MONO_PATCH_INFO_IID:
	case MONO_PATCH_INFO_ADJUSTED_IID:
	case MONO_PATCH_INFO_METHODCONST:
	case MONO_PATCH_INFO_METHOD:
	case MONO_PATCH_INFO_METHOD_JUMP:
	case MONO_PATCH_INFO_METHOD_FTNDESC:
	case MONO_PATCH_INFO_IMAGE:
	case MONO_PATCH_INFO_ICALL_ADDR:
	case MONO_PATCH_INFO_ICALL_ADDR_CALL:
	case MONO_PATCH_INFO_FIELD:
	case MONO_PATCH_INFO_SFLDA:
	case MONO_PATCH_INFO_SEQ_POINT_INFO:
	case MONO_PATCH_INFO_METHOD_RGCTX:
	case MONO_PATCH_INFO_SIGNATURE:
	case MONO_PATCH_INFO_METHOD_CODE_SLOT:
	case MONO_PATCH_INFO_AOT_JIT_INFO:
	case MONO_PATCH_INFO_METHOD_PINVOKE_ADDR_CACHE:
		return hash | (gssize)ji->data.target;
	case MONO_PATCH_INFO_GSHAREDVT_CALL:
		return hash | (gssize)ji->data.gsharedvt->method;
	case MONO_PATCH_INFO_RGCTX_FETCH:
	case MONO_PATCH_INFO_RGCTX_SLOT_INDEX: {
		MonoJumpInfoRgctxEntry *e = ji->data.rgctx_entry;
		hash |= e->in_mrgctx | e->info_type | mono_patch_info_hash (e->data);
		if (e->in_mrgctx)
			return hash | (gssize)e->d.method;
		else
			return hash | (gssize)e->d.klass;
	}
	case MONO_PATCH_INFO_INTERRUPTION_REQUEST_FLAG:
	case MONO_PATCH_INFO_MSCORLIB_GOT_ADDR:
	case MONO_PATCH_INFO_GC_CARD_TABLE_ADDR:
	case MONO_PATCH_INFO_GC_NURSERY_START:
	case MONO_PATCH_INFO_GC_NURSERY_BITS:
	case MONO_PATCH_INFO_GOT_OFFSET:
	case MONO_PATCH_INFO_GC_SAFE_POINT_FLAG:
	case MONO_PATCH_INFO_AOT_MODULE:
	case MONO_PATCH_INFO_PROFILER_ALLOCATION_COUNT:
	case MONO_PATCH_INFO_PROFILER_CLAUSE_COUNT:
	case MONO_PATCH_INFO_SPECIFIC_TRAMPOLINES:
	case MONO_PATCH_INFO_SPECIFIC_TRAMPOLINES_GOT_SLOTS_BASE:
		return hash;
	case MONO_PATCH_INFO_SPECIFIC_TRAMPOLINE_LAZY_FETCH_ADDR:
		return hash | ji->data.uindex;
	case MONO_PATCH_INFO_JIT_ICALL_ID:
	case MONO_PATCH_INFO_JIT_ICALL_ADDR:
	case MONO_PATCH_INFO_JIT_ICALL_ADDR_NOCALL:
	case MONO_PATCH_INFO_CASTCLASS_CACHE:
		return hash | ji->data.index;
	case MONO_PATCH_INFO_SWITCH:
		return hash | ji->data.table->table_size;
	case MONO_PATCH_INFO_GSHAREDVT_METHOD:
		return hash | (gssize)ji->data.gsharedvt_method->method;
	case MONO_PATCH_INFO_DELEGATE_TRAMPOLINE:
		return hash | (gsize)ji->data.del_tramp->klass | (gsize)ji->data.del_tramp->method | (gsize)ji->data.del_tramp->is_virtual;
	case MONO_PATCH_INFO_VIRT_METHOD: {
		MonoJumpInfoVirtMethod *info = ji->data.virt_method;

		return hash | (gssize)info->klass | (gssize)info->method;
	}
	case MONO_PATCH_INFO_GSHAREDVT_IN_WRAPPER:
		return hash | mono_signature_hash (ji->data.sig);
	case MONO_PATCH_INFO_R8_GOT:
		return hash | (guint32)*(double*)ji->data.target;
	case MONO_PATCH_INFO_R4_GOT:
		return hash | (guint32)*(float*)ji->data.target;
	default:
		printf ("info type: %d\n", ji->type);
		mono_print_ji (ji); printf ("\n");
		g_assert_not_reached ();
	case MONO_PATCH_INFO_NONE:
		return 0;
	}
}

/*
 * mono_patch_info_equal:
 *
 * This might fail to recognize equivalent patches, i.e. floats, so its only
 * usable in those cases where this is not a problem, i.e. sharing GOT slots
 * in AOT.
 */
gint
mono_patch_info_equal (gconstpointer ka, gconstpointer kb)
{
	const MonoJumpInfo *ji1 = (MonoJumpInfo*)ka;
	const MonoJumpInfo *ji2 = (MonoJumpInfo*)kb;

	MonoJumpInfoType const ji1_type = ji1->type;
	MonoJumpInfoType const ji2_type = ji2->type;

	if (ji1_type != ji2_type)
		return 0;

	switch (ji1_type) {
	case MONO_PATCH_INFO_RVA:
	case MONO_PATCH_INFO_LDSTR:
	case MONO_PATCH_INFO_TYPE_FROM_HANDLE:
	case MONO_PATCH_INFO_LDTOKEN:
	case MONO_PATCH_INFO_DECLSEC:
		return ji1->data.token->image == ji2->data.token->image &&
		       ji1->data.token->token == ji2->data.token->token &&
		       ji1->data.token->has_context == ji2->data.token->has_context &&
		       ji1->data.token->context.class_inst == ji2->data.token->context.class_inst &&
		       ji1->data.token->context.method_inst == ji2->data.token->context.method_inst;
	case MONO_PATCH_INFO_OBJC_SELECTOR_REF:
	case MONO_PATCH_INFO_LDSTR_LIT:
		return g_str_equal (ji1->data.name, ji2->data.name);
	case MONO_PATCH_INFO_RGCTX_FETCH:
	case MONO_PATCH_INFO_RGCTX_SLOT_INDEX: {
		MonoJumpInfoRgctxEntry *e1 = ji1->data.rgctx_entry;
		MonoJumpInfoRgctxEntry *e2 = ji2->data.rgctx_entry;

		return e1->d.method == e2->d.method && e1->d.klass == e2->d.klass && e1->in_mrgctx == e2->in_mrgctx && e1->info_type == e2->info_type && mono_patch_info_equal (e1->data, e2->data);
	}
	case MONO_PATCH_INFO_GSHAREDVT_CALL: {
		MonoJumpInfoGSharedVtCall *c1 = ji1->data.gsharedvt;
		MonoJumpInfoGSharedVtCall *c2 = ji2->data.gsharedvt;

		return c1->sig == c2->sig && c1->method == c2->method;
	}
	case MONO_PATCH_INFO_GSHAREDVT_METHOD:
		return ji1->data.gsharedvt_method->method == ji2->data.gsharedvt_method->method;
	case MONO_PATCH_INFO_DELEGATE_TRAMPOLINE:
		return ji1->data.del_tramp->klass == ji2->data.del_tramp->klass && ji1->data.del_tramp->method == ji2->data.del_tramp->method && ji1->data.del_tramp->is_virtual == ji2->data.del_tramp->is_virtual;
	case MONO_PATCH_INFO_SPECIFIC_TRAMPOLINE_LAZY_FETCH_ADDR:
		return ji1->data.uindex == ji2->data.uindex;
	case MONO_PATCH_INFO_CASTCLASS_CACHE:
		return ji1->data.index == ji2->data.index;
	case MONO_PATCH_INFO_JIT_ICALL_ID:
	case MONO_PATCH_INFO_JIT_ICALL_ADDR:
	case MONO_PATCH_INFO_JIT_ICALL_ADDR_NOCALL:
		return ji1->data.jit_icall_id == ji2->data.jit_icall_id;
	case MONO_PATCH_INFO_VIRT_METHOD:
		return ji1->data.virt_method->klass == ji2->data.virt_method->klass && ji1->data.virt_method->method == ji2->data.virt_method->method;
	case MONO_PATCH_INFO_GSHAREDVT_IN_WRAPPER:
		return mono_metadata_signature_equal (ji1->data.sig, ji2->data.sig);
	case MONO_PATCH_INFO_GC_SAFE_POINT_FLAG:
	case MONO_PATCH_INFO_NONE:
		return 1;
	default:
		break;
	}

	return ji1->data.target == ji2->data.target;
}

/*
 * mini_register_jump_site:
 *
 *   Register IP as a jump/tailcall site which calls METHOD.
 * This is needed because common_call_trampoline () cannot patch
 * the call site because the caller ip is not available for jumps.
 */
void
mini_register_jump_site (MonoDomain *domain, MonoMethod *method, gpointer ip)
{
	MonoJumpList *jlist;

	MonoMethod *shared_method = mini_method_to_shared (method);
	method = shared_method ? shared_method : method;

	mono_domain_lock (domain);
	jlist = (MonoJumpList *)g_hash_table_lookup (domain_jit_info (domain)->jump_target_hash, method);
	if (!jlist) {
		jlist = (MonoJumpList *)mono_domain_alloc0 (domain, sizeof (MonoJumpList));
		g_hash_table_insert (domain_jit_info (domain)->jump_target_hash, method, jlist);
	}
	jlist->list = g_slist_prepend (jlist->list, ip);
	mono_domain_unlock (domain);
}

/*
 * mini_patch_jump_sites:
 *
 *   Patch jump/tailcall sites calling METHOD so the jump to ADDR.
 */
void
mini_patch_jump_sites (MonoDomain *domain, MonoMethod *method, gpointer addr)
{
	GHashTable *hash = domain_jit_info (domain)->jump_target_hash;

	if (!hash)
		return;

	MonoJumpInfo patch_info;
	MonoJumpList *jlist;
	GSList *tmp;

	/* The caller/callee might use different instantiations */
	MonoMethod *shared_method = mini_method_to_shared (method);
	method = shared_method ? shared_method : method;

	mono_domain_lock (domain);
	jlist = (MonoJumpList *)g_hash_table_lookup (hash, method);
	if (jlist)
		g_hash_table_remove (hash, method);
	mono_domain_unlock (domain);
	if (jlist) {
		patch_info.next = NULL;
		patch_info.ip.i = 0;
		patch_info.type = MONO_PATCH_INFO_METHOD_JUMP;
		patch_info.data.method = method;

		mono_codeman_enable_write ();

#ifdef MONO_ARCH_HAVE_PATCH_CODE_NEW
		for (tmp = jlist->list; tmp; tmp = tmp->next)
			mono_arch_patch_code_new (NULL, domain, (guint8 *)tmp->data, &patch_info, addr);
#else
		// FIXME: This won't work since it ends up calling mono_create_jump_trampoline () which returns a trampoline
		// for gshared methods
		for (tmp = jlist->list; tmp; tmp = tmp->next) {
			ERROR_DECL (error);
			mono_arch_patch_code (NULL, NULL, domain, tmp->data, &patch_info, TRUE, error);
			mono_error_assert_ok (error);
		}
#endif

		mono_codeman_disable_write ();
	}
}

void
mini_init_gsctx (MonoDomain *domain, MonoMemPool *mp, MonoGenericContext *context, MonoGenericSharingContext *gsctx)
{
	MonoGenericInst *inst;
	int i;

	memset (gsctx, 0, sizeof (MonoGenericSharingContext));

	if (context && context->class_inst) {
		inst = context->class_inst;
		for (i = 0; i < inst->type_argc; ++i) {
			MonoType *type = inst->type_argv [i];

			if (mini_is_gsharedvt_gparam (type))
				gsctx->is_gsharedvt = TRUE;
		}
	}
	if (context && context->method_inst) {
		inst = context->method_inst;

		for (i = 0; i < inst->type_argc; ++i) {
			MonoType *type = inst->type_argv [i];

			if (mini_is_gsharedvt_gparam (type))
				gsctx->is_gsharedvt = TRUE;
		}
	}
}

MonoClass*
mini_get_class (MonoMethod *method, guint32 token, MonoGenericContext *context)
{
	ERROR_DECL (error);
	MonoClass *klass;

	if (method->wrapper_type != MONO_WRAPPER_NONE) {
		klass = (MonoClass *)mono_method_get_wrapper_data (method, token);
		if (context) {
			klass = mono_class_inflate_generic_class_checked (klass, context, error);
			mono_error_cleanup (error); /* FIXME don't swallow the error */
		}
	} else {
		klass = mono_class_get_and_inflate_typespec_checked (m_class_get_image (method->klass), token, context, error);
		mono_error_cleanup (error); /* FIXME don't swallow the error */
	}
	if (klass)
		mono_class_init_internal (klass);
	return klass;
}

#if ENABLE_JIT_MAP
static FILE* perf_map_file;

void
mono_enable_jit_map (void)
{
	if (!perf_map_file) {
		char name [64];
		g_snprintf (name, sizeof (name), "/tmp/perf-%d.map", getpid ());
		unlink (name);
		perf_map_file = fopen (name, "w");
	}
}

void
mono_emit_jit_tramp (void *start, int size, const char *desc)
{
	if (perf_map_file)
		fprintf (perf_map_file, "%" PRIx64 " %x %s\n", (guint64)(gsize)start, size, desc);
}

void
mono_emit_jit_map (MonoJitInfo *jinfo)
{
	if (perf_map_file) {
		char *name = mono_method_full_name (jinfo_get_method (jinfo), TRUE);
		mono_emit_jit_tramp (jinfo->code_start, jinfo->code_size, name);
		g_free (name);
	}
}

gboolean
mono_jit_map_is_enabled (void)
{
	return perf_map_file != NULL;
}

#endif

#ifdef ENABLE_JIT_DUMP
#include <sys/mman.h>
#include <sys/syscall.h>
#include <elf.h>

static FILE *perf_dump_file;
static mono_mutex_t perf_dump_mutex;
static void *perf_dump_mmap_addr = MAP_FAILED;
static guint32 perf_dump_pid;
static clockid_t clock_id = CLOCK_MONOTONIC;

enum {
	JIT_DUMP_MAGIC = 0x4A695444,
	/* The only version perf has ever defined; it rejects the file otherwise. */
	JIT_DUMP_VERSION = 1,
#if HOST_X86
	ELF_MACHINE = EM_386,
#elif HOST_AMD64
	ELF_MACHINE = EM_X86_64,
#elif HOST_ARM
	ELF_MACHINE = EM_ARM,
#elif HOST_ARM64
	ELF_MACHINE = EM_AARCH64,
#elif HOST_POWERPC64
	ELF_MACHINE = EM_PPC64,
#elif HOST_S390X
	ELF_MACHINE = EM_S390,
#elif HOST_RISCV
	ELF_MACHINE = EM_RISCV,
#elif HOST_MIPS
	ELF_MACHINE = EM_MIPS,
#endif
	JIT_CODE_LOAD = 0
};
typedef struct
{
	guint32 magic;
	guint32 version;
	guint32 total_size;
	guint32 elf_mach;
	guint32 pad1;
	guint32 pid;
	guint64 timestamp;
	guint64 flags;
} FileHeader;
typedef struct
{
	guint32 id;
	guint32 total_size;
	guint64 timestamp;
} RecordHeader;
typedef struct
{
	RecordHeader header;
	guint32 pid;
	guint32 tid;
	guint64 vma;
	guint64 code_addr;
	guint64 code_size;
	guint64 code_index;
	// Null terminated function name
	// Native code
} JitCodeLoadRecord;

static void add_file_header_info (FileHeader *header);
static void add_basic_JitCodeLoadRecord_info (JitCodeLoadRecord *record);

void
mono_enable_jit_dump (void)
{
	if (perf_dump_pid == 0)
		perf_dump_pid = getpid();
	
	if (!perf_dump_file) {
		char name [64];
		FileHeader header;
		memset (&header, 0, sizeof (header));

		mono_os_mutex_init (&perf_dump_mutex);
		mono_os_mutex_lock (&perf_dump_mutex);
		
		g_snprintf (name, sizeof (name), "/tmp/jit-%d.dump", perf_dump_pid);
		unlink (name);
		/* Readable as well as writable: the marker mmap below asks for
		 * PROT_READ, which the kernel refuses on a write-only descriptor. */
		perf_dump_file = fopen (name, "w+");

		add_file_header_info (&header);
		if (perf_dump_file) {
			fwrite (&header, sizeof (header), 1, perf_dump_file);
			//This informs perf of the presence of the jitdump file and support for the feature.
			perf_dump_mmap_addr = mmap (NULL, sizeof (header), PROT_READ | PROT_EXEC, MAP_PRIVATE, fileno (perf_dump_file), 0);
		}
		
		mono_os_mutex_unlock (&perf_dump_mutex);
	}
}

static void
add_file_header_info (FileHeader *header)
{
	header->magic = JIT_DUMP_MAGIC;
	header->version = JIT_DUMP_VERSION;
	/* The size of the header itself, which is how perf finds the first record. */
	header->total_size = sizeof (*header);
	header->elf_mach = ELF_MACHINE;
	header->pad1 = 0;
	header->pid = perf_dump_pid;
	header->timestamp = mono_clock_get_time_ns (clock_id);
	header->flags = 0;
}

gboolean
mono_jit_dump_is_enabled (void)
{
	return perf_dump_file != NULL;
}

void
mono_emit_jit_dump_code (const char *name, gpointer code, guint32 code_size, const guint8 *pre_record, guint32 pre_record_size)
{
	static uint64_t code_index;

	if (perf_dump_file) {
		JitCodeLoadRecord record;
		size_t nameLen = strlen (name);
		memset (&record, 0, sizeof (record));

		add_basic_JitCodeLoadRecord_info (&record);
		record.header.total_size = sizeof (record) + nameLen + 1 + code_size;
		record.vma = (guint64)(gsize)code;
		record.code_addr = (guint64)(gsize)code;
		record.code_size = (guint64)code_size;

		mono_os_mutex_lock (&perf_dump_mutex);

		record.code_index = ++code_index;

		record.header.timestamp = mono_clock_get_time_ns (clock_id);

		if (pre_record)
			fwrite (pre_record, pre_record_size, 1, perf_dump_file);

		fwrite (&record, sizeof (record), 1, perf_dump_file);
		fwrite (name, nameLen + 1, 1, perf_dump_file);
		fwrite (code, code_size, 1, perf_dump_file);

		mono_os_mutex_unlock (&perf_dump_mutex);
	}
}

void
mono_emit_jit_dump (MonoJitInfo *jinfo, gpointer code)
{
	mono_emit_jit_dump_code (jinfo->d.method->name, code, (guint32)jinfo->code_size, NULL, 0);
}

static void
add_basic_JitCodeLoadRecord_info (JitCodeLoadRecord *record)
{
	record->header.id = JIT_CODE_LOAD;
	record->header.timestamp = mono_clock_get_time_ns (clock_id);
	record->pid = perf_dump_pid;
	record->tid = syscall (SYS_gettid);
}

void
mono_jit_dump_cleanup (void)
{
	if (perf_dump_mmap_addr != MAP_FAILED)
		munmap (perf_dump_mmap_addr, sizeof(FileHeader));
	if (perf_dump_file)
		fclose (perf_dump_file);
}

#else

void
mono_enable_jit_dump (void)
{
}

gboolean
mono_jit_dump_is_enabled (void)
{
	return FALSE;
}

void
mono_emit_jit_dump_code (const char *name, gpointer code, guint32 code_size, const guint8 *pre_record, guint32 pre_record_size)
{
}

void
mono_emit_jit_dump (MonoJitInfo *jinfo, gpointer code)
{
}

void
mono_jit_dump_cleanup (void)
{
}

#endif

static void
no_gsharedvt_in_wrapper (void)
{
	g_assert_not_reached ();
}

/*
Overall algorithm:

When a JIT request is made, we check if there's an outstanding one for that method and, if it exits, put the thread to sleep.
	If the current thread is already JITing another method, don't wait as it might cause a deadlock.
	Dependency management in this case is too complex to justify implementing it.

If there are no outstanding requests, the current thread is doing nothing and there are already mono_cpu_count threads JITing, go to sleep.

TODO:
	Get rid of cctor invocations from within the JIT, it increases JIT duration and complicates things A LOT.
	Can we get rid of ref_count and use `done && threads_waiting == 0` as the equivalent of `ref_count == 0`?
	Reduce amount of dynamically allocated - possible once the JIT is no longer reentrant
	Maybe pool JitCompilationEntry, specially those with an inited cond var;
*/
typedef struct {
	MonoMethod *method;
	MonoDomain *domain;
	int compilation_count; /* Number of threads compiling this method - This happens due to the JIT being reentrant */
	int ref_count; /* Number of threads using this JitCompilationEntry, roughtly 1 + threads_waiting */
	int threads_waiting; /* Number of threads waiting on this job */
	gboolean has_cond; /* True if @cond was initialized */
	gboolean done; /* True if the method finished JIT'ing */
	MonoCoopCond cond; /* Cond sleeping threads wait one */
} JitCompilationEntry;

typedef struct {
	GPtrArray *in_flight_methods; //JitCompilationEntry*
	MonoCoopMutex lock;
} JitCompilationData;

/*
Timeout, in millisecounds, that we wait other threads to finish JITing.
This value can't be too small or we won't see enough methods being reused and it can't be too big to cause massive stalls due to unforseable circunstances.
*/
#define MAX_JIT_TIMEOUT_MS 1000


static JitCompilationData compilation_data;
static int jit_methods_waited, jit_methods_multiple, jit_methods_overload, jit_spurious_wakeups_or_timeouts;

static void
mini_jit_init_job_control (void)
{
	mono_coop_mutex_init (&compilation_data.lock);
	compilation_data.in_flight_methods = g_ptr_array_new ();
}

static void
lock_compilation_data (void)
{
	mono_coop_mutex_lock (&compilation_data.lock);
}

static void
unlock_compilation_data (void)
{
	mono_coop_mutex_unlock (&compilation_data.lock);
}

static JitCompilationEntry*
find_method (MonoMethod *method, MonoDomain *domain)
{
	int i;
	for (i = 0; i < compilation_data.in_flight_methods->len; ++i){
		JitCompilationEntry *e = (JitCompilationEntry*)compilation_data.in_flight_methods->pdata [i];
		if (e->method == method && e->domain == domain)
			return e;
	}

	return NULL;
}

static void
add_current_thread (MonoJitTlsData *jit_tls)
{
	++jit_tls->active_jit_methods;
}

static void
unref_jit_entry (JitCompilationEntry *entry)
{
	--entry->ref_count;
	if (entry->ref_count)
		return;
	if (entry->has_cond)
		mono_coop_cond_destroy (&entry->cond);
	g_free (entry);
}

/*
 * Returns true if this method waited successfully for another thread to JIT it
 */
static gboolean
wait_or_register_method_to_compile (MonoMethod *method, MonoDomain *domain)
{
	MonoJitTlsData *jit_tls = mono_tls_get_jit_tls ();
	JitCompilationEntry *entry;

	static gboolean inited;
	if (!inited) {
		mono_counters_register ("JIT compile waited others", MONO_COUNTER_INT|MONO_COUNTER_JIT, &jit_methods_waited);
		mono_counters_register ("JIT compile 1+ jobs", MONO_COUNTER_INT|MONO_COUNTER_JIT, &jit_methods_multiple);
		mono_counters_register ("JIT compile overload wait", MONO_COUNTER_INT|MONO_COUNTER_JIT, &jit_methods_overload);
		mono_counters_register ("JIT compile spurious wakeups or timeouts", MONO_COUNTER_INT|MONO_COUNTER_JIT, &jit_spurious_wakeups_or_timeouts);
		inited = TRUE;
	}

	lock_compilation_data ();

	if (!(entry = find_method (method, domain))) {
		entry = g_new0 (JitCompilationEntry, 1);
		entry->method = method;
		entry->domain = domain;
		entry->compilation_count = entry->ref_count = 1;
		g_ptr_array_add (compilation_data.in_flight_methods, entry);
		g_assert (find_method (method, domain) == entry);
		add_current_thread (jit_tls);

		unlock_compilation_data ();
		return FALSE;
	} else if (jit_tls->active_jit_methods > 0 || mono_threads_is_current_thread_in_protected_block ()
	           || mono_loader_lock_is_owned_by_self ()) {
		//We can't suspend the current thread if it's already JITing a method.
		//Dependency management is too compilated and we want to get rid of this anyways.

		//We can't suspend the current thread if it's running a protected block (such as a cctor)
		//We can't rely only on JIT nesting as cctor's can be run from outside the JIT.

		/*
		 * And we cannot suspend it while it holds the loader lock. The thread we
		 * wait for takes that lock to compile, so it cannot finish, and we wait
		 * out the whole timeout. mono_class_proxy_vtable () compiles one method
		 * for each slot of a transparent proxy's vtable, with the loader lock
		 * held. So the pair costs a second for each slot, which reads as a hang.
		 */

		//Finally, he hit a timeout or spurious wakeup. We're better off just giving up and keep recompiling
		++entry->compilation_count;
		++jit_methods_multiple;
		++jit_tls->active_jit_methods;

		unlock_compilation_data ();
		return FALSE;
	} else {
		++jit_methods_waited;
		++entry->ref_count;

		if (!entry->has_cond) {
			mono_coop_cond_init (&entry->cond);
			entry->has_cond = TRUE;
		}

		while (TRUE) {
			++entry->threads_waiting;

			g_assert (entry->has_cond);
			mono_coop_cond_timedwait (&entry->cond, &compilation_data.lock, MAX_JIT_TIMEOUT_MS);
			--entry->threads_waiting;

			if (entry->done) {
				unref_jit_entry (entry);
				unlock_compilation_data ();
				return TRUE;
			} else {
				//We hit the timeout or a spurious wakeup, fallback to JITing
				g_assert (entry->ref_count > 1);
				unref_jit_entry (entry);
				++jit_spurious_wakeups_or_timeouts;

				++entry->compilation_count;
				++jit_methods_multiple;
				++jit_tls->active_jit_methods;

				unlock_compilation_data ();
				return FALSE;
			}
		}
	}
}

static void
unregister_method_for_compile (MonoMethod *method, MonoDomain *target_domain)
{
	MonoJitTlsData *jit_tls = mono_tls_get_jit_tls ();

	lock_compilation_data ();

	g_assert (jit_tls->active_jit_methods > 0);
	--jit_tls->active_jit_methods;

	JitCompilationEntry *entry = find_method (method, target_domain);
	g_assert (entry); // It would be weird to fail
	entry->done = TRUE;

	if (entry->threads_waiting) {
		g_assert (entry->has_cond);
		mono_coop_cond_broadcast (&entry->cond);
	}

	if (--entry->compilation_count == 0) {
		g_ptr_array_remove (compilation_data.in_flight_methods, entry);
		unref_jit_entry (entry);
	}

	unlock_compilation_data ();
}

static MonoJitInfo*
create_jit_info_for_trampoline (MonoMethod *wrapper, MonoTrampInfo *info)
{
	MonoDomain *domain = mono_get_root_domain ();
	MonoJitInfo *jinfo;
	guint8 *uw_info;
	guint32 info_len;

	if (info->uw_info) {
		uw_info = info->uw_info;
		info_len = info->uw_info_len;
	} else {
		uw_info = mono_unwind_ops_encode (info->unwind_ops, &info_len);
	}

	jinfo = (MonoJitInfo *)mono_domain_alloc0 (domain, MONO_SIZEOF_JIT_INFO);
	jinfo->d.method = wrapper;
	jinfo->code_start = MINI_FTNPTR_TO_ADDR (info->code);
	jinfo->code_size = info->code_size;
	jinfo->unwind_info = mono_cache_unwind_info (uw_info, info_len);

	if (!info->uw_info)
		g_free (uw_info);

	return jinfo;
}

static gpointer
compile_special (MonoMethod *method, MonoDomain *target_domain, MonoError *error)
{
	MonoJitInfo *jinfo;
	gpointer code;

	if (mono_llvm_only) {
		if (method->wrapper_type == MONO_WRAPPER_OTHER) {
			WrapperInfo *info = mono_marshal_get_wrapper_info (method);

			if (info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_IN_SIG) {
				/*
				 * These wrappers are only created for signatures which are in the program, but
				 * sometimes we load methods too eagerly and have to create them even if they
				 * will never be called.
				 */
				return (gpointer)no_gsharedvt_in_wrapper;
			}
		}
	}

	if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) ||
	    (method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)) {
		MonoMethodPInvoke* piinfo = (MonoMethodPInvoke *) method;

		if (!piinfo->addr) {
			if (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) {
				guint32 flags = MONO_ICALL_FLAGS_NONE;
				gpointer icall_addr;
				icall_addr = (gpointer)mono_lookup_internal_call_full_with_flags (method, TRUE, (guint32 *)&flags);
				if (flags & MONO_ICALL_FLAGS_NO_WRAPPER) {
					piinfo->icflags = MONO_ICALL_FLAGS_NO_WRAPPER;
					mono_memory_write_barrier ();
				}
				piinfo->addr = icall_addr;
			} else if (method->iflags & METHOD_IMPL_ATTRIBUTE_NATIVE) {
#ifdef HOST_WIN32
				g_warning ("Method '%s' in assembly '%s' contains native code that cannot be executed by Mono in modules loaded from byte arrays. The assembly was probably created using C++/CLI.\n", mono_method_full_name (method, TRUE), m_class_get_image (method->klass)->name);
#else
				g_warning ("Method '%s' in assembly '%s' contains native code that cannot be executed by Mono on this platform. The assembly was probably created using C++/CLI.\n", mono_method_full_name (method, TRUE), m_class_get_image (method->klass)->name);
#endif
			} else {
				ERROR_DECL (ignored_error);
				mono_lookup_pinvoke_call_internal (method, ignored_error);
				mono_error_cleanup (ignored_error);
			}
		}

		mono_memory_read_barrier ();

		gpointer compiled_method = NULL;
		if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) && (piinfo->icflags & MONO_ICALL_FLAGS_NO_WRAPPER)) {
			compiled_method = piinfo->addr;
		} else {
			MonoMethod *nm = mono_marshal_get_native_wrapper (method, TRUE, mono_aot_only);
			compiled_method = mono_jit_compile_method_jit_only (nm, error);
			return_val_if_nok (error, NULL);
		}

		code = mono_get_addr_from_ftnptr (compiled_method);
		jinfo = mono_jit_info_table_find (target_domain, code);
		if (!jinfo)
			jinfo = mono_jit_info_table_find (mono_domain_get (), code);
		if (jinfo)
			MONO_PROFILER_RAISE (jit_done, (method, jinfo));
		return code;
	} else if ((method->iflags & METHOD_IMPL_ATTRIBUTE_RUNTIME)) {
		const char *name = method->name;
		char *full_name;
		MonoMethod *nm;

		if (m_class_get_parent (method->klass) == mono_defaults.multicastdelegate_class) {
			if (*name == '.' && (strcmp (name, ".ctor") == 0)) {
				MonoJitICallInfo *mi = &mono_get_jit_icall_info ()->ves_icall_mono_delegate_ctor;
				/*
				 * We need to make sure this wrapper
				 * is compiled because it might end up
				 * in an (M)RGCTX if generic sharing
				 * is enabled, and would be called
				 * indirectly.  If it were a
				 * trampoline we'd try to patch that
				 * indirect call, which is not
				 * possible.
				 */
				return mono_get_addr_from_ftnptr ((gpointer)mono_icall_get_wrapper_full (mi, TRUE));
			} else if (*name == 'I' && (strcmp (name, "Invoke") == 0)) {
				if (mono_llvm_only) {
					nm = mono_marshal_get_delegate_invoke (method, NULL);
					gpointer compiled_ptr = mono_jit_compile_method_jit_only (nm, error);
					return_val_if_nok (error, NULL);
					return mono_get_addr_from_ftnptr (compiled_ptr);
				}

				/*
				 * Standing in for the gsharedvt_out wrappers that would have to sit
				 * between a shared frame and the delegate trampoline, which
				 * interpreter-only mode does not have.
				 *
				 * Nothing reaches it. With the interpreter as the whole engine every
				 * request for a method is answered by it before compile_special runs,
				 * and the one caller that insists on the JIT - the interpreter's own
				 * MINT_JIT_CALL - never carries a delegate's Invoke, because
				 * interp_transform_call emits MINT_CALL_DELEGATE for one instead. It
				 * goes live as soon as something routes a runtime-implemented method
				 * to the JIT in a mostly-interpreted process, and the answer there is
				 * the trampoline below rather than a NULL: a caller reads a NULL as
				 * "ask the JIT", and the JIT hands a method implemented outside IL
				 * straight back to this function.
				 */
				if (mono_ee_features.force_use_interpreter)
					return NULL;

				return mono_create_delegate_trampoline (target_domain, method->klass);
			} else if (*name == 'B' && (strcmp (name, "BeginInvoke") == 0)) {
				nm = mono_marshal_get_delegate_begin_invoke (method);
				gpointer compiled_ptr = mono_jit_compile_method_jit_only (nm, error);
				return_val_if_nok (error, NULL);
				return mono_get_addr_from_ftnptr (compiled_ptr);
			} else if (*name == 'E' && (strcmp (name, "EndInvoke") == 0)) {
				nm = mono_marshal_get_delegate_end_invoke (method);
				gpointer compiled_ptr = mono_jit_compile_method_jit_only (nm, error);
				return_val_if_nok (error, NULL);
				return mono_get_addr_from_ftnptr (compiled_ptr);
			}
		}

		full_name = mono_method_full_name (method, TRUE);
		mono_error_set_invalid_program (error, "Unrecognizable runtime implemented method '%s'", full_name);
		g_free (full_name);
		return NULL;
	}

	if (method->wrapper_type == MONO_WRAPPER_OTHER) {
		WrapperInfo *info = mono_marshal_get_wrapper_info (method);

		if (info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_IN || info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_OUT) {
			static MonoTrampInfo *in_tinfo, *out_tinfo;
			MonoTrampInfo *tinfo;
			MonoJitInfo *jinfo;
			gboolean is_in = info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_IN;

			if (is_in && in_tinfo)
				return in_tinfo->code;
			else if (!is_in && out_tinfo)
				return out_tinfo->code;

			/*
			 * This is a special wrapper whose body is implemented in assembly, like a trampoline. We use a wrapper so EH
			 * works.
			 * FIXME: The caller signature doesn't match the callee, which might cause problems on some platforms
			 */
			if (mono_ee_features.use_aot_trampolines)
				mono_aot_get_trampoline_full (is_in ? "gsharedvt_trampoline" : "gsharedvt_out_trampoline", &tinfo);
			else
				mono_arch_get_gsharedvt_trampoline (&tinfo, FALSE);
			jinfo = create_jit_info_for_trampoline (method, tinfo);
			mono_jit_info_table_add (mono_get_root_domain (), jinfo);
			if (is_in)
				in_tinfo = tinfo;
			else
				out_tinfo = tinfo;
			return tinfo->code;
		}
	}

	return NULL;
}

static gpointer
mono_jit_compile_method_with_opt (MonoMethod *method, guint32 opt, gboolean jit_only, MonoError *error)
{
	MonoDomain *target_domain, *domain = mono_domain_get ();
	gpointer code = NULL, p;
	MonoJitICallInfo *callinfo = NULL;
	WrapperInfo *winfo = NULL;
	gboolean use_interp = FALSE;

	error_init (error);

	if (mono_ee_features.force_use_interpreter && !jit_only)
		use_interp = TRUE;
	if (!use_interp && mono_interp_only_classes) {
		for (GSList *l = mono_interp_only_classes; l; l = l->next) {
			if (!strcmp (m_class_get_name (method->klass), (char*)l->data))
				use_interp = TRUE;
		}
	}
	if (use_interp) {
		code = mini_get_interp_callbacks ()->create_method_pointer (method, TRUE, error);
		if (code)
			return code;
	}

	if (mono_llvm_only)
		/* Should be handled by the caller */
		g_assert (!(method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED));

	/*
	 * ICALL wrappers are handled specially, since there is only one copy of them
	 * shared by all appdomains.
	 */
	if (method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE)
		winfo = mono_marshal_get_wrapper_info (method);
	if (winfo && winfo->subtype == WRAPPER_SUBTYPE_ICALL_WRAPPER) {
		callinfo = mono_find_jit_icall_info (winfo->d.icall.jit_icall_id);

		/* Must be domain neutral since there is only one copy */
		opt |= MONO_OPT_SHARED;
	} else {
		/* MONO_OPT_SHARED is no longer supported, we only use it for icall wrappers */
		opt &= ~MONO_OPT_SHARED;
	}

	if (opt & MONO_OPT_SHARED)
		target_domain = mono_get_root_domain ();
	else
		target_domain = domain;

	if (method->wrapper_type == MONO_WRAPPER_OTHER) {
		WrapperInfo *info = mono_marshal_get_wrapper_info (method);

		g_assert (info);
		if (info->subtype == WRAPPER_SUBTYPE_SYNCHRONIZED_INNER) {
			MonoGenericContext *ctx = NULL;
			if (method->is_inflated)
				ctx = mono_method_get_context (method);
			method = info->d.synchronized_inner.method;
			if (ctx) {
				method = mono_class_inflate_generic_method_checked (method, ctx, error);
				g_assert (is_ok (error)); /* FIXME don't swallow the error */
			}
		}
	}

/* Where a thread that waited out another thread's compile starts again. */
lookup_start:

#ifdef MONO_USE_AOT_COMPILER
	if (opt & MONO_OPT_AOT) {
		MonoDomain *domain = NULL;

		if (mono_aot_mode == MONO_AOT_MODE_INTERP && method->wrapper_type == MONO_WRAPPER_OTHER) {
			WrapperInfo *info = mono_marshal_get_wrapper_info (method);
			g_assert (info);
			if (info->subtype == WRAPPER_SUBTYPE_INTERP_IN || info->subtype == WRAPPER_SUBTYPE_INTERP_LMF)
				/* AOT'd wrappers for interp must be owned by root domain */
				domain = mono_get_root_domain ();
		}

		if (!domain)
			domain = mono_domain_get ();

		mono_class_init_internal (method->klass);

		code = mono_aot_get_method (domain, method, error);
		if (code) {
			MonoVTable *vtable;

			if (mono_gc_is_critical_method (method)) {
				/*
				 * The suspend code needs to be able to lookup these methods by ip in async context,
				 * so preload their jit info.
				 */
				MonoJitInfo *ji = mini_jit_info_table_find (domain, code, NULL);
				g_assert (ji);
			}

			/*
			 * In llvm-only mode, method might be a shared method, so we can't initialize its class.
			 * This is not a problem, since it will be initialized when the method is first
			 * called by init_method ().
			 */
			if (!mono_llvm_only && !mono_class_is_open_constructed_type (m_class_get_byval_arg (method->klass))) {
				vtable = mono_class_vtable_checked (domain, method->klass, error);
				mono_error_assert_ok (error);
				if (!mono_runtime_class_init_full (vtable, error))
					return NULL;
			}
		}
		if (!is_ok (error))
			return NULL;
	}
#endif

	if (!code) {
		code = compile_special (method, target_domain, error);

		if (!is_ok (error))
			return NULL;
	}

	if (!jit_only && !code && mono_aot_only && mono_use_interpreter && method->wrapper_type != MONO_WRAPPER_OTHER) {
		if (mono_llvm_only) {
			/* Signal to the caller that AOTed code is not found */
			return NULL;
		}
		code = mini_get_interp_callbacks ()->create_method_pointer (method, TRUE, error);

		if (!is_ok (error))
			return NULL;
	}

	if (!code) {
		if (mono_class_is_open_constructed_type (m_class_get_byval_arg (method->klass))) {
			char *full_name = mono_type_get_full_name (method->klass);
			mono_error_set_invalid_operation (error, "Could not execute the method because the containing type '%s', is not fully instantiated.", full_name);
			g_free (full_name);
			return NULL;
		}

		if (mono_aot_only) {
			char *fullname = mono_method_get_full_name (method);
			mono_error_set_execution_engine (error, "Attempting to JIT compile method '%s' while running in aot-only mode. See https://docs.microsoft.com/xamarin/ios/internals/limitations for more information.\n", fullname);
			g_free (fullname);

			return NULL;
		}

		if (wait_or_register_method_to_compile (method, target_domain))
			goto lookup_start;
		code = mono_llvm_jit_compile_method (method, target_domain, error);
		unregister_method_for_compile (method, target_domain);
	}
	if (!is_ok (error))
		return NULL;

	if (!code && mono_llvm_only) {
		printf ("AOT method not found in llvmonly mode: %s\n", mono_method_full_name (method, 1));
		g_assert_not_reached ();
	}

	if (!code)
		return NULL;

	//FIXME mini_jit_info_table_find doesn't work yet under wasm due to code_start/code_end issues.
#ifndef HOST_WASM
	if ((method->wrapper_type == MONO_WRAPPER_WRITE_BARRIER || method->wrapper_type == MONO_WRAPPER_ALLOC)) {
		MonoDomain *d;

		/*
		 * SGEN requires the JIT info for these methods to be registered, see is_ip_in_managed_allocator ().
		 */
		MonoJitInfo *ji = mini_jit_info_table_find (mono_domain_get (), (char *)code, &d);
		g_assert (ji);
	}
#endif

	p = mono_create_ftnptr (target_domain, code);

	if (callinfo) {
		// FIXME Locking here is somewhat historical due to mono_register_jit_icall_wrapper taking loader lock.
		// atomic_compare_exchange should suffice.
		mono_loader_lock ();
		mono_jit_lock ();
		if (!callinfo->wrapper) {
			callinfo->wrapper = p;
		}
		mono_jit_unlock ();
		mono_loader_unlock ();
	}

	// FIXME p or callinfo->wrapper or does not matter?
	return p;
}

gpointer
mono_jit_compile_method (MonoMethod *method, MonoError *error)
{
	gpointer code;

	code = mono_jit_compile_method_with_opt (method, mono_get_optimizations_for_method (method, default_opt), FALSE, error);
	return code;
}

/*
 * mono_jit_compile_method_jit_only:
 *
 *   Compile METHOD using the JIT/AOT, even in interpreted mode.
 */
gpointer
mono_jit_compile_method_jit_only (MonoMethod *method, MonoError *error)
{
	gpointer code;

	code = mono_jit_compile_method_with_opt (method, mono_get_optimizations_for_method (method, default_opt), TRUE, error);
	return code;
}

/*
 * mono_jit_free_method:
 *
 *  Free all memory allocated by the JIT for METHOD.
 */
static void
mono_jit_free_method (MonoDomain *domain, MonoMethod *method)
{
	MonoJitDomainInfo *info = domain_jit_info (domain);

	g_assert (method->dynamic);

	/*
	 * mono_free_method () hands METHOD straight back to the allocator, so
	 * everything keyed by it has to go now whether or not the classic back end
	 * ever compiled it: an entry left behind is one the next method to land on
	 * this address finds and takes for its own. This drops the record, and with
	 * it whatever the interpreter kept for the method.
	 */
	mono_llvm_jit_free_method (method);

	mono_debug_remove_method (method, domain);

	mono_domain_lock (domain);
	g_hash_table_remove (info->jump_trampoline_hash, method);
	g_hash_table_remove (info->seq_points, method);
	/* requires the domain lock - took above */
	mono_conc_hashtable_remove (info->runtime_invoke_hash, method);
	mono_domain_unlock (domain);
}

gpointer
mono_jit_search_all_backends_for_jit_info (MonoDomain *domain, MonoMethod *method, MonoJitInfo **out_ji)
{
	gpointer code;
	MonoJitInfo *ji = NULL;

	/* Might be JITted. The backend's own map is the only place a body is recorded. */
	code = mono_llvm_jit_find_body (domain, method);
	if (code)
		ji = mono_jit_info_table_find (domain, code);

	if (!code) {
		ERROR_DECL (oerror);

		/* Might be AOTed code */
		mono_class_init_internal (method->klass);
		code = mono_aot_get_method (domain, method, oerror);
		if (code) {
			mono_error_assert_ok (oerror);
			ji = mono_jit_info_table_find (domain, code);
		} else {
			if (!is_ok (oerror))
				mono_error_cleanup (oerror);

			/* Might be interpreted */
			ji = mini_get_interp_callbacks ()->find_jit_info (domain, method);
		}
	}

	*out_ji = ji;

	return code;
}

static void
add_body (MonoJitInfo *ji, void *user_data)
{
	GPtrArray *bodies = (GPtrArray *)user_data;
	guint i;

	if (!ji)
		return;
	for (i = 0; i < bodies->len; ++i)
		if (g_ptr_array_index (bodies, i) == ji)
			return;
	g_ptr_array_add (bodies, ji);
}

/*
 * mono_jit_search_all_backends_for_all_jit_infos:
 *
 *   Append to BODIES the jit info of every live body of METHOD in DOMAIN.
 *
 * A method has more than one when it has been compiled more than once: the
 * stubs name the newest, but a thread that entered an older body is still in
 * it. The two engines can also both hold a body, since a method the
 * interpreter has transformed can be JITted afterwards.
 */
void
mono_jit_search_all_backends_for_all_jit_infos (MonoDomain *domain, MonoMethod *method, GPtrArray *bodies)
{
	gpointer code;

	mono_llvm_jit_foreach_body (domain, method, add_body, bodies);

	/*
	 * AOT code only when nothing has been compiled: asking for it loads it, and
	 * a method already running out of the JIT has no use for another body.
	 */
	if (bodies->len == 0) {
		ERROR_DECL (oerror);

		mono_class_init_internal (method->klass);
		code = mono_aot_get_method (domain, method, oerror);
		if (code) {
			mono_error_assert_ok (oerror);
			add_body (mono_jit_info_table_find (domain, code), bodies);
		} else if (!is_ok (oerror)) {
			mono_error_cleanup (oerror);
		}
	}

	add_body (mini_get_interp_callbacks ()->find_jit_info (domain, method), bodies);
}

/*
 * mini_install_pending_breakpoints:
 *
 *   Install every breakpoint already set on METHOD into JI, a body of METHOD in
 * DOMAIN.
 *
 * Called from the compiler before the body goes live, so that a body arriving
 * while the debugger is attached starts out carrying what the debugger asked
 * for rather than picking it up afterwards.
 */
void
mini_install_pending_breakpoints (MonoDomain *domain, MonoMethod *method, MonoJitInfo *ji)
{
#ifndef DISABLE_SDB
	mono_de_add_pending_breakpoints (domain, method, ji);
#endif
}

static guint32 bisect_opt = 0;
static GHashTable *bisect_methods_hash = NULL;

void
mono_set_bisect_methods (guint32 opt, const char *method_list_filename)
{
	FILE *file;
	char method_name [2048];

	bisect_opt = opt;
	bisect_methods_hash = g_hash_table_new (g_str_hash, g_str_equal);
	g_assert (bisect_methods_hash);

	file = fopen (method_list_filename, "r");
	g_assert (file);

	while (fgets (method_name, sizeof (method_name), file)) {
		size_t len = strlen (method_name);
		g_assert (len > 0);
		g_assert (method_name [len - 1] == '\n');
		method_name [len - 1] = 0;
		g_hash_table_insert (bisect_methods_hash, g_strdup (method_name), GINT_TO_POINTER (1));
	}
	g_assert (feof (file));
}

gboolean mono_do_single_method_regression = FALSE;
guint32 mono_single_method_regression_opt = 0;
MonoMethod *mono_current_single_method;
GSList *mono_single_method_list;
GHashTable *mono_single_method_hash;

guint32
mono_get_optimizations_for_method (MonoMethod *method, guint32 opt)
{
	g_assert (method);

	if (bisect_methods_hash) {
		char *name = mono_method_full_name (method, TRUE);
		void *res = g_hash_table_lookup (bisect_methods_hash, name);
		g_free (name);
		if (res)
			return opt | bisect_opt;
	}
	if (!mono_do_single_method_regression)
		return opt;
	if (!mono_current_single_method) {
		if (!mono_single_method_hash)
			mono_single_method_hash = g_hash_table_new (g_direct_hash, g_direct_equal);
		if (!g_hash_table_lookup (mono_single_method_hash, method)) {
			g_hash_table_insert (mono_single_method_hash, method, method);
			mono_single_method_list = g_slist_prepend (mono_single_method_list, method);
		}
		return opt;
	}
	if (method == mono_current_single_method)
		return mono_single_method_regression_opt;
	return opt;
}

/*
 * mono_jit_method_is_compiled:
 *
 *   Whether METHOD already has native code in DOMAIN, without compiling it if it
 * does not. The answer only ever goes from FALSE to TRUE - a body is never taken
 * back, and recompiling redirects the method's stubs rather than replacing them -
 * so a caller may cache a TRUE for good.
 */
gboolean
mono_jit_method_is_compiled (MonoDomain *domain, MonoMethod *method)
{
	return mono_llvm_jit_find_body (domain, method) != NULL;
}

typedef struct {
	MonoMethod *method;
	gpointer compiled_method;
	gpointer runtime_invoke;
	MonoVTable *vtable;
	MonoDynCallInfo *dyn_call_info;
	MonoClass *ret_box_class;
	MonoMethodSignature *sig;
	gboolean gsharedvt_invoke;
	gboolean use_interp;
	gpointer *wrapper_arg;
} RuntimeInvokeInfo;

static RuntimeInvokeInfo*
create_runtime_invoke_info (MonoDomain *domain, MonoMethod *method, gpointer compiled_method, gboolean callee_gsharedvt, gboolean use_interp, MonoError *error)
{
	MonoMethod *invoke;
	RuntimeInvokeInfo *info = NULL;
	RuntimeInvokeInfo *ret = NULL;

	info = g_new0 (RuntimeInvokeInfo, 1);
	info->compiled_method = compiled_method;
	info->use_interp = use_interp;
	if (mono_llvm_only && method->string_ctor)
		info->sig = mono_marshal_get_string_ctor_signature (method);
	else
		info->sig = mono_method_signature_internal (method);

	invoke = mono_marshal_get_runtime_invoke (method, FALSE);
	(void)invoke;
	info->vtable = mono_class_vtable_checked (domain, method->klass, error);
	if (!is_ok (error))
		goto exit;
	g_assert (info->vtable);

	MonoMethodSignature *sig;
	sig = info->sig;
	MonoType *ret_type;

	/*
	 * We want to avoid AOTing 1000s of runtime-invoke wrappers when running
	 * in full-aot mode, so we use a slower, but more generic wrapper if
	 * possible, built on top of the OP_DYN_CALL opcode provided by the JIT.
	 */
#ifdef MONO_ARCH_DYN_CALL_SUPPORTED
	if (!mono_llvm_only && (mono_aot_only || mini_debug_options.dyn_runtime_invoke)) {
		gboolean supported = TRUE;
		int i;

		if (method->string_ctor)
			sig = mono_marshal_get_string_ctor_signature (method);

		for (i = 0; i < sig->param_count; ++i) {
			MonoType *t = sig->params [i];

			if (t->byref && t->type == MONO_TYPE_GENERICINST && mono_class_is_nullable (mono_class_from_mono_type_internal (t)))
				supported = FALSE;
		}

		if (mono_class_is_contextbound (method->klass) || !info->compiled_method)
			supported = FALSE;

		if (supported) {
			info->dyn_call_info = mono_arch_dyn_call_prepare (sig);
			if (mini_debug_options.dyn_runtime_invoke)
				g_assert (info->dyn_call_info);
		}
	}
#endif

	ret_type = sig->ret;
	switch (ret_type->type) {
	case MONO_TYPE_VOID:
		break;
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
		info->ret_box_class = mono_class_from_mono_type_internal (ret_type);
		break;
	case MONO_TYPE_PTR:
		info->ret_box_class = mono_defaults.int_class;
		break;
	case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_OBJECT:
		break;
	case MONO_TYPE_GENERICINST:
		if (!MONO_TYPE_IS_REFERENCE (ret_type))
			info->ret_box_class = mono_class_from_mono_type_internal (ret_type);
		break;
	case MONO_TYPE_VALUETYPE:
		info->ret_box_class = mono_class_from_mono_type_internal (ret_type);
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	if (info->use_interp) {
		ret = info;
		info = NULL;
		goto exit;
	}

	if (!info->dyn_call_info) {
		if (mono_llvm_only) {
#ifndef MONO_ARCH_GSHAREDVT_SUPPORTED
			g_assert_not_reached ();
#endif
			info->gsharedvt_invoke = TRUE;
			if (!callee_gsharedvt) {
				/* Invoke a gsharedvt out wrapper instead */
				MonoMethod *wrapper = mini_get_gsharedvt_out_sig_wrapper (sig);
				MonoMethodSignature *wrapper_sig = mini_get_gsharedvt_out_sig_wrapper_signature (sig->hasthis, sig->ret->type != MONO_TYPE_VOID, sig->param_count);

				info->wrapper_arg = g_malloc0 (2 * sizeof (gpointer));
				info->wrapper_arg [0] = mini_llvmonly_add_method_wrappers (method, info->compiled_method, FALSE, FALSE, &(info->wrapper_arg [1]));

				/* Pass has_rgctx == TRUE since the wrapper has an extra arg */
				invoke = mono_marshal_get_runtime_invoke_for_sig (wrapper_sig);
				g_free (wrapper_sig);

				info->compiled_method = mono_jit_compile_method (wrapper, error);
				if (!is_ok (error))
					goto exit;
			} else {
				/* Gsharedvt methods can be invoked the same way */
				/* The out wrapper has the same signature as the compiled gsharedvt method */
				MonoMethodSignature *wrapper_sig = mini_get_gsharedvt_out_sig_wrapper_signature (sig->hasthis, sig->ret->type != MONO_TYPE_VOID, sig->param_count);

				info->wrapper_arg = (gpointer*)(mono_method_needs_static_rgctx_invoke (method, TRUE) ? mini_method_get_rgctx (method) : NULL);

				invoke = mono_marshal_get_runtime_invoke_for_sig (wrapper_sig);
				g_free (wrapper_sig);
			}
		}
		info->runtime_invoke = mono_jit_compile_method (invoke, error);
		if (!is_ok (error))
			goto exit;
	}

	ret = info;
	info = NULL;
exit:
	g_free (info);
	return ret;
}

static MonoObject*
mono_llvmonly_runtime_invoke (MonoMethod *method, RuntimeInvokeInfo *info, void *obj, void **params, MonoObject **exc, MonoError *error)
{
	MonoMethodSignature *sig = info->sig;
	MonoDomain *domain = mono_domain_get ();
	MonoObject *(*runtime_invoke) (MonoObject *this_obj, void **params, MonoObject **exc, void* compiled_method);
	gpointer retval_ptr;
	guint8 retval [256];
	int i, pindex;

	error_init (error);

	g_assert (info->gsharedvt_invoke);

	/*
	 * Instead of invoking the method directly, we invoke a gsharedvt out wrapper.
	 * The advantage of this is the gsharedvt out wrappers have a reduced set of
	 * signatures, so we only have to generate runtime invoke wrappers for these
	 * signatures.
	 * This code also handles invocation of gsharedvt methods directly, no
	 * out wrappers are used in that case.
	 */
	// allocate param_refs = param_count and args = param_count + hasthis + 2.
	int const param_count = sig->param_count;
	gpointer* const param_refs = g_newa (gpointer, param_count * 2 + sig->hasthis + 2);
	gpointer* const args = param_refs + param_count;
	pindex = 0;
	/*
	 * The runtime invoke wrappers expects pointers to primitive types, so have to
	 * use indirections.
	 */
	if (sig->hasthis)
		args [pindex ++] = &obj;
	if (sig->ret->type != MONO_TYPE_VOID) {
		retval_ptr = &retval;
		args [pindex ++] = &retval_ptr;
	}
	for (i = 0; i < sig->param_count; ++i) {
		MonoType *t = sig->params [i];

		if (t->type == MONO_TYPE_GENERICINST && mono_class_is_nullable (mono_class_from_mono_type_internal (t))) {
			MonoClass *klass = mono_class_from_mono_type_internal (t);
			guint8 *nullable_buf;
			int size;

			size = mono_class_value_size (klass, NULL);
			nullable_buf = g_alloca (size);
			g_assert (nullable_buf);

			/* The argument pointed to by params [i] is either a boxed vtype or null */
			mono_nullable_init (nullable_buf, (MonoObject*)params [i], klass);
			params [i] = nullable_buf;
		}

		if (!t->byref && (MONO_TYPE_IS_REFERENCE (t) || t->type == MONO_TYPE_PTR)) {
			param_refs [i] = params [i];
			params [i] = &(param_refs [i]);
		}
		args [pindex ++] = &params [i];
	}
	/* The gsharedvt out wrapper has an extra argument which contains the method to call */
	args [pindex ++] = &info->wrapper_arg;

	runtime_invoke = (MonoObject *(*)(MonoObject *, void **, MonoObject **, void *))info->runtime_invoke;

	runtime_invoke (NULL, args, exc, info->compiled_method);
	if (exc && *exc)
		return NULL;

	if (sig->ret->type != MONO_TYPE_VOID) {
		if (info->ret_box_class)
			return mono_value_box_checked (domain, info->ret_box_class, retval, error);
		else
			return *(MonoObject**)retval;
	} else {
		return NULL;
	}
}

/**
 * mono_jit_runtime_invoke:
 * \param method: the method to invoke
 * \param obj: this pointer
 * \param params: array of parameter values.
 * \param exc: Set to the exception raised in the managed method.
 * \param error: error or caught exception object
 * If \p exc is NULL, \p error is thrown instead.
 * If coop is enabled, \p exc argument is ignored -
 * all exceptions are caught and propagated through \p error
 */
static MonoObject*
mono_jit_runtime_invoke (MonoMethod *method, void *obj, void **params, MonoObject **exc, MonoError *error)
{
	MonoMethod *invoke, *callee;
	MonoObject *(*runtime_invoke) (MonoObject *this_obj, void **params, MonoObject **exc, void* compiled_method);
	MonoDomain *domain = mono_domain_get ();
	MonoJitDomainInfo *domain_info;
	RuntimeInvokeInfo *info, *info2;
	MonoJitInfo *ji = NULL;
	gboolean callee_gsharedvt = FALSE;

	if (mono_ee_features.force_use_interpreter)
		return mini_get_interp_callbacks ()->runtime_invoke (method, obj, params, exc, error);

	error_init (error);
	if (exc)
		*exc = NULL;

	if (obj == NULL && !(method->flags & METHOD_ATTRIBUTE_STATIC) && !method->string_ctor && (method->wrapper_type == 0)) {
		g_warning ("Ignoring invocation of an instance method on a NULL instance.\n");
		return NULL;
	}

	domain_info = domain_jit_info (domain);

	info = (RuntimeInvokeInfo *)mono_conc_hashtable_lookup (domain_info->runtime_invoke_hash, method);

	if (!info) {
		if (mono_security_core_clr_enabled ()) {
			/*
			 * This might be redundant since mono_class_vtable () already does this,
			 * but keep it just in case for moonlight.
			 */
			mono_class_setup_vtable (method->klass);
			if (mono_class_has_failure (method->klass)) {
				mono_error_set_for_class_failure (error, method->klass);
				if (exc)
					*exc = (MonoObject*)mono_class_get_exception_for_failure (method->klass);
				return NULL;
			}
		}

		gpointer compiled_method;

		callee = method;
		if (m_class_get_rank (method->klass) && (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) &&
			(method->iflags & METHOD_IMPL_ATTRIBUTE_NATIVE)) {
			/*
			 * Array Get/Set/Address methods. The JIT implements them using inline code
			 * inside the runtime invoke wrappers, so no need to compile them.
			 */
			if (mono_aot_only) {
				/*
				 * Call a wrapper, since the runtime invoke wrapper was not generated.
				 */
				MonoMethod *wrapper;

				wrapper = mono_marshal_get_array_accessor_wrapper (method);
				invoke = mono_marshal_get_runtime_invoke (wrapper, FALSE);
				callee = wrapper;
			} else {
				callee = NULL;
			}
		}

		gboolean use_interp = FALSE;

		if (callee) {
			compiled_method = mono_jit_compile_method_jit_only (callee, error);
			if (!compiled_method) {
				g_assert (!is_ok (error));

				if (mono_use_interpreter)
					use_interp = TRUE;
				else
					return NULL;
			} else {
				if (mono_llvm_only) {
					ji = mini_jit_info_table_find (mono_domain_get (), (char *)mono_get_addr_from_ftnptr (compiled_method), NULL);
					callee_gsharedvt = mini_jit_info_is_gsharedvt (ji);
					if (callee_gsharedvt)
						callee_gsharedvt = mini_is_gsharedvt_variable_signature (mono_method_signature_internal (jinfo_get_method (ji)));
				}

				if (!callee_gsharedvt)
					compiled_method = mini_add_method_trampoline (callee, compiled_method, FALSE);
			}
		} else {
			compiled_method = NULL;
		}

		info = create_runtime_invoke_info (domain, method, compiled_method, callee_gsharedvt, use_interp, error);
		if (!is_ok (error))
			return NULL;

		mono_domain_lock (domain);
		info2 = (RuntimeInvokeInfo *)mono_conc_hashtable_insert (domain_info->runtime_invoke_hash, method, info);
		mono_domain_unlock (domain);
		if (info2) {
			g_free (info);
			info = info2;
		}
	}

	/*
	 * We need this here because mono_marshal_get_runtime_invoke can place
	 * the helper method in System.Object and not the target class.
	 */
	if (!mono_runtime_class_init_full (info->vtable, error)) {
		if (exc)
			*exc = (MonoObject*) mono_error_convert_to_exception (error);
		return NULL;
	}

	/* If coop is enabled, and the caller didn't ask for the exception to be caught separately,
	   we always catch the exception and propagate it through the MonoError */
	gboolean catchExcInMonoError =
		(exc == NULL) && mono_threads_are_safepoints_enabled ();
	MonoObject *invoke_exc = NULL;
	if (catchExcInMonoError)
		exc = &invoke_exc;

	/* The wrappers expect this to be initialized to NULL */
	if (exc)
		*exc = NULL;

#ifdef MONO_ARCH_DYN_CALL_SUPPORTED
	static RuntimeInvokeDynamicFunction dyn_runtime_invoke = NULL;
	if (info->dyn_call_info) {
		if (!dyn_runtime_invoke) {
			mono_domain_lock (domain);

			invoke = mono_marshal_get_runtime_invoke_dynamic ();
			dyn_runtime_invoke = (RuntimeInvokeDynamicFunction)mono_jit_compile_method_jit_only (invoke, error);
			if (!dyn_runtime_invoke && mono_use_interpreter) {
				info->use_interp = TRUE;
				info->dyn_call_info = NULL;
			} else if (!is_ok (error)) {
				mono_domain_unlock (domain);
				return NULL;
			}
			mono_domain_unlock (domain);
		}
	}
	if (info->dyn_call_info) {
		MonoMethodSignature *sig = mono_method_signature_internal (method);
		gpointer *args;
		int i, pindex, buf_size;
		guint8 *buf;
		guint8 retval [256];

		/* Convert the arguments to the format expected by start_dyn_call () */
		args = (void **)g_alloca ((sig->param_count + sig->hasthis) * sizeof (gpointer));
		pindex = 0;
		if (sig->hasthis)
			args [pindex ++] = &obj;
		for (i = 0; i < sig->param_count; ++i) {
			MonoType *t = sig->params [i];

			if (t->byref) {
				args [pindex ++] = &params [i];
			} else if (MONO_TYPE_IS_REFERENCE (t) || t->type == MONO_TYPE_PTR) {
				args [pindex ++] = &params [i];
			} else {
				args [pindex ++] = params [i];
			}
		}

		//printf ("M: %s\n", mono_method_full_name (method, TRUE));

		buf_size = mono_arch_dyn_call_get_buf_size (info->dyn_call_info);
		buf = g_alloca (buf_size);
		memset (buf, 0, buf_size);
		g_assert (buf);

		mono_arch_start_dyn_call (info->dyn_call_info, (gpointer**)args, retval, buf);

		dyn_runtime_invoke (buf, exc, info->compiled_method);
		mono_arch_finish_dyn_call (info->dyn_call_info, buf);

		if (catchExcInMonoError && *exc != NULL) {
			mono_error_set_exception_instance (error, (MonoException*) *exc);
			return NULL;
		}

		if (info->ret_box_class)
			return mono_value_box_checked (domain, info->ret_box_class, retval, error);
		else
			return *(MonoObject**)retval;
	}
#endif

	MonoObject *result;

	if (info->use_interp) {
		result = mini_get_interp_callbacks ()->runtime_invoke (method, obj, params, exc, error);
		return_val_if_nok (error, NULL);
	} else if (mono_llvm_only) {
		result = mono_llvmonly_runtime_invoke (method, info, obj, params, exc, error);
		if (!is_ok (error))
			return NULL;
	} else {
		runtime_invoke = (MonoObject *(*)(MonoObject *, void **, MonoObject **, void *))info->runtime_invoke;

		result = runtime_invoke ((MonoObject *)obj, params, exc, info->compiled_method);
	}
	if (catchExcInMonoError && *exc != NULL) {
		((MonoException *)(*exc))->caught_in_unmanaged = TRUE;
		mono_error_set_exception_instance (error, (MonoException*) *exc);
	}
	return result;
}

MONO_SIG_HANDLER_FUNC (, mono_sigfpe_signal_handler)
{
	MonoException *exc = NULL;
	MonoJitInfo *ji;
	MonoContext mctx;
	MONO_SIG_HANDLER_INFO_TYPE *info = MONO_SIG_HANDLER_GET_INFO ();
	MONO_SIG_HANDLER_GET_CONTEXT;

	ji = mono_jit_info_table_find_internal (mono_domain_get (), mono_arch_ip_from_context (ctx), TRUE, TRUE);

	MONO_ENTER_GC_UNSAFE_UNBALANCED;

#if defined(MONO_ARCH_HAVE_IS_INT_OVERFLOW)
	if (mono_arch_is_int_overflow (ctx, info))
		/*
		 * The spec says this throws ArithmeticException, but MS throws the derived
		 * OverflowException.
		 */
		exc = mono_get_exception_overflow ();
	else
		exc = mono_get_exception_divide_by_zero ();
#else
	exc = mono_get_exception_divide_by_zero ();
#endif

	if (!ji) {
		if (!mono_do_crash_chaining && mono_chain_signal (MONO_SIG_HANDLER_PARAMS))
			goto exit;

		mono_sigctx_to_monoctx (ctx, &mctx);
		if (mono_dump_start ())
			mono_handle_native_crash (mono_get_signame (SIGFPE), &mctx, info);
		if (mono_do_crash_chaining) {
			mono_chain_signal (MONO_SIG_HANDLER_PARAMS);
			goto exit;
		}
	}

	mono_arch_handle_exception (ctx, exc);

exit:
	MONO_EXIT_GC_UNSAFE_UNBALANCED;
}

MONO_SIG_HANDLER_FUNC (, mono_crashing_signal_handler)
{
	MonoContext mctx;
	MONO_SIG_HANDLER_INFO_TYPE *info = MONO_SIG_HANDLER_GET_INFO ();
	MONO_SIG_HANDLER_GET_CONTEXT;

	if (mono_runtime_get_no_exec ())
		exit (1);

	mono_sigctx_to_monoctx (ctx, &mctx);
	if (mono_dump_start ())
#if defined(HAVE_SIG_INFO) && !defined(HOST_WIN32) // info is a siginfo_t
		mono_handle_native_crash (mono_get_signame (info->si_signo), &mctx, info);
#else
		mono_handle_native_crash (mono_get_signame (SIGTERM), &mctx, info);
#endif
	if (mono_do_crash_chaining) {
		mono_chain_signal (MONO_SIG_HANDLER_PARAMS);
		return;
	}
}

#if defined(MONO_ARCH_USE_SIGACTION) || defined(HOST_WIN32)

#define HAVE_SIG_INFO
#define MONO_SIG_HANDLER_DEBUG 1 // "with_fault_addr" but could be extended in future, so "debug"

#ifdef MONO_SIG_HANDLER_DEBUG
// Same as MONO_SIG_HANDLER_FUNC but debug_fault_addr is added to params, and no_optimize.
// The Krait workaround is not needed here, due to this not actually being the signal handler,
// so MONO_SIGNAL_HANDLER_FUNC is combined into it.
#define MONO_SIG_HANDLER_FUNC_DEBUG(access, ftn) access MONO_NO_OPTIMIZATION void ftn \
	(int _dummy, MONO_SIG_HANDLER_INFO_TYPE *_info, void *context, void * volatile debug_fault_addr G_GNUC_UNUSED)
#define MONO_SIG_HANDLER_PARAMS_DEBUG MONO_SIG_HANDLER_PARAMS, debug_fault_addr
#endif

#endif

gboolean
mono_is_addr_implicit_null_check (void *addr)
{
	/* implicit null checks are only expected to work on the first page. larger
	 * offsets are expected to have an explicit null check */
	return addr <= GUINT_TO_POINTER (mono_target_pagesize ());
}

// This function is separate from mono_sigsegv_signal_handler
// so debug_fault_addr can be seen in debugger stacks.
#ifdef MONO_SIG_HANDLER_DEBUG
MONO_NEVER_INLINE
MONO_SIG_HANDLER_FUNC_DEBUG (static, mono_sigsegv_signal_handler_debug)
#else
MONO_SIG_HANDLER_FUNC (, mono_sigsegv_signal_handler)
#endif
{
	MonoJitInfo *ji = NULL;
	MonoDomain *domain = mono_domain_get ();
	gpointer fault_addr = NULL;
	MonoContext mctx;

#if defined(HAVE_SIG_INFO) || defined(MONO_ARCH_SIGSEGV_ON_ALTSTACK)
	MonoJitTlsData *jit_tls = mono_tls_get_jit_tls ();
#endif
#ifdef HAVE_SIG_INFO
	MONO_SIG_HANDLER_INFO_TYPE *info = MONO_SIG_HANDLER_GET_INFO ();
#else
	void *info = NULL;
#endif
	MONO_SIG_HANDLER_GET_CONTEXT;

	mono_sigctx_to_monoctx (ctx, &mctx);

#if defined(MONO_ARCH_SOFT_DEBUG_SUPPORTED) && defined(HAVE_SIG_INFO)
	if (mono_arch_is_single_step_event (info, ctx)) {
		mini_get_dbg_callbacks ()->single_step_event (ctx);
		return;
	} else if (mono_arch_is_breakpoint_event (info, ctx)) {
		mini_get_dbg_callbacks ()->breakpoint_hit (ctx);
		return;
	}
#endif

#if defined(HAVE_SIG_INFO)
#if !defined(HOST_WIN32)
	fault_addr = info->si_addr;
	if (mono_aot_is_pagefault (info->si_addr)) {
		mono_aot_handle_pagefault (info->si_addr);
		return;
	}
	int signo = info->si_signo;
#else
	int signo = SIGSEGV;
#endif

	/* The thread might no be registered with the runtime */
	if (!mono_domain_get () || !jit_tls) {
		if (!mono_do_crash_chaining && mono_chain_signal (MONO_SIG_HANDLER_PARAMS))
			return;
		if (mono_dump_start())
			mono_handle_native_crash (mono_get_signame (signo), &mctx, info);
		if (mono_do_crash_chaining) {
			mono_chain_signal (MONO_SIG_HANDLER_PARAMS);
			return;
		}
	}
#endif

	if (domain) {
		gpointer ip = MINI_FTNPTR_TO_ADDR (mono_arch_ip_from_context (ctx));
		ji = mono_jit_info_table_find_internal (domain, ip, TRUE, TRUE);
	}

#ifdef MONO_ARCH_SIGSEGV_ON_ALTSTACK
	if (mono_handle_soft_stack_ovf (jit_tls, ji, ctx, info, (guint8*)info->si_addr))
		return;

	/* info->si_addr seems to be NULL on some kernels when handling stack overflows */
	fault_addr = info->si_addr;
	if (fault_addr == NULL) {
		fault_addr = MONO_CONTEXT_GET_SP (&mctx);
	}

	if (jit_tls && jit_tls->stack_size &&
		ABS ((guint8*)fault_addr - ((guint8*)jit_tls->end_of_stack - jit_tls->stack_size)) < 8192 * sizeof (gpointer)) {
		/*
		 * The hard-guard page has been hit: there is not much we can do anymore
		 * Print a hopefully clear message and abort.
		 */
		mono_handle_hard_stack_ovf (jit_tls, ji, &mctx, (guint8*)info->si_addr);
		g_assert_not_reached ();
	} else {
		/* The original handler might not like that it is executed on an altstack... */
		if (!ji && mono_chain_signal (MONO_SIG_HANDLER_PARAMS))
			return;

#ifdef TARGET_AMD64
		/* exceptions-amd64.c handles the check itself */
		mono_arch_handle_altstack_exception (ctx, info, info->si_addr, FALSE);
#else
		if (mono_is_addr_implicit_null_check (info->si_addr)) {
			mono_arch_handle_altstack_exception (ctx, info, info->si_addr, FALSE);
		} else {
			// FIXME: This shouldn't run on the altstack
			if (mono_dump_start ())
				mono_handle_native_crash (mono_get_signame (SIGSEGV), &mctx, info);
		}
#endif
	}
#else

	if (!ji) {
		if (!mono_do_crash_chaining && mono_chain_signal (MONO_SIG_HANDLER_PARAMS))
			return;

		if (mono_dump_start ())
			mono_handle_native_crash (mono_get_signame (SIGSEGV), &mctx, (MONO_SIG_HANDLER_INFO_TYPE*)info);

		if (mono_do_crash_chaining) {
			mono_chain_signal (MONO_SIG_HANDLER_PARAMS);
			return;
		}
	}

	if (mono_is_addr_implicit_null_check (fault_addr)) {
		mono_arch_handle_exception (ctx, NULL);
	} else {
		if (mono_dump_start ())
			mono_handle_native_crash (mono_get_signame (SIGSEGV), &mctx, (MONO_SIG_HANDLER_INFO_TYPE*)info);
		if (mono_do_crash_chaining) {
			mono_chain_signal (MONO_SIG_HANDLER_PARAMS);
			return;
		}
	}
#endif
}

#ifdef MONO_SIG_HANDLER_DEBUG

// This function is separate from mono_sigsegv_signal_handler_debug
// so debug_fault_addr can be seen in debugger stacks.
MONO_SIG_HANDLER_FUNC (, mono_sigsegv_signal_handler)
{
#ifdef HOST_WIN32
	gpointer const debug_fault_addr = (gpointer)MONO_SIG_HANDLER_GET_INFO () ->ep->ExceptionRecord->ExceptionInformation [1];
#elif defined (HAVE_SIG_INFO)
	gpointer const debug_fault_addr = MONO_SIG_HANDLER_GET_INFO ()->si_addr;
#else
#error No extra parameter is passed, not even 0, to avoid any confusion.
#endif
	mono_sigsegv_signal_handler_debug (MONO_SIG_HANDLER_PARAMS_DEBUG);
}

#endif // MONO_SIG_HANDLER_DEBUG

MONO_SIG_HANDLER_FUNC (, mono_sigint_signal_handler)
{
	MonoException *exc;
	MONO_SIG_HANDLER_GET_CONTEXT;

	MONO_ENTER_GC_UNSAFE_UNBALANCED;

	exc = mono_get_exception_execution_engine ("Interrupted (SIGINT).");

	mono_arch_handle_exception (ctx, exc);

	MONO_EXIT_GC_UNSAFE_UNBALANCED;
}

#ifndef DISABLE_REMOTING
/* mono_jit_create_remoting_trampoline:
 * @method: pointer to the method info
 *
 * Creates a trampoline which calls the remoting functions. This
 * is used in the vtable of transparent proxies.
 *
 * Returns: a pointer to the newly created code
 */
static gpointer
mono_jit_create_remoting_trampoline (MonoDomain *domain, MonoMethod *method, MonoRemotingTarget target, MonoError *error)
{
	MonoMethod *nm;
	guint8 *addr = NULL;

	error_init (error);

	if ((method->flags & METHOD_ATTRIBUTE_VIRTUAL) && mono_method_signature_internal (method)->generic_param_count) {
		return mono_create_specific_trampoline (method, MONO_TRAMPOLINE_GENERIC_VIRTUAL_REMOTING,
			domain, NULL);
	}

	if ((method->flags & METHOD_ATTRIBUTE_ABSTRACT) ||
	    (mono_method_signature_internal (method)->hasthis && (mono_class_is_marshalbyref (method->klass) || method->klass == mono_defaults.object_class)))
		nm = mono_marshal_get_remoting_invoke_for_target (method, target, error);
	else
		nm = method;
	return_val_if_nok (error, NULL);
	addr = (guint8 *)mono_compile_method_checked (nm, error);
	return_val_if_nok (error, NULL);
	return mono_get_addr_from_ftnptr (addr);
}
#endif

static G_GNUC_UNUSED void
no_imt_trampoline (void)
{
	g_assert_not_reached ();
}

static G_GNUC_UNUSED void
no_vcall_trampoline (void)
{
	g_assert_not_reached ();
}

static gpointer *vtable_trampolines;
static int vtable_trampolines_size;

gpointer
mini_get_vtable_trampoline (MonoVTable *vt, int slot_index)
{
	int index = slot_index + MONO_IMT_SIZE;

	if (mono_llvm_only)
		return mini_llvmonly_get_vtable_trampoline (vt, slot_index, index);

	g_assert (slot_index >= - MONO_IMT_SIZE);
	if (!vtable_trampolines || slot_index + MONO_IMT_SIZE >= vtable_trampolines_size) {
		mono_jit_lock ();
		if (!vtable_trampolines || index >= vtable_trampolines_size) {
			int new_size;
			gpointer new_table;

			new_size = vtable_trampolines_size ? vtable_trampolines_size * 2 : 128;
			while (new_size <= index)
				new_size *= 2;
			new_table = g_new0 (gpointer, new_size);

			if (vtable_trampolines)
				memcpy (new_table, vtable_trampolines, vtable_trampolines_size * sizeof (gpointer));
			g_free (vtable_trampolines);
			mono_memory_barrier ();
			vtable_trampolines = (void **)new_table;
			vtable_trampolines_size = new_size;
		}
		mono_jit_unlock ();
	}

	if (!vtable_trampolines [index])
		vtable_trampolines [index] = mono_create_specific_trampoline (GUINT_TO_POINTER (slot_index), MONO_TRAMPOLINE_VCALL, mono_get_root_domain (), NULL);
	return vtable_trampolines [index];
}

static gpointer
mini_get_imt_trampoline (MonoVTable *vt, int slot_index)
{
	return mini_get_vtable_trampoline (vt, slot_index - MONO_IMT_SIZE);
}

static gboolean
mini_imt_entry_inited (MonoVTable *vt, int imt_slot_index)
{
	if (mono_llvm_only)
		return FALSE;

	gpointer *imt = (gpointer*)vt;
	imt -= MONO_IMT_SIZE;

	return (imt [imt_slot_index] != mini_get_imt_trampoline (vt, imt_slot_index));
}

static gpointer
create_delegate_method_ptr (MonoMethod *method, MonoError *error)
{
	gpointer func;

	if (method_is_dynamic (method)) {
		/* Creating a trampoline would leak memory */
		func = mono_compile_method_checked (method, error);
		return_val_if_nok (error, NULL);
	} else {
		gpointer trampoline = mono_create_jump_trampoline (mono_domain_get (), method, TRUE, error);
		return_val_if_nok (error, NULL);
		func = mono_create_ftnptr (mono_domain_get (), trampoline);
	}
	return func;
}

static void
mini_init_delegate (MonoDelegateHandle delegate, MonoObjectHandle target, gpointer addr, MonoMethod *method, MonoError *error)
{
	MonoDelegate *del = MONO_HANDLE_RAW (delegate);
	MonoDomain *domain = MONO_HANDLE_DOMAIN (delegate);
	/*
	 * Only the address is known here when the delegate comes from ldftn +
	 * newobj; every other caller already knows the method it is binding.
	 */
	gboolean from_ldftn = method == NULL;

	if (!method) {
		MonoJitInfo *ji;
		gpointer lookup_addr = MINI_FTNPTR_TO_ADDR (addr);

		g_assert (addr);
		ji = mono_jit_info_table_find_internal (domain, mono_get_addr_from_ftnptr (lookup_addr), TRUE, TRUE);
		/* Shared code */
		if (!ji && domain != mono_get_root_domain ())
			ji = mono_jit_info_table_find_internal (mono_get_root_domain (), mono_get_addr_from_ftnptr (lookup_addr), TRUE, TRUE);
		if (ji) {
			if (ji->is_trampoline) {
				/* Could be an unbox trampoline etc. */
				method = ji->d.tramp_info->method;
			} else {
				method = mono_jit_info_get_method (ji);
				g_assert (!mono_class_is_gtd (method->klass));
			}
		}

		/*
		 * An entry point the interpreter published is not code the jit-info table
		 * knows about, so the engine that made it is the one that can name it.
		 */
		if (!method && mono_use_interpreter)
			method = mini_get_interp_callbacks ()->method_from_entry (domain, lookup_addr);
	}

	/*
	 * Binding an instance method needs a receiver to bind it to, and a delegate
	 * built over a null one would not fail until it was invoked. Classic mini
	 * emits the check into the code it generates for the ldftn + newobj pair
	 * (method-to-ir.c) and the interpreter makes it in interp_delegate_ctor; a
	 * back end that calls the real constructor rather than open-coding it
	 * reaches neither, so the rule has to hold here too.
	 *
	 * An open delegate is the exception. Its Invoke passes the receiver as its
	 * own first argument, which leaves it one parameter longer than the method
	 * being bound, and there a null target is exactly what is meant.
	 */
	if (from_ldftn && method && MONO_HANDLE_IS_NULL (target)
	    && !(method->flags & METHOD_ATTRIBUTE_STATIC)) {
		MonoMethod *invoke = mono_get_delegate_invoke_internal (mono_handle_class (MONO_HANDLE_CAST (MonoObject, delegate)));
		MonoMethodSignature *invoke_sig = invoke ? mono_method_signature_internal (invoke) : NULL;
		MonoMethodSignature *bound_sig = mono_method_signature_internal (method);

		if (invoke_sig && bound_sig && invoke_sig->param_count == bound_sig->param_count) {
			mono_error_set_argument (error, "this", "Delegate to an instance method cannot have null 'this'");
			return;
		}
	}

	if (method)
		MONO_HANDLE_SETVAL (delegate, method, MonoMethod*, method);

	if (addr)
		MONO_HANDLE_SETVAL (delegate, method_ptr, gpointer, addr);

#ifndef DISABLE_REMOTING
	/*
	 * Either engine can invoke the delegate, and each reads a field of its own:
	 * the interpreter runs interp_method and compiled code calls method_ptr. So
	 * both have to name the remoting wrapper rather than the method behind it.
	 */
	if (!MONO_HANDLE_IS_NULL (target) && mono_class_is_transparent_proxy (mono_handle_class (target))) {
		g_assert (method);
		if (mono_use_interpreter) {
			MONO_HANDLE_SETVAL (delegate, interp_method, gpointer,
					    mini_get_interp_callbacks ()->get_remoting_invoke (method, NULL, error));
			return_if_nok (error);
		}
		if (!mono_ee_features.force_use_interpreter) {
			MonoMethod *invoke = mono_marshal_get_remoting_invoke (method, error);
			return_if_nok (error);
			MONO_HANDLE_SETVAL (delegate, method_ptr, gpointer, mono_compile_method_checked (invoke, error));
			return_if_nok (error);
		}
	}
#endif

	MONO_HANDLE_SET (delegate, target, target);

	/*
	 * A delegate whose Invoke names a type that will not load has the method
	 * and not its signature, and everything below builds the invoke path out of
	 * that signature. This is where the caller can still be told.
	 */
	{
		MonoMethod *invoke = mono_get_delegate_invoke_internal (mono_handle_class (MONO_HANDLE_CAST (MonoObject, delegate)));

		if (invoke) {
			mono_method_signature_checked (invoke, error);
			return_if_nok (error);
		}
	}

	MONO_HANDLE_SETVAL (delegate, invoke_impl, gpointer, mono_create_delegate_trampoline (domain, mono_handle_class (delegate)));

	if (mono_use_interpreter) {
		mini_get_interp_callbacks ()->init_delegate (del, error);
		return_if_nok (error);
	}

	if (mono_llvm_only) {
		g_assert (del->method);
		/* del->method_ptr might already be set to no_llvmonly_interp_method_pointer if the delegate was created from the interpreter */
		del->method_ptr = mini_llvmonly_load_method_delegate (del->method, FALSE, FALSE, &del->extra_arg, error);
	} else if (!del->method_ptr) {
		del->method_ptr = create_delegate_method_ptr (del->method, error);
		return_if_nok (error);
	}
}

/**
 * mini_parse_debug_option:
 * @option: The option to parse.
 *
 * Parses debug options for the mono runtime. The options are the same as for
 * the MONO_DEBUG environment variable.
 *
 */
gboolean
mini_parse_debug_option (const char *option)
{
	// Empty string is ok as consequence of appending ",foo"
	// without first checking for empty.
	if (*option == 0)
		return TRUE;

	if (!strcmp (option, "handle-sigint"))
		mini_debug_options.handle_sigint = TRUE;
	else if (!strcmp (option, "keep-delegates"))
		mini_debug_options.keep_delegates = TRUE;
	else if (!strcmp (option, "reverse-pinvoke-exceptions"))
		mini_debug_options.reverse_pinvoke_exceptions = TRUE;
	else if (!strcmp (option, "collect-pagefault-stats"))
		mini_debug_options.collect_pagefault_stats = TRUE;
	else if (!strcmp (option, "break-on-unverified"))
		mini_debug_options.break_on_unverified = TRUE;
	else if (!strcmp (option, "no-gdb-backtrace"))
		mini_debug_options.no_gdb_backtrace = TRUE;
	else if (!strcmp (option, "suspend-on-native-crash") || !strcmp (option, "suspend-on-sigsegv"))
		mini_debug_options.suspend_on_native_crash = TRUE;
	else if (!strcmp (option, "suspend-on-exception"))
		mini_debug_options.suspend_on_exception = TRUE;
	else if (!strcmp (option, "suspend-on-unhandled"))
		mini_debug_options.suspend_on_unhandled = TRUE;
	else if (!strcmp (option, "dont-free-domains"))
		mono_dont_free_domains = TRUE;
	else if (!strcmp (option, "dyn-runtime-invoke"))
		mini_debug_options.dyn_runtime_invoke = TRUE;
	else if (!strcmp (option, "lldb"))
		mini_debug_options.lldb = TRUE;
	else if (!strcmp (option, "llvm-disable-inlining"))
		mini_debug_options.llvm_disable_inlining = TRUE;
	else if (!strcmp (option, "llvm-disable-implicit-null-checks"))
		mini_debug_options.llvm_disable_implicit_null_checks = TRUE;
	else if (!strncmp (option, "unity-mixed-callstack", strlen("unity-mixed-callstack"))) {
		if (!strncmp (option, "unity-mixed-callstack=", strlen("unity-mixed-callstack=")))
			mini_debug_options.unity_mixed_callstack = atoi(option + strlen ("unity-mixed-callstack="));
		else
			mini_debug_options.unity_mixed_callstack = 1;
	}
	else if (!strcmp (option, "explicit-null-checks"))
		mini_debug_options.explicit_null_checks = TRUE;
	else if (!strcmp (option, "gen-seq-points"))
		mini_debug_options.gen_sdb_seq_points = TRUE;
	else if (!strcmp (option, "gen-compact-seq-points"))
		fprintf (stderr, "Mono Warning: option gen-compact-seq-points is deprecated.\n");
	else if (!strcmp (option, "no-compact-seq-points"))
		mini_debug_options.no_seq_points_compact_data = TRUE;
	else if (!strcmp (option, "single-imm-size"))
		mini_debug_options.single_imm_size = TRUE;
	else if (!strcmp (option, "init-stacks"))
		mini_debug_options.init_stacks = TRUE;
	else if (!strcmp (option, "casts"))
		mini_debug_options.better_cast_details = TRUE;
	else if (!strcmp (option, "soft-breakpoints"))
		mini_debug_options.soft_breakpoints = TRUE;
	else if (!strcmp (option, "check-pinvoke-callconv"))
		mini_debug_options.check_pinvoke_callconv = TRUE;
	else if (!strcmp (option, "use-fallback-tls"))
		mini_debug_options.use_fallback_tls = TRUE;
	else if (!strcmp (option, "debug-domain-unload"))
		mono_enable_debug_domain_unload (TRUE);
	else if (!strcmp (option, "partial-sharing"))
		mono_set_partial_sharing_supported (TRUE);
	else if (!strcmp (option, "align-small-structs"))
		mono_align_small_structs = TRUE;
	else if (!strcmp (option, "native-debugger-break"))
		mini_debug_options.native_debugger_break = TRUE;
	else if (!strcmp (option, "disable_omit_fp"))
		mini_debug_options.disable_omit_fp = TRUE;
	// This is an internal testing feature.
	// Every tail. encountered is required to be optimized.
	// It is asserted.
	else if (!strcmp (option, "test-tailcall-require"))
		mini_debug_options.test_tailcall_require = TRUE;
	else if (!strcmp (option, "verbose-gdb"))
		mini_debug_options.verbose_gdb = TRUE;
	else if (!strcmp (option, "clr-memory-model"))
		// FIXME Kill this debug flag
		mini_debug_options.weak_memory_model = FALSE;
	else if (!strcmp (option, "weak-memory-model"))
		mini_debug_options.weak_memory_model = TRUE;
	else if (!strcmp (option, "top-runtime-invoke-unhandled"))
		mini_debug_options.top_runtime_invoke_unhandled = TRUE;
	else if (!strncmp (option, "thread-dump-dir=", 16))
		mono_set_thread_dump_dir(g_strdup(option + 16));
	else if (!strncmp (option, "aot-skip=", 9)) {
		mini_debug_options.aot_skip_set = TRUE;
		mini_debug_options.aot_skip = atoi (option + 9);
	} else
		return FALSE;

	return TRUE;
}

static void
mini_parse_debug_options (void)
{
	char *options = g_getenv ("MONO_DEBUG");
	gchar **args, **ptr;

	if (!options)
		return;

	args = g_strsplit (options, ",", -1);
	g_free (options);

	for (ptr = args; ptr && *ptr; ptr++) {
		const char *arg = *ptr;

		if (!mini_parse_debug_option (arg)) {
			fprintf (stderr, "Invalid option for the MONO_DEBUG env variable: %s\n", arg);
			// test-tailcall-require is also accepted but not documented.
			// empty string is also accepted and ignored as a consequence
			// of appending ",foo" without checking for empty.
			fprintf (stderr, "Available options: 'handle-sigint', 'keep-delegates', 'reverse-pinvoke-exceptions', 'collect-pagefault-stats', 'break-on-unverified', 'no-gdb-backtrace', 'suspend-on-native-crash', 'suspend-on-sigsegv', 'suspend-on-exception', 'suspend-on-unhandled', 'dont-free-domains', 'dyn-runtime-invoke', 'explicit-null-checks', 'gen-seq-points', 'no-compact-seq-points', 'single-imm-size', 'init-stacks', 'casts', 'soft-breakpoints', 'check-pinvoke-callconv', 'use-fallback-tls', 'debug-domain-unload', 'partial-sharing', 'align-small-structs', 'native-debugger-break', 'thread-dump-dir=DIR', 'no-verbose-gdb', 'llvm_disable_inlining', 'llvm-disable-self-init', 'llvm-disable-implicit-null-checks', 'weak-memory-model'.\n");
			exit (1);
		}
	}

	g_strfreev (args);
}

MonoDebugOptions *
mini_get_debug_options (void)
{
	return &mini_debug_options;
}

static gpointer
mini_create_ftnptr (MonoDomain *domain, gpointer addr)
{
#if defined(PPC_USES_FUNCTION_DESCRIPTOR)
	gpointer* desc = NULL;

	if ((desc = (gpointer*)g_hash_table_lookup (domain->ftnptrs_hash, addr)))
		return desc;
#if defined(__mono_ppc64__)
	desc = mono_domain_alloc0 (domain, 3 * sizeof (gpointer));

	desc [0] = addr;
	desc [1] = NULL;
	desc [2] = NULL;
#	endif
	g_hash_table_insert (domain->ftnptrs_hash, addr, desc);
	return desc;
#else
	return addr;
#endif
}

static gpointer
mini_get_addr_from_ftnptr (gpointer descr)
{
#if defined(PPC_USES_FUNCTION_DESCRIPTOR)
	return *(gpointer*)descr;
#else
	return descr;
#endif
}

static void
register_counters (void)
{
	mono_counters_register ("Compiled methods", MONO_COUNTER_JIT | MONO_COUNTER_INT, &mono_jit_stats.methods_compiled);
	mono_counters_register ("Methods from AOT", MONO_COUNTER_JIT | MONO_COUNTER_INT, &mono_jit_stats.methods_aot);
	mono_counters_register ("Methods from AOT+LLVM", MONO_COUNTER_JIT | MONO_COUNTER_INT, &mono_jit_stats.methods_aot_llvm);
	mono_counters_register ("Methods JITted using mono JIT", MONO_COUNTER_JIT | MONO_COUNTER_INT, &mono_jit_stats.methods_without_llvm);
	mono_counters_register ("Methods JITted using LLVM", MONO_COUNTER_JIT | MONO_COUNTER_INT, &mono_jit_stats.methods_with_llvm);
	mono_counters_register ("Methods using the interpreter", MONO_COUNTER_JIT | MONO_COUNTER_INT, &mono_jit_stats.methods_with_interp);
}

static void runtime_invoke_info_free (gpointer value);

static gint
class_method_pair_equal (gconstpointer ka, gconstpointer kb)
{
	const MonoClassMethodPair *apair = (const MonoClassMethodPair *)ka;
	const MonoClassMethodPair *bpair = (const MonoClassMethodPair *)kb;

	return apair->klass == bpair->klass && apair->method == bpair->method ? 1 : 0;
}

static guint
class_method_pair_hash (gconstpointer data)
{
	const MonoClassMethodPair *pair = (const MonoClassMethodPair *)data;

	return (gsize)pair->klass ^ (gsize)pair->method;
}

/*
 * A method's entry in the seq_points hash is the list of tables its bodies
 * published, one each, and the domain owns every one of them.
 */
static void
free_seq_point_list (gpointer value)
{
	GSList *l;

	for (l = (GSList *)value; l; l = l->next)
		mono_seq_point_info_free (l->data);
	g_slist_free ((GSList *)value);
}

static void
mini_create_jit_domain_info (MonoDomain *domain)
{
	MonoJitDomainInfo *info = g_new0 (MonoJitDomainInfo, 1);

	info->jump_trampoline_hash = g_hash_table_new (mono_aligned_addr_hash, NULL);
	info->jit_trampoline_hash = g_hash_table_new (mono_aligned_addr_hash, NULL);
	info->delegate_trampoline_hash = g_hash_table_new (class_method_pair_hash, class_method_pair_equal);
	info->runtime_invoke_hash = mono_conc_hashtable_new_full (mono_aligned_addr_hash, NULL, NULL, runtime_invoke_info_free);
	info->seq_points = g_hash_table_new_full (mono_aligned_addr_hash, NULL, NULL, free_seq_point_list);
	info->arch_seq_points = g_hash_table_new (mono_aligned_addr_hash, NULL);
	info->jump_target_hash = g_hash_table_new (NULL, NULL);

	domain->runtime_info = info;

	/* After runtime_info is set: the table hangs off it. */
	mono_domain_method_table_init (domain);
}

static void
delete_jump_list (gpointer key, gpointer value, gpointer user_data)
{
	MonoJumpList *jlist = (MonoJumpList *)value;
	g_slist_free ((GSList*)jlist->list);
}

static void
runtime_invoke_info_free (gpointer value)
{
	RuntimeInvokeInfo *info = (RuntimeInvokeInfo*)value;

#ifdef MONO_ARCH_DYN_CALL_SUPPORTED
	if (info->dyn_call_info)
		mono_arch_dyn_call_free (info->dyn_call_info);
#endif
	g_free (info);
}

/*
 * The domain is still whole here - this runs before the unload has taken
 * anything apart. All it does is get the backend's compile worker out of the
 * domain; mini_free_jit_domain_info () is what frees anything.
 */
static void
mini_unloading_jit_domain_info (MonoDomain *domain)
{
	mono_llvm_jit_stop_compiling_for_domain (domain);
}

static void
mini_free_jit_domain_info (MonoDomain *domain)
{
	MonoJitDomainInfo *info = domain_jit_info (domain);

	g_hash_table_foreach (info->jump_target_hash, delete_jump_list, NULL);
	g_hash_table_destroy (info->jump_target_hash);
	g_hash_table_destroy (info->jump_trampoline_hash);
	g_hash_table_destroy (info->jit_trampoline_hash);
	g_hash_table_destroy (info->delegate_trampoline_hash);
	g_hash_table_destroy (info->mrgctx_hash);
	g_hash_table_destroy (info->method_rgctx_hash);
	g_hash_table_destroy (info->interp_method_pointer_hash);
	mono_conc_hashtable_destroy (info->runtime_invoke_hash);
	g_hash_table_destroy (info->seq_points);
	g_hash_table_destroy (info->arch_seq_points);
	if (info->agent_info)
		mini_get_dbg_callbacks ()->free_domain_info (domain);
	g_hash_table_destroy (info->gsharedvt_arg_tramp_hash);

	/*
	 * Before the engine's state for the domain goes: a record names stubs
	 * carved out of that state.
	 */
	mono_domain_method_table_free (domain);
	mono_llvm_jit_free_domain (domain);

	g_free (domain->runtime_info);
	domain->runtime_info = NULL;
}

void
mini_add_profiler_argument (const char *desc)
{
	if (!profile_options)
		profile_options = g_ptr_array_new ();

	g_ptr_array_add (profile_options, (gpointer) g_strdup (desc));
}


const MonoEECallbacks *mono_interp_callbacks_pointer;

void
mini_install_interp_callbacks (const MonoEECallbacks *cbs)
{
	mono_interp_callbacks_pointer = cbs;
}

static MonoDebuggerCallbacks dbg_cbs;

void
mini_install_dbg_callbacks (MonoDebuggerCallbacks *cbs)
{
	g_assert (cbs->version == MONO_DBG_CALLBACKS_VERSION);
	memcpy (&dbg_cbs, cbs, sizeof (MonoDebuggerCallbacks));
}

MonoDebuggerCallbacks*
mini_get_dbg_callbacks (void)
{
	return &dbg_cbs;
}

int
mono_ee_api_version (void)
{
	return MONO_EE_API_VERSION;
}

static gboolean
mini_is_interpreter_enabled (void)
{
	return mono_use_interpreter;
}

static const char*
mono_get_runtime_build_version (void);

MonoDomain *
mini_init (const char *filename, const char *runtime_version)
{
	ERROR_DECL (error);
	MonoDomain *domain;

	MonoRuntimeCallbacks callbacks;

	static const MonoThreadInfoRuntimeCallbacks ticallbacks = {
		MONO_THREAD_INFO_RUNTIME_CALLBACKS (MONO_INIT_CALLBACK, mono)
	};

	MONO_VES_INIT_BEGIN ();

	CHECKED_MONO_INIT ();

#if defined(__linux__)
	if (access ("/proc/self/maps", F_OK) != 0) {
		g_print ("Mono requires /proc to be mounted.\n");
		exit (1);
	}
#endif

	mono_llvm_jit_init ();

	mono_interp_stub_init ();
#if !defined (DISABLE_INTERPRETER) && defined (MONO_ARCH_INTERPRETER_SUPPORTED) && !defined (MONO_CROSS_COMPILE)
	/* Tier 0 is the entry tier, so the interpreter runs beside the JIT. */
	if (mono_llvm_jit_tier0_enabled ())
		mono_use_interpreter = TRUE;
#endif
#ifndef DISABLE_INTERPRETER
	if (mono_use_interpreter)
		mono_ee_interp_init (mono_interp_opts_string);
#endif

	mono_debugger_agent_stub_init ();
#ifndef DISABLE_SDB
	mono_debugger_agent_init ();
#endif

	if (sdb_options)
		mini_get_dbg_callbacks ()->parse_options (sdb_options);

	mono_os_mutex_init_recursive (&jit_mutex);

	mono_cross_helpers_run ();

	mono_counters_init ();

	mini_jit_init ();

	mini_jit_init_job_control ();

	/* Happens when using the embedding interface */
	if (!default_opt_set)
		default_opt = mono_parse_default_optimizations (NULL);

#ifdef MONO_ARCH_GSHAREDVT_SUPPORTED
	if (mono_aot_only)
		mono_set_generic_sharing_vt_supported (TRUE);
#else
	if (mono_llvm_only)
		mono_set_generic_sharing_vt_supported (TRUE);
#endif

	mono_tls_init_runtime_keys ();

	if (!global_codeman)
		global_codeman = mono_code_manager_new ();

	memset (&callbacks, 0, sizeof (callbacks));
	callbacks.create_ftnptr = mini_create_ftnptr;
	callbacks.get_addr_from_ftnptr = mini_get_addr_from_ftnptr;
	callbacks.get_runtime_build_info = mono_get_runtime_build_info;
	callbacks.get_runtime_build_version = mono_get_runtime_build_version;
	callbacks.set_cast_details = mono_set_cast_details;
	callbacks.debug_log = mini_get_dbg_callbacks ()->debug_log;
	callbacks.debug_log_is_enabled = mini_get_dbg_callbacks ()->debug_log_is_enabled;
	callbacks.get_vtable_trampoline = mini_get_vtable_trampoline;
	callbacks.get_imt_trampoline = mini_get_imt_trampoline;
	callbacks.imt_entry_inited = mini_imt_entry_inited;
	callbacks.init_delegate = mini_init_delegate;
#define JIT_INVOKE_WORKS
#ifdef JIT_INVOKE_WORKS
	callbacks.runtime_invoke = mono_jit_runtime_invoke;
#endif
#define JIT_TRAMPOLINES_WORK
#ifdef JIT_TRAMPOLINES_WORK
	callbacks.compile_method = mono_jit_compile_method;
	callbacks.create_jit_trampoline = mono_create_jit_trampoline;
	callbacks.create_delegate_trampoline = mono_create_delegate_trampoline;
	callbacks.free_method = mono_jit_free_method;
#ifndef DISABLE_REMOTING
	callbacks.create_remoting_trampoline = mono_jit_create_remoting_trampoline;
#endif
#endif
#ifndef DISABLE_REMOTING
	if (mono_use_interpreter)
		callbacks.interp_get_remoting_invoke = mini_get_interp_callbacks ()->get_remoting_invoke;
#endif
	callbacks.is_interpreter_enabled = mini_is_interpreter_enabled;
	callbacks.get_weak_field_indexes = mono_aot_get_weak_field_indexes;

#ifndef DISABLE_CRASH_REPORTING
	callbacks.install_state_summarizer = mini_register_sigterm_handler;
#endif
#ifdef ENABLE_METADATA_UPDATE
	callbacks.metadata_update_init = mini_metadata_update_init;
	callbacks.metadata_update_published = mini_invalidate_transformed_interp_methods;
#endif

	mono_install_callbacks (&callbacks);

#ifndef HOST_WIN32
	mono_w32handle_init ();
#endif

	mono_thread_info_runtime_init (&ticallbacks);

	if (g_hasenv ("MONO_DEBUG")) {
		mini_parse_debug_options ();
	}

	mono_code_manager_init ();

#ifdef MONO_ARCH_HAVE_CODE_CHUNK_TRACKING

	static const MonoCodeManagerCallbacks code_manager_callbacks = {

#undef MONO_CODE_MANAGER_CALLBACK
#define MONO_CODE_MANAGER_CALLBACK(ret, name, sig) mono_arch_code_ ## name,
		MONO_CODE_MANAGER_CALLBACKS

	};

	mono_code_manager_install_callbacks (&code_manager_callbacks);
#endif

	mono_hwcap_init ();

	mono_arch_cpu_init ();

	mono_arch_init ();

	mono_unwind_init ();

	if (mini_debug_options.lldb || g_hasenv ("MONO_LLDB")) {
		mono_lldb_init ("");
		mono_dont_free_domains = TRUE;
	}

	mono_trampolines_init ();

	if (default_opt & MONO_OPT_AOT)
		mono_aot_init ();

	mini_get_dbg_callbacks ()->init ();

#ifdef TARGET_WASM
	mono_wasm_debugger_init ();
#endif

#ifdef MONO_ARCH_GSHARED_SUPPORTED
	mono_set_generic_sharing_supported ((default_opt & MONO_OPT_GSHARED) != 0);
#endif

	mono_thread_info_signals_init ();

	mono_init_native_crash_info ();

#ifndef MONO_CROSS_COMPILE
	mono_runtime_install_handlers ();
#endif
	mono_threads_install_cleanup (mini_thread_cleanup);

#ifdef JIT_TRAMPOLINES_WORK
	mono_install_create_domain_hook (mini_create_jit_domain_info);
	mono_install_unloading_domain_hook (mini_unloading_jit_domain_info);
	mono_install_free_domain_hook (mini_free_jit_domain_info);
#endif
	mono_install_get_cached_class_info (mono_aot_get_cached_class_info);
	mono_install_get_class_from_name (mono_aot_get_class_from_name);
	mono_install_jit_info_find_in_aot (mono_aot_find_jit_info);

	mono_profiler_state.context_enable = mini_profiler_context_enable;
	mono_profiler_state.context_get_this = mini_profiler_context_get_this;
	mono_profiler_state.context_get_argument = mini_profiler_context_get_argument;
	mono_profiler_state.context_get_local = mini_profiler_context_get_local;
	mono_profiler_state.context_get_result = mini_profiler_context_get_result;
	mono_profiler_state.context_free_buffer = mini_profiler_context_free_buffer;

	if (g_hasenv ("MONO_PROFILE")) {
		gchar *profile_env = g_getenv ("MONO_PROFILE");
		mini_add_profiler_argument (profile_env);
		g_free (profile_env);
	}

	if (profile_options)
		for (guint i = 0; i < profile_options->len; i++)
			mono_profiler_load ((const char *) g_ptr_array_index (profile_options, i));

	mono_profiler_started ();

	if (mini_debug_options.collect_pagefault_stats)
		mono_aot_set_make_unreadable (TRUE);

	if (runtime_version)
		domain = mono_init_version (filename, runtime_version);
	else
		domain = mono_init_from_assembly (filename, filename);

	guint mono_mixed_options = g_getenv ("UNITY_MIXED_CALLSTACK") ? atoi(g_getenv("UNITY_MIXED_CALLSTACK")) : 0;
	if (mini_get_debug_options()->unity_mixed_callstack || mono_mixed_options) {
		mono_mixed_options = mini_get_debug_options()->unity_mixed_callstack ? mini_get_debug_options()->unity_mixed_callstack : mono_mixed_options;
		mixed_callstack_plugin_init(mono_mixed_options, domain);
	}

#if defined(ENABLE_PERFTRACING) && !defined(DISABLE_EVENTPIPE)
	if (mono_compile_aot)
		ds_server_disable ();

	ep_init ();
#endif

	if (mono_aot_only) {
		/* This helps catch code allocation requests */
		mono_code_manager_set_read_only (mono_domain_ambient_memory_manager (domain)->code_mp);
		mono_marshal_use_aot_wrappers (TRUE);
	}

	if (mono_llvm_only) {
		mono_install_imt_trampoline_builder (mini_llvmonly_get_imt_trampoline);
		mono_set_always_build_imt_trampolines (TRUE);
	} else if (mono_aot_only) {
		mono_install_imt_trampoline_builder (mono_aot_get_imt_trampoline);
	} else {
		mono_install_imt_trampoline_builder (mono_arch_build_imt_trampoline);
	}

	/*Init arch tls information only after the metadata side is inited to make sure we see dynamic appdomain tls keys*/
	mono_arch_finish_init ();

	/* This must come after mono_init () in the aot-only case */
	mono_exceptions_init ();

	/* This should come after mono_init () too */
	mini_gc_init ();

	mono_create_icall_signatures ();

	register_counters ();

#define JIT_CALLS_WORK
#ifdef JIT_CALLS_WORK
	/* Needs to be called here since register_jit_icall depends on it */
	mono_marshal_init ();

	mono_arch_register_lowlevel_calls ();

	register_icalls ();

	mono_generic_sharing_init ();
#endif

#ifdef MONO_ARCH_SIMD_INTRINSICS
#endif

#ifndef ENABLE_NETCORE
	mono_tasklets_init ();
#endif

	register_trampolines (domain);

	if (mono_compile_aot)
		/*
		 * Avoid running managed code when AOT compiling, since the platform
		 * might only support aot-only execution.
		 */
		mono_runtime_set_no_exec (TRUE);

	mono_mem_account_register_counters ();

#define JIT_RUNTIME_WORKS
#ifdef JIT_RUNTIME_WORKS
	mono_install_runtime_cleanup (runtime_cleanup);
	mono_runtime_init_checked (domain, (MonoThreadStartCB)mono_thread_start_cb, mono_thread_attach_cb, error);
	mono_error_assert_ok (error);
	mono_thread_internal_attach (domain);
	MONO_PROFILER_RAISE (thread_name, (MONO_NATIVE_THREAD_ID_TO_UINT (mono_native_thread_id_get ()), "Main"));
#endif
	mono_threads_set_runtime_startup_finished ();

#if defined(ENABLE_PERFTRACING) && !defined(DISABLE_EVENTPIPE)
	ep_finish_init ();
#endif

	if (mono_profiler_sampling_enabled ())
		mono_runtime_setup_stat_profiler ();

	MONO_PROFILER_RAISE (runtime_initialized, ());

	/*
	 * Last, because reading the override assembly loads assemblies and reads
	 * their custom attributes, and because an override may name a method in any
	 * of them - so nothing the overrides could target must have run yet.
	 */
	mono_method_overrides_init ();

	MONO_VES_INIT_END ();

	return domain;
}

static void
register_icalls (void)
{
	mono_add_internal_call_internal ("System.Diagnostics.StackFrame::get_frame_info",
				ves_icall_get_frame_info);
	mono_add_internal_call_internal ("System.Diagnostics.StackTrace::get_trace",
				ves_icall_get_trace);
	mono_add_internal_call_internal ("Mono.Runtime::mono_runtime_install_handlers",
				mono_runtime_install_handlers);
	mono_add_internal_call_internal ("Mono.Runtime::mono_runtime_cleanup_handlers",
				mono_runtime_cleanup_handlers);
	mono_domain_method_register_icalls ();
	mono_llvm_jit_register_icalls ();

#if defined(HOST_ANDROID) || defined(TARGET_ANDROID)
	mono_add_internal_call_internal ("System.Diagnostics.Debugger::Mono_UnhandledException_internal",
							mini_get_dbg_callbacks ()->unhandled_exception);
#endif

	/*
	 * It's important that we pass `TRUE` as the last argument here, as
	 * it causes the JIT to omit a wrapper for these icalls. If the JIT
	 * *did* emit a wrapper, we'd be looking at infinite recursion since
	 * the wrapper would call the icall which would call the wrapper and
	 * so on.
	 */
	register_icall (mono_profiler_raise_method_enter, mono_icall_sig_void_ptr_ptr, TRUE);
	register_icall (mono_profiler_raise_method_leave, mono_icall_sig_void_ptr_ptr, TRUE);
	register_icall (mono_profiler_raise_method_tail_call, mono_icall_sig_void_ptr_ptr, TRUE);
	register_icall (mono_profiler_raise_exception_clause, mono_icall_sig_void_ptr_int_int_object, TRUE);

	register_icall (mono_trace_enter_method, mono_icall_sig_void_ptr_ptr_ptr, TRUE);
	register_icall (mono_trace_leave_method, mono_icall_sig_void_ptr_ptr_ptr, TRUE);
	register_icall (mono_trace_tail_method, mono_icall_sig_void_ptr_ptr_ptr, TRUE);
	g_assert (mono_get_lmf_addr == mono_tls_get_lmf_addr);
	register_icall (mono_jit_set_domain, mono_icall_sig_void_ptr, TRUE);
	register_icall (mono_domain_get, mono_icall_sig_ptr, TRUE);

	register_icall (mono_llvm_throw_exception, mono_icall_sig_void_object, TRUE);
	register_icall (mono_llvm_rethrow_exception, mono_icall_sig_void_object, TRUE);
	register_icall (mono_llvm_resume_exception, mono_icall_sig_void, TRUE);
	register_icall (mono_llvm_match_exception, mono_icall_sig_int_ptr_int_int_ptr_object, TRUE);
	register_icall (mono_llvm_clear_exception, NULL, TRUE);
	register_icall (mono_llvm_load_exception, mono_icall_sig_object, TRUE);
	register_icall (mono_llvm_throw_corlib_exception, mono_icall_sig_void_int, TRUE);
#if defined(ENABLE_LLVM) && defined(HAVE_UNWIND_H)
	// FIXME: This is broken
#ifndef TARGET_WASM
	register_icall (mono_debug_personality, mono_icall_sig_int_int_int_ptr_ptr_ptr, TRUE);
#endif
#endif

	if (!mono_llvm_only) {
		register_dyn_icall (mono_get_throw_exception (), mono_arch_throw_exception, mono_icall_sig_void_object, TRUE);
		register_dyn_icall (mono_get_rethrow_exception (), mono_arch_rethrow_exception, mono_icall_sig_void_object, TRUE);
		register_dyn_icall (mono_get_throw_corlib_exception (), mono_arch_throw_corlib_exception, mono_icall_sig_void_ptr, TRUE);
	}
	register_icall (mono_thread_get_undeniable_exception, mono_icall_sig_object, FALSE);
	register_icall (ves_icall_thread_finish_async_abort, mono_icall_sig_void, FALSE);
	register_icall (mono_thread_interruption_checkpoint, mono_icall_sig_object, FALSE);
	register_icall (mono_thread_force_interruption_checkpoint_noraise, mono_icall_sig_object, FALSE);

	register_icall (mono_threads_state_poll, mono_icall_sig_void, FALSE);

#ifdef MONO_ARCH_SOFT_FLOAT_FALLBACK
	if (mono_arch_is_soft_float ()) {
		register_icall (mono_fload_r4, mono_icall_sig_double_ptr, FALSE);
		register_icall (mono_fstore_r4, mono_icall_sig_void_double_ptr, FALSE);
		register_icall (mono_fload_r4_arg, mono_icall_sig_uint32_double, FALSE);
		register_icall (mono_isfinite_double, mono_icall_sig_int32_double, FALSE);
	}
#endif
	register_icall (mono_ckfinite, mono_icall_sig_double_double, FALSE);

#ifdef COMPRESSED_INTERFACE_BITMAP
	register_icall (mono_class_interface_match, mono_icall_sig_uint32_ptr_int32, TRUE);
#endif

	/* other jit icalls */
	register_icall (ves_icall_mono_delegate_ctor, mono_icall_sig_void_object_object_ptr, FALSE);
	register_icall (mono_class_static_field_address,
				 mono_icall_sig_ptr_ptr_ptr, FALSE);
	register_icall (mono_ldtoken_wrapper, mono_icall_sig_ptr_ptr_ptr_ptr, FALSE);
	register_icall (mono_ldtoken_wrapper_generic_shared,
		mono_icall_sig_ptr_ptr_ptr_ptr, FALSE);
	register_icall (mono_get_special_static_data, mono_icall_sig_ptr_int, FALSE);
	register_icall (ves_icall_mono_ldstr, mono_icall_sig_object_ptr_ptr_int32, FALSE);
	register_icall (mono_helper_stelem_ref_check, mono_icall_sig_void_object_object, FALSE);
	register_icall (ves_icall_object_new, mono_icall_sig_object_ptr_ptr, FALSE);
	register_icall (ves_icall_object_new_specific, mono_icall_sig_object_ptr, FALSE);
	register_icall (ves_icall_array_new, mono_icall_sig_object_ptr_ptr_int32, FALSE);
	register_icall (ves_icall_array_new_specific, mono_icall_sig_object_ptr_int32, FALSE);
	register_icall (ves_icall_runtime_class_init, mono_icall_sig_void_ptr, FALSE);
	register_icall (mono_ldftn, mono_icall_sig_ptr_ptr, FALSE);
	register_icall (mono_ldvirtfn, mono_icall_sig_ptr_object_ptr, FALSE);
	register_icall (mono_ldvirtfn_gshared, mono_icall_sig_ptr_object_ptr, FALSE);
	register_icall (mono_helper_compile_generic_method, mono_icall_sig_ptr_object_ptr_ptr, FALSE);
	register_icall (mono_helper_ldstr, mono_icall_sig_object_ptr_int, FALSE);
	register_icall (mono_helper_ldstr_mscorlib, mono_icall_sig_object_int, FALSE);
	register_icall (mono_helper_newobj_mscorlib, mono_icall_sig_object_int, FALSE);
	register_icall (mono_value_copy_internal, mono_icall_sig_void_ptr_ptr_ptr, FALSE);
	register_icall (mono_object_castclass_unbox, mono_icall_sig_object_object_ptr, FALSE);
	register_icall (mono_break, NULL, TRUE);
	register_icall (mono_create_corlib_exception_0, mono_icall_sig_object_int, TRUE);
	register_icall (mono_create_corlib_exception_1, mono_icall_sig_object_int_object, TRUE);
	register_icall (mono_create_corlib_exception_2, mono_icall_sig_object_int_object_object, TRUE);
	register_icall (mono_array_new_1, mono_icall_sig_object_ptr_int, FALSE);
	register_icall (mono_array_new_2, mono_icall_sig_object_ptr_int_int, FALSE);
	register_icall (mono_array_new_3, mono_icall_sig_object_ptr_int_int_int, FALSE);
	register_icall (mono_array_new_4, mono_icall_sig_object_ptr_int_int_int_int, FALSE);
	register_icall (mono_array_new_n_icall, mono_icall_sig_object_ptr_int_ptr, FALSE);
	register_icall (mono_get_native_calli_wrapper, mono_icall_sig_ptr_ptr_ptr_ptr, FALSE);
	register_icall (mono_resume_unwind, mono_icall_sig_void_ptr, TRUE);
	register_icall (mono_gsharedvt_constrained_call, mono_icall_sig_object_ptr_ptr_ptr_ptr_ptr, FALSE);
	register_icall (mono_gsharedvt_value_copy, mono_icall_sig_void_ptr_ptr_ptr, TRUE);

	/* The stores the LLVM backend emits a direct call to. */
	register_icall_no_wrapper (mono_gc_wbarrier_generic_store_internal, mono_icall_sig_void_ptr_object);
	register_icall_no_wrapper (mono_gc_wbarrier_value_copy_internal, mono_icall_sig_void_ptr_ptr_int_ptr);

	register_icall (mono_object_castclass_with_cache, mono_icall_sig_object_object_ptr_ptr, FALSE);
	register_icall (mono_object_isinst_with_cache, mono_icall_sig_object_object_ptr_ptr, FALSE);
	register_icall (mono_generic_class_init, mono_icall_sig_void_ptr, FALSE);
	register_icall (mono_fill_class_rgctx, mono_icall_sig_ptr_ptr_int, FALSE);
	register_icall (mono_fill_method_rgctx, mono_icall_sig_ptr_ptr_int, FALSE);

	register_dyn_icall (mini_get_dbg_callbacks ()->user_break, mono_debugger_agent_user_break, mono_icall_sig_void, FALSE);

	register_icall (mini_llvm_init_method, mono_icall_sig_void_ptr_ptr_ptr_ptr, TRUE);
	register_icall_no_wrapper (mini_llvmonly_resolve_iface_call_gsharedvt, mono_icall_sig_ptr_object_int_ptr_ptr);
	register_icall_no_wrapper (mini_llvmonly_resolve_vcall_gsharedvt, mono_icall_sig_ptr_object_int_ptr_ptr);
	register_icall_no_wrapper (mini_llvmonly_resolve_generic_virtual_call, mono_icall_sig_ptr_ptr_int_ptr);
	register_icall_no_wrapper (mini_llvmonly_resolve_generic_virtual_iface_call, mono_icall_sig_ptr_ptr_int_ptr);
	/* This needs a wrapper so it can have a preserveall cconv */
	register_icall (mini_llvmonly_init_vtable_slot, mono_icall_sig_ptr_ptr_int, FALSE);
	register_icall (mini_llvmonly_init_delegate, mono_icall_sig_void_object, TRUE);
	register_icall (mini_llvmonly_init_delegate_virtual, mono_icall_sig_void_object_object_ptr, TRUE);
	register_icall (mini_llvmonly_throw_nullref_exception, mono_icall_sig_void, TRUE);
	register_icall (mini_llvmonly_throw_aot_failed_exception, mono_icall_sig_void_ptr, TRUE);
	register_icall (mini_llvmonly_pop_lmf, mono_icall_sig_void_ptr, TRUE);
	register_icall (mini_llvmonly_get_interp_entry, mono_icall_sig_ptr_ptr, TRUE);

	register_icall (mono_get_assembly_object, mono_icall_sig_object_ptr, TRUE);
	register_icall (mono_get_method_object, mono_icall_sig_object_ptr, TRUE);
	register_icall (mono_throw_method_access, mono_icall_sig_void_ptr_ptr, FALSE);
	register_icall (mono_throw_unmanaged_callers_only, mono_icall_sig_void_ptr_ptr, FALSE);
	register_icall (mono_throw_bad_image, mono_icall_sig_void, FALSE);
	register_icall (mono_throw_not_supported, mono_icall_sig_void, FALSE);
	register_icall (mono_throw_invalid_program, mono_icall_sig_void_ptr, FALSE);
	register_icall_no_wrapper (mono_dummy_jit_icall, mono_icall_sig_void);

	register_icall_with_wrapper (mono_monitor_enter_internal, mono_icall_sig_int32_obj);
	register_icall_with_wrapper (mono_monitor_enter_v4_internal, mono_icall_sig_void_obj_ptr);
	register_icall_no_wrapper (mono_monitor_enter_fast, mono_icall_sig_int_obj);
	register_icall_no_wrapper (mono_monitor_enter_v4_fast, mono_icall_sig_int_obj_ptr);
	register_icall_no_wrapper (mono_monitor_exit_fast, mono_icall_sig_int_obj);

#ifdef TARGET_IOS
	register_icall (pthread_getspecific, mono_icall_sig_ptr_ptr, TRUE);
#endif
	/* Register tls icalls */
	register_icall_no_wrapper (mono_tls_get_thread_extern, mono_icall_sig_ptr);
	register_icall_no_wrapper (mono_tls_get_jit_tls_extern, mono_icall_sig_ptr);
	register_icall_no_wrapper (mono_tls_get_domain_extern, mono_icall_sig_ptr);
	register_icall_no_wrapper (mono_tls_get_sgen_thread_info_extern, mono_icall_sig_ptr);
	register_icall_no_wrapper (mono_tls_get_lmf_addr_extern, mono_icall_sig_ptr);

	register_icall_no_wrapper (mono_interp_entry_from_trampoline, mono_icall_sig_void_ptr_ptr);
	register_icall_no_wrapper (mono_interp_to_native_trampoline, mono_icall_sig_void_ptr_ptr);

#ifdef MONO_ARCH_HAS_REGISTER_ICALL
	mono_arch_register_icall ();
#endif
}

MonoJitStats mono_jit_stats = {0};

/**
 * Counters of mono_stats and mono_jit_stats can be read without locking during shutdown.
 * For all other contexts, assumes that the domain lock is held.
 * MONO_NO_SANITIZE_THREAD tells Clang's ThreadSanitizer to hide all reports of these (known) races.
 */
MONO_NO_SANITIZE_THREAD
void
mono_runtime_print_stats (void)
{
	if (mono_jit_stats.enabled) {
		g_print ("Mono Jit statistics\n");
		g_print ("Max code size ratio:    %.2f (%s)\n", mono_jit_stats.max_code_size_ratio / 100.0,
				 mono_jit_stats.max_ratio_method);
		g_print ("Biggest method:         %" G_GINT32_FORMAT " (%s)\n", mono_jit_stats.biggest_method_size,
				 mono_jit_stats.biggest_method);

		g_print ("Delegates created:      %" G_GINT32_FORMAT "\n", mono_stats.delegate_creations);
		g_print ("Initialized classes:    %" G_GINT32_FORMAT "\n", mono_stats.initialized_class_count);
		g_print ("Used classes:           %" G_GINT32_FORMAT "\n", mono_stats.used_class_count);
		g_print ("Generic vtables:        %" G_GINT32_FORMAT "\n", mono_stats.generic_vtable_count);
		g_print ("Methods:                %" G_GINT32_FORMAT "\n", mono_stats.method_count);
		g_print ("Static data size:       %" G_GINT32_FORMAT "\n", mono_stats.class_static_data_size);
		g_print ("VTable data size:       %" G_GINT32_FORMAT "\n", mono_stats.class_vtable_size);
		g_print ("Mscorlib mempool size:  %d\n", mono_mempool_get_allocated (mono_defaults.corlib->mempool));

		g_print ("\nInitialized classes:    %" G_GINT32_FORMAT "\n", mono_stats.generic_class_count);
		g_print ("Inflated types:         %" G_GINT32_FORMAT "\n", mono_stats.inflated_type_count);
		g_print ("Generics virtual invokes: %ld\n", mono_jit_stats.generic_virtual_invocations);

		g_print ("Sharable generic methods: %" G_GINT32_FORMAT "\n", mono_stats.generics_sharable_methods);
		g_print ("Unsharable generic methods: %" G_GINT32_FORMAT "\n", mono_stats.generics_unsharable_methods);
		g_print ("Shared generic methods: %" G_GINT32_FORMAT "\n", mono_stats.generics_shared_methods);
		g_print ("Shared vtype generic methods: %" G_GINT32_FORMAT "\n", mono_stats.gsharedvt_methods);

		g_print ("IMT tables size:        %" G_GINT32_FORMAT "\n", mono_stats.imt_tables_size);
		g_print ("IMT number of tables:   %" G_GINT32_FORMAT "\n", mono_stats.imt_number_of_tables);
		g_print ("IMT number of methods:  %" G_GINT32_FORMAT "\n", mono_stats.imt_number_of_methods);
		g_print ("IMT used slots:         %" G_GINT32_FORMAT "\n", mono_stats.imt_used_slots);
		g_print ("IMT colliding slots:    %" G_GINT32_FORMAT "\n", mono_stats.imt_slots_with_collisions);
		g_print ("IMT max collisions:     %" G_GINT32_FORMAT "\n", mono_stats.imt_max_collisions_in_slot);
		g_print ("IMT methods at max col: %" G_GINT32_FORMAT "\n", mono_stats.imt_method_count_when_max_collisions);
		g_print ("IMT trampolines size:   %" G_GINT32_FORMAT "\n", mono_stats.imt_trampolines_size);

		g_print ("JIT info table inserts: %" G_GINT32_FORMAT "\n", mono_stats.jit_info_table_insert_count);
		g_print ("JIT info table removes: %" G_GINT32_FORMAT "\n", mono_stats.jit_info_table_remove_count);
		g_print ("JIT info table lookups: %" G_GINT32_FORMAT "\n", mono_stats.jit_info_table_lookup_count);

		mono_counters_dump (MONO_COUNTER_SECTION_MASK | MONO_COUNTER_MONOTONIC, NULL);
		g_print ("\n");
	}
}

static void
jit_stats_cleanup (void)
{
	g_free (mono_jit_stats.max_ratio_method);
	mono_jit_stats.max_ratio_method = NULL;
	g_free (mono_jit_stats.biggest_method);
	mono_jit_stats.biggest_method = NULL;
}

static void
runtime_cleanup (MonoDomain *domain, gpointer user_data)
{
	mini_cleanup (domain);
}

#ifdef DISABLE_CLEANUP
void
mini_cleanup (MonoDomain *domain)
{
	if (mono_stats.enabled)
		g_printf ("Printing runtime stats at shutdown\n");
	mono_runtime_print_stats ();
	jit_stats_cleanup ();
	mono_jit_dump_cleanup ();
	mini_get_interp_callbacks ()->cleanup ();
#if defined(ENABLE_PERFTRACING) && !defined(DISABLE_EVENTPIPE)
	ep_shutdown ();
	ds_server_shutdown ();
#endif
}
#else
void
mini_cleanup (MonoDomain *domain)
{
	/*
	 * First, because everything below it is torn down under a compile that is
	 * still running: the string table one interns into, the assemblies one
	 * reads. This waits for the compile in hand and queues nothing more.
	 */
	mono_llvm_jit_stop_compiling ();

	if (mono_stats.enabled)
		g_printf ("Printing runtime stats at shutdown\n");
	if (mono_profiler_sampling_enabled ())
		mono_runtime_shutdown_stat_profiler ();

	MONO_PROFILER_RAISE (runtime_shutdown_begin, ());

#ifndef DISABLE_COM
	mono_cominterop_release_all_rcws ();
#endif

#ifndef MONO_CROSS_COMPILE
	/*
	 * mono_domain_finalize () needs to be called early since it needs the
	 * execution engine still fully working (it may invoke managed finalizers).
	 */
	mono_domain_finalize (domain, 2000);
#endif

	/* This accesses metadata so needs to be called before runtime shutdown */
	mono_runtime_print_stats ();
	jit_stats_cleanup ();

#ifndef MONO_CROSS_COMPILE
	mono_runtime_cleanup (domain);
#endif

#ifndef ENABLE_NETCORE
	mono_threadpool_cleanup ();
#endif

	MONO_PROFILER_RAISE (runtime_shutdown_end, ());

	mono_profiler_cleanup ();

	if (profile_options) {
		for (guint i = 0; i < profile_options->len; ++i)
			g_free (g_ptr_array_index (profile_options, i));
		g_ptr_array_free (profile_options, TRUE);
	}

	mono_icall_cleanup ();

	mono_runtime_cleanup_handlers ();

#ifndef MONO_CROSS_COMPILE
	mono_domain_free (domain, TRUE);
#endif
	free_jit_tls_data (mono_tls_get_jit_tls ());

	mono_aot_cleanup ();

	mono_trampolines_cleanup ();

	mono_unwind_cleanup ();

	mono_code_manager_destroy (global_codeman);
	g_free (vtable_trampolines);

	mini_get_interp_callbacks ()->cleanup ();

	mono_tramp_info_cleanup ();

	mono_arch_cleanup ();

	mono_generic_sharing_cleanup ();

	mono_cleanup_native_crash_info ();

	mono_cleanup ();

	mono_trace_cleanup ();

	if (mono_inject_async_exc_method)
		mono_method_desc_free (mono_inject_async_exc_method);

	mono_tls_free_keys ();

	mono_os_mutex_destroy (&jit_mutex);

	mono_code_manager_cleanup ();

#ifndef HOST_WIN32
	mono_w32handle_cleanup ();
#endif
}
#endif

void
mono_set_defaults (int verbose_level, guint32 opts)
{
	mini_verbose = verbose_level;
	mono_set_optimizations (opts);
}

void
mono_disable_optimizations (guint32 opts)
{
	default_opt &= ~opts;
}

void
mono_set_optimizations (guint32 opts)
{
	if (opts & MONO_OPT_AGGRESSIVE_INLINING)
		opts |= MONO_OPT_INLINE;

	default_opt = opts;
	default_opt_set = TRUE;
#ifdef MONO_ARCH_GSHARED_SUPPORTED
	mono_set_generic_sharing_supported ((default_opt & MONO_OPT_GSHARED) != 0);
#endif
#ifdef MONO_ARCH_GSHAREDVT_SUPPORTED
	mono_set_generic_sharing_vt_supported (mono_aot_only || ((default_opt & MONO_OPT_GSHAREDVT) != 0));
#else
	if (mono_llvm_only)
		mono_set_generic_sharing_vt_supported (TRUE);
#endif
}

void
mono_set_verbose_level (guint32 level)
{
	mini_verbose = level;
}

static const char*
mono_get_runtime_build_version (void)
{
	return FULL_VERSION;
}

/**
 * mono_get_runtime_build_info:
 * The returned string is owned by the caller. The returned string
 * format is <code>VERSION (FULL_VERSION BUILD_DATE)</code> and build date is optional.
 * \returns the runtime version + build date in string format.
 */
char*
mono_get_runtime_build_info (void)
{
	if (mono_build_date)
		return g_strdup_printf ("%s (%s %s)", VERSION, FULL_VERSION, mono_build_date);
	else
		return g_strdup_printf ("%s (%s)", VERSION, FULL_VERSION);
}

static void
mono_precompile_assembly (MonoAssembly *ass, void *user_data)
{
	GHashTable *assemblies = (GHashTable*)user_data;
	MonoImage *image = mono_assembly_get_image_internal (ass);
	MonoMethod *method, *invoke;
	int i, count = 0;

	if (g_hash_table_lookup (assemblies, ass))
		return;

	g_hash_table_insert (assemblies, ass, ass);

	if (mini_verbose > 0)
		printf ("PRECOMPILE: %s.\n", mono_image_get_filename (image));

	for (i = 0; i < mono_image_get_table_rows (image, MONO_TABLE_METHOD); ++i) {
		ERROR_DECL (error);

		method = mono_get_method_checked (image, MONO_TOKEN_METHOD_DEF | (i + 1), NULL, NULL, error);
		if (!method) {
			mono_error_cleanup (error); /* FIXME don't swallow the error */
			continue;
		}
		if (method->flags & METHOD_ATTRIBUTE_ABSTRACT)
			continue;
		if (method->is_generic || mono_class_is_gtd (method->klass))
			continue;

		count++;
		if (mini_verbose > 1) {
			char * desc = mono_method_full_name (method, TRUE);
			g_print ("Compiling %d %s\n", count, desc);
			g_free (desc);
		}
		mono_compile_method_checked (method, error);
		if (!is_ok (error)) {
			mono_error_cleanup (error); /* FIXME don't swallow the error */
			continue;
		}
		if (strcmp (method->name, "Finalize") == 0) {
			invoke = mono_marshal_get_runtime_invoke (method, FALSE);
			mono_compile_method_checked (invoke, error);
			mono_error_assert_ok (error);
		}
#ifndef DISABLE_REMOTING
		if (mono_class_is_marshalbyref (method->klass) && mono_method_signature_internal (method)->hasthis) {
			invoke = mono_marshal_get_remoting_invoke_with_check (method, error);
			mono_error_assert_ok (error);
			mono_compile_method_checked (invoke, error);
			mono_error_assert_ok (error);
		}
#endif
	}

	/* Load and precompile referenced assemblies as well */
	for (i = 0; i < mono_image_get_table_rows (image, MONO_TABLE_ASSEMBLYREF); ++i) {
		mono_assembly_load_reference (image, i);
		if (image->references [i])
			mono_precompile_assembly (image->references [i], assemblies);
	}
}

void mono_precompile_assemblies ()
{
	GHashTable *assemblies = g_hash_table_new (NULL, NULL);

	mono_assembly_foreach ((GFunc)mono_precompile_assembly, assemblies);

	g_hash_table_destroy (assemblies);
}

static MonoBreakPolicy
always_insert_breakpoint (MonoMethod *method)
{
	return MONO_BREAK_POLICY_ALWAYS;
}

static MonoBreakPolicyFunc break_policy_func = always_insert_breakpoint;

/**
 * mono_set_break_policy:
 * \param policy_callback the new callback function
 *
 * Allow embedders to decide whether to actually obey breakpoint instructions
 * (both break IL instructions and \c Debugger.Break method calls), for example
 * to not allow an app to be aborted by a perfectly valid IL opcode when executing
 * untrusted or semi-trusted code.
 *
 * \p policy_callback will be called every time a break point instruction needs to
 * be inserted with the method argument being the method that calls \c Debugger.Break
 * or has the IL \c break instruction. The callback should return \c MONO_BREAK_POLICY_NEVER
 * if it wants the breakpoint to not be effective in the given method.
 * \c MONO_BREAK_POLICY_ALWAYS is the default.
 */
void
mono_set_break_policy (MonoBreakPolicyFunc policy_callback)
{
	if (policy_callback)
		break_policy_func = policy_callback;
	else
		break_policy_func = always_insert_breakpoint;
}

gboolean
mini_should_insert_breakpoint (MonoMethod *method)
{
	switch (break_policy_func (method)) {
	case MONO_BREAK_POLICY_ALWAYS:
		return TRUE;
	case MONO_BREAK_POLICY_NEVER:
		return FALSE;
	case MONO_BREAK_POLICY_ON_DBG:
		g_warning ("mdb no longer supported");
		return FALSE;
	default:
		g_warning ("Incorrect value returned from break policy callback");
		return FALSE;
	}
}

// Custom handlers currently only implemented by Windows.
#ifndef HOST_WIN32
gboolean
mono_runtime_install_custom_handlers (const char *handlers)
{
	return FALSE;
}

void
mono_runtime_install_custom_handlers_usage (void)
{
	fprintf (stdout,
		 "Custom Handlers:\n"
		 "   --handlers=HANDLERS            Enable handler support, HANDLERS is a comma\n"
		 "                                  separated list of available handlers to install.\n"
		 "\n"
		 "No handlers supported on current platform.\n");
}
#endif /* HOST_WIN32 */

#ifdef ENABLE_METADATA_UPDATE
void
mini_metadata_update_init (MonoError *error)
{
	mini_get_interp_callbacks ()->metadata_update_init (error);
}

void
mini_invalidate_transformed_interp_methods (MonoDomain *domain, MonoAssemblyLoadContext *alc G_GNUC_UNUSED, uint32_t generation G_GNUC_UNUSED)
{
	mini_get_interp_callbacks ()->invalidate_transformed (domain);
}
#endif
