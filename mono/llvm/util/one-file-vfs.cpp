#include "one-file-vfs.hpp"
#include <cassert>
#include <llvm/Support/Chrono.h>
#include <llvm/Support/FileSystem/UniqueID.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <system_error>

namespace vfs = llvm::vfs;

namespace mono {

namespace {
vfs::Status
default_status ()
{
	return vfs::Status ("", llvm::sys::fs::UniqueID (0, 0), llvm::sys::TimePoint<> (), 0, 0, 0,
	                    llvm::sys::fs::file_type::regular_file, llvm::sys::fs::all_all);
}

class InMemoryFile : public vfs::File {
private:
	std::shared_ptr<llvm::MemoryBuffer> buffer;

public:
	InMemoryFile (const std::shared_ptr<llvm::MemoryBuffer> &buffer) : buffer (buffer) {}

	llvm::ErrorOr<vfs::Status> status () override { return default_status (); }

	llvm::ErrorOr<std::string> getName () override { return ""; }

	llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer (const llvm::Twine &Name,
	                                                              int64_t FileSize,
	                                                              bool RequiresNullTerminator,
	                                                              bool IsVolatile) override
	{
		// buffer's bytes are never null-terminated (pushProfile (),
		// makeProfileFileSystem ()). getMemBuffer () only wraps existing
		// bytes, so a copy is the only way to add the terminator a true
		// request asks for.
		if (RequiresNullTerminator)
			return llvm::MemoryBuffer::getMemBufferCopy (buffer->getBuffer (),
			                                             buffer->getBufferIdentifier ());

		return llvm::MemoryBuffer::getMemBuffer (
			buffer->getBuffer (), buffer->getBufferIdentifier (), /*RequiresNullTerminator=*/false);
	}

	std::error_code close () override { return {}; }
};

class EmptyDirectoryIter : public vfs::detail::DirIterImpl {
public:
	EmptyDirectoryIter () { CurrentEntry = vfs::directory_entry (); }

	std::error_code increment () override { return {}; }
};

} // namespace

/// The address is the identity ExtensibleRTTI compares, so the definition has
/// to be here rather than in the header.
const char OneFileFS::ID = 0;

OneFileFS::OneFileFS (std::unique_ptr<llvm::MemoryBuffer> fallback)
    : fallback (std::move (fallback))
{
	assert (this->fallback != nullptr);
}

OneFileFS::~OneFileFS () = default;

std::error_code
OneFileFS::setCurrentWorkingDirectory (const llvm::Twine &path)
{
	// Every path reads the current buffer, so there is nothing for a working
	// directory to change.
	return {};
}

OneFileFS::CurrentFileGuard
OneFileFS::set (std::unique_ptr<llvm::MemoryBuffer> buffer)
{
	buffers.emplace_back (std::move (buffer));
	return CurrentFileGuard (this);
}

llvm::ErrorOr<vfs::Status>
OneFileFS::status (const llvm::Twine &path)
{
	return default_status ();
}

llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
OneFileFS::openFileForRead (const llvm::Twine &path)
{
	return std::make_unique<InMemoryFile> (current ());
}

vfs::directory_iterator
OneFileFS::dir_begin (const llvm::Twine &dir, std::error_code &ec)
{
	return vfs::directory_iterator (std::make_shared<EmptyDirectoryIter> ());
}

llvm::ErrorOr<std::string>
OneFileFS::getCurrentWorkingDirectory () const
{
	return std::string ();
}

} // namespace mono
