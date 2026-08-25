#include "debugging/perf/dump-method.hpp"

#include "debugging/perf/jitdump.hpp"
#include "runtime/naming.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace mono::perf {

namespace {

/// One function or linker stub of the object a method was linked into.
struct Piece {
	const uint8_t *code = nullptr;
	size_t size = 0;
	/// Empty for a stub, which carries no symbol.
	llvm::StringRef symbol;
};

/// Whether the object puts nothing of its own between these two pieces.
///
/// With no layout to read, only two pieces that touch count as adjacent. That
/// is the conservative answer: a record over them covers no other code.
bool
nothing_between (const CompiledMethod &compiled, const Piece &before, const Piece &after)
{
	const uint8_t *gap = before.code + before.size;

	if (compiled.object_code == nullptr)
		return gap == after.code;

	for (const auto &[code, size] : *compiled.object_code)
		if (code >= gap && code < after.code)
			return false;
	return true;
}

/// Publishes one run of pieces as a record, named for the piece it starts with.
void
publish_run (MonoMethod *method, const CompiledMethod &compiled, const Piece *run,
             size_t count)
{
	const uint8_t *start = run[0].code;
	size_t extent = (size_t) (run[count - 1].code + run[count - 1].size - start);
	std::vector<FrameFunction> described;
	std::string display;

	for (size_t i = 0; i < count; ++i) {
		FrameFunction fn{(size_t) (run[i].code - start), run[i].size, {}};

		/* An FDE with no rules says the piece still has the frame it was called
		 * with. That is what a jump has, so a stub gets one.
		 *
		 * A function whose block cannot be read is left out instead. A body with
		 * a prologue does not keep the caller's frame, so publishing a no-rule
		 * FDE there unwinds to a wrong answer. Leaving it out only stops the
		 * walk. */
		if (run[i].symbol.empty ())
			described.push_back (std::move (fn));
		else if (parse_unwind_records (compiled.unwind_table,
		                               compiled.unwind_table_size, run[i].code,
		                               fn.records))
			described.push_back (std::move (fn));
	}

	if (!run[0].symbol.empty ())
		display = display_name (method, run[0].symbol);
	else
		/* The stubs belong to the object, and the member that carries them is
		 * whichever of a batch came first, so no method's name is right here. */
		display = "linker stubs";

	publish (display.c_str (), {start, extent, extent + code_slack ()},
	         std::move (described));
}

} // namespace

void
dump_method (MonoMethod *method, const CompiledMethod &compiled)
{
	if (!enabled ())
		return;

	std::vector<Piece> pieces;

	for (const auto &[symbol, extent] : compiled.functions)
		if (extent.first != nullptr && extent.second != 0)
			pieces.push_back ({extent.first, extent.second, symbol});

	for (const auto &[code, size] : compiled.linker_stubs)
		if (code != nullptr && size != 0)
			pieces.push_back ({code, size, {}});

	std::sort (pieces.begin (), pieces.end (),
	           [] (const Piece &a, const Piece &b) { return a.code < b.code; });

	/*
	 * A record covers a run of this method's pieces that the object puts nothing
	 * else between. What lies between two pieces of a run is padding, and no
	 * sample lands in padding. A tier-1 promotion links a batch into one object,
	 * so a record that reached further would name another method's bytes.
	 *
	 * A record for each piece is tighter still. The linker's stubs get no slack
	 * of their own, so a record over one of them overlaps the piece after it,
	 * and perf cuts the earlier map back. A run keeps a stub inside the record
	 * of the function it sits behind.
	 */
	for (size_t i = 0; i < pieces.size ();) {
		size_t j = i + 1;

		while (j < pieces.size ()
		       && nothing_between (compiled, pieces[j - 1], pieces[j]))
			++j;
		publish_run (method, compiled, pieces.data () + i, j - i);
		i = j;
	}
}

} // namespace mono::perf
