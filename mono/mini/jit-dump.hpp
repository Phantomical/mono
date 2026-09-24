/**
 * \file
 * \brief The dump points MONO_JIT_DUMP turns on, and where their output goes.
 *
 * Both engines print through here, so one variable selects what to see and one
 * filter selects which methods to see it for. A point is off unless
 * MONO_JIT_DUMP names it, and reading the variable happens once.
 */

#ifndef MONO_MINI_JIT_DUMP_HPP
#define MONO_MINI_JIT_DUMP_HPP

#include <cstdint>
#include <string>

typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodHeader MonoMethodHeader;

namespace mono {

/**
 * A stage of a compile that can print what it holds.
 *
 * MONO_JIT_DUMP names the ones it wants, separated by `;` or `,`, and `all`
 * names every one. The name a point carries here is the name the variable
 * takes and the name of the directory its files land in.
 */
enum class DumpPoint : uint32_t {
	il = 1u << 0,        ///< `il`: the method's CIL.
	unopt_ir = 1u << 2,  ///< `unopt-ir`: the IR the translator wrote.
	tier1_ir = 1u << 3,  ///< `tier1-ir`: that IR after the tier-1 pipeline.
	/// `tier2-inlined-ir`: the IR the tier-2 inliners leave, in front of the
	/// lowering and the optimization pipeline.
	tier2_inlined_ir = 1u << 4,
	tier2_ir = 1u << 5,   ///< `tier2-ir`: after the tier-2 pipeline.
	tier1_asm = 1u << 6,  ///< `tier1-asm`: the code tier 1 emits.
	tier2_asm = 1u << 7,  ///< `tier2-asm`: the code tier 2 emits.
	tier0_asm = 1u << 8,  ///< `tier0-asm`: the code the classic tier-0 compiler emits.
};

/// Whether MONO_JIT_DUMP asked for this point, whatever the method is.
///
/// Answers before there is a name to test, which is what a caller needs when
/// producing the name itself costs something.
bool dump_point_enabled (DumpPoint point);

/// Whether MONO_JIT_DUMP asked for any point at all.
///
/// Building the name a dump is filed under costs a full name, so a compile that
/// prints nothing asks this before paying for one.
bool any_dump_point_enabled ();

/**
 * Whether this point is on and MONO_JIT_DUMP_FILTER names this method.
 *
 * The filter is matched as a substring, and every point matches it against the
 * same string: `Class:Method (argtypes)@0xADDR`, which is what
 * `dump_name ()` builds. An unset filter takes every method.
 */
bool dumping (DumpPoint point, const char *name);

/// The string every dump point names a method by, and matches the filter
/// against. The caller owns the result.
std::string dump_name (MonoMethod *method);

/**
 * Writes one whole dump to stdout, or to a file under MONO_JIT_DUMP_DIR.
 *
 * Pass the whole dump in one call. Two compiles can dump at once, and only a
 * whole dump is kept in one piece on stdout.
 *
 * With a directory set, a writer thread writes the file after this returns.
 * mono_jit_dump_flush () waits for it. The file is
 * `<dir>/<point>/<method>.<extension>`. The first dump of a name in a run
 * overwrites whatever an earlier run left under it. A second dump under that
 * name inside the same run takes a counted suffix instead. A file that does not
 * open is reported on stderr.
 */
void write_dump (DumpPoint point, const char *name, std::string text);

/**
 * Returns a method's CIL, inside the class and the signature it is
 * declared with.
 *
 * The shape is ilasm's, so that a reader can tell an argument from a local
 * without going to the metadata. An operand that is a metadata token prints as
 * what it names, with the token itself in a comment behind it. A wrapper and a
 * dynamic method carry no metadata tokens, so every operand of one prints as
 * the raw value. So does a token that fails to load.
 */
std::string dump_il (MonoMethod *method, MonoMethodHeader *header);

} // namespace mono

#endif
