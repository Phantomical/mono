/*
 * Tests for OneFileFS, the single-file VFS the tier-2 pipeline reads a PGO
 * profile through.
 *
 * Pure LLVM: the class wraps an in-memory llvm::MemoryBuffer and never
 * touches mono state.
 */

#include "util/one-file-vfs.hpp"

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>

#include <gtest/gtest.h>

#include <vector>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// Storage shaped like pushProfile()'s buffers: real data, then a sentinel
/// byte that proves getBuffer() never treats it as a terminator.
/// getMemBuffer() wraps storage without copying, so the sentinel must
/// outlive every buffer this makes.
struct NonTerminatedBuffer {
	std::vector<char> storage = {'h', 'e', 'l', 'l', 'o', 'X'};

	std::unique_ptr<MemoryBuffer> take (StringRef name)
	{
		return MemoryBuffer::getMemBuffer (StringRef (storage.data (), 5), name,
		                                   /*RequiresNullTerminator=*/false);
	}
};

TEST (OneFileFS, HandsBackTheFallback)
{
	NonTerminatedBuffer source;
	OneFileFS fs (source.take ("fallback"));

	auto file = fs.openFileForRead ("whatever/path");
	ASSERT_TRUE (bool (file));

	auto buffer = (*file)->getBuffer ("whatever/path", -1, /*RequiresNullTerminator=*/false, false);
	ASSERT_TRUE (bool (buffer));
	EXPECT_EQ ((*buffer)->getBuffer (), "hello");
}

TEST (OneFileFS, HandsBackWhateverSetPushedMost)
{
	NonTerminatedBuffer fallback, pushed;
	pushed.storage = {'w', 'o', 'r', 'l', 'd', 'Y'};
	OneFileFS fs (fallback.take ("fallback"));

	auto guard = fs.set (pushed.take ("pushed"));
	auto file = fs.openFileForRead ("whatever/path");
	ASSERT_TRUE (bool (file));

	auto buffer = (*file)->getBuffer ("whatever/path", -1, /*RequiresNullTerminator=*/false, false);
	ASSERT_TRUE (bool (buffer));
	EXPECT_EQ ((*buffer)->getBuffer (), "world");
}

/// OneFileFS must honor RequiresNullTerminator even though the buffer it
/// holds was never given one itself. IndexedInstrProfReader::create() asks
/// in exactly this shape, through vfs::FileSystem::getBufferForFile()'s
/// default argument.
TEST (OneFileFS, MeetsARequestForANullTerminator)
{
	NonTerminatedBuffer source;
	OneFileFS fs (source.take ("fallback"));

	auto file = fs.openFileForRead ("whatever/path");
	ASSERT_TRUE (bool (file));

	auto buffer = (*file)->getBuffer ("whatever/path", -1, /*RequiresNullTerminator=*/true, false);
	ASSERT_TRUE (bool (buffer));
	EXPECT_EQ ((*buffer)->getBuffer (), "hello");
}

} // namespace
} // namespace test
} // namespace mono
