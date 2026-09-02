/**
 * \file
 * \brief Adding a compiled method, or a whole batch of them, to the perf jit
 * dump.
 *
 * Where the backend's own types meet perf. The rest of this directory is written
 * against a name and a range of bytes, and knows nothing of mono. This is the
 * translation: a second thing the profiler wants to hear about belongs here,
 * beside it, rather than back in the engine.
 */

#ifndef MONO_LLVM_DEBUGGING_PERF_DUMP_METHOD_HPP
#define MONO_LLVM_DEBUGGING_PERF_DUMP_METHOD_HPP

#include "debugging/perf/eh-frame.hpp"
#include "jit.hpp"

#include <memory>
#include <string>
#include <vector>

typedef struct _MonoMethod MonoMethod;

namespace mono::perf {

/// Name a compiled method's code in the dump, with the frame description that
/// lets a profile unwind out of it.
///
/// A record covers a run of the method's code that the object puts nothing else
/// between, so a method that shares an object with a batch claims its own bytes
/// and no more. It takes its name from the function it starts with, and a sample
/// in a filter body after that function prints under the same name.
///
/// The linker's stubs go in as "linker stubs" rather than under a method name.
///
/// Does nothing unless a dump is open, so a caller needs no guard of its own.
///
/// Equivalent to publishing a `BatchSink` of one method: a batch compile that
/// wants its whole object described in fewer, larger records should collect
/// every member into a `BatchSink` instead of calling this once per member.
void dump_method (MonoMethod *method, const CompiledMethod &compiled);

/// Collects the pieces of every method one batch compile publishes, so the
/// whole batch reaches the dump as the runs its object actually has, rather
/// than one run per method.
///
/// A record still covers only a run the object puts nothing else between - the
/// grouping is unchanged - but a run no longer stops at a method's own
/// boundary, because two batch members that sit back to back are exactly the
/// case that boundary used to draw a line through for no reason the object
/// itself has. `dump_method ()` is what publishing a batch of one comes to.
///
/// A caller may take methods that did not, in the end, share a module: a
/// batch translate_and_compile_batch () gives up on compiles its members one
/// at a time instead, each against its own object, and every add () still
/// reaches the same sink. So nothing here may assume two adds share a table -
/// each piece keeps the object_code and unwind_table it actually came from,
/// and a run stops wherever those differ even if the addresses look adjacent.
class BatchSink {
public:
	/// Takes this method's pieces into the batch. Safe to call for every
	/// member of one batch, in any order. Does nothing unless a dump is
	/// open, so a caller needs no guard of its own.
	void add (MonoMethod *method, const CompiledMethod &compiled);

	/// Publishes every run the batch has collected so far, then clears it.
	/// Safe to call on a batch that took nothing.
	void flush ();

private:
	struct Piece {
		const uint8_t *code = nullptr;
		size_t size = 0;
		/// Empty for a stub, which carries no symbol.
		std::string symbol;
		/// Null for a stub. Owned by no one method in particular otherwise:
		/// whichever member's own function this piece is.
		MonoMethod *owner = nullptr;
		/// The object this piece's own compile produced. Two pieces from
		/// different objects are never "nothing between them", whatever
		/// their addresses look like.
		std::shared_ptr<const std::vector<std::pair<const uint8_t *, size_t>>>
			object_code;
		const uint8_t *unwind_table = nullptr;
		size_t unwind_table_size = 0;
	};

	bool nothing_between (const Piece &before, const Piece &after) const;
	/// The FrameFunction list a run's pieces describe: one entry for a piece
	/// whose block parse_unwind_records () can read, an empty-rule one for a
	/// stub, and nothing for a piece whose block cannot be read.
	static std::vector<FrameFunction> described_functions (const uint8_t *start,
	                                                        const Piece *run,
	                                                        size_t count);
	/// Whether a run's own description would fit publish ()'s room for it,
	/// mirroring that check exactly rather than approximating it. flush ()
	/// asks this before extending a run any further, so a batch's own record
	/// stays inside the room the code allocator actually left past each
	/// piece - the whole reason a run otherwise stops at a real gap between
	/// two pieces, asked again for the gap a run's own description needs
	/// past its last one.
	static bool fits (const Piece *run, size_t count);
	void publish_run (const Piece *run, size_t count) const;

	std::vector<Piece> pieces_;
};

} // namespace mono::perf

#endif /* MONO_LLVM_DEBUGGING_PERF_DUMP_METHOD_HPP */
