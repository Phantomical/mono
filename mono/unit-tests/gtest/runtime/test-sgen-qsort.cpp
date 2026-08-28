/*
 * test-sgen-qsort.cpp: Unit test for quicksort.
 *
 * Copyright (C) 2013 Xamarin Inc
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "config.h"

#define  HAVE_SGEN_GC

#include <mono/sgen/sgen-gc.h>
#include <mono/sgen/sgen-qsort.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vector>

#include <gtest/gtest.h>

namespace {

int
compare_ints (const void *pa, const void *pb)
{
	int a = *(const int*)pa;
	int b = *(const int*)pb;
	if (a < b)
		return -1;
	if (a == b)
		return 0;
	return 1;
}

typedef struct {
	int key;
	int val;
} teststruct_t;

int
compare_teststructs (const void *pa, const void *pb)
{
	int a = ((const teststruct_t*)pa)->key;
	int b = ((const teststruct_t*)pb)->key;
	if (a < b)
		return -1;
	if (a == b)
		return 0;
	return 1;
}

int
compare_teststructs2 (const void *pa, const void *pb)
{
	int a = (*((const teststruct_t**)pa))->key;
	int b = (*((const teststruct_t**)pb))->key;
	if (a < b)
		return -1;
	if (a == b)
		return 0;
	return 1;
}

DEF_QSORT_INLINE(test_struct, teststruct_t*, compare_teststructs)

/*
 * We can't assert that qsort and sgen_qsort agree element for element, because
 * qsort is not guaranteed to be stable and the two will tend to differ among
 * adjacent equal elements.  What is checked instead is that the result is
 * ordered by the comparator.
 */
void
check_sorted (const void *base, size_t nel, size_t width, int (*compar) (const void*, const void*))
{
	for (size_t i = 0; i < nel - 1; ++i)
		ASSERT_LE (compar ((const char *)base + i * width, (const char *)base + (i + 1) * width), 0)
			<< "element " << i << " of " << nel << " is out of order";
}

void
compare_sorts (const void *base, size_t nel, size_t width, int (*compar) (const void*, const void*))
{
	std::vector<char> b1 (nel * width), b2 (nel * width);

	memcpy (b1.data (), base, b1.size ());
	memcpy (b2.data (), base, b2.size ());

	mono_qsort (b1.data (), nel, width, compar);
	sgen_qsort (b2.data (), nel, width, compar);

	check_sorted (b2.data (), nel, width, compar);
}

void
compare_sorts2 (const void *base, size_t nel)
{
	size_t width = sizeof (teststruct_t*);
	std::vector<char> b1 (nel * width), b2 (nel * width);

	memcpy (b1.data (), base, b1.size ());
	memcpy (b2.data (), base, b2.size ());

	qsort (b1.data (), nel, width, compare_teststructs2);
	qsort_test_struct ((teststruct_t **)b2.data (), nel);

	check_sorted (b2.data (), nel, width, compare_teststructs2);
}

} // namespace

TEST (SgenQsort, ReversedInts)
{
	for (int i = 1; i < 4000; ++i) {
		std::vector<int> a (i);

		for (int j = 0; j < i; ++j)
			a [j] = i - j - 1;
		ASSERT_NO_FATAL_FAILURE (compare_sorts (a.data (), i, sizeof (int), compare_ints));
	}
}

TEST (SgenQsort, RandomStructs)
{
	srand ((unsigned) time (NULL));
	for (int i = 0; i < 2000; ++i) {
		teststruct_t a [200];

		for (int j = 0; j < 200; ++j) {
			a [j].key = rand ();
			a [j].val = rand ();
		}

		ASSERT_NO_FATAL_FAILURE (compare_sorts (a, 200, sizeof (teststruct_t), compare_teststructs));
	}
}

/* The sort DEF_QSORT_INLINE generated, over an array of pointers. */
TEST (SgenQsort, InlinedStructPointers)
{
	srand ((unsigned) time (NULL));
	for (int i = 0; i < 2000; ++i) {
		teststruct_t a [200];
		teststruct_t *b [200];

		for (int j = 0; j < 200; ++j) {
			a [j].key = rand ();
			a [j].val = rand ();
			b [j] = &a [j];
		}

		ASSERT_NO_FATAL_FAILURE (compare_sorts2 (b, 200));
	}
}
