/*
 * tiered.cpp: tier-0 -> tier-1 promotion policy for the LLVM backend.
 *
 * The model (design C3):
 *
 *   Tier 0 is the classic mini JIT, lazy, on first call - unchanged.
 *   On a successful tier-0 compile the method is queued for tier 1
 *   unconditionally; there is no hotness counting. Tier 1 is terminal: if it
 *   fails, the method latches tier-0-terminal and is never retried.
 *
 * "Tier 1" means the terminal body, not necessarily the LLVM body. A method
 * that hits one of the backend's gates (EH clauses, gshared, save_lmf) simply
 * stays tier-0 terminal; that is a normal outcome, not a failure.
 *
 * Why the queue exists rather than promoting inline at the end of the tier-0
 * compile: mono's JIT nests. Compiling a method runs class initializers, which
 * compile more methods, which run more initializers; observed nesting reaches
 * dozens of frames. A tier-0 frame is small, but an LLVM codegen frame is not,
 * so promoting inline would stack full LLVM pipelines to the same depth as the
 * nest. Instead the method is queued and the queue is drained only when the
 * compile nesting returns to zero, so tier-1 compilation always runs on a
 * shallow stack regardless of how deep the tier-0 nest went.
 *
 * Per scope decision D2 the drain is synchronous - it runs on the thread that
 * finished the outermost compile. Moving it to a background thread is W4 and is
 * deliberately deferred; when that happens only the drain site changes, not the
 * policy here.
 */

#include <config.h>
#include <glib.h>

#include "mini.h"
#include "mini-runtime.h"
#include "backend.h"

enum TierState {
	/* queued or compiling; tier 0 is the current body */
	TIER_STATE_TIER0 = 0,
	/* tier 1 published; terminal */
	TIER_STATE_PROMOTED,
	/* tier 1 declined or failed; tier 0 is terminal, never retry */
	TIER_STATE_TIER0_TERMINAL
};

/*
 * A queued method, with the optimization set its tier-0 compile used - tier 1
 * must be built with the same opts, and the drain runs long after the compile
 * that chose them has returned.
 */
typedef struct {
	MonoMethod *method;
	guint32 opt;
} TieredEntry;

static mono_mutex_t tiered_mutex;
static GHashTable *tiered_state;	/* MonoMethod* -> TierState */
static GQueue *tiered_queue;		/* pending TieredEntry* */
static gboolean tiered_inited;

/*
 * Promotion is unsafe until mini_init () has finished: the domain is still
 * being constructed before that (create_domain_objects compiles methods), and
 * running LLVM codegen there reaches domain state that does not exist yet.
 * Methods compiled during startup still queue; they are promoted by the first
 * drain after the runtime is up.
 */
static gboolean tiered_ready;

/*
 * Nesting depth of mini_method_compile on this thread. Only a transition back
 * to zero is a safe point to run tier-1 compilation.
 */
static __thread int tiered_compile_depth;

/* Guards against a drain re-entering itself through the compile it triggers. */
static __thread gboolean tiered_draining;

static void
tiered_init_locked (void)
{
	if (tiered_inited)
		return;
	mono_os_mutex_init_recursive (&tiered_mutex);
	tiered_state = g_hash_table_new (g_direct_hash, g_direct_equal);
	tiered_queue = g_queue_new ();
	tiered_inited = TRUE;
}

gboolean
mono_llvm_tiered_enabled (void)
{
	static gboolean inited, enabled;

	/*
	 * Unsynchronized lazy init, as mono does for its other env-var switches.
	 * A race at worst reads the variable twice and reaches the same answer.
	 */
	if (!inited) {
		enabled = g_getenv ("MONO_TIERED") != NULL;
		if (enabled) {
			mono_os_mutex_init_recursive (&tiered_mutex);
			tiered_state = g_hash_table_new (g_direct_hash, g_direct_equal);
			tiered_queue = g_queue_new ();
			tiered_inited = TRUE;
		}
		inited = TRUE;
	}
	return enabled;
}

void
mono_llvm_tiered_set_ready (void)
{
	if (!mono_llvm_tiered_enabled ())
		return;
	tiered_ready = TRUE;
}

void
mono_llvm_tiered_compile_begin (void)
{
	if (!mono_llvm_tiered_enabled ())
		return;
	tiered_compile_depth ++;
}

/*
 * Queue a method that has just been compiled and published at tier 0.
 * Ignored if the method is already promoted or latched terminal.
 */
void
mono_llvm_tiered_enqueue (MonoMethod *method, guint32 opt)
{
	if (!mono_llvm_tiered_enabled () || !method)
		return;

	mono_os_mutex_lock (&tiered_mutex);
	tiered_init_locked ();
	if (!g_hash_table_lookup_extended (tiered_state, method, NULL, NULL)) {
		TieredEntry *entry = g_new0 (TieredEntry, 1);

		entry->method = method;
		entry->opt = opt;
		g_hash_table_insert (tiered_state, method, GINT_TO_POINTER (TIER_STATE_TIER0));
		g_queue_push_tail (tiered_queue, entry);
	}
	mono_os_mutex_unlock (&tiered_mutex);
}

static void
tiered_set_state (MonoMethod *method, TierState state)
{
	mono_os_mutex_lock (&tiered_mutex);
	g_hash_table_insert (tiered_state, method, GINT_TO_POINTER (state));
	mono_os_mutex_unlock (&tiered_mutex);
}

/*
 * Called when mini_method_compile unwinds. When the nesting returns to zero we
 * are on a shallow stack, so it is safe to run tier-1 compilation for whatever
 * accumulated during the nest.
 */
void
mono_llvm_tiered_compile_end (void)
{
	if (!mono_llvm_tiered_enabled ())
		return;

	tiered_compile_depth --;
	if (tiered_compile_depth != 0)
		return;
	if (!tiered_ready)
		return;
	/* The promotion compile below re-enters here; do not recurse. */
	if (tiered_draining)
		return;

	tiered_draining = TRUE;

	for (;;) {
		TieredEntry *entry;

		mono_os_mutex_lock (&tiered_mutex);
		entry = (TieredEntry *)(tiered_queue ? g_queue_pop_head (tiered_queue) : NULL);
		mono_os_mutex_unlock (&tiered_mutex);

		if (!entry)
			break;

		/*
		 * mini_tiered_promote () returns FALSE when the backend declined the
		 * method (a gate) or the compile failed. Either way tier 0 is the
		 * terminal body and we must not try again.
		 */
		if (mini_tiered_promote (entry->method, entry->opt))
			tiered_set_state (entry->method, TIER_STATE_PROMOTED);
		else
			tiered_set_state (entry->method, TIER_STATE_TIER0_TERMINAL);

		g_free (entry);
	}

	tiered_draining = FALSE;
}

/*
 * TRUE while a tier-1 promotion compile is in flight on this thread. mini.c
 * uses this to select the LLVM path for that one compile without turning on
 * mono_use_llvm globally, which would make tier 0 use LLVM too.
 */
gboolean
mono_llvm_tiered_in_promotion (void)
{
	return tiered_draining;
}
