/**
 * \file
 * ipnsort, ported from https://github.com/Voultapher/sort-research-rs
 * (the implementation behind Rust's slice::sort_unstable), specialised to
 * sorting pointers by address.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include <stdint.h>
#include <string.h>
#include <glib.h>

#include "sgen/sgen-ipnsort.h"

#define INSERTION_SORT_THRESHOLD 20
#define SMALL_SORT_THRESHOLD 32
#define PSEUDO_MEDIAN_REC_THRESHOLD 64

typedef uintptr_t elem_t;

static void sort9_optimal (elem_t *v);
static void sort13_optimal (elem_t *v);

static inline gboolean
is_less (elem_t a, elem_t b)
{
	return a < b;
}

static inline void
swap_if_less (elem_t *v, size_t a, size_t b)
{
	elem_t x = v [a];
	elem_t y = v [b];
	gboolean lt = is_less (y, x);
	v [a] = lt ? y : x;
	v [b] = lt ? x : y;
}

/* Requires v [0 .. offset) to be sorted already. */
static void
insertion_sort_shift_left (elem_t *v, size_t len, size_t offset)
{
	for (size_t i = offset; i < len; ++i) {
		elem_t tmp = v [i];
		size_t j = i;
		while (j > 0 && is_less (tmp, v [j - 1])) {
			v [j] = v [j - 1];
			--j;
		}
		v [j] = tmp;
	}
}

/*
 * Merges the sorted halves src [0 .. len / 2) and src [len / 2 .. len) into
 * dst, from both ends at once.
 */
static void
bidirectional_merge (const elem_t *src, size_t len, elem_t *dst)
{
	size_t half = len / 2;
	ptrdiff_t left = 0;
	ptrdiff_t right = half;
	ptrdiff_t out = 0;
	ptrdiff_t left_rev = half - 1;
	ptrdiff_t right_rev = len - 1;
	ptrdiff_t out_rev = len - 1;

	for (size_t i = 0; i < half; ++i) {
		gboolean take_left = !is_less (src [right], src [left]);
		dst [out++] = take_left ? src [left] : src [right];
		left += take_left;
		right += !take_left;

		gboolean take_right = !is_less (src [right_rev], src [left_rev]);
		dst [out_rev--] = take_right ? src [right_rev] : src [left_rev];
		right_rev -= take_right;
		left_rev -= !take_right;
	}

	if (len % 2 != 0) {
		gboolean left_nonempty = left <= left_rev;
		dst [out] = left_nonempty ? src [left] : src [right];
	}
}

static void
small_sort_network (elem_t *v, size_t len)
{
	elem_t scratch [SMALL_SORT_THRESHOLD];

	g_assert (len <= SMALL_SORT_THRESHOLD);
	if (len < 2)
		return;

	size_t half = len / 2;
	gboolean no_merge = len < 18;
	size_t regions [2][2] = { { 0, no_merge ? len : half }, { half, len - half } };

	for (int r = 0; r < (no_merge ? 1 : 2); ++r) {
		elem_t *region = v + regions [r][0];
		size_t region_len = regions [r][1];
		size_t presorted;

		if (region_len >= 13) {
			sort13_optimal (region);
			presorted = 13;
		} else if (region_len >= 9) {
			sort9_optimal (region);
			presorted = 9;
		} else {
			presorted = 1;
		}
		insertion_sort_shift_left (region, region_len, presorted);
	}

	if (no_merge)
		return;

	memcpy (scratch, v, len * sizeof (elem_t));
	bidirectional_merge (scratch, len, v);
}

static void
sift_down (elem_t *v, size_t len, size_t node)
{
	for (;;) {
		size_t child = 2 * node + 1;
		if (child >= len)
			break;
		if (child + 1 < len)
			child += is_less (v [child], v [child + 1]);
		if (!is_less (v [node], v [child]))
			break;
		elem_t tmp = v [node];
		v [node] = v [child];
		v [child] = tmp;
		node = child;
	}
}

static void
heapsort (elem_t *v, size_t len)
{
	for (size_t i = len + len / 2; i-- > 0;) {
		size_t sift_idx;
		if (i >= len) {
			sift_idx = i - len;
		} else {
			elem_t tmp = v [0];
			v [0] = v [i];
			v [i] = tmp;
			sift_idx = 0;
		}
		sift_down (v, MIN (i, len), sift_idx);
	}
}

static inline size_t
median3 (const elem_t *v, size_t a, size_t b, size_t c)
{
	gboolean x = is_less (v [a], v [b]);
	gboolean y = is_less (v [a], v [c]);
	if (x == y) {
		gboolean z = is_less (v [b], v [c]);
		return (z ^ x) ? c : b;
	}
	return a;
}

static size_t
median3_rec (const elem_t *v, size_t a, size_t b, size_t c, size_t n)
{
	if (n * 8 >= PSEUDO_MEDIAN_REC_THRESHOLD) {
		size_t n8 = n / 8;
		a = median3_rec (v, a, a + n8 * 4, a + n8 * 7, n8);
		b = median3_rec (v, b, b + n8 * 4, b + n8 * 7, n8);
		c = median3_rec (v, c, c + n8 * 4, c + n8 * 7, n8);
	}
	return median3 (v, a, b, c);
}

static size_t
choose_pivot (const elem_t *v, size_t len)
{
	size_t len_div_8 = len / 8;
	size_t a = 0;
	size_t b = len_div_8 * 4;
	size_t c = len_div_8 * 7;

	if (len < PSEUDO_MEDIAN_REC_THRESHOLD)
		return median3 (v, a, b, c);
	return median3_rec (v, a, b, c, len_div_8);
}

/*
 * Branchless cyclic Lomuto: v [0] is lifted out to open a gap, and each step
 * fills the gap from the left boundary and moves the gap to the element just
 * read. The lifted element is processed last.
 */
#define DEF_PARTITION(name, lt) \
static size_t \
name (elem_t *v, size_t len, elem_t pivot) \
{ \
	if (len == 0) \
		return 0; \
	elem_t lifted = v [0]; \
	size_t gap = 0; \
	size_t num_lt = 0; \
	for (size_t right = 1; right < len; ++right) { \
		elem_t r = v [right]; \
		gboolean r_lt = lt (r, pivot); \
		v [gap] = v [num_lt]; \
		v [num_lt] = r; \
		gap = right; \
		num_lt += r_lt; \
	} \
	gboolean last_lt = lt (lifted, pivot); \
	v [gap] = v [num_lt]; \
	v [num_lt] = lifted; \
	num_lt += last_lt; \
	return num_lt; \
}

#define IS_LESS_EQ(a, b) (!is_less ((b), (a)))

DEF_PARTITION (partition_lomuto_lt, is_less)
DEF_PARTITION (partition_lomuto_le, IS_LESS_EQ)

/*
 * Moves v [pivot_pos] to its place and returns that index. With \p le, the
 * elements before it are those <= the pivot, otherwise those < the pivot.
 */
static size_t
partition (elem_t *v, size_t len, size_t pivot_pos, gboolean le)
{
	elem_t tmp = v [0];
	v [0] = v [pivot_pos];
	v [pivot_pos] = tmp;

	elem_t pivot = v [0];
	size_t num_lt = le
		? partition_lomuto_le (v + 1, len - 1, pivot)
		: partition_lomuto_lt (v + 1, len - 1, pivot);

	v [0] = v [num_lt];
	v [num_lt] = pivot;
	return num_lt;
}

/*
 * \p ancestor_pivot, when non-NULL, is a value every element of v is known
 * to be >= to. A pivot equal to it means v holds a run of duplicates, which
 * one <= partition moves out of the way.
 */
static void
quicksort (elem_t *v, size_t len, const elem_t *ancestor_pivot, unsigned limit)
{
	for (;;) {
		if (len <= SMALL_SORT_THRESHOLD) {
			small_sort_network (v, len);
			return;
		}
		if (limit == 0) {
			heapsort (v, len);
			return;
		}
		--limit;

		size_t pivot_pos = choose_pivot (v, len);

		if (ancestor_pivot && !is_less (*ancestor_pivot, v [pivot_pos])) {
			size_t num_le = partition (v, len, pivot_pos, TRUE);
			v += num_le + 1;
			len -= num_le + 1;
			ancestor_pivot = NULL;
			continue;
		}

		size_t num_lt = partition (v, len, pivot_pos, FALSE);
		quicksort (v, num_lt, ancestor_pivot, limit);
		ancestor_pivot = &v [num_lt];
		v += num_lt + 1;
		len -= num_lt + 1;
	}
}

static size_t
find_existing_run (const elem_t *v, size_t len, gboolean *was_reversed)
{
	*was_reversed = FALSE;
	if (len < 2)
		return len;

	size_t run_len = 2;
	gboolean strictly_descending = is_less (v [1], v [0]);
	if (strictly_descending) {
		while (run_len < len && is_less (v [run_len], v [run_len - 1]))
			++run_len;
	} else {
		while (run_len < len && !is_less (v [run_len], v [run_len - 1]))
			++run_len;
	}
	*was_reversed = strictly_descending;
	return run_len;
}

static inline unsigned
ilog2 (guint64 x)
{
	g_assert (x != 0);
#if defined(__GNUC__) || defined(__clang__)
	return 63 - __builtin_clzll (x);
#elif defined(_MSC_VER)
	unsigned long r;
	_BitScanReverse64 (&r, x);
	return r;
#else
	unsigned r = 0;
	while (x >>= 1)
		++r;
	return r;
#endif
}

void
sgen_ipnsort (void **array, size_t size)
{
	elem_t *v = (elem_t *) array;

	if (size < 2)
		return;

	if (size <= INSERTION_SORT_THRESHOLD) {
		insertion_sort_shift_left (v, size, 1);
		return;
	}

	gboolean was_reversed;
	size_t run_len = find_existing_run (v, size, &was_reversed);
	if (run_len == size) {
		if (was_reversed) {
			for (size_t i = 0, j = size - 1; i < j; ++i, --j) {
				elem_t tmp = v [i];
				v [i] = v [j];
				v [j] = tmp;
			}
		}
		return;
	}

	quicksort (v, size, NULL, 2 * ilog2 (size | 1));
}

static void
sort9_optimal (elem_t *v)
{
	swap_if_less (v, 0, 3);
	swap_if_less (v, 1, 7);
	swap_if_less (v, 2, 5);
	swap_if_less (v, 4, 8);
	swap_if_less (v, 0, 7);
	swap_if_less (v, 2, 4);
	swap_if_less (v, 3, 8);
	swap_if_less (v, 5, 6);
	swap_if_less (v, 0, 2);
	swap_if_less (v, 1, 3);
	swap_if_less (v, 4, 5);
	swap_if_less (v, 7, 8);
	swap_if_less (v, 1, 4);
	swap_if_less (v, 3, 6);
	swap_if_less (v, 5, 7);
	swap_if_less (v, 0, 1);
	swap_if_less (v, 2, 4);
	swap_if_less (v, 3, 5);
	swap_if_less (v, 6, 8);
	swap_if_less (v, 2, 3);
	swap_if_less (v, 4, 5);
	swap_if_less (v, 6, 7);
	swap_if_less (v, 1, 2);
	swap_if_less (v, 3, 4);
	swap_if_less (v, 5, 6);
}

static void
sort13_optimal (elem_t *v)
{
	swap_if_less (v, 0, 12);
	swap_if_less (v, 1, 10);
	swap_if_less (v, 2, 9);
	swap_if_less (v, 3, 7);
	swap_if_less (v, 5, 11);
	swap_if_less (v, 6, 8);
	swap_if_less (v, 1, 6);
	swap_if_less (v, 2, 3);
	swap_if_less (v, 4, 11);
	swap_if_less (v, 7, 9);
	swap_if_less (v, 8, 10);
	swap_if_less (v, 0, 4);
	swap_if_less (v, 1, 2);
	swap_if_less (v, 3, 6);
	swap_if_less (v, 7, 8);
	swap_if_less (v, 9, 10);
	swap_if_less (v, 11, 12);
	swap_if_less (v, 4, 6);
	swap_if_less (v, 5, 9);
	swap_if_less (v, 8, 11);
	swap_if_less (v, 10, 12);
	swap_if_less (v, 0, 5);
	swap_if_less (v, 3, 8);
	swap_if_less (v, 4, 7);
	swap_if_less (v, 6, 11);
	swap_if_less (v, 9, 10);
	swap_if_less (v, 0, 1);
	swap_if_less (v, 2, 5);
	swap_if_less (v, 6, 9);
	swap_if_less (v, 7, 8);
	swap_if_less (v, 10, 11);
	swap_if_less (v, 1, 3);
	swap_if_less (v, 2, 4);
	swap_if_less (v, 5, 6);
	swap_if_less (v, 9, 10);
	swap_if_less (v, 1, 2);
	swap_if_less (v, 3, 4);
	swap_if_less (v, 5, 7);
	swap_if_less (v, 6, 8);
	swap_if_less (v, 2, 3);
	swap_if_less (v, 4, 5);
	swap_if_less (v, 6, 7);
	swap_if_less (v, 8, 9);
	swap_if_less (v, 3, 4);
	swap_if_less (v, 5, 6);
}
