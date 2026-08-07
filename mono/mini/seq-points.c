/**
 * \file
 * Sequence Points functions
 *
 * Authors:
 *   Marcos Henrich (marcos.henrich@xamarin.com)
 *
 * Copyright 2014 Xamarin, Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "mini.h"
#include "mini-runtime.h"
#include "seq-points.h"

/*
 * mono_get_seq_points:
 *
 *   Return the sequence points registered for METHOD in DOMAIN.
 *
 * The registration is per method and first writer wins, so this answers for
 * whichever of the method's bodies was compiled first. Anything holding the
 * body it is asking about wants mono_get_seq_points_by_ji () instead.
 *
 * LOCKING: Acquires the domain lock
 */
MonoSeqPointInfo*
mono_get_seq_points (MonoDomain *domain, MonoMethod *method)
{
	ERROR_DECL (error);
	MonoSeqPointInfo *seq_points;
	MonoMethod *declaring_generic_method = NULL, *shared_method = NULL;

	if (method->is_inflated) {
		declaring_generic_method = mono_method_get_declaring_generic_method (method);
		shared_method = mini_get_shared_method_full (method, SHARE_MODE_NONE, error);
		mono_error_assert_ok (error);
	}

	mono_domain_lock (domain);
	seq_points = (MonoSeqPointInfo *)g_hash_table_lookup (domain_jit_info (domain)->seq_points, method);
	if (!seq_points && method->is_inflated) {
		/* generic sharing + aot */
		seq_points = (MonoSeqPointInfo *)g_hash_table_lookup (domain_jit_info (domain)->seq_points, declaring_generic_method);
		if (!seq_points)
			seq_points = (MonoSeqPointInfo *)g_hash_table_lookup (domain_jit_info (domain)->seq_points, shared_method);
	}
	mono_domain_unlock (domain);

	return seq_points;
}

/*
 * mono_get_seq_points_by_ji:
 *
 *   Return the sequence points describing the body JI, or NULL if there are none.
 *
 * The table hanging off the body is what it was published with, so it stays
 * right however many bodies the method ends up with. The per-method table can
 * only ever describe one of them, so it is asked only for a body that carries
 * none of its own - which is every body some other producer than the JIT
 * registered.
 *
 * LOCKING: may acquire the domain lock, so not for an async context.
 */
MonoSeqPointInfo*
mono_get_seq_points_by_ji (MonoDomain *domain, MonoJitInfo *ji)
{
	if (!ji)
		return NULL;

	if (ji->seq_points)
		return (MonoSeqPointInfo *) ji->seq_points;

	/* Neither of these names a method whose IL there could be a table for. */
	if (ji->is_trampoline || ji->async)
		return NULL;

	return mono_get_seq_points (domain, jinfo_get_method (ji));
}

/*
 * mono_find_next_seq_point_for_native_offset:
 *
 *   Find the first sequence point after NATIVE_OFFSET.
 */
gboolean
mono_find_next_seq_point_for_native_offset (MonoDomain *domain, MonoJitInfo *ji, gint32 native_offset, MonoSeqPointInfo **info, SeqPoint* seq_point)
{
	MonoSeqPointInfo *seq_points;

	seq_points = mono_get_seq_points_by_ji (domain, ji);
	if (!seq_points) {
		if (info)
			*info = NULL;
		return FALSE;
	}
	if (info)
		*info = seq_points;

	return mono_seq_point_find_next_by_native_offset (seq_points, native_offset, seq_point);
}

/*
 * mono_find_prev_seq_point_for_native_offset:
 *
 *   Find the first sequence point before NATIVE_OFFSET.
 */
gboolean
mono_find_prev_seq_point_for_native_offset (MonoDomain *domain, MonoJitInfo *ji, gint32 native_offset, MonoSeqPointInfo **info, SeqPoint* seq_point)
{
	MonoSeqPointInfo *seq_points;

	seq_points = mono_get_seq_points_by_ji (domain, ji);
	if (!seq_points) {
		if (info)
			*info = NULL;
		return FALSE;
	}
	if (info)
		*info = seq_points;

	return mono_seq_point_find_prev_by_native_offset (seq_points, native_offset, seq_point);
}

/*
 * mono_find_seq_point:
 *
 *   Find the sequence point corresponding to the IL offset IL_OFFSET, which
 * should be the location of a sequence point.
 */
gboolean
mono_find_seq_point (MonoDomain *domain, MonoJitInfo *ji, gint32 il_offset, MonoSeqPointInfo **info, SeqPoint *seq_point)
{
	MonoSeqPointInfo *seq_points;

	seq_points = mono_get_seq_points_by_ji (domain, ji);
	if (!seq_points) {
		if (info)
			*info = NULL;
		return FALSE;
	}
	if (info)
		*info = seq_points;

	return mono_seq_point_find_by_il_offset (seq_points, il_offset, seq_point);
}

/*
 * mono_find_next_seq_point_for_il_offset:
 *
 *   Find the first sequence point at or after the IL offset IL_OFFSET, which
 * need not itself be the location of one.
 */
gboolean
mono_find_next_seq_point_for_il_offset (MonoDomain *domain, MonoMethod *method, gint32 il_offset, MonoSeqPointInfo **info, SeqPoint *seq_point)
{
	MonoSeqPointInfo *seq_points;

	seq_points = mono_get_seq_points (domain, method);
	if (!seq_points) {
		if (info)
			*info = NULL;
		return FALSE;
	}
	if (info)
		*info = seq_points;

	return mono_seq_point_find_next_by_il_offset (seq_points, il_offset, seq_point);
}
