/**
 * \file
 * \brief Where the collector keeps its page-dirty bitmap.
 */

#ifndef __MONO_BOEHM_DIRTY_TABLE_H__
#define __MONO_BOEHM_DIRTY_TABLE_H__

#include <stddef.h>

/*
 * Report the bitmap GC_dirty () marks, and the hash it uses to pick a bit, for
 * a caller that would rather inline the mark than call in for it. The bit for
 * an address is (addr >> *shift_bits) & *index_mask, numbered within
 * pointer-sized words of the returned table - the same hash the collector
 * applies. Setting a bit that did not need setting only costs a rescan, so the
 * aliasing the mask implies is expected and safe.
 *
 * NULL, with both outputs zeroed, when the mutator does not maintain the bits
 * and there is therefore nothing to inline against.
 */
void *mono_boehm_dirty_page_table (unsigned *shift_bits, size_t *index_mask);

/*
 * Mark the page containing p dirty, exactly as the collector's own barrier
 * would. This is GC_dirty (), which is a macro over configuration this header's
 * includers do not share - so it is reachable as a call, for a caller wanting
 * to check its own inlined mark against the real thing.
 */
void mono_boehm_dirty_page (const void *p);

#endif /* __MONO_BOEHM_DIRTY_TABLE_H__ */
