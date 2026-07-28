/**
 * \file
 * \brief Locates the collector's page-dirty bitmap, from inside the collector's
 * own configuration.
 *
 * This is mono's code, but it is compiled into the collector's static library
 * and therefore against the collector's config.h rather than mono's - which is
 * the entire reason the file exists. `struct _GC_arrays' gains and loses
 * members with the options the collector was built with, so a translation unit
 * that does not share those options computes a different offset for the bitmap.
 * Nothing fails to build when that happens: the JIT simply bakes an address a
 * few words off, marks bits the collector never reads, and loses references
 * stored from compiled code. Ask from in here instead of from mono's side.
 */

#include "private/gc_priv.h"

#include "boehm-dirty-table.h"

void *
mono_boehm_dirty_page_table (unsigned *shift_bits, size_t *index_mask)
{
#if defined(MANUAL_VDB) && !defined(GC_DISABLE_INCREMENTAL)
	/*
	 * The caller numbers bits within pointer-sized words; the collector's own
	 * get_pht_entry_from_index () does it in `word'. Both follow the ABI's long,
	 * so they agree everywhere we run - but if that ever stops being true the
	 * numbering diverges silently, which is worth a build failure.
	 */
	GC_STATIC_ASSERT (sizeof (word) == sizeof (void *));

	*shift_bits = (unsigned) LOG_HBLKSIZE;
	*index_mask = (size_t) (PHT_ENTRIES - 1);

	return (/* no volatile */ void *) GC_dirty_pages;
#else
	/* Either a fault handler maintains the bits, or nothing does. */
	*shift_bits = 0;
	*index_mask = 0;

	return NULL;
#endif
}

void
mono_boehm_dirty_page (const void *p)
{
	GC_dirty (p);
}
