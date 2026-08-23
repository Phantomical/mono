/**
 * \file
 * The array constructors a multi-dimensional newobj reaches.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

MonoArray *
mono_array_new_n_icall (MonoMethod *cm, gint32 pcount, intptr_t *params)
{
	ERROR_DECL (error);
	g_assert (cm);
	g_assert (pcount);
	g_assert (params);
	intptr_t *lower_bounds = NULL;

	const int pcount_sig = mono_method_signature_internal (cm)->param_count;
	const int rank = m_class_get_rank (cm->klass);
	g_assert (pcount == pcount_sig);
	g_assert (rank == pcount || rank * 2 == pcount);

	uintptr_t *lengths = (uintptr_t*)params;

	if (rank == pcount) {
		/* Only lengths provided. */
		if (m_class_get_byval_arg (cm->klass)->type == MONO_TYPE_ARRAY) {
			lower_bounds = g_newa (intptr_t, rank);
			memset (lower_bounds, 0, sizeof (intptr_t) * rank);
		}
	} else {
		g_assert (pcount == (rank * 2));
		/* lower bounds are first. */
		lower_bounds = params;
		lengths += rank;
	}

	MonoArray *arr = mono_array_new_full_checked (mono_domain_get (),
		cm->klass, lengths, lower_bounds, error);

	return mono_error_set_pending_exception (error) ? NULL : arr;
}

static MonoArray *
mono_array_new_n (MonoMethod *cm, int n, uintptr_t lengths [], intptr_t lower_bounds [])
{
	ERROR_DECL (error);
	intptr_t *plower_bounds = NULL;
	const int pcount = mono_method_signature_internal (cm)->param_count;
	const int rank = m_class_get_rank (cm->klass);

	g_assert (rank == pcount);
	g_assert (rank == n);

	if (m_class_get_byval_arg (cm->klass)->type == MONO_TYPE_ARRAY)
		plower_bounds = lower_bounds;

	MonoArray *arr = mono_array_new_full_checked (mono_domain_get (),
		cm->klass, lengths, plower_bounds, error);

	return mono_error_set_pending_exception (error) ? NULL : arr;
}

/* Specialized version of mono_array_new_va () which avoids varargs */
MonoArray *
mono_array_new_1 (MonoMethod *cm, guint32 length)
{
	uintptr_t lengths [ ] = {length};
	intptr_t lower_bounds [G_N_ELEMENTS (lengths)] = {0};
	return mono_array_new_n (cm, G_N_ELEMENTS (lengths), lengths, lower_bounds);
}

MonoArray *
mono_array_new_2 (MonoMethod *cm, guint32 length1, guint32 length2)
{
	uintptr_t lengths [ ] = {length1, length2};
	intptr_t lower_bounds [G_N_ELEMENTS (lengths)] = {0};
	return mono_array_new_n (cm, G_N_ELEMENTS (lengths), lengths, lower_bounds);
}

MonoArray *
mono_array_new_3 (MonoMethod *cm, guint32 length1, guint32 length2, guint32 length3)
{
	uintptr_t lengths [ ] = {length1, length2, length3};
	intptr_t lower_bounds [G_N_ELEMENTS (lengths)] = {0};
	return mono_array_new_n (cm, G_N_ELEMENTS (lengths), lengths, lower_bounds);
}

MonoArray *
mono_array_new_4 (MonoMethod *cm, guint32 length1, guint32 length2, guint32 length3, guint32 length4)
{
	uintptr_t lengths [ ] = {length1, length2, length3, length4};
	intptr_t lower_bounds [G_N_ELEMENTS (lengths)] = {0};
	return mono_array_new_n (cm, G_N_ELEMENTS (lengths), lengths, lower_bounds);
}
