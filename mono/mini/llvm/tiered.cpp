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
#include <mono/metadata/appdomain.h>
#include <mono/utils/mono-lazy-init.h>
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
	MonoDomain *domain;
	guint32 opt;
} TieredEntry;

/*
 * The recorded state of a method that has been through the queue, plus the
 * domain it was queued for.
 *
 * The domain is recorded here as well as on the queue entry so that a domain
 * unload can drop the method's state too, not just its pending queue entry:
 * everything a MonoMethod* keys here can die with the domain (a dynamic
 * assembly's image is freed by mono_domain_free (), taking its MonoMethods
 * with it), and a later allocation reusing that address would otherwise
 * inherit a stale TIER_STATE_TIER0_TERMINAL and never be promoted.
 *
 * The record is a separate allocation from the queue entry on purpose. The
 * table owns the record and a domain purge frees it, while the drain owns the
 * entry it popped; if they were one allocation a purge could free the entry
 * out from under a promotion that is already in flight.
 */
typedef struct {
	MonoDomain *domain;
	TierState state;
} TieredRecord;

static mono_mutex_t tiered_mutex;
static GHashTable *tiered_state;	/* MonoMethod* -> TieredRecord* */
static GQueue *tiered_queue;		/* pending TieredEntry* */

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

/*
 * TRUE only for the duration of the one mini_method_compile () that is
 * promoting a method. It must NOT cover the whole drain: promotion runs cctors,
 * which compile other, unrelated methods, and those must still get a classic
 * tier-0 body. mono_jit_compile_method_inner () - the entry every nested compile
 * goes through - saves and clears this around itself, so a nested compile sees
 * FALSE while the promotion compile, which calls mini_method_compile directly,
 * keeps it.
 */
static __thread gboolean tiered_promote_active;

static mono_lazy_init_t tiered_lazy_init = MONO_LAZY_INIT_STATUS_NOT_INITIALIZED;
static gboolean tiered_enabled;

/*
 * MONO_TIERED_CALL_THRESHOLD: how many times a tier-0 body is entered before it
 * is enqueued for tier 1. Default 1000; 0 means enqueue eagerly on first publish
 * (the pre-threshold behaviour), which is also the feature's off switch. Read
 * once in tiered_do_init () and thereafter a constant the prologue emitter bakes
 * in. Meaningless - and left 0 - unless MONO_TIERED is set.
 */
static guint32 tiered_call_threshold;

/*
 * The per-method, per-domain tier-0 call counter block. COUNT is at offset 0
 * because the prologue bakes the block's address and increments/compares its
 * first word directly; OPT/METHOD/DOMAIN let the crossing helper enqueue with
 * exactly the arguments the eager path used. Allocated from the domain mem
 * manager so it is freed with the tier-0 code at domain unload; a re-JIT for a
 * new domain gets a fresh, zeroed counter, which is correct (promotion is
 * per-domain).
 */
typedef struct {
	guint32 count;
	guint32 opt;
	MonoMethod *method;
	MonoDomain *domain;
} MiniTieredCounter;

static void
tiered_do_init (void)
{
	tiered_enabled = g_getenv ("MONO_TIERED") != NULL;
	if (!tiered_enabled)
		return;
	mono_os_mutex_init_recursive (&tiered_mutex);
	tiered_state = g_hash_table_new (g_direct_hash, g_direct_equal);
	tiered_queue = g_queue_new ();

	tiered_call_threshold = 1000;
	{
		char *e = g_getenv ("MONO_TIERED_CALL_THRESHOLD");
		if (e) {
			char *end = NULL;
			guint64 v = g_ascii_strtoull (e, &end, 10);
			/* Keep the default on an empty or malformed value. */
			if (end && end != e && *end == '\0' && v <= G_MAXUINT32)
				tiered_call_threshold = (guint32) v;
			g_free (e);
		}
	}
}

/*
 * The call-count threshold, or 0 when the feature is off (MONO_TIERED unset, or
 * MONO_TIERED_CALL_THRESHOLD=0). At 0 the prologue emits no counter and the eager
 * enqueue at the tier-0 publish site is unchanged, so behaviour is byte-identical
 * to the pre-threshold runtime.
 */
guint32
mono_llvm_tiered_call_threshold (void)
{
	if (!mono_llvm_tiered_enabled ())
		return 0;
	return tiered_call_threshold;
}

/*
 * Allocate the tier-0 call counter for METHOD in DOMAIN, recording the OPT its
 * tier-0 compile used so a later crossing can enqueue tier 1 with the same opts.
 * Returns NULL (no counter emitted) when the feature is off. The dynamic-method
 * exclusion is applied by the caller (the prologue emitter), which already has
 * the predicate to hand.
 */
gpointer
mini_tiered_alloc_counter (MonoDomain *domain, MonoMethod *method, guint32 opt)
{
	MiniTieredCounter *ctr;

	if (!mono_llvm_tiered_enabled () || tiered_call_threshold == 0)
		return NULL;
	if (!domain || !method)
		return NULL;

	ctr = (MiniTieredCounter *) mono_domain_alloc0 (domain, sizeof (MiniTieredCounter));
	ctr->opt = opt;
	ctr->method = method;
	ctr->domain = domain;
	return ctr;
}

/*
 * Runs exactly once across all threads. Doing this with a plain unsynchronized
 * static would let a racing thread re-init the mutex while another holds it and
 * install a second state table and queue.
 */
gboolean
mono_llvm_tiered_enabled (void)
{
	mono_lazy_initialize (&tiered_lazy_init, tiered_do_init);
	return tiered_enabled;
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
mono_llvm_tiered_enqueue (MonoMethod *method, MonoDomain *domain, guint32 opt)
{
	if (!mono_llvm_tiered_enabled () || !method)
		return;

	mono_os_mutex_lock (&tiered_mutex);
	if (!g_hash_table_lookup (tiered_state, method)) {
		TieredEntry *entry = g_new0 (TieredEntry, 1);
		TieredRecord *rec = g_new0 (TieredRecord, 1);

		rec->domain = domain;
		rec->state = TIER_STATE_TIER0;
		entry->method = method;
		entry->domain = domain;
		entry->opt = opt;
		g_hash_table_insert (tiered_state, method, rec);
		g_queue_push_tail (tiered_queue, entry);
	}
	mono_os_mutex_unlock (&tiered_mutex);
}

/*
 * Record the outcome of a promotion attempt.
 *
 * Deliberately an update, not an insert: if the method's record is gone, its
 * domain was unloaded while the promotion ran and the method must not be
 * resurrected in the table - the MonoMethod* may already be freed memory, and
 * re-inserting it would recreate exactly the stale entry the purge removed.
 * The domain check is for the same reason.
 */
static void
tiered_set_state (MonoMethod *method, MonoDomain *domain, TierState state)
{
	TieredRecord *rec;

	mono_os_mutex_lock (&tiered_mutex);
	rec = (TieredRecord *) g_hash_table_lookup (tiered_state, method);
	if (rec && rec->domain == domain)
		rec->state = state;
	mono_os_mutex_unlock (&tiered_mutex);
}

/*
 * TRUE while a domain is being torn down, i.e. once mono_domain_try_unload ()
 * has moved it past MONO_APPDOMAIN_UNLOADING_START. The state that actually
 * holds during the purge is MONO_APPDOMAIN_UNLOADED: appdomain.c sets it before
 * calling mono_domain_free (), which is what reaches this file's hook.
 * MONO_APPDOMAIN_UNLOADING covers the window before that, while finalizers for
 * the doomed domain are still running and still compiling methods in it.
 *
 * mono_domain_is_unloading () is the runtime's own predicate for exactly this
 * pair of states; the unload path sets domain->state directly rather than going
 * through it, but every other consumer (threadpool, gc, icall) tests it this
 * way and so do we.
 *
 * Note that this is advisory, not a safety barrier - it is a racy read of
 * domain->state and the domain can enter either state immediately afterwards.
 * What actually makes the promotion safe is the appdomain-ref precondition
 * enforced in mono_llvm_tiered_compile_end (); this check only avoids the
 * pointless work of compiling into a domain already known to be going away.
 */
static gboolean
tiered_domain_is_dying (MonoDomain *domain)
{
	return !domain || mono_domain_is_unloading (domain);
}

static gboolean
tiered_record_in_domain (gpointer key, gpointer value, gpointer user_data)
{
	TieredRecord *rec = (TieredRecord *) value;

	if (rec->domain != (MonoDomain *) user_data)
		return FALSE;
	/* The table holds no value destructor, so free the record here. */
	g_free (rec);
	return TRUE;
}

/*
 * Drop everything recorded for DOMAIN. Called from mini_free_jit_domain_info (),
 * i.e. from mono_domain_free ()'s free_domain_hook, the same place the rest of
 * the JIT's per-domain state is released.
 *
 * This is the only thing that ever removes from tiered_state, and it is what
 * makes a MonoMethod* safe as a key: the methods of a dynamic assembly die with
 * the domain, and the domain's own storage is freed shortly after this hook, so
 * both pointers would otherwise dangle and a later allocation reusing either
 * address would alias a stale record.
 *
 * The hook runs before the three frees that matter - domain->jit_code_hash is
 * destroyed later in mono_domain_free (), and the MonoDomain itself later
 * still - so nothing read here is already freed.
 *
 * Not reached when mono_dont_free_domains is set (debugger paths): then
 * mono_domain_free () returns before the hook. Records for that domain leak,
 * but harmlessly - the domain and its methods are deliberately kept alive.
 */
void
mono_llvm_tiered_domain_unload (MonoDomain *domain)
{
	/*
	 * Reclaim the JIT memory FIRST, and unconditionally - before the tiering
	 * gate below, because the engine also compiles under a plain --llvm run
	 * where tiering is off, and those bodies are just as dead once the domain
	 * is being freed. mono_llvm_jit_release_domain () is a no-op if the engine
	 * was never built, which is the common case.
	 *
	 * Ordering against the purge below does not matter - one frees machine code,
	 * the other frees promotion bookkeeping, and they share nothing - but doing
	 * it first keeps the "release what the domain owns" step adjacent to the
	 * hook's own contract.
	 */
	mono_llvm_jit_release_domain (domain);

	if (!mono_llvm_tiered_enabled ())
		return;

	mono_os_mutex_lock (&tiered_mutex);

	if (tiered_queue) {
		/*
		 * Entries for a domain nobody drains just sit here (see
		 * tiered_dequeue_for_domain ()), so at unload the queue really can
		 * still hold some - this is not a theoretical arm.
		 *
		 * eglib's GQueue has no delete_link, so filter by draining into a
		 * scratch queue and pushing the survivors back, which also keeps the
		 * FIFO order the drain relies on.
		 */
		GQueue *kept = g_queue_new ();
		TieredEntry *entry;

		while ((entry = (TieredEntry *) g_queue_pop_head (tiered_queue))) {
			if (entry->domain == domain)
				g_free (entry);
			else
				g_queue_push_tail (kept, entry);
		}
		while ((entry = (TieredEntry *) g_queue_pop_head (kept)))
			g_queue_push_tail (tiered_queue, entry);
		g_queue_free (kept);
	}

	if (tiered_state)
		g_hash_table_foreach_remove (tiered_state, tiered_record_in_domain, domain);

	mono_os_mutex_unlock (&tiered_mutex);
}

/*
 * Pop the oldest queued entry belonging to DOMAIN, leaving entries for other
 * domains where they are. Returns NULL when this domain has nothing pending.
 * Caller holds tiered_mutex.
 *
 * Entries are deferred rather than dropped because a queue entry is the only
 * record that a method still wants tier 1: mono_llvm_tiered_enqueue () refuses
 * any method already in tiered_state, and a published tier-0 body is never
 * recompiled, so a dropped entry means that method is stuck at tier 0 for the
 * process lifetime. Deferring costs nothing - the entry is picked up by the
 * next drain that happens to run in its domain, which for the root domain is
 * almost immediately - and it is what lets a rolled-back unload recover.
 *
 * Cost: the slow path rebuilds the whole queue, so popping k entries out of a
 * queue of n is O(k*n), not O(n). It is only taken when the head does not
 * already match, which for a single-domain process is never. Measured on a
 * three-round unload workload: 2694 fast dequeues against 446 slow ones
 * walking about 15 entries each, peak queue length 83.
 */
static TieredEntry *
tiered_dequeue_for_domain (MonoDomain *domain)
{
	GQueue *deferred;
	TieredEntry *entry, *found = NULL;

	if (!tiered_queue || tiered_queue->length == 0)
		return NULL;

	/* Fast path: the queue is single-domain, so the head normally matches. */
	if (((TieredEntry *) tiered_queue->head->data)->domain == domain)
		return (TieredEntry *) g_queue_pop_head (tiered_queue);

	deferred = g_queue_new ();
	while ((entry = (TieredEntry *) g_queue_pop_head (tiered_queue))) {
		if (!found && entry->domain == domain)
			found = entry;
		else
			g_queue_push_tail (deferred, entry);
	}
	while ((entry = (TieredEntry *) g_queue_pop_head (deferred)))
		g_queue_push_tail (tiered_queue, entry);
	g_queue_free (deferred);

	return found;
}

/*
 * Drain the queue for this thread's current domain, promoting each entry.
 *
 * Factored out so it can be driven from two producers: a compile that unwound to
 * nesting zero (mono_llvm_tiered_compile_end), and a tier-0 body whose call count
 * just crossed the threshold during ordinary execution (mini_tiered_count_reached).
 * Both reach a drain on a shallow stack; the gates below are identical for both.
 *
 * The depth gate is the shared safety property: tier-1 LLVM codegen must run only
 * at nesting zero, never on the deep stack that class initializers create. A
 * compile_end has just decremented to its final depth; a count-reached crossing
 * that fires deep inside a cctor-driven compile nest simply sees depth != 0 here,
 * leaves the method enqueued, and lets the outer compile_end drain it later.
 */
static void
tiered_try_drain (void)
{
	MonoInternalThread *thread;
	MonoDomain *domain;

	if (tiered_compile_depth != 0)
		return;
	if (!tiered_ready)
		return;
	/* The promotion compile below re-enters here; do not recurse. */
	if (tiered_draining)
		return;

	/*
	 * Promote only into this thread's own current domain, and only when this
	 * thread actually holds an appdomain ref to it.
	 *
	 * The hazard: the queue is global, so an unrestricted drain compiles
	 * methods for whatever domain queued them. mini_tiered_promote () takes
	 * domain->jit_code_hash_lock and mutates domain->jit_code_hash, and
	 * mono_domain_try_unload () waits only for threads that hold an appdomain
	 * ref to the doomed domain (collect_appdomain_thread () selects purely on
	 * mono_thread_internal_has_appdomain_ref). A drain thread that holds no ref
	 * is not waited for, so the domain can be freed mid-compile.
	 *
	 * The two checks below enforce two different things, and neither subsumes
	 * the other:
	 *
	 *  - The ref check is the lifetime guarantee. It is enforced rather than
	 *    assumed because "current domain implies a ref" is FALSE. The finalizer
	 *    thread is the standing counterexample: gc.c makes an arbitrary object's
	 *    domain current with no ref at all - the comment there literally reads
	 *    "this thread can enter a doomed appdomain" - and then compiles in that
	 *    window. mono_runtime_class_init_full (), mono_domain_try_type_unload's
	 *    xdomain paths and cominterop transfer domains unref'd too. Once we do
	 *    hold a ref, mono_threads_abort_appdomain_threads () is called with an
	 *    infinite timeout, so the unload cannot reach mono_domain_free () until
	 *    we leave. We never push a ref of our own: refs are what
	 *    mono_thread_internal_abort () targets, so pushing one would make a
	 *    root-domain thread abortable on behalf of an unrelated domain's unload.
	 *
	 *  - The current-domain check is the execution-context guarantee, and it is
	 *    not redundant. mono_runtime_class_init_full () transfers the running
	 *    thread into the vtable's domain with mono_domain_set_fast () and no ref
	 *    (object.c) when it has to run a cctor for another domain. Promoting a
	 *    method for domain D from a thread currently in domain C would therefore
	 *    manufacture exactly the unref'd-entrant condition described above, out
	 *    of the cctors our own JIT_FLAG_RUN_CCTORS compile triggers. Requiring
	 *    D == C makes that transfer a no-op instead.
	 *
	 * Entries that fail either check are left queued, not dropped; see
	 * tiered_dequeue_for_domain ().
	 */
	domain = mono_domain_get ();

	/*
	 * The ref is read from our own thread's ref stack, which only this thread
	 * pushes and pops, so it cannot be revoked underneath us: a cctor run by
	 * the promotion compile may move us in and out of other domains, but it
	 * never pops the ref that got us here. One check per drain therefore covers
	 * every entry the drain goes on to promote.
	 *
	 * A NULL thread is a native thread that never attached; a NULL domain never
	 * matches a ref stack entry. Both correctly fall out as "cannot promote".
	 */
	thread = mono_thread_internal_current ();
	if (!thread || !mono_thread_internal_has_appdomain_ref (thread, domain))
		return;

	/*
	 * Our own domain is being torn down. Leave everything queued: on a
	 * successful unload mono_llvm_tiered_domain_unload () frees it, and on a
	 * rolled-back one (a thread-abort, threadpool or finalization timeout makes
	 * mono_domain_try_unload () restore MONO_APPDOMAIN_CREATED without ever
	 * calling mono_domain_free) the domain is usable again and the next drain
	 * promotes these normally. Dropping them here would strand every one of
	 * them at tier 0 permanently, since enqueue refuses a method that already
	 * has a record.
	 */
	if (tiered_domain_is_dying (domain))
		return;

	tiered_draining = TRUE;

	for (;;) {
		TieredEntry *entry;

		mono_os_mutex_lock (&tiered_mutex);
		entry = tiered_dequeue_for_domain (domain);
		mono_os_mutex_unlock (&tiered_mutex);

		if (!entry)
			break;

		/*
		 * mini_tiered_promote () returns FALSE when the backend declined the
		 * method (a gate) or the compile failed. Either way tier 0 is the
		 * terminal body and we must not try again.
		 */
		if (mini_tiered_promote (entry->method, entry->domain, entry->opt))
			tiered_set_state (entry->method, entry->domain, TIER_STATE_PROMOTED);
		else
			tiered_set_state (entry->method, entry->domain, TIER_STATE_TIER0_TERMINAL);

		g_free (entry);
	}

	tiered_draining = FALSE;
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
	tiered_try_drain ();
}

/*
 * The tier-0 prologue's cold path calls this once a method's call counter reaches
 * MONO_TIERED_CALL_THRESHOLD (see mono_arch_emit_prolog). COUNTER is the block
 * mini_tiered_alloc_counter () handed the emitter; it carries the method, its
 * domain and the opt its tier-0 compile used, so the enqueue matches the eager
 * path exactly. The enqueue is idempotent (the tiered_state guard), so the branch
 * firing on every subsequent call is harmless.
 *
 * With the threshold on, this REPLACES the eager enqueue at the tier-0 publish
 * site: a method is admitted to tier 1 only after it has been entered
 * threshold-many times, not on its first call.
 */
void
mini_tiered_count_reached (gpointer counter)
{
	MiniTieredCounter *ctr = (MiniTieredCounter *) counter;

	/* Reached from generated tier-0 code, so the feature is on by construction;
	 * the guard is defensive. */
	if (!mono_llvm_tiered_enabled () || !ctr)
		return;

	mono_llvm_tiered_enqueue (ctr->method, ctr->domain, ctr->opt);

	/*
	 * Drive the drain from here too: a hot steady-state method crosses the
	 * threshold during ordinary execution, when no compile is in flight whose
	 * mono_llvm_tiered_compile_end would drain the queue. tiered_try_drain ()
	 * applies the same depth==0 / ready / current-domain / appdomain-ref /
	 * not-dying gates, so a crossing deep in a cctor-driven compile nest just
	 * leaves the method enqueued for the next qualifying drain.
	 *
	 * W4: when the drain moves to a background compile thread this inline call
	 * becomes a signal to that thread instead - the counter path must only ever
	 * produce queue entries, never run LLVM codegen (or cctors) on the crossing
	 * thread.
	 */
	tiered_try_drain ();
}

/*
 * TRUE while a tier-1 promotion compile is in flight on this thread. mini.c
 * uses this to select the LLVM path for that one compile without turning on
 * mono_use_llvm globally, which would make tier 0 use LLVM too.
 */
gboolean
mono_llvm_tiered_in_promotion (void)
{
	return tiered_promote_active;
}

void
mono_llvm_tiered_promote_begin (void)
{
	tiered_promote_active = TRUE;
}

void
mono_llvm_tiered_promote_end (void)
{
	tiered_promote_active = FALSE;
}

/*
 * Clear the promotion flag for a nested compile and return the previous value,
 * so LLVM codegen cannot leak onto a nested stack via a cctor-driven compile.
 */
gboolean
mono_llvm_tiered_promotion_suspend (void)
{
	gboolean old = tiered_promote_active;

	tiered_promote_active = FALSE;
	return old;
}

void
mono_llvm_tiered_promotion_restore (gboolean old)
{
	tiered_promote_active = old;
}
