/*
 * test-memfuncs.cpp: Unit test for our own bzero/memmove.
 *
 * Copyright (C) 2013 Xamarin Inc
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "config.h"

#include "utils/memfuncs.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vector>

#include <gtest/gtest.h>

#define POOL_SIZE	2048
#define START_OFFSET	128

#define BZERO_OFFSETS	64
#define BZERO_SIZES	256

#define MEMMOVE_SRC_OFFSETS		32
#define MEMMOVE_DEST_OFFSETS		32
#define MEMMOVE_SIZES			256
#define MEMMOVE_NONOVERLAP_START	1024

namespace {

/*
 * Three pools of POOL_SIZE bytes: one of random bytes to copy from, and two that
 * the function under test and libc's own are each applied to, so the check is
 * that the whole pool comes out identical either way.
 */
class MemFuncs : public ::testing::Test {
protected:
	void SetUp () override
	{
		random_mem.resize (POOL_SIZE);
		reference.resize (POOL_SIZE);
		playground.resize (POOL_SIZE);

		srand ((unsigned) time (NULL));
		long *words = (long*)random_mem.data ();
		for (size_t i = 0; i < POOL_SIZE / sizeof (long); ++i)
			words [i] = rand ();
	}

	void reset ()
	{
		memcpy (reference.data (), random_mem.data (), POOL_SIZE);
		memcpy (playground.data (), random_mem.data (), POOL_SIZE);
	}

	std::vector<unsigned char> random_mem, reference, playground;
};

} // namespace

TEST_F (MemFuncs, BzeroAtomic)
{
	for (int offset = 0; offset <= BZERO_OFFSETS; ++offset) {
		for (int size = 0; size <= BZERO_SIZES; ++size) {
			reset ();

			memset (reference.data () + START_OFFSET + offset, 0, size);
			mono_gc_bzero_atomic (playground.data () + START_OFFSET + offset, size);

			ASSERT_EQ (0, memcmp (reference.data (), playground.data (), POOL_SIZE))
				<< "offset " << offset << " size " << size;
		}
	}
}

TEST_F (MemFuncs, MemmoveAtomic)
{
	for (int src_offset = -MEMMOVE_SRC_OFFSETS; src_offset <= MEMMOVE_SRC_OFFSETS; ++src_offset) {
		for (int dest_offset = -MEMMOVE_DEST_OFFSETS; dest_offset <= MEMMOVE_DEST_OFFSETS; ++dest_offset) {
			for (int size = 0; size <= MEMMOVE_SIZES; ++size) {
				SCOPED_TRACE (::testing::Message ()
					<< "src " << src_offset << " dest " << dest_offset << " size " << size);

				/* overlapping */
				reset ();
				memmove (reference.data () + START_OFFSET + dest_offset,
					 reference.data () + START_OFFSET + src_offset, size);
				mono_gc_memmove_atomic (playground.data () + START_OFFSET + dest_offset,
							playground.data () + START_OFFSET + src_offset, size);
				ASSERT_EQ (0, memcmp (reference.data (), playground.data (), POOL_SIZE)) << "overlapping";

				/* non-overlapping with dest < src */
				reset ();
				memmove (reference.data () + START_OFFSET + dest_offset,
					 reference.data () + MEMMOVE_NONOVERLAP_START + src_offset, size);
				mono_gc_memmove_atomic (playground.data () + START_OFFSET + dest_offset,
							playground.data () + MEMMOVE_NONOVERLAP_START + src_offset, size);
				ASSERT_EQ (0, memcmp (reference.data (), playground.data (), POOL_SIZE)) << "dest < src";

				/* non-overlapping with dest > src */
				reset ();
				memmove (reference.data () + MEMMOVE_NONOVERLAP_START + dest_offset,
					 reference.data () + START_OFFSET + src_offset, size);
				mono_gc_memmove_atomic (playground.data () + MEMMOVE_NONOVERLAP_START + dest_offset,
							playground.data () + START_OFFSET + src_offset, size);
				ASSERT_EQ (0, memcmp (reference.data (), playground.data (), POOL_SIZE)) << "dest > src";
			}
		}
	}
}
