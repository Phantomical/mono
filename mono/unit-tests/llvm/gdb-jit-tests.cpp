/*
 * Tests for the list a debugger reads JIT-produced objects out of.
 *
 * What matters here is that the list stays intact: an entry that outlives the
 * code it describes points a debugger at memory the next method has been
 * handed, so retracting one has to unlink exactly that entry and leave its
 * neighbours joined. The checks read `__jit_debug_descriptor` itself rather
 * than any bookkeeping of the registrar's, since that is what a debugger
 * walks.
 */

#include "gdb-jit.hpp"

#include <gtest/gtest.h>

#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h>

#include <vector>

extern "C" {
extern struct jit_descriptor __jit_debug_descriptor;
}

namespace mono {
namespace test {
namespace {

/// The sizes of the registered objects, newest first - enough to identify
/// entries, since each object below is given a size of its own.
std::vector<uint64_t>
registered_sizes ()
{
	std::vector<uint64_t> sizes;

	for (jit_code_entry *e = __jit_debug_descriptor.first_entry; e != nullptr;
	     e = e->next_entry)
		sizes.push_back (e->symfile_size);
	return sizes;
}

gdbjit::Registration *
publish_sized (size_t size)
{
	return gdbjit::publish (std::vector<char> (size, 'x'));
}

TEST (GdbJit, PublishLinksNewestFirst)
{
	gdbjit::Registration *a = publish_sized (11);
	gdbjit::Registration *b = publish_sized (22);
	gdbjit::Registration *c = publish_sized (33);

	EXPECT_EQ (registered_sizes (), (std::vector<uint64_t> { 33, 22, 11 }));
	EXPECT_EQ (__jit_debug_descriptor.version, 1u);

	gdbjit::retract (c);
	gdbjit::retract (b);
	gdbjit::retract (a);
	EXPECT_TRUE (registered_sizes ().empty ());
}

TEST (GdbJit, RetractFromTheMiddleJoinsNeighbours)
{
	gdbjit::Registration *a = publish_sized (11);
	gdbjit::Registration *b = publish_sized (22);
	gdbjit::Registration *c = publish_sized (33);

	gdbjit::retract (b);
	EXPECT_EQ (registered_sizes (), (std::vector<uint64_t> { 33, 11 }));

	/* Both directions: a debugger only walks forwards, but retracting the
	 * head is what proves prev_entry was mended too. */
	gdbjit::retract (c);
	EXPECT_EQ (registered_sizes (), (std::vector<uint64_t> { 11 }));

	gdbjit::retract (a);
	EXPECT_TRUE (registered_sizes ().empty ());
}

TEST (GdbJit, RetractInPublishOrderEmptiesTheList)
{
	gdbjit::Registration *a = publish_sized (11);
	gdbjit::Registration *b = publish_sized (22);

	gdbjit::retract (a);
	EXPECT_EQ (registered_sizes (), (std::vector<uint64_t> { 22 }));

	gdbjit::retract (b);
	EXPECT_TRUE (registered_sizes ().empty ());
}

TEST (GdbJit, NothingIsRegisteredForAnEmptyObject)
{
	EXPECT_EQ (gdbjit::publish (std::vector<char> ()), nullptr);
	EXPECT_TRUE (registered_sizes ().empty ());

	/* What compile () passes on when it has nothing to register. */
	gdbjit::retract (nullptr);
}

} // namespace
} // namespace test
} // namespace mono
