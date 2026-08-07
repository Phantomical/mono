/**
 * \file
 * \brief Redirectable call stubs - the only address the JIT ever publishes.
 *
 * Every method is published as a stub that jumps through a writable slot, so a
 * later tier can be swapped in by writing the slot: callers keep their direct
 * call to the stub and pick up the new code on their next call. That is the
 * mechanism promotion is built on, and it is also what makes runtime detours
 * (Harmony/MonoMod) work, which is where the unusual stub geometry comes from.
 */

#ifndef MONO_LLVM_STUBS_HPP
#define MONO_LLVM_STUBS_HPP

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/TargetParser/Triple.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mono {

class CodeSlabs;
class StubSlabs;

/// Every stub published so far, by the name it was published under.
///
/// A stub costs a block out of the code slabs and an entry here; the linker is
/// told about one only when a module turns out to name it, which for most
/// methods never happens. So this is the authority on where a name's stub is
/// and where it jumps, and the linker holds a copy of the address for the
/// subset of names some module asked for.
class StubTable {
public:
	/// A published stub: the block callers enter through, and the slot its
	/// jump reads.
	struct Stub {
		void *code = nullptr;
		std::atomic<void *> *slot = nullptr;
	};

	/// What remove () took out of the table, held until the caller has
	/// undefined the names the linker knows.
	struct Removed {
		/// The names that reached the linker, which it has to be told to
		/// forget. The rest were never symbols at all.
		std::vector<std::string> defined;
		std::vector<Stub> blocks;
	};

	/// Build the table, carving stubs out of SLABS. Fails on architectures we
	/// do not emit stubs for.
	static llvm::Expected<std::unique_ptr<StubTable>>
	create (const llvm::Triple &tt, CodeSlabs &slabs);

	~StubTable ();

	/// Reserve NAME a stub jumping to TARGET and return the address callers
	/// reach it at. Fails if NAME already has one.
	llvm::Expected<void *> reserve (llvm::StringRef name, void *target);

	/// Reserve NAME a stub that hands KEY to TARGET in the register a callee's
	/// key travels in, so that one body can serve many names and still know
	/// which of them it was entered for. Redirectable like any other stub;
	/// KEY is fixed when the stub is carved.
	///
	/// Unlike reserve (), asking for a name that already has one hands back
	/// what is there rather than failing.
	llvm::Expected<void *> reserve_keyed (llvm::StringRef name, void *target,
	                                      void *key);

	/// The address NAME's stub was reserved at, or null if it has none.
	void *find (llvm::StringRef name);

	/// Point NAME's stub at TARGET.
	llvm::Error redirect (llvm::StringRef name, void *target);

	/// The address to define NAME at in the linker, or null if NAME has no
	/// stub - or has one that was already handed out to be defined, since a
	/// name is only ever defined once.
	void *claim_for_linker (llvm::StringRef name);

	/// Take NAMES out of the table, so nothing can reach their stubs by name
	/// any more. Every name must have a stub.
	llvm::Expected<Removed> remove (llvm::ArrayRef<std::string> names);

	/// Hand a removed batch's blocks back for a later reserve () to carve
	/// again. The caller has undefined its names and proved nothing can reach
	/// the stubs.
	void reclaim (Removed &&removed);

private:
	explicit StubTable (std::unique_ptr<StubSlabs> slabs);

	struct Entry {
		Stub stub;
		/// Whether the linker has been given a definition for this name.
		bool defined = false;
	};

	std::mutex mutex_;
	std::unique_ptr<StubSlabs> slabs_;
	llvm::StringMap<Entry> stubs_;
};

} // namespace mono

#endif
