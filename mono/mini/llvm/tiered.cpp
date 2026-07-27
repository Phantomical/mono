/*
 * tiered.cpp: tier-0 -> tier-1 promotion policy for the LLVM backend.
 *
 *   Tier 0 is the classic mini JIT, lazy, on first call - unchanged. A method
 *   becomes eligible for tier 1 either on its first successful tier-0 compile
 *   (MONO_TIERED_CALL_THRESHOLD == 0) or once its tier-0 entry count crosses
 *   the configured threshold (mini_tiered_count_reached (), called from the
 *   counter mono_arch_emit_prolog bakes into the tier-0 prologue). Where the
 *   LLVM compile actually runs differs between the two:
 *
 *     - Threshold > 0: mini_tiered_count_reached () only enqueues the method
 *       and wakes the background compile worker below - it never runs LLVM
 *       codegen, or the cctors a promotion compile can trigger, on the thread
 *       that crossed the threshold. See "The queue exists..." below for why.
 *
 *     - Threshold == 0: the tier-0 publish site (mono_jit_compile_method_inner_1
 *       in mini.c) calls mono_llvm_tiered_promote_sync () directly, right
 *       there, on the thread that just finished the tier-0 compile - no
 *       queue, no worker hop. There is also no counter block at threshold 0
 *       to arm a redirect sled with, so the caller gets the tier-1 code
 *       pointer back from that same call and starts running it immediately.
 *
 *   Tier 1 is terminal: if it fails - or the backend declines the method (an
 *   EH-clause, gshared or save_lmf gate) - the method latches tier-0-terminal
 *   and is never retried. "Tier 1" means that terminal body, not necessarily
 *   the LLVM body; a decline is a normal outcome, not a failure.
 *
 * The queue exists, for the threshold path, rather than promoting inline at
 * the point a method becomes eligible, because mono's JIT nests: compiling a
 * method runs class initializers, which compile more methods, which run more
 * initializers, and a threshold crossing can equally fire from deep inside
 * such a nest. A tier-0 frame is small, but an LLVM codegen frame is not, so
 * promoting inline there could stack a full LLVM pipeline on top of however
 * deep the nest already is. The background worker sidesteps this entirely -
 * it always compiles on its own, shallow stack, regardless of what triggered
 * the enqueue or how deep that thread's own nest happened to be.
 *
 * The threshold-0 path promotes inline anyway, but two things keep it out of
 * that same trap: it always compiles with run_cctors = FALSE, so it can never
 * trigger the cctor-driven nest the queue exists to dodge, and
 * tiered_sync_active (see mono_llvm_tiered_promote_sync ()) stops a tier-0
 * compile that DOES happen to nest inside it - LLVM codegen resolving a
 * helper method it needs on the spot, say - from starting a promotion of its
 * own, which would otherwise stack a second LLVM compile on the first.
 *
 * Tier-1 compiles run concurrently: a pool of worker threads
 * (MONO_TIERED_COMPILE_THREADS) drains the queue, and the threshold-0 path
 * compiles inline on however many mutator threads reach it at once. Nothing
 * serializes them, because nothing in the compile pipeline is shared any
 * more - each compile builds in an LLVMContext of its own
 * (create_compile_module (), translator.cpp) and the engine's process-wide
 * state is individually guarded.
 */

#include <config.h>
#include <glib.h>

#include <cstddef>
#include <vector>

#include "mini.h"
#include "mini-runtime.h"
#include <mono/metadata/appdomain.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/class-inlines.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/marshal.h>
#include <mono/utils/atomic.h>
#include <mono/utils/mono-coop-mutex.h>
#include <mono/utils/mono-threads-coop.h>
#include <mono/utils/mono-lazy-init.h>
#include <mono/utils/mono-proclib.h>
#include <mono/utils/mono-time.h>
#include <mono/utils/w32api.h>
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
 * Forward declaration: TieredEntry below needs a pointer to this, but the
 * struct itself is defined further down, next to the offsetof () accessors
 * the codegen emitter uses.
 */
typedef struct MiniTieredCounter MiniTieredCounter;

/*
 * A queued method, with the optimization set its tier-0 compile used - tier 1
 * must be built with the same opts, and the background worker processes the
 * entry long after the compile that chose them has returned.
 */
typedef struct {
	MonoMethod *method;
	MonoDomain *domain;
	guint32 opt;
	/*
	 * The counter block mini_tiered_count_reached () was called with - every
	 * entry on this queue comes from there (the call-count threshold path;
	 * threshold 0 promotes synchronously in mini.c instead and never reaches
	 * this queue at all), so this is always non-NULL here. Once
	 * mini_tiered_promote () succeeds for an entry, the worker arms its
	 * redirect slot - see tiered_worker_process_entry ().
	 */
	MiniTieredCounter *ctr;
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
 * table owns the record and a domain purge frees it, while the background
 * worker owns the entry it popped; if they were one allocation a purge could
 * free the entry out from under a promotion that is already in flight.
 */
typedef struct {
	MonoDomain *domain;
	TierState state;
	/*
	 * Mirrors the queue entry's ctr for a threshold-path record. Always NULL
	 * for a threshold-0 record: mono_llvm_tiered_promote_sync () never
	 * allocates a counter block, so there is nothing to arm a sled with -
	 * that path returns the tier-1 code pointer straight to its caller
	 * instead. Kept here, past the point a threshold-path queue entry itself
	 * is freed, purely so mono_llvm_tiered_method_redirect_armed () - a test
	 * probe - can answer "has this method's sled actually been armed" for a
	 * method whose MiniTieredCounter would otherwise be unreachable once its
	 * TieredEntry is gone. Nothing in the promotion path itself reads this.
	 */
	MiniTieredCounter *ctr;
} TieredRecord;

static mono_mutex_t tiered_mutex;
static GHashTable *tiered_state;	/* MonoMethod* -> TieredRecord* */
static GQueue *tiered_queue;		/* pending TieredEntry* */

/*
 * The background workers' doorbell - what lets it sleep when the queue is empty
 * and wake when something lands on it. Declared up here, away from the rest of
 * the worker's state further down, only because tiered_do_init () initializes
 * it; see the comment there.
 */
static MonoCoopMutex tiered_worker_mutex;
static MonoCoopCond tiered_worker_wake;

/*
 * Promotion is unsafe until mini_init () has finished: the domain is still
 * being constructed before that (create_domain_objects compiles methods), and
 * running LLVM codegen there reaches domain state that does not exist yet.
 * Methods compiled during startup still queue for the threshold path; the
 * background worker holds off on all of them until this becomes TRUE. There
 * is no queue for the threshold-0 path, so a method compiled that early
 * simply never gets a synchronous promotion attempt and stays at tier 0.
 */
static gboolean tiered_ready;

/*
 * TRUE only for the duration of the one mini_method_compile () that is
 * promoting a method - meaning, with the background worker, only on the
 * worker's own thread. It must NOT cover more than that one compile: when
 * run_cctors is TRUE, promotion runs cctors, which compile other, unrelated
 * methods, and those must still get a classic tier-0 body.
 * mono_jit_compile_method_inner () - the entry every nested compile goes
 * through - saves and clears this around itself, so a nested compile sees
 * FALSE while the promotion compile, which calls mini_method_compile directly,
 * keeps it.
 */
static __thread gboolean tiered_promote_active;

/*
 * TRUE for the whole duration of one thread's synchronous, threshold-0
 * promotion attempt - including through any tier-0 compile that attempt
 * itself has to trigger on the spot (LLVM codegen resolving a helper method
 * it needs right now, say). Unlike tiered_promote_active,
 * mono_jit_compile_method_inner () does NOT suspend/restore this around such
 * a nested compile, so it stays visible for the whole nest: it is what stops
 * that nested compile's own tier-0 publish from starting a promotion of its
 * own, which would otherwise stack a second LLVM compile on top of the first
 * with no bound but the size of the call graph the first compile happens to
 * touch. See mono_llvm_tiered_promote_sync ().
 */
static __thread gboolean tiered_sync_active;

static mono_lazy_init_t tiered_lazy_init = MONO_LAZY_INIT_STATUS_NOT_INITIALIZED;
static gboolean tiered_enabled;

/*
 * MONO_TIERED_CALL_THRESHOLD: how many times a tier-0 body is entered before it
 * is enqueued for tier 1. Default 1000; 0 means promote synchronously on first
 * publish instead (see mono_llvm_tiered_promote_sync ()). Read once in
 * tiered_do_init () and thereafter a constant the prologue emitter bakes in.
 * Meaningless - and left 0 - when tiering is off.
 */
static guint32 tiered_call_threshold;

/*
 * The per-method, per-domain tier-0 call counter and redirect block. The
 * prologue bakes this block's address twice - once at method entry to check
 * tier1_entry (the redirect sled), and once at the prologue tail to
 * lock-xadd count - and OPT/METHOD/DOMAIN let mini_tiered_count_reached ()
 * enqueue with exactly the arguments its own tier-0 compile used. Allocated
 * from the domain mem manager (mono_domain_alloc0, so every field starts
 * zeroed) and freed with the tier-0 code at domain unload; a re-JIT for a new
 * domain gets a fresh, zeroed counter, which is correct (promotion is
 * per-domain). Threshold 0 never allocates one of these at all - see
 * mini_tiered_alloc_counter () below.
 */
struct MiniTieredCounter {
	/*
	 * Offset 0. The prologue's redirect check (mono_arch_emit_prolog, point A)
	 * bakes this block's address and reads tier1_entry below directly; the tail
	 * counter block (point B) bakes it again and lock-xadds this word directly.
	 * Neither has this struct's definition to hand - mini_tiered_counter_count_offset ()
	 * and mini_tiered_counter_tier1_entry_offset () are how they get the two
	 * offsets they need without hardcoding them.
	 */
	guint32 count;
	guint32 opt;
	MonoMethod *method;
	MonoDomain *domain;
	/*
	 * Idempotence guard on enqueue, not a terminal-state latch: CAS'd 0->1 by
	 * mini_tiered_count_reached () the one time it ever runs for this counter
	 * (the prologue's lock-xadd dispatches on an exact count match, so under
	 * normal operation that is already at most once; this is belt-and-suspenders
	 * against ever double-enqueuing the same counter). Read with an atomic load
	 * and set with an atomic store/CAS, never under tiered_mutex.
	 */
	gint32 settled;
	/*
	 * The redirect slot - the sled. NULL until the background compile worker
	 * finishes promoting this method, at which point it is the last thing the
	 * worker writes (mono_atomic_store_ptr, after the tier-1 jit_info is fully
	 * published) and becomes the tier-1 entry point. The prologue's point-A
	 * check loads this with a plain mov, and on x86-TSO a plain load is already
	 * an acquire, so a non-NULL read is guaranteed to see fully-formed code and
	 * its published jit_info, never a torn or in-progress write.
	 */
	gpointer tier1_entry;
};

/*
 * Byte offsets of the two words within MiniTieredCounter that generated code
 * touches directly - mono_arch_emit_prolog has no visibility into this (C++)
 * struct's layout, so it gets them from here instead of hardcoding numbers
 * that would silently desync from the struct on the next field reorder.
 */
gsize
mini_tiered_counter_count_offset (void)
{
	return offsetof (MiniTieredCounter, count);
}

gsize
mini_tiered_counter_tier1_entry_offset (void)
{
	return offsetof (MiniTieredCounter, tier1_entry);
}

/*
 * MONO_TIERED, which is on unless it is turned off by hand: an empty value or one
 * that reads as a negative disables tiering, anything else - including the variable
 * not being set at all - leaves it on.
 */
static gboolean
tiered_env_enabled (void)
{
	char *e = g_getenv ("MONO_TIERED");
	gboolean enabled = TRUE;

	if (e) {
		if (!*e || !g_ascii_strcasecmp (e, "0") || !g_ascii_strcasecmp (e, "no") || !g_ascii_strcasecmp (e, "false"))
			enabled = FALSE;
		g_free (e);
	}
	return enabled;
}

static void
tiered_do_init (void)
{
	/*
	 * Tier 1 is LLVM, so a run that turned LLVM off has no tier 1 to promote to
	 * and tiering goes with it - otherwise --nollvm would leave the counters and
	 * the worker in place and still hand back LLVM-compiled bodies. mini_init ()
	 * has settled mono_use_llvm by the time anything gets here: the first caller
	 * is always a compile, and nothing compiles before the runtime is up.
	 */
	tiered_enabled = mono_use_llvm && tiered_env_enabled ();
	if (!tiered_enabled)
		return;
	mono_os_mutex_init_recursive (&tiered_mutex);
	tiered_state = g_hash_table_new (g_direct_hash, g_direct_equal);
	tiered_queue = g_queue_new ();
	/*
	 * The worker's doorbell is set up here rather than in tiered_worker_start ()
	 * because mono_llvm_tiered_quiesce () has to be able to take it before the
	 * worker thread exists - a quiesce that no-oped for want of a worker, and
	 * then had one start under it, would leave exactly the race it is there to
	 * prevent. Costs a mutex and a condvar in a run that never promotes.
	 */
	mono_coop_mutex_init (&tiered_worker_mutex);
	mono_coop_cond_init (&tiered_worker_wake);

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
 * The call-count threshold, or 0 when the feature is off (tiering disabled, or
 * MONO_TIERED_CALL_THRESHOLD=0). At 0 the prologue emits no counter, and the
 * tier-0 publish site promotes the method to tier 1 synchronously, right there
 * on the compiling thread, instead of taking the counter/background-worker path
 * below - see mono_llvm_tiered_promote_sync ().
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

/*
 * Bracket every mini_method_compile () so nested compiles can tell they are
 * nested (mono_jit_compile_method_inner () uses this window to suspend
 * mono_llvm_tiered_promotion_suspend/restore around them). Nesting depth
 * itself no longer gates anything here - promotion runs on the background
 * worker's own stack regardless of how deep the enqueuing thread's compile
 * nest was - so these only need to keep the feature's lazy init warm.
 */
void
mono_llvm_tiered_compile_begin (void)
{
	mono_llvm_tiered_enabled ();
}

void
mono_llvm_tiered_compile_end (void)
{
	mono_llvm_tiered_enabled ();
}

/* Forward declarations: tiered_enqueue () lazily starts and wakes the
 * background worker defined further down in this file. */
static void tiered_worker_ensure_started (void);
static void tiered_worker_signal (void);

/*
 * Depth cap on the recursive dynamic-metadata scan below. Generic
 * instantiations nest (List<List<...>>), so rather than chase an arbitrarily
 * deep type we stop and treat it as "touches dynamic". A bounded, conservative
 * answer is always safe here - a false decline only costs one tier-1 body.
 */
#define TIERED_DYNAMIC_SCAN_MAX_DEPTH 8

static gboolean tiered_type_touches_dynamic (MonoType *type, int depth);

/*
 * TRUE if KLASS is defined in a dynamic (Reflection.Emit) image, or is a
 * generic instantiation any of whose type arguments is (or reaches) one.
 *
 * The container of a ginst can live in a perfectly ordinary image - e.g.
 * Dictionary<,> in mscorlib - while a type argument is a dynamic type, so the
 * arguments have to be walked even when the class's own image is normal.
 */
static gboolean
tiered_class_touches_dynamic (MonoClass *klass, int depth)
{
	if (!klass || depth > TIERED_DYNAMIC_SCAN_MAX_DEPTH)
		return TRUE;

	if (image_is_dynamic (m_class_get_image (klass)))
		return TRUE;

	if (mono_class_is_ginst (klass)) {
		MonoGenericClass *gclass = mono_class_get_generic_class (klass);
		if (!gclass)
			return TRUE;
		MonoGenericInst *inst = gclass->context.class_inst;
		if (inst) {
			for (guint i = 0; i < inst->type_argc; ++i)
				if (tiered_type_touches_dynamic (inst->type_argv [i], depth + 1))
					return TRUE;
		}
	}

	return FALSE;
}

/* TRUE if any type argument in INST is, contains, or is parameterized over a
 * dynamic type. */
static gboolean
tiered_inst_touches_dynamic (MonoGenericInst *inst, int depth)
{
	if (!inst)
		return FALSE;
	if (depth > TIERED_DYNAMIC_SCAN_MAX_DEPTH)
		return TRUE;

	for (guint i = 0; i < inst->type_argc; ++i)
		if (tiered_type_touches_dynamic (inst->type_argv [i], depth + 1))
			return TRUE;

	return FALSE;
}

/*
 * TRUE if TYPE is, contains, or is parameterized over a type from a dynamic
 * image. Walks element types (pointers, arrays) and generic arguments so a
 * dynamic type buried inside e.g. Foo<Bar<Dynamic>>[] is still found. Type
 * parameters (VAR/MVAR) and primitives reach nothing dynamic and return FALSE.
 */
static gboolean
tiered_type_touches_dynamic (MonoType *type, int depth)
{
	if (!type || depth > TIERED_DYNAMIC_SCAN_MAX_DEPTH)
		return TRUE;

	switch (type->type) {
	case MONO_TYPE_CLASS:
	case MONO_TYPE_VALUETYPE:
		return tiered_class_touches_dynamic (type->data.klass, depth + 1);
	case MONO_TYPE_GENERICINST: {
		MonoGenericClass *gclass = type->data.generic_class;
		if (!gclass)
			return TRUE;
		if (tiered_class_touches_dynamic (gclass->container_class, depth + 1))
			return TRUE;
		return tiered_inst_touches_dynamic (gclass->context.class_inst, depth + 1);
	}
	case MONO_TYPE_PTR:
		return tiered_type_touches_dynamic (type->data.type, depth + 1);
	case MONO_TYPE_SZARRAY:
		return tiered_class_touches_dynamic (type->data.klass, depth + 1);
	case MONO_TYPE_ARRAY:
		return type->data.array ? tiered_class_touches_dynamic (type->data.array->eklass, depth + 1) : TRUE;
	default:
		return FALSE;
	}
}

/*
 * TRUE when METHOD must NOT be promoted to tier 1 - it stays at tier 0 forever.
 *
 * This is the whole tier-1 decline decision, and it is deliberately made HERE,
 * at enqueue, on the mutator thread that owns METHOD's metadata. Every read
 * below - the declaring class, the generic context, the signature, a wrapper's
 * resolved target - is stable on this thread but a data race on the background
 * worker, which runs on a foreign thread concurrently with the Reflection.Emit
 * that keeps mutating dynamic metadata and the GC that moves the managed
 * builder objects behind it. So the worker does none of this: a declined
 * method is simply never queued (see tiered_enqueue ()), and the worker only
 * ever compiles methods this function has already cleared.
 *
 * Conservative throughout - when a lookup fails or a type is too deep to scan,
 * decline. A false decline only leaves a method at tier 0; a false accept is a
 * memory-safety bug on the worker.
 *
 * Declines:
 *   - NoOptimization methods: mono_llvm_check_method_supported () refuses them
 *     unconditionally, so they can never hold a tier-1 body.
 *   - anything not in the root domain: a non-root app-domain is transient and
 *     can unload out from under a background compile, and its per-domain tier-1
 *     trampolines would dangle in the process-global direct-call symbol cache
 *     (engine.cpp) after that unload.
 *   - Reflection.Emit metadata: a DynamicMethod; a method in a dynamic image; a
 *     generic method or class instantiated over a dynamic type argument; a
 *     method whose signature names a dynamic type; or a wrapper whose target is
 *     any of those.
 */
static gboolean
tiered_method_should_decline (MonoMethod *method, MonoDomain *domain)
{
	if (!method)
		return TRUE;

	/* A NoOptimization method never gets a tier-1 body (see
	 * mono_llvm_check_method_supported () in translator.cpp), so promoting it
	 * would just burn a background compile that declines at the end. Decline
	 * it here instead, before it ever gets queued. */
	if (method->iflags & METHOD_IMPL_ATTRIBUTE_NOOPTIMIZATION)
		return TRUE;

	/* Root domain is the only one with process-lifetime code/trampoline
	 * stability; NULL or any child domain declines. */
	if (domain != mono_get_root_domain ())
		return TRUE;

	if (method_is_dynamic (method))
		return TRUE;

	/* Declaring class: covers a dynamic image directly, and a generic class
	 * instantiated over a dynamic type argument (Dictionary<string, Dynamic>). */
	if (tiered_class_touches_dynamic (method->klass, 0))
		return TRUE;

	/* Method-level generic arguments (SomeClass::Bar<Dynamic> ()). */
	MonoGenericContext *ctx = mono_method_get_context (method);
	if (ctx && tiered_inst_touches_dynamic (ctx->method_inst, 0))
		return TRUE;

	/* Signature - a dynamic return or parameter type. */
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	if (!sig)
		return TRUE;
	if (tiered_type_touches_dynamic (sig->ret, 0))
		return TRUE;
	for (int i = 0; i < sig->param_count; ++i)
		if (tiered_type_touches_dynamic (sig->params [i], 0))
			return TRUE;

	/* A wrapper (e.g. the runtime-invoke wrapper MethodInfo.Invoke builds) whose
	 * resolved target is dynamic. Resolving the target is a mutator-thread read
	 * of the wrapper's own metadata, safe here for the same reason as the rest. */
	if (method->wrapper_type != MONO_WRAPPER_NONE) {
		MonoMethod *target = mono_marshal_method_from_wrapper (method);
		if (target && target != method) {
			if (method_is_dynamic (target))
				return TRUE;
			if (tiered_class_touches_dynamic (target->klass, 0))
				return TRUE;
		}
	}

	return FALSE;
}

/*
 * Queue METHOD for tier 1 and wake the background compile worker. Ignored if
 * the method is already promoted or latched terminal. CTR is the counter
 * block mini_tiered_count_reached () was called with, which the worker arms
 * once it promotes METHOD - see tiered_worker_process_entry (). The only
 * caller is mini_tiered_count_reached () below (the call-count threshold
 * path), so CTR is always non-NULL in practice; threshold 0 promotes
 * synchronously instead and never reaches this queue.
 */
static void
tiered_enqueue (MonoMethod *method, MonoDomain *domain, guint32 opt, MiniTieredCounter *ctr)
{
	gboolean queued = FALSE;

	if (!mono_llvm_tiered_enabled () || !method)
		return;

	/*
	 * Decide here, on the mutator that owns METHOD's metadata, whether tier 1
	 * must decline it - the background worker cannot make this call safely (it
	 * would race Reflection.Emit / GC; see tiered_method_should_decline ()). A
	 * declined method is latched at tier 0 by never being queued at all: the
	 * tier-0 counter's saturating increment already stops it re-crossing the
	 * threshold, so no state record is needed to hold it down, and NOT recording
	 * one avoids a dangling MonoMethod* -> record for a dynamic or non-root
	 * method whose key can die with its image or domain before we would ever
	 * purge it.
	 */
	if (tiered_method_should_decline (method, domain))
		return;

	mono_os_mutex_lock (&tiered_mutex);
	if (!g_hash_table_lookup (tiered_state, method)) {
		TieredEntry *entry = g_new0 (TieredEntry, 1);
		TieredRecord *rec = g_new0 (TieredRecord, 1);

		rec->domain = domain;
		rec->state = TIER_STATE_TIER0;
		rec->ctr = ctr;
		entry->method = method;
		entry->domain = domain;
		entry->opt = opt;
		entry->ctr = ctr;
		g_hash_table_insert (tiered_state, method, rec);
		g_queue_push_tail (tiered_queue, entry);
		queued = TRUE;
	}
	mono_os_mutex_unlock (&tiered_mutex);

	/* Only wake the worker (and pay for starting it, the first time) if this
	 * call actually added something - most calls here are the harmless repeat
	 * enqueue mini_tiered_count_reached ()'s idempotence guard already filters
	 * out before it ever gets here. */
	if (queued) {
		tiered_worker_ensure_started ();
		tiered_worker_signal ();
	}
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
 * Promotion-policy introspection for the functional test (tiered-promotion.cs,
 * through the MonoTests.Tiering.Probe internal calls). Returns METHOD's recorded
 * tier state as a TierState value (0 queued/tier-0, 1 promoted, 2 tier-0
 * terminal), or -1 when the feature is off or METHOD has no record - the latter
 * being the "never enqueued, stayed cold at tier 0" outcome the test asserts for
 * a below-threshold method. Matches any domain: the test runs in the root domain
 * and there is one record per (non-generic) method, so it need not thread a
 * domain pointer through managed code.
 */
int
mono_llvm_tiered_method_state (MonoMethod *method)
{
	TieredRecord *rec;
	int state = -1;

	if (!mono_llvm_tiered_enabled () || !method)
		return -1;

	mono_os_mutex_lock (&tiered_mutex);
	rec = (TieredRecord *) g_hash_table_lookup (tiered_state, method);
	if (rec)
		state = (int) rec->state;
	mono_os_mutex_unlock (&tiered_mutex);

	return state;
}

/*
 * TRUE once the redirect sled for METHOD has actually been armed - i.e. the
 * background worker has written a non-NULL tier1_entry into its counter
 * block - as opposed to mono_llvm_tiered_method_state () == PROMOTED, which
 * only says the hash-swap happened. Also for the functional test: promotion
 * alone proves the lookup path picks up tier 1, but a test that calls the
 * method from a single, already-JIT'd call site (the common case - a tight
 * loop) only actually exercises tier 1 if the SLED redirects that call site,
 * so this is the more precise thing to assert "the sled fires" with.
 *
 * FALSE for the eager (threshold == 0) path - there is no counter block - and
 * for a method that has not promoted (or promotion is still in flight on the
 * worker; the caller is expected to poll this, same as method state).
 */
gboolean
mono_llvm_tiered_method_redirect_armed (MonoMethod *method)
{
	TieredRecord *rec;
	gboolean armed = FALSE;

	if (!mono_llvm_tiered_enabled () || !method)
		return FALSE;

	mono_os_mutex_lock (&tiered_mutex);
	rec = (TieredRecord *) g_hash_table_lookup (tiered_state, method);
	if (rec && rec->ctr)
		armed = mono_atomic_load_ptr ((volatile gpointer *) &rec->ctr->tier1_entry) != NULL;
	mono_os_mutex_unlock (&tiered_mutex);

	return armed;
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
 * What actually makes background promotion safe is the appdomain ref the
 * worker takes before compiling (see tiered_worker_enter_domain ()); this
 * check only avoids the pointless work of compiling into a domain already
 * known to be going away.
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
		 * A domain can still have entries pending here at unload - the
		 * background worker processes the queue at its own pace, so there is
		 * no guarantee it has drained everything queued for a domain before
		 * that domain starts unloading.
		 *
		 * eglib's GQueue has no delete_link, so filter by draining into a
		 * scratch queue and pushing the survivors back, which also keeps the
		 * FIFO order the worker relies on.
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
 * The background tier-1 compile workers.
 *
 * A pool, sized by tiered_compile_thread_count (), all draining the one
 * queue and compiling concurrently. Sizing it is a throughput-versus-
 * politeness call, not a correctness one: nothing in the compile pipeline is
 * shared between compiles any more, since each builds in an LLVMContext of
 * its own (create_compile_module (), translator.cpp).
 *
 * tiered_worker_mutex/_wake are a doorbell, not what protects the queue: the
 * queue and its state table stay entirely under tiered_mutex. The doorbell
 * just lets a worker sleep when there is nothing to do and wake promptly when
 * tiered_enqueue () adds something, via the standard "recheck under the real
 * lock, then wait on the doorbell" pattern - held across the recheck-and-wait,
 * so a signal that lands between the recheck and the wait is never lost even
 * though the recheck itself briefly takes the other lock.
 *
 * Two workers never compile the same method: tiered_mutex covers the pop, so
 * an entry goes to exactly one of them, and the state table keeps a method
 * that is already queued or promoted from being enqueued again.
 */

/*
 * How long mono_llvm_tiered_shutdown () waits for the worker to exit before
 * giving up. Generous - long enough that a normal in-flight LLVM compile
 * always finishes well inside it - but not infinite; see the comment on
 * mono_llvm_tiered_shutdown () for why an unbounded wait is not acceptable
 * here.
 */
#define TIERED_SHUTDOWN_TIMEOUT_MS 5000

static mono_lazy_init_t tiered_worker_init = MONO_LAZY_INIT_STATUS_NOT_INITIALIZED;
static volatile gboolean tiered_worker_shutdown;
/*
 * Both guarded by tiered_worker_mutex, and both only interesting to
 * mono_llvm_tiered_quiesce (). A worker counts itself into _compiling across
 * the one window where it is doing real work with the doorbell dropped;
 * _quiesce_depth being nonzero is what stops any worker opening another such
 * window, and a quiesce is complete once the count falls to zero.
 */
static int tiered_workers_compiling;
static int tiered_quiesce_depth;
/*
 * How many workers have left their loop. Bumped under tiered_worker_mutex just
 * before each returns and signalled on the same doorbell - see
 * mono_llvm_tiered_shutdown (), which waits for it to reach the pool size.
 */
static int tiered_workers_exited;
/*
 * The pool's internal threads, kept from tiered_worker_start () so
 * mono_llvm_tiered_shutdown () can wait on each handle for a full OS-level exit -
 * tiered_workers_exited alone is set too early to know a thread is truly gone.
 *
 * Only mono_llvm_tiered_shutdown () reads this, and it runs the lazy-init check
 * first, so it never observes the vector mid-fill: the workers themselves never
 * touch it, even though the earlier ones are already running while
 * tiered_worker_start () is still appending the later ones.
 */
static std::vector<MonoInternalThread *> tiered_worker_threads;

static gsize WINAPI tiered_worker_main (gpointer unused);

/*
 * How many compile threads the pool runs.
 *
 * The default is a quarter of the machine, at least one and at most four.
 * Tier-1 promotion is speculative work on behalf of code that is already
 * running perfectly well at tier 0, so it should not be the reason a mutator
 * waits for a core; the cap keeps a big machine from devoting a lot of
 * hardware to it. MONO_TIERED_COMPILE_THREADS overrides, and is clamped to at
 * least 1 - "no compile threads" is spelled MONO_TIERED=0.
 */
static int
tiered_compile_thread_count (void)
{
	int n = MAX (1, MIN (4, mono_cpu_count () / 4));

	char *e = g_getenv ("MONO_TIERED_COMPILE_THREADS");
	if (e) {
		char *end = NULL;
		guint64 v = g_ascii_strtoull (e, &end, 10);
		/* Keep the default on an empty or malformed value. */
		if (end && end != e && *end == '\0' && v >= 1)
			n = (int) MIN (v, (guint64) 64);
		g_free (e);
	}

	return n;
}

static void
tiered_worker_start (void)
{
	int n = tiered_compile_thread_count ();

	tiered_worker_threads.reserve (n);
	for (int i = 0; i < n; ++i) {
		ERROR_DECL (error);
		MonoInternalThread *thread = mono_thread_create_internal (mono_get_root_domain (), tiered_worker_main, NULL, MONO_THREAD_CREATE_FLAGS_THREADPOOL, error);
		mono_error_assert_ok (error);
		tiered_worker_threads.push_back (thread);
	}
}

/*
 * Lazily create the pool on the first real enqueue - most runs of a tiered
 * build never promote anything (the default threshold is 1000 calls), so
 * there is no reason to spin up threads nobody is going to wake.
 */
static void
tiered_worker_ensure_started (void)
{
	mono_lazy_initialize (&tiered_worker_init, tiered_worker_start);
}

static void
tiered_worker_signal (void)
{
	mono_coop_mutex_lock (&tiered_worker_mutex);
	mono_coop_cond_signal (&tiered_worker_wake);
	mono_coop_mutex_unlock (&tiered_worker_mutex);
}

/*
 * Make DOMAIN current on this (the worker's) thread for the duration of a
 * promotion compile, having first confirmed it is not on its way out.
 *
 * The worker holds no appdomain ref of its own to start with - unlike a
 * mutator thread, which naturally holds one just by having entered the
 * domain to run managed code - so it takes one here with
 * mono_thread_push_appdomain_ref () + mono_domain_set_fast (), the same
 * push-ref-then-transfer pattern mono_threadpool_enqueue_work_item ()
 * (mono/metadata/threadpool.c) uses when the calling thread's current domain
 * differs from the one the work item targets. That ref is what makes
 * mono_threads_abort_appdomain_threads () wait for the
 * worker before a concurrent mono_domain_try_unload () can reach
 * mono_domain_free ().
 *
 * Checked once before taking the ref and once after: the second check closes
 * the window where DOMAIN starts unloading in between, so the worker never
 * proceeds holding a ref that was taken too late for the unload's thread scan
 * to have seen. That window - and closing it this way - is not new here; it
 * is the same one every other foreign-domain transfer in the runtime lives
 * with.
 */
static gboolean
tiered_worker_enter_domain (MonoDomain *domain)
{
	if (tiered_domain_is_dying (domain))
		return FALSE;

	mono_thread_push_appdomain_ref (domain);

	if (tiered_domain_is_dying (domain)) {
		mono_thread_pop_appdomain_ref ();
		return FALSE;
	}

	mono_domain_set_fast (domain, TRUE);
	return TRUE;
}

static void
tiered_worker_leave_domain (MonoDomain *original)
{
	mono_domain_set_fast (original, TRUE);
	mono_thread_pop_appdomain_ref ();
}

/*
 * Promote one queued entry, then - on success - arm its redirect sled.
 *
 * The sled is armed LAST, strictly after mini_tiered_promote () has both
 * compiled the tier-1 body and swapped it into domain->jit_code_hash: any
 * thread that reads a non-NULL tier1_entry (mono_arch_emit_prolog, point A)
 * must find fully-formed, callable code and a published jit_info behind it,
 * never a still-in-progress compile.
 */
static void
tiered_worker_process_entry (TieredEntry *entry)
{
	MonoDomain *original = mono_domain_get ();

	/*
	 * Whether METHOD is safe to promote on this foreign thread was already
	 * decided at enqueue, on the mutator (tiered_method_should_decline ()):
	 * non-root-domain and Reflection.Emit methods are never queued, so anything
	 * the worker dequeues is a root-domain method backed by stable, immutable
	 * metadata. The worker therefore does no metadata classification of its own -
	 * touching a dynamic method's declaring image or a wrapper's target here
	 * would be exactly the off-thread race that decision exists to avoid.
	 *
	 * The one thing still checked below is whether the (root) domain is on its
	 * way out, which tiered_worker_enter_domain () handles as it takes the ref.
	 */
	if (!tiered_worker_enter_domain (entry->domain)) {
		/*
		 * DOMAIN is unloading (or already gone). Nothing to do here:
		 * mono_llvm_tiered_domain_unload () owns purging this entry's
		 * bookkeeping and either already has or shortly will.
		 */
		return;
	}

	/* run_cctors=FALSE: this is the background worker, which must never run
	 * managed class constructors - see mini_tiered_promote ()'s doc comment. */
	if (mini_tiered_promote (entry->method, entry->domain, entry->opt, FALSE)) {
		tiered_set_state (entry->method, entry->domain, TIER_STATE_PROMOTED);

		if (entry->ctr) {
			/*
			 * mini_tiered_promote () already swapped domain->jit_code_hash
			 * over to the tier-1 jit_info before returning, so this lookup
			 * finds it (see the comment on that swap in mini.c).
			 */
			MonoJitInfo *ji = mini_lookup_method (entry->domain, entry->method, NULL);
			if (ji)
				mono_atomic_store_ptr ((volatile gpointer *) &entry->ctr->tier1_entry, ji->code_start);
		}
	} else {
		/*
		 * Declined or failed. tier1_entry stays NULL, so the redirect check
		 * never fires, and the counter block's saturating increment
		 * (mono_arch_emit_prolog, point B) has already stopped this method
		 * from ever crossing the threshold again - no retry, no re-dispatch
		 * storm.
		 */
		tiered_set_state (entry->method, entry->domain, TIER_STATE_TIER0_TERMINAL);
	}

	tiered_worker_leave_domain (original);
}

static gsize WINAPI
tiered_worker_main (gpointer unused)
{
	MonoInternalThread *internal = mono_thread_internal_current ();

	internal->state |= ThreadState_Background;
	internal->flags |= MONO_THREAD_FLAG_DONT_MANAGE;
	mono_thread_set_name_constant_ignore_error (internal, "Tiered JIT compiler", MonoSetThreadNameFlag_None);

	mono_coop_mutex_lock (&tiered_worker_mutex);
	while (!tiered_worker_shutdown) {
		TieredEntry *entry;

		/*
		 * Nothing to promote before mini_init () finishes - see the comment
		 * on tiered_ready. Poll it with a timed wait rather than blocking
		 * forever: mono_llvm_tiered_set_ready () does not signal the
		 * doorbell, so an infinite wait here would only ever be woken by an
		 * unrelated enqueue or shutdown.
		 */
		if (!tiered_ready) {
			mono_coop_cond_timedwait (&tiered_worker_wake, &tiered_worker_mutex, 200);
			continue;
		}

		/* Somebody is mutating state a compile would touch - leave the queue
		 * alone until they say otherwise. */
		if (tiered_quiesce_depth) {
			mono_coop_cond_timedwait (&tiered_worker_wake, &tiered_worker_mutex, 200);
			continue;
		}

		mono_os_mutex_lock (&tiered_mutex);
		entry = (TieredEntry *) g_queue_pop_head (tiered_queue);
		mono_os_mutex_unlock (&tiered_mutex);

		if (!entry) {
			mono_coop_cond_timedwait (&tiered_worker_wake, &tiered_worker_mutex, 200);
			continue;
		}

		/*
		 * Drop the doorbell around the actual compile - it can take
		 * milliseconds, and nothing about it needs that lock held (the
		 * queue itself is protected separately, by tiered_mutex above).
		 */
		tiered_workers_compiling++;
		mono_coop_mutex_unlock (&tiered_worker_mutex);
		tiered_worker_process_entry (entry);
		g_free (entry);
		mono_coop_mutex_lock (&tiered_worker_mutex);
		tiered_workers_compiling--;
		/* Broadcast, not signal: a quiescing thread, the shutdown path and the
		 * other workers can all be waiting on this doorbell, and they want
		 * different news. */
		mono_coop_cond_broadcast (&tiered_worker_wake);
	}

	/*
	 * Publish exit under the same mutex/cond pair mono_llvm_tiered_shutdown ()
	 * waits on, so it cannot miss this: either it is not waiting yet (in which
	 * case it will see the count already up when it checks) or it is blocked in
	 * mono_coop_cond_timedwait () on this exact mutex, which the broadcast below
	 * wakes once we unlock. Broadcast because the other workers are waiting on
	 * the same doorbell for the shutdown flag.
	 */
	tiered_workers_exited++;
	mono_coop_cond_broadcast (&tiered_worker_wake);
	mono_coop_mutex_unlock (&tiered_worker_mutex);

	return 0;
}

/*
 * Ask the worker to exit, and WAIT (bounded) for it to actually do so before
 * returning. Not a join in the thread-API sense - the worker is a background,
 * DONT_MANAGE thread, so mono_thread_manage () never waits for it - but
 * mini_cleanup () calls this immediately before it starts freeing domain and
 * LLVM state that an in-flight compile on the worker still touches, so this
 * function has to be the thing that actually waits, or that free can race a
 * compile that is still running.
 *
 * Two phases. First wait for every worker to count itself out, which each does
 * as it leaves its loop. That count alone is NOT enough to return on: a worker
 * bumps it from inside tiered_worker_main (), still several steps short of
 * actually terminating - it has yet to run its thread-start wrapper's teardown (apartment
 * cleanup and mono_thread_detach_internal ()). If we returned here, that teardown
 * would run concurrently with the mono_runtime_cleanup () mini_cleanup () does
 * next, which tears down the very thread subsystem the worker's own detach
 * touches - a shutdown race that trips "mono_thread_internal_is_current" in
 * mono_thread_detach_internal (). So, second, wait on the worker's thread
 * handle, which is signalled only once the OS thread has fully exited, then reap
 * it - the same handle-wait-then-join the finalizer thread's shutdown uses
 * (mono_gc_cleanup ()).
 *
 * Both waits are bounded, not infinite: a worker wedged in a pathological
 * compile (or stuck unable to leave a domain) must not hang process exit
 * forever. If a timeout fires, this returns anyway - the residual risk is
 * exactly the use-after-free this function exists to close, just now bounded
 * to "the worker was still running after N seconds of graceful shutdown"
 * instead of "always", and left for the process teardown to race as best it
 * can. There is no stronger cancellation available short of unsafely killing
 * the thread mid-compile, which would be worse.
 */
void
mono_llvm_tiered_shutdown (void)
{
	gint64 start;
	gboolean exited;

	if (!mono_llvm_tiered_enabled () || !mono_lazy_is_initialized (&tiered_worker_init))
		return;

	mono_coop_mutex_lock (&tiered_worker_mutex);
	tiered_worker_shutdown = TRUE;
	/* Broadcast: every worker in the pool is waiting on this one doorbell. */
	mono_coop_cond_broadcast (&tiered_worker_wake);

	start = mono_msec_ticks ();
	while (tiered_workers_exited < (int) tiered_worker_threads.size ()) {
		gint64 elapsed = mono_msec_ticks () - start;
		if (elapsed >= TIERED_SHUTDOWN_TIMEOUT_MS)
			break;
		mono_coop_cond_timedwait (&tiered_worker_wake, &tiered_worker_mutex, (guint32) (TIERED_SHUTDOWN_TIMEOUT_MS - elapsed));
	}
	exited = tiered_workers_exited == (int) tiered_worker_threads.size ();
	mono_coop_mutex_unlock (&tiered_worker_mutex);

	/*
	 * Only wait on the handles once every worker has told us it is on its way
	 * out; if the loop above timed out (one still mid-compile) its handle is not
	 * about to signal and we proceed as before. The waits run in GC-safe mode -
	 * they block, and must not hold up a GC while they do.
	 *
	 * The timeout is per thread rather than one budget shared across the pool.
	 * Overshooting it takes every worker being late, which is the case where the
	 * wait is hopeless anyway; splitting one budget between them would instead
	 * cut short the common case of one slow worker and several already done.
	 */
	if (!exited)
		return;

	for (MonoInternalThread *thread : tiered_worker_threads) {
		if (!thread || !thread->handle)
			continue;

		MONO_ENTER_GC_SAFE;
		mono_thread_info_wait_one_handle (thread->handle, TIERED_SHUTDOWN_TIMEOUT_MS, FALSE);
		MONO_EXIT_GC_SAFE;

		mono_threads_add_joinable_thread ((gpointer) (gsize) MONO_UINT_TO_NATIVE_THREAD_ID (thread->tid));
	}
}

/*
 * Stop the background worker from running any tier-1 compile, and wait for one
 * already in flight to finish, so the caller can mutate runtime state a compile
 * would otherwise be reading. Undone by mono_llvm_tiered_resume (); the pair
 * nests.
 *
 * This exists for the --regression harness, which between opt combinations
 * throws away everything the domain has compiled so far - it destroys and
 * recreates domain->jit_trampoline_hash and domain->jit_code_hash outright
 * (mini_regression_step ()). A compile on the worker walks the first of those
 * inside mono_create_jit_trampoline () and publishes into the second, both
 * under the domain locks, which does nothing to protect them from a wipe that
 * takes no lock at all: the worker ends up walking a slot chain in freed
 * memory. Nothing else in the runtime replaces those tables mid-flight, which
 * is why this is the only caller.
 *
 * Deliberately does NOT drop what is queued. A pending entry is the only record
 * that its method crossed the promotion threshold - the counter that got it
 * there is spent - so dropping it would silently strand that method at tier 0
 * for the rest of the run. The worker just picks it up after the resume, and
 * the wipe having removed the method's tier-0 jit_code_hash entry in the
 * meantime is a case mini_tiered_promote () already handles (see jit_failed).
 *
 * Takes effect whether or not the worker thread has been started yet - a
 * worker that starts during a quiesce honours it on its first trip round the
 * loop. A no-op only with tiering off entirely.
 */
void
mono_llvm_tiered_quiesce (void)
{
	if (!mono_llvm_tiered_enabled ())
		return;

	mono_coop_mutex_lock (&tiered_worker_mutex);
	tiered_quiesce_depth++;
	/*
	 * Unbounded, unlike the shutdown wait: this runs with the process healthy
	 * and the worker guaranteed to finish and clear the flag, and returning
	 * early would hand the caller exactly the use-after-free it called this to
	 * avoid. Timed rather than plain so the wait is a GC-safe point that keeps
	 * getting re-entered - the compile we are waiting on may itself need a GC,
	 * which needs this thread to stop.
	 */
	while (tiered_workers_compiling)
		mono_coop_cond_timedwait (&tiered_worker_wake, &tiered_worker_mutex, 50);
	mono_coop_mutex_unlock (&tiered_worker_mutex);
}

/*
 * Let the background worker start compiling again, undoing one
 * mono_llvm_tiered_quiesce ().
 */
void
mono_llvm_tiered_resume (void)
{
	if (!mono_llvm_tiered_enabled ())
		return;

	mono_coop_mutex_lock (&tiered_worker_mutex);
	g_assert (tiered_quiesce_depth > 0);
	if (--tiered_quiesce_depth == 0)
		mono_coop_cond_broadcast (&tiered_worker_wake);
	mono_coop_mutex_unlock (&tiered_worker_mutex);
}

/*
 * The tier-0 prologue's cold path calls this once a method's call counter reaches
 * MONO_TIERED_CALL_THRESHOLD (see mono_arch_emit_prolog, point B). COUNTER is the
 * block mini_tiered_alloc_counter () handed the emitter; it carries the method,
 * its domain and the opt its tier-0 compile used, so the enqueue has everything
 * it needs without going back to the crossing thread's own state.
 *
 * This only ever enqueues and wakes the background worker - it must NEVER run
 * LLVM codegen (or the cctors a promotion compile can trigger) on the crossing
 * thread, which is some arbitrary mutator thread that just wants to get back to
 * running the method it's calling.
 */
void
mini_tiered_count_reached (gpointer counter)
{
	MiniTieredCounter *ctr = (MiniTieredCounter *) counter;

	/* Reached from generated tier-0 code, so the feature is on by construction;
	 * the guard is defensive. */
	if (!mono_llvm_tiered_enabled () || !ctr)
		return;

	/*
	 * Idempotence guard, not a fast path: the prologue's lock-xadd already
	 * dispatches on an exact count match, so under normal operation this runs
	 * at most once per counter and the CAS always wins. It exists purely as a
	 * second line of defence against ever enqueuing the same counter twice.
	 */
	if (mono_atomic_cas_i32 (&ctr->settled, 1, 0) != 0)
		return;

	tiered_enqueue (ctr->method, ctr->domain, ctr->opt, ctr);
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

/*
 * MONO_TIERED_CALL_THRESHOLD == 0 only: promote METHOD to tier 1 right here,
 * synchronously, on the thread that just published its tier-0 body, and
 * return the resulting tier-1 code pointer so the caller can start running it
 * immediately. There is no counter block at threshold 0 (mini_tiered_alloc_counter ()
 * never allocates one), hence no redirect sled to arm - handing the tier-1
 * pointer straight back is the only way this method's own trigger call site
 * ever ends up on tier 1.
 *
 * Returns NULL - "stay on the tier-0 body you already have" - when the
 * feature is off, METHOD already has a record (should not normally happen;
 * mini.c only calls this once per method, right after its first tier-0
 * publish), the compile declined or failed, or promotion hasn't opened for
 * business yet (tiered_ready, still FALSE this early only during mini_init ()
 * itself). The recursion guard below is also a NULL case: this thread is
 * already inside another synchronous promotion, most likely because the LLVM
 * compile in progress needed some other method compiled on the spot and that
 * compile's own tier-0 publish landed right back here. Promoting it too would
 * stack a second LLVM compile on top of the first with no bound but the
 * shape of the call graph, so it just stays at tier 0 instead - see
 * tiered_sync_active's comment.
 */
gpointer
mono_llvm_tiered_promote_sync (MonoMethod *method, MonoDomain *domain, guint32 opt)
{
	TieredRecord *rec;
	gpointer code = NULL;

	if (!mono_llvm_tiered_enabled () || !method)
		return NULL;

	if (tiered_sync_active)
		return NULL;

	/* See the comment on tiered_ready: LLVM codegen this early would reach
	 * domain state mini_init () hasn't finished building yet. */
	if (!tiered_ready)
		return NULL;

	/*
	 * The decline decision is tier-independent - it is about which methods can
	 * ever hold a tier-1 body, not about which thread compiles it. A non-root
	 * domain's per-domain trampolines still dangle in the process-global
	 * direct-call symbol cache after that domain unloads, and a Reflection.Emit
	 * body is still unstable metadata, no matter that this synchronous path runs
	 * on the mutator that owns it. So the same gate the background enqueue uses
	 * (tiered_method_should_decline ()) applies here too; a declined method just
	 * stays on the tier-0 body it already has. Running it on this thread, which
	 * owns METHOD's metadata, keeps every read stable.
	 */
	if (tiered_method_should_decline (method, domain))
		return NULL;

	mono_os_mutex_lock (&tiered_mutex);
	if (g_hash_table_lookup (tiered_state, method)) {
		mono_os_mutex_unlock (&tiered_mutex);
		return NULL;
	}
	rec = g_new0 (TieredRecord, 1);
	rec->domain = domain;
	rec->state = TIER_STATE_TIER0;
	g_hash_table_insert (tiered_state, method, rec);
	mono_os_mutex_unlock (&tiered_mutex);

	/*
	 * Unlike the background worker, this thread does not need a
	 * push-appdomain-ref dance to make DOMAIN safe to compile into: it got
	 * here by compiling a tier-0 body for METHOD in DOMAIN a few lines up
	 * mini.c's call stack, on this same thread, so DOMAIN is provably alive
	 * for as long as this call runs.
	 */

	tiered_sync_active = TRUE;
	/*
	 * run_cctors = FALSE: this runs on a mutator thread, not the background
	 * worker, but tier 1 must still see the same class-init state tier 0 did -
	 * see mini_tiered_promote ()'s doc comment - so codegen is identical no
	 * matter which path promoted the method. It also means this compile can't
	 * run a cctor that turns around and asks for another synchronous
	 * promotion, which is one less thing tiered_sync_active has to guard
	 * against.
	 */
	if (mini_tiered_promote (method, domain, opt, FALSE)) {
		MonoJitInfo *ji = mini_lookup_method (domain, method, NULL);

		tiered_set_state (method, domain, TIER_STATE_PROMOTED);
		if (ji)
			code = ji->code_start;
	} else {
		tiered_set_state (method, domain, TIER_STATE_TIER0_TERMINAL);
	}
	tiered_sync_active = FALSE;

	return code;
}
