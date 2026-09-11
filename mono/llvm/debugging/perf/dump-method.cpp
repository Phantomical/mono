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
	size_t epilog_offset = jinfo->has_arch_eh_info
	                                ? size - mono_jinfo_get_epilog_size (jinfo)
	                                : no_epilog_offset;

	std::vector<FrameFunction> described;
	FrameFunction fn{0, size, {}};

	if (decode_mono_unwind_ops (cfi, cfi_size, epilog_offset, fn.records))
		described.push_back (std::move (fn));

	/*
	 * The body's own map is already what publish () asks a line table for:
	 * ascending by offset and one row per offset. Nothing inlines into a classic
	 * body, so every row names the method itself.
	 */
	std::vector<DebugLine> lines;

	lines.reserve (jinfo->n_il_offsets);
	for (guint32 i = 0; i < jinfo->n_il_offsets; ++i)
		lines.push_back ({jinfo->il_offsets [i].native_offset,
		                  jinfo->il_offsets [i].il_offset, display});

	/* The room is what mono_codegen () reserves past the body. */
	publish (display.c_str (),
	         {(const uint8_t *) jinfo->code_start, size, size + code_slack ()},
	         std::move (described), std::move (lines));
}

namespace {

/// How far a record for the piece at \p code can reach: to the next piece the
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

/// The line rows for the function symbol names, or null where it has none.
const std::vector<IlLineRow> *
il_lines_for (const CompiledMethod &compiled, llvm::StringRef symbol)
{
	if (symbol == compiled.functions.front ().first)
		return &compiled.il_lines;

	for (const auto &[name, rows] : compiled.other_il_lines) {
		if (name == symbol)
			return &rows;
	}

	return nullptr;
}

/// The same, for the bodies an inliner inlined into that function.
const std::vector<IlInlineRow> *
inline_frames_for (const CompiledMethod &compiled, llvm::StringRef symbol)
{
	if (symbol == compiled.functions.front ().first)
		return &compiled.inline_frames;

	for (const auto &[name, rows] : compiled.other_inline_frames) {
		if (name == symbol)
			return &rows;
	}

	return nullptr;
}

/// Turn one function's line table into the rows a dump record carries.
///
/// perf keeps one position per address. So a row an inliner covered names the
/// innermost body inlined there, which is the one the address is running.
std::vector<DebugLine>
debug_lines (MonoMethod *method, const CompiledMethod &compiled, llvm::StringRef symbol)
{
	const std::vector<IlLineRow> *rows = il_lines_for (compiled, symbol);

	if (rows == nullptr || rows->empty ())
		return {};

	const std::vector<IlInlineRow> *inlined = inline_frames_for (compiled, symbol);
	std::string own = display_name (method, symbol);
	std::vector<DebugLine> lines;

	lines.reserve (rows->size ());
	for (const IlLineRow &row : *rows) {
		DebugLine line;

		line.offset = row.native_offset;
		line.line = row.il_offset;
		line.file = own;

		// The rows ascend by offset and then by depth, so the first row on
		// an offset is the innermost body.
		if (inlined != nullptr) {
			auto at = std::lower_bound (
				inlined->begin (), inlined->end (), row.native_offset,
				[] (const IlInlineRow &frame, uint32_t offset) {
					return frame.native_offset < offset;
				});

			if (at != inlined->end () && at->native_offset == row.native_offset) {
				line.line = at->il_offset;
				line.file = method_display_name (
					(MonoMethod *) (uintptr_t) at->callee);
			}
		}

		lines.push_back (std::move (line));
	}

	return lines;
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
		 * FDE there unwinds to a wrong answer. Leaving it out only stops the
		 * walk. */
		if (parse_unwind_records (compiled.unwind_table, compiled.unwind_table_size,
		                          code, fn.records))
			described.push_back (std::move (fn));

		std::string display = display_name (method, symbol);

		publish (display.c_str (), {code, size, room_past (code, size, compiled)},
		         std::move (described), debug_lines (method, compiled, symbol));
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
