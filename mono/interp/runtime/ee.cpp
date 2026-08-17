
/**
 * \file
 * \brief Registering this engine with mini, and the options it takes.
 */

#include "config.h"

#include "callbacks.hpp"
#include "internals.hpp"
#include "context.hpp"

#include <mono/metadata/mono-debug.h>
#include <mono/mini/mini-runtime.h>
#include <mono/utils/mono-counters.h>
#include <mono/utils/mono-tls-inline.h>

#include <string>
#include <string_view>
#include <vector>

/* Declared at global scope in internals.hpp, so defined there too. */
std::vector<std::string> mono_interp_jit_classes;
/* Optimizations enabled with interpreter */
int mono_interp_opt = INTERP_OPT_DEFAULT;
/* If TRUE, interpreted code will be interrupted at function entry/backward branches */
gboolean mono_interp_ss_enabled;
/* The transform's default verbose level. */
int mono_interp_traceopt = 0;

namespace mono::interp {

static gboolean interp_init_done = FALSE;

/// Whether arg starts with prefix, and if it does, what follows it.
static bool
takes_value (std::string_view arg, std::string_view prefix, std::string_view *value)
{
	if (arg.substr (0, prefix.size ()) != prefix)
		return false;

	*value = arg.substr (prefix.size ());
	return true;
}

static void
interp_parse_options (const char *options)
{
	if (!options)
		return;

	char **args = g_strsplit (options, ",", -1);

	for (char **ptr = args; ptr && *ptr; ptr++) {
		std::string_view arg = *ptr;
		std::string_view value;

		if (takes_value (arg, "jit=", &value))
			mono_interp_jit_classes.emplace_back (value);
		else if (takes_value (arg, "interp-only=", &value))
			/* mini owns this list and never frees it, so it gets a copy of
			 * its own rather than a pointer into the split below. */
			mono_interp_only_classes = g_slist_prepend (
				mono_interp_only_classes, g_strndup (value.data (), value.size ()));
		else if (arg == "-inline")
			mono_interp_opt &= ~INTERP_OPT_INLINE;
		else if (arg == "-cprop")
			mono_interp_opt &= ~INTERP_OPT_CPROP;
		else if (arg == "-bblocks")
			mono_interp_opt &= ~INTERP_OPT_BBLOCKS;
		else if (arg == "-all")
			mono_interp_opt = INTERP_OPT_NONE;
		else if (takes_value (arg, "verbose=", &value))
			mono_interp_traceopt = atoi (std::string (value).c_str ());
	}

	g_strfreev (args);
}

void
interp_set_optimizations (guint32 opts)
{
	mono_interp_opt = opts;
}

/* Nothing to give back. The engine's state is per thread, and interp_free_context ()
 * has already run for each of them. */
void
interp_cleanup (void)
{
}

static void
register_interp_stats (void)
{
	mono_counters_init ();
	mono_counters_register ("Total transform time", MONO_COUNTER_INTERP | MONO_COUNTER_LONG | MONO_COUNTER_TIME, &mono_interp_stats.transform_time);
	mono_counters_register ("Methods transformed", MONO_COUNTER_INTERP | MONO_COUNTER_LONG, &mono_interp_stats.methods_transformed);
	mono_counters_register ("Line number table size", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.line_numbers_size);
	mono_counters_register ("Total cprop time", MONO_COUNTER_INTERP | MONO_COUNTER_LONG | MONO_COUNTER_TIME, &mono_interp_stats.cprop_time);
	mono_counters_register ("STLOC_NP count", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.stloc_nps);
	mono_counters_register ("MOVLOC count", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.movlocs);
	mono_counters_register ("Copy propagations", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.copy_propagations);
	mono_counters_register ("Added pop count", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.added_pop_count);
	mono_counters_register ("Constant folds", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.constant_folds);
	mono_counters_register ("Ldlocas removed", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.ldlocas_removed);
	mono_counters_register ("Killed instructions", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.killed_instructions);
	mono_counters_register ("Emitted instructions", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.emitted_instructions);
	mono_counters_register ("Methods inlined", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.inlined_methods);
	mono_counters_register ("Inline failures", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.inline_failures);
}

/*
 * The engine itself lives in interp.cpp, and these five callbacks land there rather
 * than here. The table is still generated from the one list, so a callback added to
 * ee.h stays a compile error until something answers it.
 */
#define interp_entry_from_trampoline mono_interp_entry_from_ccontext
#define interp_entry_from_args       mono_interp_entry_from_args
#define interp_runtime_invoke        mono_interp_runtime_invoke
#define interp_run_finally           mono_interp_run_finally
#define interp_run_filter            mono_interp_run_filter

#undef MONO_EE_CALLBACK
#define MONO_EE_CALLBACK(ret, name, sig) interp_ ## name,

static const MonoEECallbacks mono_interp_callbacks = {
	MONO_EE_CALLBACKS
};

} // namespace mono::interp

/* Outside the namespace, because interp-internals.hpp declares them there. */

using namespace mono::interp;

void
mono_ee_interp_init (const char *opts)
{
	g_assert (mono_ee_api_version () == MONO_EE_API_VERSION);
	g_assert (!interp_init_done);
	interp_init_done = TRUE;

	interp_context_init ();

	interp_parse_options (opts);
	/* Don't do any optimizations if running under debugger */
	if (mini_get_debug_options ()->mdb_optimizations)
		mono_interp_opt = 0;
	mono_interp_transform_init ();

	mini_install_interp_callbacks (&mono_interp_callbacks);

	register_interp_stats ();
}

