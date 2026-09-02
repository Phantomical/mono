#include "debugging/perf/dump-method.hpp"

#include "debugging/perf/jitdump.hpp"
#include "runtime/naming.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace mono::perf {

/// Whether the object puts nothing of its own between these two pieces.
///
/// With no layout to read, only two pieces that touch count as adjacent. That
/// is the conservative answer: a record over them covers no other code.
bool
BatchSink::nothing_between (const Piece &before, const Piece &after) const
{
	/* Two pieces from different compiles are never one object's neighbours,
	 * whatever their addresses look like. translate_and_compile_batch ()
	 * falls back to compiling a batch's members one at a time when it gives
	 * up on a shared module, and the arena that hands code out to every
	 * compile can still place two such objects back to back - merging them
	 * would ask this run's one unwind_table about a piece it says nothing
	 * about, which is indistinguishable from a body the block cannot read. */
	if (before.object_code != after.object_code)
		return false;

	const uint8_t *gap = before.code + before.size;

	if (before.object_code == nullptr)
		return gap == after.code;

	for (const auto &[code, size] : *before.object_code)
		if (code >= gap && code < after.code)
			return false;
	return true;
}

std::vector<FrameFunction>
BatchSink::described_functions (const uint8_t *start, const Piece *run, size_t count)
{
	std::vector<FrameFunction> described;

	for (size_t i = 0; i < count; ++i) {
		FrameFunction fn{(size_t) (run[i].code - start), run[i].size, {}};

		/* An FDE with no rules says the piece still has the frame it was
		 * called with. That is what a jump has, so a stub gets one.
		 *
		 * A function whose block cannot be read is left out instead. A body
		 * with a prologue does not keep the caller's frame, so publishing a
		 * no-rule FDE there unwinds to a wrong answer. Leaving it out only
		 * stops the walk. */
		if (run[i].symbol.empty ())
			described.push_back (std::move (fn));
		else if (parse_unwind_records (run[i].unwind_table,
		                               run[i].unwind_table_size, run[i].code,
		                               fn.records))
			described.push_back (std::move (fn));
	}

	return described;
}

bool
BatchSink::fits (const Piece *run, size_t count)
{
	const uint8_t *start = run[0].code;
	size_t extent = (size_t) (run[count - 1].code + run[count - 1].size - start);
	size_t room = extent + code_slack ();
	size_t frame_size =
		build_eh_frame (described_functions (start, run, count), extent)
			.bytes.size ();

	/* The same test publish () makes before it would otherwise discard the
	 * description outright - asked here first, so flush () never builds a
	 * run publish () would only take the frame back off. */
	return ((extent + 7) & ~(size_t) 7) + frame_size <= room;
}

/// Publishes one run of pieces as a record, named for the piece it starts with.
void
BatchSink::publish_run (const Piece *run, size_t count) const
{
	const uint8_t *start = run[0].code;
	size_t extent = (size_t) (run[count - 1].code + run[count - 1].size - start);
	std::string display;

	if (!run[0].symbol.empty ())
		display = display_name (run[0].owner, run[0].symbol);
	else
		/* The stubs belong to the object, and the member that carries them is
		 * whichever of a batch came first, so no method's name is right here. */
		display = "linker stubs";

	publish (display.c_str (), {start, extent, extent + code_slack ()},
	         described_functions (start, run, count));
}

void
BatchSink::add (MonoMethod *method, const CompiledMethod &compiled)
{
	if (!enabled ())
		return;

	for (const auto &[symbol, extent] : compiled.functions)
		if (extent.first != nullptr && extent.second != 0)
			pieces_.push_back ({extent.first, extent.second, symbol, method,
			                    compiled.object_code, compiled.unwind_table,
			                    compiled.unwind_table_size});

	for (const auto &[code, size] : compiled.linker_stubs)
		if (code != nullptr && size != 0)
			pieces_.push_back ({code, size, {}, nullptr, compiled.object_code,
			                    compiled.unwind_table,
			                    compiled.unwind_table_size});
}

void
BatchSink::flush ()
{
	if (pieces_.empty ())
		return;

	std::sort (pieces_.begin (), pieces_.end (),
	           [] (const Piece &a, const Piece &b) { return a.code < b.code; });

	/*
	 * A record covers a run of pieces the object puts nothing else between.
	 * What lies between two pieces of a run is padding, and no sample lands
	 * in padding. A batch links every member into one object, so a run that
	 * crosses from one member's pieces into the next's is exactly the case
	 * this collects them for: the two are as adjacent as two pieces of the
	 * same method's own object ever were, and drawing a record boundary at a
	 * method's edge for no reason the object itself has is what publishing
	 * each member on its own used to do.
	 *
	 * A record for each piece is tighter still. The linker's stubs get no
	 * slack of their own, so a record over one of them overlaps the piece
	 * after it, and perf cuts the earlier map back. A run keeps a stub
	 * inside the record of the function it sits behind.
	 *
	 * A run also stops short of a piece that would grow its own description
	 * past code_slack (): that room is real, but it is what the code
	 * allocator left past each piece taken alone, and it does not grow with
	 * how many pieces one record ends up naming. Publishing past it is what
	 * jitdump.hpp's own code_slack () warns against - the description
	 * reaches into the next record's range, and perf keeps whichever record
	 * it read later. Stopping here instead costs only a second record next
	 * to this one, at the same real gap the object already left.
	 */
	for (size_t i = 0; i < pieces_.size ();) {
		size_t j = i + 1;

		while (j < pieces_.size ()
		       && nothing_between (pieces_[j - 1], pieces_[j])
		       && fits (pieces_.data () + i, j - i + 1))
			++j;
		publish_run (pieces_.data () + i, j - i);
		i = j;
	}

	pieces_.clear ();
}

void
dump_method (MonoMethod *method, const CompiledMethod &compiled)
{
	BatchSink sink;

	sink.add (method, compiled);
	sink.flush ();
}

} // namespace mono::perf
