#include "debugging/perf/dump-method.hpp"

#include "debugging/perf/eh-frame.hpp"
#include "debugging/perf/jitdump.hpp"
#include "runtime/naming.hpp"
#include "sidetables.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "mini.h"

namespace mono::perf {

void
dump_method (MonoMethod *method, MonoJitInfo *jinfo)
{
	if (!enabled ())
		return;

	guint32 cfi_size = 0;
	const guint8 *cfi = mono_jinfo_get_unwind_info (jinfo, &cfi_size);
	std::string display = method_display_name (method);
	size_t size = jinfo->code_size;

	/* The room is what mono_codegen () reserves past the body. */
	publish (display.c_str (),
	         {(const uint8_t *) jinfo->code_start, size, size + code_slack ()},
	         cfi, cfi_size);
}

namespace {

/// How far a record for the piece at \p code may reach: to the next piece the
/// object placed, or past the object's last piece to where the code allocator's
/// slack ends.
size_t
room_past (const uint8_t *code, size_t size, const CompiledMethod &compiled)
{
	if (compiled.object_code != nullptr) {
		const auto &pieces = *compiled.object_code;
		auto next = std::upper_bound (
			pieces.begin (), pieces.end (), code,
			[] (const uint8_t *at, const std::pair<const uint8_t *, size_t> &piece) {
				return at < piece.first;
			});

		if (next != pieces.end ())
			return (size_t) (next->first - code);
	}

	return size + code_slack ();
}

} // namespace

void
dump_method (MonoMethod *method, const CompiledMethod &compiled)
{
	if (!enabled ())
		return;

	for (const auto &[symbol, extent] : compiled.functions) {
		const auto &[code, size] = extent;

		if (code == nullptr || size == 0)
			continue;

		std::vector<FrameFunction> described;
		FrameFunction fn{0, size, {}};

		/* A function whose block cannot be read is named and left undescribed.
		 * A body with a prologue does not keep the caller's frame, so a no-rule
		 * FDE there unwinds to a wrong answer, where leaving it out only stops
		 * the walk. */
		if (parse_unwind_records (compiled.unwind_table, compiled.unwind_table_size,
		                          code, fn.records))
			described.push_back (std::move (fn));

		std::string display = display_name (method, symbol);

		publish (display.c_str (), {code, size, room_past (code, size, compiled)},
		         std::move (described));
	}

	for (const auto &[code, size] : compiled.linker_stubs) {
		if (code == nullptr || size == 0)
			continue;

		/* An FDE with no rules says the piece still has the frame it was
		 * called with, which is what a jump has.
		 *
		 * The stubs belong to the object, and the member that carries them is
		 * whichever of a batch came first, so no method's name is right here. */
		publish ("linker stubs", {code, size, room_past (code, size, compiled)},
		         {FrameFunction{0, size, {}}});
	}
}

} // namespace mono::perf
