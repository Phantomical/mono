/**
 * \file
 * Lock ordering checks.
 */

#include <config.h>
#include <glib.h>

#include <mono/utils/mono-lock-rank.h>

#if defined (MONO_ENABLE_LOCK_RANK_CHECKS) && defined (MONO_KEYWORD_THREAD)

MONO_KEYWORD_THREAD MonoLockRankState mono_lock_rank_state;

static const char *const rank_names [MONO_LOCK_RANK_COUNT] = {
	"unranked",
	"domain-unload",
	"loader",
	"domain",
	"domain-method-table",
	"domain-method",
	"engine",
	"leaf",
};

/* Outermost first, which is the order they were taken in. */
static char *
describe_held (guint32 held)
{
	GString *out = g_string_new ("");
	int rank;

	for (rank = 1; rank < MONO_LOCK_RANK_COUNT; rank++)
		if (held & (1u << rank))
			g_string_append_printf (out, "%s%s", out->len != 0 ? ", " : "",
			                        rank_names [rank]);

	if (out->len == 0)
		g_string_append (out, "nothing");

	return g_string_free (out, FALSE);
}

void
mono_lock_rank_self_deadlock (MonoLockRank rank, guint32 held)
{
	g_error ("lock rank: re-entering the %s lock, which is not recursive "
	         "(this thread holds: %s)", rank_names [rank], describe_held (held));
}

void
mono_lock_rank_inversion (MonoLockRank rank, guint32 held)
{
	g_error ("lock rank: taking the %s lock while holding %s, which outranks it",
	         rank_names [rank], describe_held (held));
}

void
mono_lock_rank_unsafe_operation (const char *operation, MonoLockRank rank, guint32 held)
{
	char *holding = describe_held (held);

	g_error ("lock rank: %s () can take the %s lock, and this thread holds: %s",
	         operation, rank_names [rank], holding);
}

#endif
