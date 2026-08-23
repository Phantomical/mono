#ifndef __MONO_LLVM_UTIL_ONE_FILE_VFS_HPP__
#define __MONO_LLVM_UTIL_ONE_FILE_VFS_HPP__

#include <llvm/Support/ExtensibleRTTI.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <memory>

namespace mono {

/// A file system holding one file, whatever path it is asked for.
///
/// The file it serves is a stack, so a caller can put its own contents in front
/// of what is there and take them off again by dropping the guard. A reader
/// that opens the path once per run therefore reads whatever the run pushed.
///
/// There is always a file. The bottom of the stack is the fallback given at
/// construction, which no guard can pop, so a reader never has to cope with
/// the path being absent.
class OneFileFS : public llvm::RTTIExtends<OneFileFS, llvm::vfs::FileSystem> {
public:
	static const char ID;

private:
	std::shared_ptr<llvm::MemoryBuffer> fallback;
	llvm::SmallVector<std::shared_ptr<llvm::MemoryBuffer>, 4> buffers;

	/// Returns the content every path currently reads.
	const std::shared_ptr<llvm::MemoryBuffer> &current () const
	{
		return buffers.empty () ? fallback : buffers.back ();
	}

public:
	/// fallback is what the path reads as while nothing is pushed. It must not
	/// be null: a reader is entitled to an openable file at any time.
	explicit OneFileFS (std::unique_ptr<llvm::MemoryBuffer> fallback);
	~OneFileFS ();

	struct CurrentFileGuard {
	private:
		friend class OneFileFS;

		OneFileFS *vfs;

		CurrentFileGuard (OneFileFS *vfs) : vfs (vfs) {}

	public:
		CurrentFileGuard (CurrentFileGuard &&other) : vfs (other.vfs) { other.vfs = nullptr; }
		CurrentFileGuard &operator= (CurrentFileGuard &&other)
		{
			OneFileFS *prev = vfs;
			vfs = other.vfs;
			other.vfs = nullptr;

			if (prev && !prev->buffers.empty ())
				prev->buffers.pop_back ();

			return *this;
		}

		~CurrentFileGuard ()
		{
			if (vfs && !vfs->buffers.empty ())
				vfs->buffers.pop_back ();
		}
	};

	/// Set the data currently active for this VFS.
	CurrentFileGuard set (std::unique_ptr<llvm::MemoryBuffer> buffer);

public:
	llvm::ErrorOr<llvm::vfs::Status> status (const llvm::Twine &path) override;

	llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
	openFileForRead (const llvm::Twine &path) override;

	llvm::vfs::directory_iterator dir_begin (const llvm::Twine &dir, std::error_code &ec) override;

	std::error_code setCurrentWorkingDirectory (const llvm::Twine &path) override;

	llvm::ErrorOr<std::string> getCurrentWorkingDirectory () const override;
};

} // namespace mono

#endif
