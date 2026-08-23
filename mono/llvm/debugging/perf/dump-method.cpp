#include "debugging/perf/dump-method.hpp"

#include "debugging/perf/jitdump.hpp"
#include "runtime/naming.hpp"

#include <string>
#include <vector>

#include "mini-runtime.h"

namespace mono::perf {

void
dump_method (MonoMethod *method, const CompiledMethod &compiled)
{
	if (!enabled ())
		return;

	/*
	 * A whole object goes in as one record. perf claims the bytes behind a record
	 * for the frame description it carries, and the functions of an object sit
	 * against each other, so a record per function claims the next function's
	 * bytes for every one but the last.
	 *
	 * The description covers them all instead: one FDE per function, and one for
	 * each of the linker's stubs with no rules at all, which says the stub still
	 * has the frame it was called with. That is what a jump has.
	 */
	const uint8_t *start = nullptr;
	const uint8_t *end = nullptr;

	auto span = [&] (const uint8_t *code, size_t size) {
		if (start == nullptr || code < start)
			start = code;
		if (end == nullptr || code + size > end)
			end = code + size;
	};

	for (const auto &[symbol, extent] : compiled.functions)
		if (extent.first != nullptr && extent.second != 0)
			span (extent.first, extent.second);

	for (const auto &[code, size] : compiled.linker_stubs)
		if (code != nullptr && size != 0)
			span (code, size);

	if (start == nullptr)
		return;

	std::string display;
	std::vector<FrameFunction> functions;

	for (const auto &[symbol, extent] : compiled.functions) {
		const auto &[code, size] = extent;

		if (code == nullptr || size == 0)
			continue;
		if (code == start)
			display = display_name (method, symbol);

		FrameFunction fn{(size_t) (code - start), size, {}};

		/* A function whose block cannot be read is left out. An FDE with no
		 * rules says the function still has the frame it was called with. A body
		 * with a prologue does not, so publishing one there unwinds to a wrong
		 * answer. Leaving the function out only stops the walk instead. */
		if (parse_unwind_records (compiled.unwind_table, compiled.unwind_table_size,
		                          code, fn.records))
			functions.push_back (std::move (fn));
	}

	for (const auto &[code, size] : compiled.linker_stubs)
		if (code != nullptr && size != 0)
			functions.push_back ({(size_t) (code - start), size, {}});

	if (display.empty ()) {
		char *full = mono_method_full_name (method, TRUE);

		display = full;
		g_free (full);
	}

	size_t extent = (size_t) (end - start);

	publish (display.c_str (), {start, extent, extent + code_slack ()},
	         std::move (functions));
}

} // namespace mono::perf
