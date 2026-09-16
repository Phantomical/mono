/**
 * \file
 * Lock ordering checks.
 */

#ifndef __MONO_UTILS_MONO_LOCK_RANK_H__
#define __MONO_UTILS_MONO_LOCK_RANK_H__

#include <config.h>
#include <glib.h>

G_BEGIN_DECLS

/*
 * The runtime's lock order, outermost first: a thread may take a lower rank
 * while holding a higher one and never the reverse.
 *
 * A rank no acquisition site passes never fires, so a lock can be numbered here
 * before every site that takes it has been checked.
 */
typedef enum {
	/* Every entry point below returns without doing anything for this one. */
	MONO_LOCK_RANK_NONE = 0,
	MONO_LOCK_RANK_DOMAIN_UNLOAD,
	MONO_LOCK_RANK_LOADER,
	MONO_LOCK_RANK_DOMAIN,
	MONO_LOCK_RANK_DOMAIN_METHOD_TABLE,
	MONO_LOCK_RANK_DOMAIN_METHOD,
	MONO_LOCK_RANK_ENGINE,
	MONO_LOCK_RANK_COUNT
} MonoLockRank;

/*
 * A TLS key is not available: the loader lock is taken during init, and on
 * threads that never attach. A host without the keyword gets no checks.
 */
#if defined (MONO_ENABLE_LOCK_RANK_CHECKS) && defined (MONO_KEYWORD_THREAD)

/*
 * The ranks the runtime initializes recursive. Re-entering any other rank is a
 * deadlock against this thread itself rather than an ordering mistake.
 */
#define MONO_LOCK_RANKS_RECURSIVE \
	((1u << MONO_LOCK_RANK_DOMAIN_UNLOAD) \
	 | (1u << MONO_LOCK_RANK_LOADER) \
	 | (1u << MONO_LOCK_RANK_DOMAIN))

typedef struct {
	/* Bit r is set while rank r is held. */
	guint32 held;
	guint16 depth [MONO_LOCK_RANK_COUNT];
} MonoLockRankState;

G_EXTERN_C_VAR MONO_KEYWORD_THREAD MonoLockRankState mono_lock_rank_state;

void mono_lock_rank_self_deadlock (MonoLockRank rank, guint32 held);
void mono_lock_rank_inversion (MonoLockRank rank, guint32 held);
void mono_lock_rank_unsafe_operation (const char *operation, MonoLockRank rank, guint32 held);

/**
 * Aborts unless \p rank may be taken with what this thread already holds.
 *
 * Call it before the acquisition rather than after: a thread that has already
 * blocked on the lock reports nothing.
 */
static inline void
mono_lock_rank_acquiring (MonoLockRank rank)
{
	guint32 held, mine;

	if (rank == MONO_LOCK_RANK_NONE)
		return;

	held = mono_lock_rank_state.held;
	mine = 1u << rank;

	/*
	 * Taking a rank this thread already holds cannot block, so the order does
	 * not apply to it. mono_class_init_internal () re-takes the loader lock
	 * with the domain lock held on an ordinary class load.
	 */
	if (G_UNLIKELY ((held & mine) != 0)) {
		if (G_UNLIKELY ((mine & ~MONO_LOCK_RANKS_RECURSIVE) != 0))
			mono_lock_rank_self_deadlock (rank, held);
		return;
	}

	if (G_UNLIKELY ((held >> (rank + 1)) != 0))
		mono_lock_rank_inversion (rank, held);
}

static inline void
mono_lock_rank_acquired (MonoLockRank rank)
{
	if (rank == MONO_LOCK_RANK_NONE)
		return;

	mono_lock_rank_state.held |= 1u << rank;
	mono_lock_rank_state.depth [rank]++;
}

static inline void
mono_lock_rank_released (MonoLockRank rank)
{
	if (rank == MONO_LOCK_RANK_NONE)
		return;

	if (--mono_lock_rank_state.depth [rank] == 0)
		mono_lock_rank_state.held &= ~(1u << rank);
}

/** Whether this thread holds \p rank. */
static inline gboolean
mono_lock_rank_is_held (MonoLockRank rank)
{
	return rank != MONO_LOCK_RANK_NONE
	       && (mono_lock_rank_state.held & (1u << rank)) != 0;
}

/**
 * Aborts if this thread holds anything that outranks \p rank.
 *
 * For an operation rather than a lock. One that reaches \p rank on only some
 * runs still fires here every time.
 */
#define MONO_ASSERT_NO_LOCK_ABOVE(rank) \
	do { \
		guint32 held_ = mono_lock_rank_state.held; \
		if (G_UNLIKELY ((held_ & (1u << (rank))) == 0 \
		                && (held_ >> ((rank) + 1)) != 0)) \
			mono_lock_rank_unsafe_operation (__func__, (rank), held_); \
	} while (0)

#else

static inline void mono_lock_rank_acquiring (MonoLockRank rank) { (void) rank; }
static inline void mono_lock_rank_acquired (MonoLockRank rank) { (void) rank; }
static inline void mono_lock_rank_released (MonoLockRank rank) { (void) rank; }

static inline gboolean
mono_lock_rank_is_held (MonoLockRank rank)
{
	(void) rank;
	return FALSE;
}

#define MONO_ASSERT_NO_LOCK_ABOVE(rank) do { } while (0)

#endif

G_END_DECLS

#ifdef __cplusplus

/// A mutex that carries its rank, so that no acquisition site has to state it.
template <typename Mutex, MonoLockRank Rank>
class MonoRankedMutex {
public:
	void lock ()
	{
		mono_lock_rank_acquiring (Rank);
		mutex_.lock ();
		mono_lock_rank_acquired (Rank);
	}

	void unlock ()
	{
		mono_lock_rank_released (Rank);
		mutex_.unlock ();
	}

	void lock_shared ()
	{
		mono_lock_rank_acquiring (Rank);
		mutex_.lock_shared ();
		mono_lock_rank_acquired (Rank);
	}

	void unlock_shared ()
	{
		mono_lock_rank_released (Rank);
		mutex_.unlock_shared ();
	}

private:
	Mutex mutex_;
};

#endif /* __cplusplus */

#endif /* __MONO_UTILS_MONO_LOCK_RANK_H__ */
